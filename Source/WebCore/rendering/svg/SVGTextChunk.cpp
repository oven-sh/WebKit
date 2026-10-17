/*
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 * Copyright (C) 2015-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2017 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "SVGTextChunk.h"

#include "RenderSVGInlineText.h"
#include "SVGTextContentElement.h"
#include "SVGTextFragment.h"
#include "StyleComputedStyle+GettersInlines.h"

namespace WebCore {

SVGTextChunk::SVGTextChunk(const RenderSVGInlineText& firstText)
{
    CheckedRef style = firstText.style();

    if (style->writingMode().isBidiRTL())
        m_chunkStyle.add(ChunkStyle::RightToLeftText);

    if (style->writingMode().isVertical())
        m_chunkStyle.add(ChunkStyle::VerticalText);

    switch (style->textAnchor()) {
    case TextAnchor::Start:
        break;
    case TextAnchor::Middle:
        m_chunkStyle.add(ChunkStyle::MiddleAnchor);
        break;
    case TextAnchor::End:
        m_chunkStyle.add(ChunkStyle::EndAnchor);
        break;
    }

    if (RefPtr textContentElement = SVGTextContentElement::elementFromRenderer(firstText.parent())) {
        m_textContentElement = textContentElement.get();
        SVGLengthContext lengthContext(textContentElement.get());
        m_desiredTextLength = textContentElement->specifiedTextLength().value(lengthContext);

        switch (textContentElement->lengthAdjust()) {
        case SVGLengthAdjustUnknown:
            break;
        case SVGLengthAdjustSpacing:
            m_chunkStyle.add(ChunkStyle::LengthAdjustSpacing);
            break;
        case SVGLengthAdjustSpacingAndGlyphs:
            m_chunkStyle.add(ChunkStyle::LengthAdjustSpacingAndGlyphs);
            break;
        }
    }
}

void SVGTextChunk::appendFragments(std::span<SVGTextFragment> fragments)
{
    if (fragments.empty())
        return;
    m_fragmentRanges.constructAndAppend(fragments);
}

const SVGTextFragment* SVGTextChunk::firstFragment() const
{
    if (m_fragmentRanges.isEmpty())
        return nullptr;
    return &m_fragmentRanges.first().front();
}

unsigned SVGTextChunk::totalCharacters() const
{
    unsigned characters = 0;
    for (auto fragments : m_fragmentRanges) {
        for (auto& fragment : fragments)
            characters += fragment.length;
    }
    return characters;
}

float SVGTextChunk::totalLength() const
{
    if (m_fragmentRanges.isEmpty())
        return 0;

    auto& firstFragment = m_fragmentRanges.first().front();
    auto& lastFragment = m_fragmentRanges.last().back();

    if (isVerticalText())
        return (lastFragment.y + lastFragment.height) - firstFragment.y;

    return (lastFragment.x + lastFragment.width) - firstFragment.x;
}

float SVGTextChunk::totalAnchorShift() const
{
    float length = totalLength();
    if (m_chunkStyle.contains(ChunkStyle::MiddleAnchor))
        return -length / 2;
    return (m_chunkStyle.contains(ChunkStyle::EndAnchor) == m_chunkStyle.contains(ChunkStyle::RightToLeftText)) ? 0 : -length;
}

void SVGTextChunk::layout() const
{
    // ElementGroup mode: textLength was already applied by SVGTextChunkBuilder. webkit.org/b/61855.
    if (hasDesiredTextLength() && m_textLengthLayoutMode == TextLengthLayoutMode::SingleChunk) {
        if (hasLengthAdjustSpacing())
            processTextLengthSpacingCorrection();
        else {
            ASSERT(hasLengthAdjustSpacingAndGlyphs());
            applySpacingAndGlyphsTransform();
        }
    }

    if (hasTextAnchor())
        processTextAnchorCorrection();
}

void SVGTextChunk::processTextLengthSpacingCorrection() const
{
    float textLengthShift = 0;
    if (totalCharacters() > 1)
        textLengthShift = (desiredTextLength() - totalLength()) / (totalCharacters() - 1);

    bool isVerticalText = this->isVerticalText();
    unsigned atCharacter = 0;

    for (auto fragments : m_fragmentRanges) {
        for (auto& fragment : fragments) {
            if (isVerticalText)
                fragment.y += textLengthShift * atCharacter;
            else
                fragment.x += textLengthShift * atCharacter;

            atCharacter += fragment.length;
        }
    }
}

void SVGTextChunk::applySpacingAndGlyphsTransform() const
{
    auto* fragment = firstFragment();
    if (!fragment)
        return;

    float scale = desiredTextLength() / totalLength();

    AffineTransform spacingAndGlyphsTransform;
    spacingAndGlyphsTransform.translate(fragment->x, fragment->y);

    if (isVerticalText())
        spacingAndGlyphsTransform.scaleNonUniform(1, scale);
    else
        spacingAndGlyphsTransform.scaleNonUniform(scale, 1);

    spacingAndGlyphsTransform.translate(-fragment->x, -fragment->y);

    setLengthAdjustTransform(spacingAndGlyphsTransform);
}

void SVGTextChunk::setLengthAdjustTransform(const AffineTransform& transform) const
{
    for (auto fragments : m_fragmentRanges) {
        for (auto& fragment : fragments) {
            ASSERT(fragment.lengthAdjustTransform.isIdentity());
            fragment.lengthAdjustTransform = transform;
        }
    }
}

void SVGTextChunk::processTextAnchorCorrection() const
{
    float textAnchorShift = totalAnchorShift();
    bool isVerticalText = this->isVerticalText();

    for (auto fragments : m_fragmentRanges) {
        for (auto& fragment : fragments) {
            if (isVerticalText)
                fragment.y += textAnchorShift;
            else
                fragment.x += textAnchorShift;
        }
    }
}

} // namespace WebCore
