/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Samuel Weinig <sam@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"
#include "StyleCrossfadeImage.h"

#include "AnimationUtilities.h"
#include "CSSCrossfadeValue.h"
#include "CSSValuePool.h"
#include "CachedImage.h"
#include "CachedResourceLoader.h"
#include "DeprecatedCSSOMValue.h"
#include "Document.h"
#include "GraphicsContext.h"
#include "ImageBuffer.h"
#include "ImageQualityController.h"
#include "NinePieceGeometry.h"
#include "RenderElement.h"
#include "RenderObjectDocument.h"
#include "StylePrimitiveNumericTypes+Blending.h"
#include "StylePrimitiveNumericTypes+Conversions.h"
#include <wtf/PointerComparison.h>

#if ENABLE(AX_CUSTOM_COLOR_MODE)
#include <WebKitAdditions/AXCustomColorModeController.h>
#endif

namespace WebCore {
namespace Style {

CrossfadeImage::CrossfadeImage(RefPtr<Image>&& from, RefPtr<Image>&& to, Progress progress, bool isPrefixed)
    : GeneratedImage { Type::CrossfadeImage, CrossfadeImage::isFixedSize }
    , m_from { WTF::move(from) }
    , m_to { WTF::move(to) }
    , m_progress { progress }
    , m_isPrefixed { isPrefixed }
    , m_inputImagesAreReady { false }
{
}

CrossfadeImage::~CrossfadeImage()
{
    if (m_cachedFromImage)
        protect(m_cachedFromImage)->removeClient(*this);
    if (m_cachedToImage)
        protect(m_cachedToImage)->removeClient(*this);
}

bool CrossfadeImage::operator==(const Image& other) const
{
    auto* otherCrossfadeImage = dynamicDowncast<CrossfadeImage>(other);
    return otherCrossfadeImage && equals(*otherCrossfadeImage);
}

bool CrossfadeImage::equals(const CrossfadeImage& other) const
{
    return equalInputImages(other)
        && m_progress == other.m_progress;
}

bool CrossfadeImage::equalInputImages(const CrossfadeImage& other) const
{
    return arePointingToEqualData(m_from, other.m_from)
        && arePointingToEqualData(m_to, other.m_to);
}

RefPtr<CrossfadeImage> CrossfadeImage::blend(const CrossfadeImage& from, const BlendingContext& context) const
{
    ASSERT(equalInputImages(from));

    if (!m_cachedToImage || !m_cachedFromImage)
        return nullptr;

    auto newProgress = Style::blend(from.m_progress, m_progress, context);
    return CrossfadeImage::create(m_from, m_to, newProgress, from.m_isPrefixed && m_isPrefixed);
}

Ref<CSSValue> CrossfadeImage::computedStyleValue(const Style::ComputedStyle& style) const
{
    auto fromComputedValue = m_from ? protect(m_from)->computedStyleValue(style) : upcast<CSSValue>(CSSKeywordValue::create(CSSValueNone));
    auto toComputedValue = m_to ? protect(m_to)->computedStyleValue(style) : upcast<CSSValue>(CSSKeywordValue::create(CSSValueNone));

    return CSSCrossfadeValue::create(
        WTF::move(fromComputedValue),
        WTF::move(toComputedValue),
        toCSS(m_progress, style),
        m_isPrefixed
    );
}

Ref<DeprecatedCSSOMValue> CrossfadeImage::computedStyleDeprecatedCSSOMValue(CSSValuePool&, const Style::ComputedStyle& style, CSSStyleDeclaration& owner) const
{
    return computedStyleValue(style)->createDeprecatedCSSOMWrapper(owner);
}

bool CrossfadeImage::isPending() const
{
    if (m_from && protect(m_from)->isPending())
        return true;
    if (m_to && protect(m_to)->isPending())
        return true;
    return false;
}

void CrossfadeImage::load(CachedResourceLoader& loader, const ResourceLoaderOptions& options)
{
    auto oldCachedFromImage = m_cachedFromImage;
    auto oldCachedToImage = m_cachedToImage;

    if (m_from) {
        RefPtr from = m_from;
        if (from->isPending())
            from->load(loader, options);
        m_cachedFromImage = from->cachedImage();
    } else
        m_cachedFromImage = nullptr;

    if (m_to) {
        RefPtr to = m_to;
        if (to->isPending())
            to->load(loader, options);
        m_cachedToImage = to->cachedImage();
    } else
        m_cachedToImage = nullptr;

    if (m_cachedFromImage != oldCachedFromImage) {
        if (oldCachedFromImage)
            protect(oldCachedFromImage)->removeClient(*this);
        if (m_cachedFromImage)
            protect(m_cachedFromImage)->addClient(*this);
    }

    if (m_cachedToImage != oldCachedToImage) {
        if (oldCachedToImage)
            protect(oldCachedToImage)->removeClient(*this);
        if (m_cachedToImage)
            protect(m_cachedToImage)->addClient(*this);
    }

    m_inputImagesAreReady = true;
}

static void drawCrossfadeInput(GraphicsContext& context, const RenderElement& renderer, const Image& input, ImagePaintingOptions inputOptions, CompositeOperator operation, float opacity, const FloatSize& crossfadeSize, bool isForFirstLine)
{
    // SVGImage resets the opacity when painting, so we have to use transparency layers to accurately paint one at a given opacity.
    bool useTransparencyLayer = input.drawsSVGImage();

    GraphicsContextStateSaver stateSaver(context);

    auto options = inputOptions;
    if (useTransparencyLayer) {
        context.setCompositeOperation(operation);
        context.beginTransparencyLayer(opacity);
    } else {
        context.setAlpha(opacity);
        options = { options, operation };
    }

    auto rect = FloatRect { { }, crossfadeSize };
    input.draw(context, renderer, ConcreteObjectSize::fixed(crossfadeSize), rect, rect, options, isForFirstLine);

    if (useTransparencyLayer)
        context.endTransparencyLayer();
}

ImageDrawResult CrossfadeImage::drawCrossfade(GraphicsContext& context, const RenderElement& renderer, const FloatSize& crossfadeSize, bool isForFirstLine) const
{
    if (crossfadeSize.isEmpty())
        return ImageDrawResult::DidNothing;

    ImagePaintingOptions inputOptions;
#if ENABLE(AX_CUSTOM_COLOR_MODE)
    inputOptions = ImagePaintingOptions { AXCustomColorModeController::shouldInvertSVGImage(renderer) ? InvertContent::Yes : InvertContent::No };
#endif

    GraphicsContextStateSaver stateSaver(context);

    context.clip(FloatRect { { }, crossfadeSize });
    context.beginTransparencyLayer(1);

    auto progress = m_progress.value.value;
    drawCrossfadeInput(context, renderer, protect(*m_from), inputOptions, CompositeOperator::SourceOver, 1 - progress, crossfadeSize, isForFirstLine);
    drawCrossfadeInput(context, renderer, protect(*m_to), inputOptions, CompositeOperator::PlusLighter, progress, crossfadeSize, isForFirstLine);

    context.endTransparencyLayer();

    return ImageDrawResult::DidDraw;
}

ImageDrawResult CrossfadeImage::drawInCrossfadeSpace(GraphicsContext& context, const RenderElement& renderer, const FloatSize& crossfadeSize, const FloatRect& destination, const FloatRect& source, ImagePaintingOptions options, bool isForFirstLine) const
{
    return drawIntoDestination(context, destination, source, options, [&](GraphicsContext& context) {
        return drawCrossfade(context, renderer, crossfadeSize, isForFirstLine);
    });
}

ImageDrawResult CrossfadeImage::drawPatternInCrossfadeSpace(GraphicsContext& context, const RenderElement& renderer, const FloatSize& crossfadeSize, const FloatRect& destination, const FloatRect& tile, const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing, ImagePaintingOptions options, bool isForFirstLine) const
{
    if (auto imageBuffer = context.createImageBuffer(crossfadeSize)) {
        drawCrossfade(imageBuffer->context(), renderer, crossfadeSize, isForFirstLine);
        context.drawPattern(*imageBuffer, destination, tile, patternTransform, phase, spacing, options);
    }
    return ImageDrawResult::DidDraw;
}

ImageDrawResult CrossfadeImage::draw(GraphicsContext& context, const RenderElement& renderer, ConcreteObjectSize concreteObjectSize, const FloatRect& destination, const FloatRect& source, ImagePaintingOptions options, bool isForFirstLine) const
{
    if (isPending() || !canDrawAtSize(renderer, flooredIntSize(destination.size())))
        return ImageDrawResult::DidNothing;

    auto crossfadeSize = fixedSize(renderer);
    return drawInCrossfadeSpace(context, renderer, crossfadeSize, destination, mapSourceToSize(source, concreteObjectSize, crossfadeSize), options, isForFirstLine);
}

ImageDrawResult CrossfadeImage::drawAsPattern(GraphicsContext& context, const RenderElement& renderer, ConcreteObjectSize concreteObjectSize, const FloatRect& destination, const FloatRect& tile, const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing, ImagePaintingOptions options, bool isForFirstLine) const
{
    auto size = concreteObjectSize.size() * concreteObjectSize.zoom();
    if (!canDrawAtSize(renderer, size) || context.paintingDisabled())
        return ImageDrawResult::DidNothing;

    return drawPatternInCrossfadeSpace(context, renderer, fixedSize(renderer), destination, tile, patternTransform, phase, spacing, options, isForFirstLine);
}

ImageDrawResult CrossfadeImage::drawTiled(GraphicsContext& context, const RenderElement& renderer, ConcreteObjectSize, const FloatRect& destination, const FloatPoint& phase, const FloatSize& tileSize, const FloatSize& spacing, ImagePaintingOptions options, bool isForFirstLine) const
{
    if (!canDrawAtSize(renderer, tileSize) || context.paintingDisabled())
        return ImageDrawResult::DidNothing;

    auto crossfadeSize = fixedSize(renderer);
    return drawTiledUsing(context, NaturalDimensions::fixed(crossfadeSize), [&](GraphicsContext& context, ConcreteObjectSize, const FloatRect& destination, const FloatRect& source) {
        return drawInCrossfadeSpace(context, renderer, crossfadeSize, destination, source, options, isForFirstLine);
    }, [&](GraphicsContext& context, ConcreteObjectSize, const FloatRect& destination, const FloatRect& tile, const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing) {
        return drawPatternInCrossfadeSpace(context, renderer, crossfadeSize, destination, tile, patternTransform, phase, spacing, options, isForFirstLine);
    }, ConcreteObjectSize::fixed(crossfadeSize), destination, phase, tileSize, spacing, options);
}

ImageDrawResult CrossfadeImage::drawNinePiece(GraphicsContext& context, const RenderElement& renderer, ConcreteObjectSize concreteObjectSize, const NinePieceGeometry& geometry, ImagePaintingOptions options) const
{
    auto size = concreteObjectSize.size() * concreteObjectSize.zoom();
    if (!canDrawAtSize(renderer, size) || context.paintingDisabled())
        return ImageDrawResult::DidNothing;

    auto crossfadeSize = fixedSize(renderer);
    return drawNinePieceUsing(context, [&](GraphicsContext& context, ConcreteObjectSize, const FloatRect& destination, const FloatRect& source) {
        return drawInCrossfadeSpace(context, renderer, crossfadeSize, destination, source, options, false);
    }, [&](GraphicsContext& context, ConcreteObjectSize, const FloatRect& destination, const FloatRect& tile, const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing) {
        return drawPatternInCrossfadeSpace(context, renderer, crossfadeSize, destination, tile, patternTransform, phase, spacing, { options.compositeOperator(), options.interpolationQuality() }, false);
    }, ConcreteObjectSize::fixed(crossfadeSize), geometry);
}

bool CrossfadeImage::currentFrameIsComplete(const RenderElement* renderer) const
{
    if (m_from && !protect(m_from)->currentFrameIsComplete(renderer))
        return false;
    if (m_to && !protect(m_to)->currentFrameIsComplete(renderer))
        return false;
    return true;
}

bool CrossfadeImage::knownToBeOpaque(const RenderElement& renderer) const
{
    if (m_from && !protect(m_from)->knownToBeOpaque(renderer))
        return false;
    if (m_to && !protect(m_to)->knownToBeOpaque(renderer))
        return false;
    return true;
}

bool CrossfadeImage::canDrawAtSize(const RenderElement& renderer, const FloatSize& size) const
{
    return !size.isEmpty() && m_from && m_to && protect(m_from)->canDrawAtSize(renderer, size) && protect(m_to)->canDrawAtSize(renderer, size);
}

FloatSize CrossfadeImage::fixedSize(const RenderElement& renderer) const
{
    if (!m_from || !m_to)
        return { };

    auto fromImageSize = protect(m_from)->imageSize(&renderer, 1);
    auto toImageSize = protect(m_to)->imageSize(&renderer, 1);

    // Rounding issues can cause transitions between images of equal size to return
    // a different fixed size; avoid performing the interpolation if the images are the same size.
    if (fromImageSize == toImageSize)
        return fromImageSize;

    float progress = m_progress.value.value;
    float inverseProgress = 1 - progress;

    return fromImageSize * inverseProgress + toImageSize * progress;
}

InterpolationQuality CrossfadeImage::interpolationQualityForImageDraw(GraphicsContext& context, const RenderElement& renderer, const void* layer, const LayoutSize& size) const
{
    return ImageQualityController::chooseInterpolationQualityForBitmapOfSize(context, renderer, expandedIntSize(fixedSize(renderer)), layer, size);
}

NaturalDimensions CrossfadeImage::naturalDimensions(const RenderElement& renderer, const ImageSizingContext&) const
{
    // FIXME: Add support for negotiating each input in the given context as per https://drafts.csswg.org/css-images-4/#cross-fade-sizing.

    return NaturalDimensions::fixed(floorSizeToDevicePixels(LayoutSize(fixedSize(renderer)), protect(renderer.document())->deviceScaleFactor()));
}

void CrossfadeImage::imageChanged(WebCore::CachedImage*, const IntRect*)
{
    if (!m_inputImagesAreReady)
        return;
    for (auto entry : clients()) {
        CheckedRef client = entry.key;
        client->imageChanged(static_cast<WrappedImagePtr>(this));
    }
}

} // namespace Style
} // namespace WebCore
