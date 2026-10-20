/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesCustomAccessors.h"
#include "AOTPropertyCachesFront.h"
#include "AOTPropertyCachesHashedTable.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyCachesWatchedValues.h"
#include "AOTSiteCaches.h"
#include <optional>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class Structure;
class VM;

namespace AOT {

struct Instance;
struct State;

struct CachedName {
    UniquedStringImpl* uid;
    std::optional<uint32_t> identifierNumber;
};

class VMPropertyCaches final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(VMPropertyCaches);
    WTF_MAKE_TZONE_ALLOCATED(VMPropertyCaches);
public:
    explicit VMPropertyCaches(VM&);
    ~VMPropertyCaches();

    CustomAccessorTable& customAccessors() { return m_customAccessors; }
    RowDisplacementTable& front() { return m_front; }
    HashedSlotTable& hashedTable() { return m_hashedTable; }

    bool isSharedByInstances() const { return m_isSharedByInstances; }
    void didCreateInstance(Instance&);
    void publishWords(Instance&) const;

    void set(Structure*, const CachedName&, SlotAccess, const Slot&);
    std::optional<Slot> find(StructureID, const CachedName&, SlotAccess) const;
    void remove(StructureID, const CachedName&, SlotAccess);

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) const final;

    void validate(VM&) const;

private:
    using ColumnsByNameAddress = UncheckedKeyHashMap<UniquedStringImpl*, uint16_t, WTF::PtrHash<UniquedStringImpl*>>;

    void publishWordsToEveryInstance() const;
    std::optional<uint32_t> columnOf(UniquedStringImpl*) const;
    std::optional<uint32_t> findColumnByAddress(UniquedStringImpl*) const;
    void ensureColumnByAddress(UniquedStringImpl*, uint32_t column);

    VM& m_vm;
    CustomAccessorTable m_customAccessors;
    RowDisplacementTable m_front;
    HashedSlotTable m_hashedTable;
    ColumnsByNameAddress m_columnsByNameAddress;
    bool m_hasInstance { false };
    bool m_isSharedByInstances { false };
};

struct CachedAccess {
    Structure* structure;
    CachedName name;
    SlotAccess access;
    SiteCache site { nullptr };
};

class PropertyCaches {
    WTF_MAKE_NONCOPYABLE(PropertyCaches);
    WTF_MAKE_TZONE_ALLOCATED(PropertyCaches);
public:
    explicit PropertyCaches(State&);
    ~PropertyCaches();

    SiteCaches& siteCaches() { return m_siteCaches; }
    WatchedValues& watchedValues() { return m_watchedValues; }
    StringKeyCache& stringKeys() { return m_stringKeys; }
    VMPropertyCaches& tables() { return m_tables; }

    CachedName nameOf(uint32_t identifierNumber) const;
    CachedName nameOf(UniquedStringImpl*) const;

    void cache(const CachedAccess&, const PropertyAnswer&);
    // An access by key probes the hashed table alone, so a slot that only the front has is copied there first.
    const Slot* findByKey(Structure*, UniquedStringImpl*, SlotAccess);
    void fillSiteCacheFromTables(SiteCache, JSCell* base, uint32_t identifierNumber, SlotAccess);
    void cacheFreeName(SiteCache, UniquedStringImpl*, SlotAccess, const PropertyAnswer&);

    void didFinishCollection(VM&, CollectionScope);

private:
    void fillHandlerTables();
    bool canCache(Structure*, const PropertyAnswer&) const;
    void registerChainDependents(const CachedAccess&, const PropertyAnswer&);

    State& m_state;
    VM& m_vm;
    VMPropertyCaches& m_tables;
    ChainDependentOwnerID m_tablesOwnerID;
    SiteCaches m_siteCaches;
    WatchedValues m_watchedValues;
    StringKeyCache m_stringKeys;
};

static_assert(RowDisplacementTable::maxColumn <= UINT16_MAX);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
