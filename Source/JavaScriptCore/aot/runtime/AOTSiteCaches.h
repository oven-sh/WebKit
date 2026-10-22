/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTRuntimeRecords.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class CustomAccessorTable;
struct Instance;
struct State;

class SiteCaches final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(SiteCaches);
public:
    static constexpr unsigned regionShift = 8;

    explicit SiteCaches(State&);

    uint32_t size() const { return static_cast<uint32_t>(m_slots.size()); }
    uint32_t indexOf(SiteCache site) const { return static_cast<uint32_t>(site - m_slots.data()); }
    SiteCache at(uint32_t index) const { return &m_slots[index]; }
    SiteCacheState& stateOf(SiteCache site) const { return *reinterpret_cast<SiteCacheState*>(reinterpret_cast<uint8_t*>(m_states) + ((std::bit_cast<uintptr_t>(site) >> SiteCacheState::slotAddressShift) & SiteCacheState::offsetMask)); }
    // Sites this far apart share a bit. A site that finds the bit of another one set is filled one sample early, and nothing else follows from the bit.
    static constexpr uint32_t maxNumberOfMissBits = 1u << 17;
    static constexpr size_t numberOfMissWords(size_t numberOfSites) { return (std::min<size_t>(numberOfSites, maxNumberOfMissBits) + 63) / 64; }
    uint32_t missBitOf(SiteCache site) const { return indexOf(site) % maxNumberOfMissBits; }
    bool hasMissed(SiteCache site) const { return m_sitesThatMissed[missBitOf(site) / 64] >> (missBitOf(site) % 64) & 1; }
    std::span<const uint64_t> sitesThatMissed() const { return { m_sitesThatMissed, numberOfMissWords(m_slots.size()) }; }
    std::span<const uint8_t> cards() { return { m_writtenRegions.cards(), RememberedSlotSet::numberOfCardsFor(m_slots.size_bytes(), regionShift) }; }

    struct NamedFill {
        UniquedStringImpl* uid;
        SlotAccess access;
        std::span<JSObject* const> changeableObjects;
    };
    struct ChainDependence {
        UniquedStringImpl* uid;
        std::span<JSObject* const> changeableObjects;
    };

    bool recordMiss(SiteCache);
    bool didMissBefore(SiteCache);
    void didMissRepeatedly(SiteCache);
    void didFailToFill(SiteCache);
    void fill(SiteCache, const Slot&);
    void fill(SiteCache, const Slot&, const NamedFill&);
    void fill(SiteCache, const Slot&, std::span<const ChainDependence>);
    void clear(SiteCache);

    void dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) const final;

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    template<typename Functor>
    void forEachValidatedSite(const Functor& functor) const
    {
        for (const auto& [index, site] : m_validatedSites)
            functor(m_slots[index].get(), site.uid, site.access);
    }

private:
    struct ValidatedSite {
        UniquedStringImpl* uid;
        SlotAccess access;
    };

    bool shouldFill(SiteCache);
    void write(SiteCache, const Slot&);

    State& m_state;
    const CustomAccessorTable& m_customAccessors;
    std::span<RememberedSlot<Slot>> m_slots;
    SiteCacheState* m_states { nullptr };
    uint64_t* m_sitesThatMissed { nullptr };
    ChainDependentOwnerID m_ownerID;
    RememberedSlotSet m_writtenRegions;
    bool m_wasWrittenSinceLastSweep { false };
    // Named by a site, and what they hang on was not marked when the collection of that version last looked.
    UncheckedKeyHashSet<StructureID> m_structuresThatWait;
    uint32_t m_versionOfStructuresThatWait { 0 };
    UncheckedKeyHashMap<uint32_t, ValidatedSite, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_validatedSites;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
