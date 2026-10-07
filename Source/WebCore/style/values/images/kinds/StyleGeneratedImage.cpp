/*
 * Copyright (C) 2000 Lars Knoll (knoll@kde.org)
 *           (C) 2000 Antti Koivisto (koivisto@kde.org)
 *           (C) 2000 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003-2021 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Samuel Weinig <sam@webkit.org>
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
 *
 */

#include "config.h"
#include "StyleGeneratedImage.h"

#include "Document.h"
#include "RenderElement.h"
#include "RenderObjectInlines.h"
#include "StyleResolver.h"

namespace WebCore {
namespace Style {

// MARK: - GeneratedImage.

GeneratedImage::GeneratedImage(Image::Type type, bool fixedSize)
    : Image { type }
    , m_fixedSize { fixedSize }
{
}

GeneratedImage::~GeneratedImage() = default;

FloatSize GeneratedImage::imageSize(const RenderElement* renderer, float multiplier, WebCore::CachedImage::SizeType) const
{
    if (!m_fixedSize)
        return m_containerSize;

    if (!renderer)
        return { };

    FloatSize fixedSize = this->fixedSize(*renderer);
    if (multiplier == 1.0f)
        return fixedSize;

    float width = fixedSize.width() * multiplier;
    float height = fixedSize.height() * multiplier;

    // Don't let images that have a width/height >= 1 shrink below 1 device pixel when zoomed.
    float deviceScaleFactor = protect(renderer->document())->deviceScaleFactor();
    if (fixedSize.width() > 0)
        width = std::max<float>(1 / deviceScaleFactor, width);
    if (fixedSize.height() > 0)
        height = std::max<float>(1 / deviceScaleFactor, height);

    return { width, height };
}

NaturalDimensions GeneratedImage::naturalDimensions(const RenderElement&, const ImageSizingContext&) const
{
    return NaturalDimensions::none();
}

// MARK: Client support.

void GeneratedImage::addClient(RenderElement& renderer)
{
    if (m_clients.isEmptyIgnoringNullReferences())
        ref();

    m_clients.add(renderer);

    this->didAddClient(renderer);
}

void GeneratedImage::removeClient(RenderElement& renderer)
{
    ASSERT(m_clients.contains(renderer));
    if (!m_clients.remove(renderer))
        return;

    this->didRemoveClient(renderer);

    if (m_clients.isEmptyIgnoringNullReferences())
        deref();
}

bool GeneratedImage::hasClient(RenderElement& renderer) const
{
    return m_clients.contains(renderer);
}

} // namespace Style
} // namespace WebCore
