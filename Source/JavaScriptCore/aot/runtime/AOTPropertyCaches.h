/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTFieldTable.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesCustomAccessors.h"
#include "AOTPropertyCachesFront.h"
#include "AOTPropertyCachesHashedTable.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyCachesWatchedValues.h"
#include "AOTSiteCaches.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

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

    // For an ordinary object right below the Object.prototype of the realm. Does nothing for any other.
    void fileAbsenceFilter(JSGlobalObject*, JSObject*);
    // For a store that creates a property of the object: it asks whether anything above the object stops it.
    void fileAbsenceFilterForStores(JSGlobalObject*, JSObject*);
    void didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull);
    uint32_t absenceFilterGenerationForStores() const { return m_absenceFilterGenerationForStores; }
    bool wasChangedOnSomeChain(uint32_t identifierNumber) const { return m_namesChangedOnChains[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    uint32_t absenceFilterGeneration() const { return m_absenceFilterGeneration; }
    // uidOrNull as in didChangeChainObject().
    void didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull);
    bool mayBeNameOfObjectPrototype(uint32_t identifierNumber) const { return m_namesOfObjectPrototypes[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    const AbsenceFilter& absenceFilterFor(StructureID id) const { return m_absenceFilters[AbsenceFilter::indexFor(id.bits())]; }
    // A key that was no atom has turned out to be this name.
    void rememberNameOfText(UniquedStringImpl*);

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) const final;

    void validate(VM&) const;

private:
    using ColumnsByNameAddress = UncheckedKeyHashMap<UniquedStringImpl*, uint16_t, WTF::PtrHash<UniquedStringImpl*>>;

    void publishWordsToEveryInstance() const;
    enum class FilterUse : bool { Reads, Stores };
    void fileAbsenceFilterOf(JSGlobalObject*, JSObject*, FilterUse);
    std::optional<uint32_t> columnOf(UniquedStringImpl*) const;
    std::optional<uint32_t> findColumnByAddress(UniquedStringImpl*) const;
    void ensureColumnByAddress(UniquedStringImpl*, uint32_t column);

    VM& m_vm;
    CustomAccessorTable m_customAccessors;
    RowDisplacementTable m_front;
    HashedSlotTable m_hashedTable;
    ColumnsByNameAddress m_columnsByNameAddress;
    std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> m_absenceFilters { };
    // Never zero, which is the generation of a filter that is not filled.
    uint32_t m_absenceFilterGeneration { 1 };
    uint32_t m_absenceFilterGenerationForStores { 1 };
    std::array<uint64_t, (FieldTableEntry::maxName + 1) / 64> m_namesChangedOnChains { };
    std::array<uint64_t, (FieldTableEntry::maxName + 1) / 64> m_namesOfObjectPrototypes { };
    std::array<uint8_t, MissesOfPairs::numberOfCounters> m_missesOfPairs { };
    std::array<NameOfText, NameOfText::numberOfEntries> m_namesOfTexts { };
    bool m_hasInstance { false };
    bool m_isSharedByInstances { false };
};

struct CachedAccess {
    Structure* structure;
    CachedName name;
    SlotAccess access;
    SiteCache site { nullptr };
};

// What inline code and the stubs read whether or not the instance has its PropertyCaches yet.
void initializePropertyCacheWords(Instance&);

// State::propertyCaches creates them at their first use.
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
    // Counts a miss of the pair. Says whether it has missed often enough for its answer to be classified and filed.
    bool shouldFile(StructureID, UniquedStringImpl*, SlotAccess);
    bool countsNames() const { return !!m_countedFilings; }
    void countNamed(ASCIILiteral what, UniquedStringImpl* name);
    // An access by name that left compiled code: what its receiver was, and where it stands (the offset in the image's code of the address that the nearest compiled function is returned to).
    void countMiss(ASCIILiteral access, bool shouldFile, JSValue receiver, UniquedStringImpl* name);
    void dumpNamedCounts(PrintStream&) const;
    // For an addition that is not filed because it is the first of its kind, too: if its Structure ended at the next collection, the second would look like the first for ever.
    void didAddProperty(Structure* before, Structure* after);
    // An access by key probes the hashed table alone, so a slot that only the front has is copied there first.
    const Slot* findByKey(Structure*, UniquedStringImpl*, SlotAccess);
    void fillSiteCacheFromTables(SiteCache, JSCell* base, uint32_t identifierNumber, SlotAccess);
    void cacheFreeName(SiteCache, UniquedStringImpl*, SlotAccess, const PropertyAnswer&);

    template<typename Visitor> void visitRoots(Visitor&);
    void didFinishCollection(VM&, CollectionScope);

private:
    struct Addition {
        StructureID before;
        StructureID after;
    };

    static constexpr uint32_t numberOfMissCounters = 1 << 15;
    static constexpr uint32_t bitsOfMissCount = 2;
    // A miss that is answered and not filed costs about a fifth of one that is classified and filed, and a third of the pairs never come a third time.
    static constexpr uint32_t missesBeforeFiling = 2;
    // A store that is answered and not filed costs half of one that is filed, and of the pairs that come a second time four in five come a third time.
    static constexpr uint32_t storeMissesBeforeFiling = 1;

    void publishIfStandsForPrimitives(Structure*);
    void ensureChangesAreReported(std::span<JSObject* const>);
    bool canCache(Structure*, const PropertyAnswer&) const;
    void registerChainDependents(const CachedAccess&, const PropertyAnswer&);

    State& m_state;
    VM& m_vm;
    VMPropertyCaches& m_tables;
    ChainDependentOwnerID m_tablesOwnerID;
    SiteCaches& m_siteCaches;
    WatchedValues m_watchedValues;
    StringKeyCache m_stringKeys;
    Vector<Addition> m_additionsSinceCollection;
    void countFiling(const CachedAccess&);

    std::array<uint8_t, numberOfMissCounters> m_missCounters { };
    std::atomic<bool> m_shouldForgetMisses { false };
    std::unique_ptr<UncheckedKeyHashSet<uint64_t>> m_countedFilings;
    UncheckedKeyHashMap<String, uint64_t> m_namedCounts;
};

static_assert(RowDisplacementTable::maxColumn <= UINT16_MAX);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
