/*
 * Copyright (C) 2021-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CGDisplayListImageBufferBackend.h"

#if ENABLE(CG_DISPLAY_LIST_BACKED_IMAGE_BUFFER)

#include "Logging.h"
#include <WebCore/GraphicsContextCG.h>
#include <WebCore/PixelBuffer.h>
#include <WebKitAdditions/CGDisplayListImageBufferAdditions.h>
#include <wtf/IsoMallocInlines.h>

namespace WebKit {

class GraphicsContextCGDisplayList : public WebCore::GraphicsContextCG {
public:
    GraphicsContextCGDisplayList(CGContextRef cgContext, const CGDisplayListImageBufferBackend::Parameters& parameters)
        : GraphicsContextCG(cgContext)
    {
        m_immutableBaseTransform.scale(parameters.resolutionScale, -parameters.resolutionScale);
        m_immutableBaseTransform.translate(0, -parameters.logicalSize.height());
        m_inverseImmutableBaseTransform = *m_immutableBaseTransform.inverse();
    }

    void setCTM(const WebCore::AffineTransform& transform) final
    {
        GraphicsContextCG::setCTM(m_inverseImmutableBaseTransform * transform);
    }

    WebCore::AffineTransform getCTM(IncludeDeviceScale includeDeviceScale) const final
    {
        return m_immutableBaseTransform * GraphicsContextCG::getCTM(includeDeviceScale);
    }

    bool canUseShadowBlur() const final { return false; }

private:
    WebCore::AffineTransform m_immutableBaseTransform;
    WebCore::AffineTransform m_inverseImmutableBaseTransform;
};

WTF_MAKE_ISO_ALLOCATED_IMPL(CGDisplayListImageBufferBackend);

size_t CGDisplayListImageBufferBackend::calculateMemoryCost(const Parameters& parameters)
{
    // FIXME: This is fairly meaningless, because we don't actually have a bitmap, and
    // should really be based on the encoded data size.
    auto backendSize = calculateBackendSize(parameters);
    return WebCore::ImageBufferBackend::calculateMemoryCost(backendSize, calculateBytesPerRow(backendSize));
}

std::unique_ptr<CGDisplayListImageBufferBackend> CGDisplayListImageBufferBackend::create(const Parameters& parameters)
{
    auto logicalSize = parameters.logicalSize;
    if (logicalSize.isEmpty())
        return nullptr;

    auto cgContext = adoptCF(WKCGCommandsContextCreate(logicalSize, nullptr));
    if (!cgContext)
        return nullptr;

    auto context = makeUnique<GraphicsContextCGDisplayList>(cgContext.get(), parameters);
    return std::unique_ptr<CGDisplayListImageBufferBackend>(new CGDisplayListImageBufferBackend(parameters, WTFMove(context)));
}

std::unique_ptr<CGDisplayListImageBufferBackend> CGDisplayListImageBufferBackend::create(const Parameters& parameters, const WebCore::ImageBuffer::CreationContext&)
{
    return CGDisplayListImageBufferBackend::create(parameters);
}

CGDisplayListImageBufferBackend::CGDisplayListImageBufferBackend(const Parameters& parameters, std::unique_ptr<WebCore::GraphicsContext>&& context)
    : ImageBufferCGBackend { parameters }
    , m_context { WTFMove(context) }
{
}

ImageBufferBackendHandle CGDisplayListImageBufferBackend::createBackendHandle(SharedMemory::Protection) const
{
    auto data = adoptCF(WKCGCommandsContextCopyEncodedData(m_context->platformContext()));
    ASSERT(data);

#if !RELEASE_LOG_DISABLED
    auto size = backendSize();
    RELEASE_LOG(RemoteLayerTree, "CGDisplayListImageBufferBackend of size %dx%d encoded display list of %ld bytes", size.width(), size.height(), CFDataGetLength(data.get()));
#endif

    return ImageBufferBackendHandle { IPC::SharedBufferReference { WebCore::SharedBuffer::create(data.get()) } };
}

WebCore::GraphicsContext& CGDisplayListImageBufferBackend::context() const
{
    return *m_context;
}

WebCore::IntSize CGDisplayListImageBufferBackend::backendSize() const
{
    return calculateBackendSize(m_parameters);
}

unsigned CGDisplayListImageBufferBackend::bytesPerRow() const
{
    return calculateBytesPerRow(backendSize());
}

RefPtr<WebCore::NativeImage> CGDisplayListImageBufferBackend::copyNativeImage(WebCore::BackingStoreCopy) const
{
    ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<WebCore::PixelBuffer> CGDisplayListImageBufferBackend::getPixelBuffer(const WebCore::PixelBufferFormat&, const WebCore::IntRect&, const WebCore::ImageBufferAllocator&) const
{
    ASSERT_NOT_REACHED();
    return nullptr;
}

void CGDisplayListImageBufferBackend::putPixelBuffer(const WebCore::PixelBuffer&, const WebCore::IntRect&, const WebCore::IntPoint&, WebCore::AlphaPremultiplication)
{
    ASSERT_NOT_REACHED();
}

}

#endif // ENABLE(CG_DISPLAY_LIST_BACKED_IMAGE_BUFFER)
