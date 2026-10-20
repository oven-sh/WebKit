/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteCaches.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include <bit>
#include <type_traits>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(!((1u << SiteCaches::regionShift) % sizeof(Slot)));
static_assert(std::is_same_v<decltype(SiteCacheState::missesLeft), uint8_t>, "recordMiss and the stub's countSiteCacheMiss both wrap from 0 to 255");

static void stopWatchingReplacedConstant(State& state, SiteCache site)
{
    if (site->get().kind == Slot::Kind::Constant)
        state.propertyCaches->watchedValues().stopWatchingConstant(site);
}

SiteCaches::SiteCaches(State& state)
    : m_state(state)
    , m_customAccessors(state.instance.vm->aotState().propertyCaches->customAccessors())
    , m_slots(state.instance.siteCaches())
    , m_states(state.tableArena.allocate<SiteCacheState>(m_slots.size()))
    , m_ownerID(state.chainDependents->addOwner(*this))
    , m_writtenRegions(asByteSpan(m_slots), regionShift)
{
    state.instance.propertyCaches.siteCacheStateBias = std::bit_cast<uintptr_t>(m_states) - (std::bit_cast<uintptr_t>(m_slots.data()) >> SiteCacheState::slotAddressShift);
    state.caches->add(CacheKind::WeakByKey, "SiteCaches"_s, *this);
}

bool SiteCaches::recordMiss(SiteCache site)
{
    return !m_states[indexOf(site)].missesLeft--;
}

// The stubs count the misses of a site that the lookup tables answer, so its first fill does not wait for another one.
void SiteCaches::didMissRepeatedly(SiteCache site)
{
    m_states[indexOf(site)].hasMissed = true;
}

// The due miss left the counter at 255.
void SiteCaches::didFailToFill(SiteCache site)
{
    m_states[indexOf(site)].missesLeft = SiteCacheState::missesBeforeRefill(0);
}

// A site that runs once would write a cache that nothing reads, on a page that holds 256 of them. The states are eight times as dense.
// Only a site without lookup tables behind it comes here before the stubs have counted its misses.
bool SiteCaches::shouldFill(SiteCache site)
{
    SiteCacheState& state = m_states[indexOf(site)];
    if (state.hasMissed)
        return true;
    state.hasMissed = true;
    state.missesLeft = 0;
    return false;
}

void SiteCaches::write(SiteCache site, const Slot& slot)
{
    stopWatchingReplacedConstant(m_state, site);
    if (!site->get().isEmpty())
        count(m_state.instance, RuntimeCounterID::SiteCacheRefills);
    site->set(m_writtenRegions, slot);

    SiteCacheState& state = m_states[indexOf(site)];
    state.missesLeft = SiteCacheState::missesBeforeRefill(state.refillCount);
    if (state.refillCount < SiteCacheState::maxRefillCount)
        ++state.refillCount;
}

void SiteCaches::fill(SiteCache site, const Slot& slot)
{
    RELEASE_ASSERT(!slot.has(Slot::dependsOnChain));
    if (shouldFill(site))
        write(site, slot);
}

void SiteCaches::fill(SiteCache site, const Slot& slot, const NamedFill& named)
{
    if (!shouldFill(site))
        return;
    write(site, slot);
    if (slot.has(Slot::dependsOnChain))
        m_state.chainDependents->add(named.changeableObjects, { named.uid, indexOf(site), m_ownerID });
    if (validatesRuntime(*m_state.instance.vm)) [[unlikely]]
        m_validatedSites.set(indexOf(site), ValidatedSite { named.uid, named.access });
}

void SiteCaches::fill(SiteCache site, const Slot& slot, std::span<const ChainDependence> dependences)
{
    if (!shouldFill(site))
        return;
    write(site, slot);
    for (const ChainDependence& dependence : dependences) {
        RELEASE_ASSERT(slot.has(Slot::dependsOnChain) || dependence.changeableObjects.empty());
        m_state.chainDependents->add(dependence.changeableObjects, { dependence.uid, indexOf(site), m_ownerID });
    }
}

void SiteCaches::clear(SiteCache site)
{
    stopWatchingReplacedConstant(m_state, site);
    site->clear();
}

void SiteCaches::dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl*)
{
    if (m_slots[siteIndex].get().has(Slot::dependsOnChain))
        clear(at(siteIndex));
}

bool SiteCaches::hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl*) const
{
    return m_slots[siteIndex].get().has(Slot::dependsOnChain);
}

static void resetRefillBackoff(std::span<SiteCacheState> states)
{
    for (SiteCacheState& state : states) {
        state.refillCount = 0;
        state.missesLeft = 0;
    }
}

size_t SiteCaches::removeDeadEntries(VM& vm, CollectionScope scope)
{
    auto isMarked = [&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    };
    uintptr_t cageBase = vm.heap.cage().base();
    auto isDead = [&](const Slot& slot) {
        if (!slot.namesOnlyLiveCells(isMarked, cageBase))
            return true;
        return Slot::payloadOf(slot.kind) == SlotPayload::CustomAccessor && !m_customAccessors.isInUse(slot.customAccessor.index);
    };
    // With validation on, the registry sweeps every region again after an Eden collection, and a check must not change when a site refills.
    bool resetsRefillBackoff = scope == CollectionScope::Full && vm.heap.collectionScope() != CollectionScope::Eden;
    size_t numberOfClearedSites = 0;
    m_writtenRegions.forEachRegionToSweep(scope, [&](size_t beginOffset, size_t endOffset) {
        if (resetsRefillBackoff)
            resetRefillBackoff({ m_states + beginOffset / sizeof(Slot), (endOffset - beginOffset) / sizeof(Slot) });
        RememberedSlotSet::forEachEntryInRegion(m_slots, beginOffset, endOffset, [&](RememberedSlot<Slot>& site) {
            if (site.get().isEmpty() || !isDead(site.get()))
                return;
            // Not SiteCaches::clear(): a sweep unlinks no watchpoint. WatchedValues::removeDeadEntries drops the watch of a Constant slot.
            site.clear();
            ++numberOfClearedSites;
        });
    });
    return numberOfClearedSites;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
