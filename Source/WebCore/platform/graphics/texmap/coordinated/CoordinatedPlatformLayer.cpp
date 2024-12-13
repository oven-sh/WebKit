/*
 * Copyright (C) 2024 Igalia S.L.
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
#include "CoordinatedPlatformLayer.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedBackingStoreProxy.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsContext.h"
#include "GraphicsLayerCoordinated.h"
#include <wtf/MainThread.h>

#if USE(CAIRO)
#include "CairoPaintingContext.h"
#include "CairoPaintingEngine.h"
#endif

#if USE(SKIA)
#include "SkiaPaintingEngine.h"
#endif

namespace WebCore {

Ref<CoordinatedPlatformLayer> CoordinatedPlatformLayer::create(Client& client)
{
    return adoptRef(*new CoordinatedPlatformLayer(&client));
}

Ref<CoordinatedPlatformLayer> CoordinatedPlatformLayer::create()
{
    return adoptRef(*new CoordinatedPlatformLayer(nullptr));
}

CoordinatedPlatformLayer::CoordinatedPlatformLayer(Client* client)
    : m_client(client)
    , m_id(PlatformLayerIdentifier::generate())
{
    ASSERT(isMainThread());
    m_nicosia.layer = Nicosia::CompositionLayer::create(m_id.object().toUInt64());
}

CoordinatedPlatformLayer::~CoordinatedPlatformLayer() = default;

void CoordinatedPlatformLayer::setOwner(GraphicsLayerCoordinated* owner)
{
    ASSERT(isMainThread());
    if (m_owner == owner)
        return;

    m_owner = owner;
    if (!m_client)
        return;

    if (m_owner)
        m_client->attachLayer(*this);
    else {
        purgeBackingStores();
        m_client->detachLayer(*this);
    }
}

GraphicsLayerCoordinated* CoordinatedPlatformLayer::owner() const
{
    ASSERT(isMainThread());
    return m_owner;
}

void CoordinatedPlatformLayer::invalidateClient()
{
    ASSERT(isMainThread());
    purgeBackingStores();
    m_client = nullptr;
}

void CoordinatedPlatformLayer::notifyCompositionRequired()
{
    if (!m_client)
        return;
    m_client->notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setPosition(FloatPoint&& position)
{
    ASSERT(m_lock.isHeld());
    if (m_position == position)
        return;

    m_position = WTFMove(position);
    m_nicosia.delta.positionChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::syncPosition(const FloatPoint& position)
{
    Locker locker { m_lock };
    if (m_position == position)
        return;

    m_position = position;
    notifyCompositionRequired();
}

const FloatPoint& CoordinatedPlatformLayer::position() const
{
    ASSERT(m_lock.isHeld());
    return m_position;
}

void CoordinatedPlatformLayer::setBoundsOrigin(const FloatPoint& origin)
{
    ASSERT(m_lock.isHeld());
    if (m_boundsOrigin == origin)
        return;

    m_boundsOrigin = origin;
    m_nicosia.delta.boundsOriginChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::syncBoundsOrigin(const FloatPoint& origin)
{
    Locker locker { m_lock };
    if (m_boundsOrigin == origin)
        return;

    m_boundsOrigin = origin;
    notifyCompositionRequired();
}

const FloatPoint& CoordinatedPlatformLayer::boundsOrigin() const
{
    ASSERT(m_lock.isHeld());
    return m_boundsOrigin;
}

void CoordinatedPlatformLayer::setAnchorPoint(FloatPoint3D&& point)
{
    ASSERT(m_lock.isHeld());
    if (m_anchorPoint == point)
        return;

    m_anchorPoint = WTFMove(point);
    m_nicosia.delta.anchorPointChanged = true;
    notifyCompositionRequired();
}

const FloatPoint3D& CoordinatedPlatformLayer::anchorPoint() const
{
    ASSERT(m_lock.isHeld());
    return m_anchorPoint;
}

void CoordinatedPlatformLayer::setSize(FloatSize&& size)
{
    ASSERT(m_lock.isHeld());
    if (m_size == size)
        return;

    m_size = WTFMove(size);
    m_nicosia.delta.sizeChanged = true;
    notifyCompositionRequired();
}

const FloatSize& CoordinatedPlatformLayer::size() const
{
    ASSERT(m_lock.isHeld());
    return m_size;
}

void CoordinatedPlatformLayer::setTransform(const TransformationMatrix& matrix)
{
    ASSERT(m_lock.isHeld());
    if (m_transform == matrix)
        return;

    m_transform = matrix;
    m_nicosia.delta.transformChanged = true;
    notifyCompositionRequired();
}

const TransformationMatrix& CoordinatedPlatformLayer::transform() const
{
    ASSERT(m_lock.isHeld());
    return m_transform;
}

void CoordinatedPlatformLayer::setChildrenTransform(const TransformationMatrix& matrix)
{
    ASSERT(m_lock.isHeld());
    if (m_childrenTransform == matrix)
        return;

    m_childrenTransform = matrix;
    m_nicosia.delta.childrenTransformChanged = true;
    notifyCompositionRequired();
}

const TransformationMatrix& CoordinatedPlatformLayer::childrenTransform() const
{
    ASSERT(m_lock.isHeld());
    return m_childrenTransform;
}

void CoordinatedPlatformLayer::didUpdateLayerTransform()
{
    m_needsTilesUpdate = true;
}

void CoordinatedPlatformLayer::setVisibleRect(const FloatRect& visibleRect)
{
    ASSERT(m_lock.isHeld());
    if (m_visibleRect == visibleRect)
        return;

    m_visibleRect = visibleRect;
}

void CoordinatedPlatformLayer::setTransformedVisibleRect(IntRect&& transformedVisibleRect, IntRect&& transformedVisibleRectIncludingFuture)
{
    ASSERT(m_lock.isHeld());
    if (m_transformedVisibleRect == transformedVisibleRect && m_transformedVisibleRectIncludingFuture == transformedVisibleRectIncludingFuture)
        return;

    m_transformedVisibleRect = WTFMove(transformedVisibleRect);
    m_transformedVisibleRectIncludingFuture = WTFMove(transformedVisibleRectIncludingFuture);
    m_needsTilesUpdate = true;
}

#if ENABLE(SCROLLING_THREAD)
void CoordinatedPlatformLayer::setScrollingNodeID(std::optional<ScrollingNodeID> nodeID)
{
    if (m_scrollingNodeID == nodeID)
        return;

    m_scrollingNodeID = nodeID;
    m_nicosia.delta.scrollingNodeChanged = true;
    notifyCompositionRequired();
}
#endif

void CoordinatedPlatformLayer::setDrawsContent(bool drawsContent)
{
    ASSERT(m_lock.isHeld());
    if (m_drawsContent == drawsContent)
        return;

    m_drawsContent = drawsContent;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setMasksToBounds(bool masksToBounds)
{
    ASSERT(m_lock.isHeld());
    if (m_masksToBounds == masksToBounds)
        return;

    m_masksToBounds = masksToBounds;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setPreserves3D(bool preserves3D)
{
    ASSERT(m_lock.isHeld());
    if (m_preserves3D == preserves3D)
        return;

    m_preserves3D = preserves3D;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackfaceVisibility(bool backfaceVisibility)
{
    ASSERT(m_lock.isHeld());
    if (m_backfaceVisibility == backfaceVisibility)
        return;

    m_backfaceVisibility = backfaceVisibility;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setOpacity(float opacity)
{
    ASSERT(m_lock.isHeld());
    if (m_opacity == opacity)
        return;

    m_opacity = opacity;
    m_nicosia.delta.opacityChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsVisible(bool contentsVisible)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsVisible == contentsVisible)
        return;

    m_contentsVisible = contentsVisible;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

bool CoordinatedPlatformLayer::contentsVisible() const
{
    ASSERT(m_lock.isHeld());
    return m_contentsVisible;
}

void CoordinatedPlatformLayer::setContentsOpaque(bool contentsOpaque)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsOpaque == contentsOpaque)
        return;

    m_contentsOpaque = contentsOpaque;
    m_nicosia.delta.flagsChanged = true;
    // FIXME: request a full repaint.
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsRect(const FloatRect& contentsRect)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsRect == contentsRect)
        return;

    m_contentsRect = contentsRect;
    m_nicosia.delta.contentsRectChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsRectClipsDescendants(bool contentsRectClipsDescendants)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsRectClipsDescendants == contentsRectClipsDescendants)
        return;

    m_contentsRectClipsDescendants = contentsRectClipsDescendants;
    m_nicosia.delta.flagsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsClippingRect(const FloatRoundedRect& contentsClippingRect)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsClippingRect == contentsClippingRect)
        return;

    m_contentsClippingRect = contentsClippingRect;
    m_nicosia.delta.contentsClippingRectChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsScale(float contentsScale)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsScale == contentsScale)
        return;

    m_contentsScale = contentsScale;
    if (m_backingStore) {
        m_backingStore->setContentsScale(m_contentsScale);
        m_nicosia.delta.backingStoreChanged = true;
    }
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsBuffer(TextureMapperPlatformLayerProxy* contentsBuffer)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsBuffer == contentsBuffer)
        return;

    m_contentsBuffer = contentsBuffer;
    if (m_contentsBuffer)
        m_contentsBufferNeedsDisplay = true;
    m_nicosia.delta.contentLayerChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsBufferNeedsDisplay()
{
    ASSERT(m_lock.isHeld());
    if (m_contentsBufferNeedsDisplay)
        return;

    m_contentsBufferNeedsDisplay = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsImage(RefPtr<NativeImage>&& image)
{
    ASSERT(m_lock.isHeld());
    if (image) {
        if (m_imageBackingStore && m_imageBackingStore->isSameNativeImage(*image))
            return;

        ASSERT(m_client);
        m_imageBackingStore = m_client->imageBackingStore(image.releaseNonNull());
    } else
        m_imageBackingStore = nullptr;
    m_nicosia.delta.imageBackingChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsColor(const Color& color)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsColor == color)
        return;

    m_contentsColor = color;
    m_nicosia.delta.solidColorChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsTileSize(const FloatSize& contentsTileSize)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsTileSize == contentsTileSize)
        return;

    m_contentsTileSize = contentsTileSize;
    m_nicosia.delta.contentsTilingChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsTilePhase(const FloatSize& contentsTilePhase)
{
    ASSERT(m_lock.isHeld());
    if (m_contentsTilePhase == contentsTilePhase)
        return;

    m_contentsTilePhase = contentsTilePhase;
    m_nicosia.delta.contentsTilingChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setDirtyRegion(Vector<IntRect, 1>&& dirtyRegion)
{
    ASSERT(m_lock.isHeld());
    if (m_dirtyRegion == dirtyRegion)
        return;

    m_dirtyRegion = WTFMove(dirtyRegion);
    if (m_backingStore)
        m_nicosia.delta.backingStoreChanged = true;
    notifyCompositionRequired();
}

#if ENABLE(DAMAGE_TRACKING)
void CoordinatedPlatformLayer::setDamage(Damage&& damage)
{
    ASSERT(m_lock.isHeld());
    if (m_damage == damage)
        return;

    m_damage = WTFMove(damage);
    m_nicosia.delta.damageChanged = true;
}
#endif

void CoordinatedPlatformLayer::setFilters(const FilterOperations& filters)
{
    ASSERT(m_lock.isHeld());
    if (m_filters == filters)
        return;

    m_filters = filters;
    m_nicosia.delta.filtersChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setMask(CoordinatedPlatformLayer* mask)
{
    ASSERT(m_lock.isHeld());
    if (m_mask == mask)
        return;

    m_mask = mask;
    m_nicosia.delta.maskChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setReplica(CoordinatedPlatformLayer* replica)
{
    ASSERT(m_lock.isHeld());
    if (m_replica == replica)
        return;

    m_replica = replica;
    m_nicosia.delta.replicaChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackdrop(CoordinatedPlatformLayer* backdrop)
{
    ASSERT(m_lock.isHeld());
    if (m_backdrop == backdrop)
        return;

    m_backdrop = backdrop;
    m_nicosia.delta.backdropFiltersChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackdropRect(const FloatRoundedRect& backdropRect)
{
    ASSERT(m_lock.isHeld());
    if (m_backdropRect == backdropRect)
        return;

    m_backdropRect = backdropRect;
    m_nicosia.delta.backdropFiltersRectChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setAnimations(const TextureMapperAnimations& animations)
{
    ASSERT(m_lock.isHeld());
    m_animations = animations;
    m_nicosia.delta.animationsChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setChildren(Vector<Ref<CoordinatedPlatformLayer>>&& children)
{
    ASSERT(m_lock.isHeld());
    if (m_children == children)
        return;

    m_children = WTFMove(children);
    m_nicosia.delta.childrenChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setEventRegion(const EventRegion& eventRegion)
{
    ASSERT(m_lock.isHeld());
    if (m_eventRegion == eventRegion)
        return;

    m_eventRegion = eventRegion;
    m_nicosia.delta.eventRegionChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setDebugBorder(Color&& borderColor, float borderWidth)
{
    ASSERT(m_lock.isHeld());
    if (m_debugBorderColor == borderColor && m_debugBorderWidth == borderWidth)
        return;

    m_debugBorderColor = WTFMove(borderColor);
    m_debugBorderWidth = borderWidth;
    m_nicosia.delta.debugBorderChanged = true;
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setShowRepaintCounter(bool showRepaintCounter)
{
    ASSERT(m_lock.isHeld());
    if ((m_repaintCount != -1 && showRepaintCounter) || (m_repaintCount == -1 && !showRepaintCounter))
        return;

    m_repaintCount = showRepaintCounter ? m_owner->repaintCount() : -1;
    m_nicosia.delta.repaintCounterChanged = true;
    notifyCompositionRequired();
}

bool CoordinatedPlatformLayer::needsBackingStore() const
{
    ASSERT(m_lock.isHeld());
    if (!m_drawsContent || !m_contentsVisible || m_size.isEmpty())
        return false;

    // If the CSS opacity value is 0 and there's no animation over the opacity property, the layer is invisible.
    if (!m_opacity && !m_animations.hasActiveAnimationsOfType(AnimatedProperty::Opacity))
        return false;

    // Check if there's a filter that sets the opacity to zero.
    bool hasOpacityZeroFilter = std::ranges::any_of(m_filters, [](auto& operation) {
        return operation->type() == FilterOperation::Type::Opacity && !downcast<BasicComponentTransferFilterOperation>(operation.get()).amount();
    });

    return !hasOpacityZeroFilter;
}

void CoordinatedPlatformLayer::updateBackingStore()
{
    if (!m_owner)
        return;

    bool scaleChanged = m_backingStore->setContentsScale(m_contentsScale);
    if (!scaleChanged && m_dirtyRegion.isEmpty() && !m_pendingTilesCreation && !m_needsTilesUpdate)
        return;

    IntRect contentsRect(IntPoint::zero(), IntSize(m_size));
    auto updateResult = m_backingStore->updateIfNeeded(m_transformedVisibleRectIncludingFuture, contentsRect, m_pendingTilesCreation || m_needsTilesUpdate, m_dirtyRegion, *this);
    m_needsTilesUpdate = false;
    m_dirtyRegion.clear();
    if (m_animatedBackingStoreClient)
        m_animatedBackingStoreClient->update(m_visibleRect, m_backingStore->coverRect(), m_size, m_contentsScale);

    if (updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::TilesChanged)) {
        if (m_repaintCount != -1 && updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::BuffersChanged)) {
            m_repaintCount = m_owner->incrementRepaintCount();
            m_nicosia.delta.repaintCounterChanged = true;
        }
        notifyCompositionRequired();
    }

    m_pendingTilesCreation = updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::TilesPending);
}

void CoordinatedPlatformLayer::updateContents(bool affectedByTransformAnimation)
{
    ASSERT(m_owner);
    Locker locker { m_lock };

    if (m_contentsBufferNeedsDisplay) {
        if (m_contentsBuffer)
            m_contentsBuffer->swapBuffersIfNeeded();
        m_contentsBufferNeedsDisplay = false;
    }

    if (needsBackingStore()) {
        if (!m_backingStore) {
            m_backingStore = CoordinatedBackingStoreProxy::create(m_contentsScale);
            m_nicosia.delta.backingStoreChanged = true;
            m_needsTilesUpdate = true;
        }
    } else if (m_backingStore) {
        m_backingStore = nullptr;
        m_nicosia.delta.backingStoreChanged = true;
    }

    if (m_backingStore && affectedByTransformAnimation) {
        if (!m_animatedBackingStoreClient) {
            m_animatedBackingStoreClient = CoordinatedAnimatedBackingStoreClient::create(*m_owner);
            m_nicosia.delta.animatedBackingStoreClientChanged = true;
        }
    } else if (m_animatedBackingStoreClient) {
        m_animatedBackingStoreClient->invalidate();
        m_animatedBackingStoreClient = nullptr;
        m_nicosia.delta.animatedBackingStoreClientChanged = true;
    }

    if (m_imageBackingStore) {
        bool wasVisible = m_imageBackingStoreVisible;
        m_imageBackingStoreVisible = m_transformedVisibleRect.intersects(IntRect(m_contentsRect));
        if (wasVisible != m_imageBackingStoreVisible)
            m_nicosia.delta.imageBackingChanged = true;
    }

    bool hadPendingChanges = false;
    if (!!m_nicosia.delta.value) {
        m_nicosia.layer->accessPending([this](Nicosia::CompositionLayer::LayerState& state) {
            auto& localDelta = m_nicosia.delta;
            state.delta.value |= localDelta.value;

            if (localDelta.positionChanged)
                state.position = m_position;
            if (localDelta.anchorPointChanged)
                state.anchorPoint = m_anchorPoint;
            if (localDelta.sizeChanged)
                state.size = m_size;
            if (localDelta.boundsOriginChanged)
                state.boundsOrigin = m_boundsOrigin;
            if (localDelta.transformChanged)
                state.transform = m_transform;
            if (localDelta.childrenTransformChanged)
                state.childrenTransform = m_childrenTransform;
            if (localDelta.contentsRectChanged)
                state.contentsRect = m_contentsRect;
            if (localDelta.contentsClippingRectChanged)
                state.contentsClippingRect = m_contentsClippingRect;
            if (localDelta.contentsTilingChanged) {
                state.contentsTileSize = m_contentsTileSize;
                state.contentsTilePhase = m_contentsTilePhase;
            }
            if (localDelta.flagsChanged) {
                state.flags.drawsContent = m_drawsContent;
                state.flags.masksToBounds = m_masksToBounds;
                state.flags.preserves3D = m_preserves3D;
                state.flags.backfaceVisible = m_backfaceVisibility;
                state.flags.contentsVisible = m_contentsVisible;
                state.flags.contentsOpaque = m_contentsOpaque;
                state.flags.contentsRectClipsDescendants = m_contentsRectClipsDescendants;
            }
            if (localDelta.opacityChanged)
                state.opacity = m_opacity;
            if (localDelta.filtersChanged)
                state.filters = m_filters;
            if (localDelta.maskChanged)
                state.mask = m_mask ? m_mask->compositionLayer() : nullptr;
            if (localDelta.replicaChanged)
                state.replica = m_replica ? m_replica->compositionLayer() : nullptr;
            if (localDelta.backdropFiltersChanged)
                state.backdropLayer = m_backdrop ? m_backdrop->compositionLayer() : nullptr;
            if (localDelta.backdropFiltersRectChanged)
                state.backdropFiltersRect = m_backdropRect;
            if (localDelta.animationsChanged)
                state.animations = m_animations;
            if (localDelta.childrenChanged) {
                state.children = m_children.map<Vector<RefPtr<Nicosia::CompositionLayer>>>([](const auto& child) {
                    return child->compositionLayer();
                });
            }
            if (localDelta.backingStoreChanged)
                state.backingStore = m_backingStore;
            if (localDelta.animatedBackingStoreClientChanged)
                state.animatedBackingStoreClient = m_animatedBackingStoreClient;
            if (localDelta.imageBackingChanged) {
                state.imageBacking.store = m_imageBackingStore;
                state.imageBacking.isVisible = m_imageBackingStoreVisible;
            }
            if (localDelta.solidColorChanged)
                state.solidColor = m_contentsColor;
            if (localDelta.contentLayerChanged)
                state.contentLayer = m_contentsBuffer;
            if (localDelta.eventRegionChanged)
                state.eventRegion = m_eventRegion;
#if ENABLE(SCROLLING_THREAD)
            if (localDelta.scrollingNodeChanged)
                state.scrollingNodeID = m_scrollingNodeID;
#endif
            if (localDelta.debugBorderChanged) {
                state.debugBorder.visible = m_debugBorderColor.isVisible();
                state.debugBorder.color = m_debugBorderColor;
                state.debugBorder.width = m_debugBorderWidth;
            }
            if (localDelta.repaintCounterChanged) {
                state.repaintCounter.visible = m_repaintCount != -1;
                state.repaintCounter.count = m_repaintCount;
            }
#if ENABLE(DAMAGE_TRACKING)
            if (localDelta.damageChanged)
                state.damage = WTFMove(m_damage);
#endif
        });
        hadPendingChanges = true;
        m_nicosia.delta = { };
    }

    if (m_backingStore)
        updateBackingStore();

    if (hadPendingChanges)
        m_nicosia.layer->flushState();

    if (m_backdrop)
        m_backdrop->updateContents(affectedByTransformAnimation);
}

void CoordinatedPlatformLayer::purgeBackingStores()
{
    Locker locker { m_lock };
    m_backingStore = nullptr;
    if (m_animatedBackingStoreClient) {
        m_animatedBackingStoreClient->invalidate();
        m_animatedBackingStoreClient = nullptr;
    }
    m_imageBackingStore = nullptr;
}

Ref<CoordinatedTileBuffer> CoordinatedPlatformLayer::paint(const IntRect& dirtyRect)
{
    ASSERT(m_lock.isHeld());
    ASSERT(m_client);
    ASSERT(m_owner);
#if USE(CAIRO)
    FloatRect scaledDirtyRect(dirtyRect);
    scaledDirtyRect.scale(1 / m_contentsScale);

    auto buffer = CoordinatedUnacceleratedTileBuffer::create(dirtyRect.size(), m_contentsOpaque ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha);
    m_client->paintingEngine().paint(*m_owner, buffer.get(), dirtyRect, enclosingIntRect(scaledDirtyRect), IntRect { { }, dirtyRect.size() }, m_contentsScale);
    return buffer;
#elif USE(SKIA)
    return m_client->paintingEngine().paintLayer(*m_owner, dirtyRect, m_contentsOpaque, m_contentsScale);
#endif
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
