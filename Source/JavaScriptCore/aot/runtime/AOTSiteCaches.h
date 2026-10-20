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
    const SiteCacheState& stateOf(SiteCache site) const { return m_states[indexOf(site)]; }

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
    void didMissRepeatedly(SiteCache);
    void didFailToFill(SiteCache);
    void fill(SiteCache, const Slot&);
    void fill(SiteCache, const Slot&, const NamedFill&);
    void fill(SiteCache, const Slot&, std::span<const ChainDependence>);
    void clear(SiteCache);

    void dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) const final;

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
    ChainDependentOwnerID m_ownerID;
    RememberedSlotSet m_writtenRegions;
    UncheckedKeyHashMap<uint32_t, ValidatedSite, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_validatedSites;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
