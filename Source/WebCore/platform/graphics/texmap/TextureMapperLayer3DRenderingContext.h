/*
 * Copyright (C) 2024 Jani Hautakangas <jani@kodegood.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "FloatPolygon3D.h"
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class ClipPath;
class FloatPlane3D;
class TextureMapper;
class TextureMapperLayer;

class TextureMapperLayer3DRenderingContext final {
    WTF_MAKE_TZONE_ALLOCATED(TextureMapperLayerPreserves3DContext);
public:
    void paint(TextureMapper&, const Vector<TextureMapperLayer*>&,
        const std::function<void(TextureMapperLayer*, const ClipPath&)>&);

private:
    enum class PolygonPosition {
        InFront,
        Behind,
        Coplanar,
        Intersecting
    };

    struct TextureMapperLayerPolygon final {
        FloatPolygon3D geometry;
        TextureMapperLayer* layer = { nullptr };
        bool isSplitted = { false };
        unsigned clipVertexBufferOffset { 0 };
    };

    struct TextureMapperLayerNode final {
        WTF_MAKE_STRUCT_FAST_ALLOCATED;

        explicit TextureMapperLayerNode(TextureMapperLayerPolygon&& polygon)
        {
            polygons.append(WTFMove(polygon));
        }

        const TextureMapperLayerPolygon& firstPolygon() const  { return polygons[0]; }

        Vector<TextureMapperLayerPolygon> polygons;
        std::unique_ptr<TextureMapperLayerNode> frontNode;
        std::unique_ptr<TextureMapperLayerNode> backNode;
    };

    void buildTree(TextureMapperLayerNode&, Deque<TextureMapperLayerPolygon>&);
    void traverseTree(TextureMapperLayerNode&, const std::function<void(TextureMapperLayerNode&)>&);
    static PolygonPosition classifyPolygon(const TextureMapperLayerPolygon&, const FloatPlane3D&);
};

} // namespace WebCore
