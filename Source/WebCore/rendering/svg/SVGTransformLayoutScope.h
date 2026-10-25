/*
 * Copyright (C) 2020, 2021, 2022, 2026 Igalia S.L.
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

#pragma once

#include "RenderLayerModelObject.h"
#include <wtf/CheckedRef.h>

namespace WebCore {

class SVGTransformLayoutScope {
    WTF_MAKE_NONCOPYABLE(SVGTransformLayoutScope);
public:
    SVGTransformLayoutScope(RenderLayerModelObject& renderer)
        : m_renderer(renderer)
    {
        bool hasLayer = m_renderer->hasLayer();
        if (hasLayer || m_renderer->transformReferenceBoxIsSVGViewport())
            m_transformReferenceBox = m_renderer->transformReferenceBoxRect();

        if (hasLayer)
            m_layerTransform = m_renderer->layerTransform();

        // Always call updateLayerTransform(), even without a layer. SVG renderers
        // (e.g. RenderSVGViewportContainer) compute supplemental transforms (viewBox,
        // zoom, pan) in their override, which are needed by applyTransform() during painting.
        m_renderer->updateLayerTransform();
    }

    ~SVGTransformLayoutScope()
    {
        if (m_renderer->transformReferenceBoxRect() == m_transformReferenceBox)
            return;

        m_renderer->updateLayerTransform();
    }

    // FIXME: m_layerTransform points at the layer's own matrix, which updateLayerTransform() rewrites
    // in place, so this never sees a change unless the layer gains or loses its transform.
    bool layerTransformChanged() const
    {
        auto* layerTransform = m_renderer->layerTransform();

        bool hasTransform = !!layerTransform;
        bool hadTransform = !!m_layerTransform;
        if (hasTransform != hadTransform)
            return true;

        return hasTransform && (*layerTransform != *m_layerTransform);
    }

private:
    const CheckedRef<RenderLayerModelObject> m_renderer;
    std::optional<FloatRect> m_transformReferenceBox;
    TransformationMatrix* m_layerTransform { nullptr };
};

} // namespace WebCore

