/*
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 * Copyright (C) 2015 Apple Inc. All rights reserved.
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
#include "SVGTextChunkBuilder.h"

#include "AffineTransform.h"
#include "SVGElement.h"
#include "SVGLengthContext.h"
#include "SVGTextContentElement.h"
#include "SVGTextFragment.h"
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {

SVGTextChunkBuilder::SVGTextChunkBuilder() = default;

unsigned SVGTextChunkBuilder::totalCharacters() const
{
    unsigned characters = 0;
    for (const auto& chunk : m_textChunks)
        characters += chunk.totalCharacters();
    return characters;
}

float SVGTextChunkBuilder::totalLength() const
{
    float length = 0;
    for (const auto& chunk : m_textChunks)
        length += chunk.totalLength();
    return length;
}

float SVGTextChunkBuilder::totalAnchorShift() const
{
    float anchorShift = 0;
    for (const auto& chunk : m_textChunks)
        anchorShift += chunk.totalAnchorShift();
    return anchorShift;
}

void SVGTextChunkBuilder::buildTextChunks(std::span<const SVGTextChunkBox> boxes, std::span<const unsigned> chunkStarts)
{
    for (auto& box : boxes) {
        auto boxChunkStarts = consumeSpan(chunkStarts, box.chunkStartCount);
        auto fragments = box.fragments;
        size_t rangeStart = 0;

        for (auto chunkStart : boxChunkStarts) {
            ASSERT(chunkStart >= rangeStart && chunkStart < fragments.size());
            if (!m_textChunks.isEmpty())
                m_textChunks.last().appendFragments(fragments.subspan(rangeStart, chunkStart - rangeStart));
            m_textChunks.append(SVGTextChunk(box.text));
            rangeStart = chunkStart;
        }

        if (!m_textChunks.isEmpty())
            m_textChunks.last().appendFragments(fragments.subspan(rangeStart));
    }
    ASSERT(chunkStarts.empty());
}

void SVGTextChunkBuilder::layoutTextChunks(std::span<const SVGTextChunkBox> boxes, std::span<const unsigned> chunkStarts)
{
    buildTextChunks(boxes, chunkStarts);
    if (m_textChunks.isEmpty())
        return;

    applyElementLevelTextLength();

    for (const auto& chunk : m_textChunks)
        chunk.layout();

    m_textChunks.clear();
}

void SVGTextChunkBuilder::applyElementLevelTextLength()
{
    if (m_textChunks.size() < 2)
        return;

    HashMap<CheckedRef<const SVGTextContentElement>, Vector<SVGTextChunk*>> chunksByOwner;
    for (auto& chunk : m_textChunks) {
        if (!chunk.hasDesiredTextLength() || !chunk.m_textContentElement)
            continue;
        chunksByOwner.add(CheckedRef { *chunk.m_textContentElement }, Vector<SVGTextChunk*> { }).iterator->value.append(&chunk);
    }

    for (auto& [owner, chunks] : chunksByOwner) {
        if (chunks.size() < 2)
            continue;

        auto* representative = chunks.first();

        // FIXME: webkit.org/b/61855 — extend to lengthAdjust="spacing".
        if (!representative->hasLengthAdjustSpacingAndGlyphs())
            continue;

        // Sum of glyph advances across the element (SVG2 §11.10).
        float groupTotalLength = 0;
        const SVGTextFragment* groupFirstFragment = nullptr;
        for (auto* chunk : chunks) {
            groupTotalLength += chunk->totalLength();
            if (!groupFirstFragment)
                groupFirstFragment = chunk->firstFragment();
        }

        if (!groupFirstFragment || groupTotalLength <= 0)
            continue;

        float scale = representative->desiredTextLength() / groupTotalLength;
        AffineTransform transform;
        transform.translate(groupFirstFragment->x, groupFirstFragment->y);
        if (representative->isVerticalText())
            transform.scaleNonUniform(1, scale);
        else
            transform.scaleNonUniform(scale, 1);
        transform.translate(-groupFirstFragment->x, -groupFirstFragment->y);

        for (auto* chunk : chunks) {
            chunk->setLengthAdjustTransform(transform);
            chunk->m_textLengthLayoutMode = SVGTextChunk::TextLengthLayoutMode::ElementGroup;
        }
    }
}

}
