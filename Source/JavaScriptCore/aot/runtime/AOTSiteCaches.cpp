/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteCaches.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTEmptyPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include <bit>
#include <cstring>
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

// A page of cards is first written when a site that it stands for is filled.
static std::span<uint8_t> allocateCards(State& state, size_t sizeOfSlots)
{
    size_t numberOfCards = RememberedSlotSet::numberOfCardsFor(sizeOfSlots, SiteCaches::regionShift);
    return { state.tableArena.allocate<uint8_t>(numberOfCards), numberOfCards };
}

SiteCaches::SiteCaches(State& state)
    : m_state(state)
    , m_customAccessors(state.instance.vm->aotState().propertyCaches->customAccessors())
    , m_slots(state.instance.siteCaches())
    , m_states(state.tableArena.allocate<SiteCacheState>(SiteCacheState::numberOfStates))
    , m_sitesThatMissed(state.tableArena.allocate<uint64_t>(numberOfMissWords(m_slots.size())))
    , m_ownerID(state.chainDependents->addOwner(*this))
    , m_writtenRegions(asByteSpan(m_slots), regionShift, allocateCards(state, m_slots.size_bytes()))
{
    state.instance.propertyCaches.siteCacheStates = m_states;
    state.caches->add(CacheKind::WeakByKey, "SiteCaches"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "Structures that sites name"_s, *this);
}

bool SiteCaches::recordMiss(SiteCache site)
{
    return !stateOf(site).missesLeft--;
}

// A site that runs once would write a cache that nothing reads, on a page that holds 256 of them. A page of these bits stands for 32,768 sites.
bool SiteCaches::didMissBefore(SiteCache site)
{
    uint64_t& word = m_sitesThatMissed[missBitOf(site) / 64];
    uint64_t bit = 1ull << (missBitOf(site) % 64);
    if (word & bit)
        return true;
    word |= bit;
    return false;
}

void SiteCaches::didMissRepeatedly(SiteCache site)
{
    didMissBefore(site);
}

static void waitLongerForNextFill(SiteCacheState& state)
{
    state.missesLeft = SiteCacheState::missesBeforeRefill(state.refillCount);
    if (state.refillCount < SiteCacheState::maxRefillCount)
        ++state.refillCount;
}

// A fill that fails costs a lookup and a walk of the chain, as one that succeeds does.
void SiteCaches::didFailToFill(SiteCache site)
{
    SiteCacheState& state = stateOf(site);
    if (site->get().isEmpty())
        state.missesLeft = SiteCacheState::missesBeforeRefill(0);
    else
        waitLongerForNextFill(state);
}

// Only a site without lookup tables behind it comes here before it was sampled. Its next miss asks again.
bool SiteCaches::shouldFill(SiteCache site)
{
    if (didMissBefore(site))
        return true;
    stateOf(site).missesLeft = 0;
    return false;
}

void SiteCaches::write(SiteCache site, const Slot& slot)
{
    stopWatchingReplacedConstant(m_state, site);
    if (!site->get().isEmpty())
        count(m_state.instance, RuntimeCounterID::SiteCacheRefills);
    site->set(m_writtenRegions, slot);
    m_wasWrittenSinceLastSweep = true;
    waitLongerForNextFill(stateOf(site));
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

static void forEachStructureNamedBy(const Slot& slot, const Invocable<void(StructureID)> auto& functor)
{
    if (slot.structureID)
        functor(slot.structureID);
    switch (Slot::payloadOf(slot.kind)) {
    case SlotPayload::ExtraProperty:
        if (slot.extraProperty.structureID)
            functor(slot.extraProperty.structureID);
        return;
    case SlotPayload::ExtraPropertyAddition:
        functor(slot.extraPropertyAddition.newStructureID);
        return;
    case SlotPayload::Transition:
        functor(slot.transition.newStructureID);
        return;
    case SlotPayload::None:
    case SlotPayload::WordIndex:
    case SlotPayload::Field:
    case SlotPayload::CustomAccessor:
    case SlotPayload::Holder:
    case SlotPayload::FieldAndHolder:
    case SlotPayload::Constant:
        return;
    }
}

// The Structure keeps nothing alive that would be gone without it.
template<typename Visitor>
static bool hangsOnMarkedCellsOnly(Visitor& visitor, Structure* structure)
{
    if (auto* compiled = dynamicDowncast<CompiledStructure>(structure); compiled && compiled->heldExtraProperties())
        return false;
    JSObject* prototype = structure->hasMonoProto() ? structure->storedPrototype().getObject() : nullptr;
    JSGlobalObject* realm = structure->globalObject();
    return (!prototype || visitor.isMarked(prototype)) && (!realm || visitor.isMarked(realm));
}

// The objects of a shape come and go, and a full collection may find none: a server between two requests. Without this the Structure would end, and with it whatever the program has learned about the shape.
// A site has one entry, so the image bounds what the sites name. An Eden collection leaves old Structures alone, and StructureTable keeps the young ones.
template<typename Visitor>
void SiteCaches::visitRoots(Visitor& visitor)
{
    if (visitor.collectionScope() == CollectionScope::Eden)
        return;

    // The GC verifier looks at every site each time.
    constexpr bool looksOnceAtEachSite = std::is_same_v<Visitor, SlotVisitor>;
    if constexpr (looksOnceAtEachSite) {
        if (m_versionOfStructuresThatWait != visitor.markingVersion()) {
            m_versionOfStructuresThatWait = visitor.markingVersion();
            m_structuresThatWait.clear();
        }
    }
    auto isSettled = [&](StructureID id) {
        Structure* structure = id.decode();
        if (visitor.isMarked(structure))
            return true;
        if (!hangsOnMarkedCellsOnly(visitor, structure))
            return false;
        visitor.appendUnbarriered(structure);
        return true;
    };
    m_structuresThatWait.removeIf(isSettled);
    m_writtenRegions.forEachRegionToVisit(visitor, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(m_slots, beginOffset, endOffset, [&](RememberedSlot<Slot>& site) {
            if (site.get().isEmpty())
                return;
            forEachStructureNamedBy(site.get(), [&](StructureID id) {
                if (!isSettled(id) && looksOnceAtEachSite)
                    m_structuresThatWait.add(id);
            });
        });
    });
}

template void SiteCaches::visitRoots(AbstractSlotVisitor&);
template void SiteCaches::visitRoots(SlotVisitor&);

size_t SiteCaches::removeDeadEntries(VM& vm, CollectionScope scope)
{
    // An Eden collection kills young cells only, and a site that names one was written since the last sweep.
    if (scope == CollectionScope::Eden && !m_wasWrittenSinceLastSweep)
        return 0;
    m_wasWrittenSinceLastSweep = false;

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
    bool isFullCollection = scope == CollectionScope::Full && vm.heap.collectionScope() != CollectionScope::Eden;
    size_t numberOfClearedSites = 0;
    m_writtenRegions.forEachRegionToSweep(scope, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(m_slots, beginOffset, endOffset, [&](RememberedSlot<Slot>& site) {
            if (site.get().isEmpty() || !isDead(site.get()))
                return;
            // Not SiteCaches::clear(): a sweep unlinks no watchpoint. WatchedValues::removeDeadEntries drops the watch of a Constant slot.
            site.clear();
            ++numberOfClearedSites;
        });
    });
    if (isFullCollection)
        decommitEmptyPages(asWritableBytes(m_slots), m_writtenRegions);
    return numberOfClearedSites;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
