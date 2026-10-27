/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTFieldTable.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesFront.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeRecords.h"
#include "AOTWatchpoints.h"
#include "CollectionScope.h"
#include "Identifier.h"
#include "RememberedSlot.h"
#include <atomic>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSObject;
class Symbol;

namespace AOT {

class CustomAccessorTable;
class Image;
struct Instance;
struct State;

class SiteCaches final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(SiteCaches);
public:
    static constexpr unsigned regionShift = 8;
    static constexpr uint32_t maxNumberOfMissBits = 1u << 17;
    static constexpr size_t numberOfMissWords(size_t numberOfSites) { return (std::min<size_t>(numberOfSites, maxNumberOfMissBits) + 63) / 64; }

    explicit SiteCaches(State&);

    uint32_t size() const { return static_cast<uint32_t>(m_slots.size()); }
    uint32_t indexOf(SiteCache site) const { return static_cast<uint32_t>(site - m_slots.data()); }
    SiteCache at(uint32_t index) const { return &m_slots[index]; }
    SiteCacheState& stateOf(SiteCache site) const { return *reinterpret_cast<SiteCacheState*>(reinterpret_cast<uint8_t*>(m_states) + ((std::bit_cast<uintptr_t>(site) >> SiteCacheState::slotAddressShift) & SiteCacheState::offsetMask)); }

    struct NamedFill {
        UniquedStringImpl* uid;
        SlotAccess access;
        std::span<JSObject* const> changeableObjects;
    };
    struct ChainDependence {
        UniquedStringImpl* uid;
        std::span<JSObject* const> changeableObjects;
    };

    bool didMissBefore(SiteCache);
    void didMissRepeatedly(SiteCache site) { didMissBefore(site); }
    void didFailToFill(SiteCache);
    void didFindNothingToFill(SiteCache);
    void fill(SiteCache, const Slot&);
    void fill(SiteCache, const Slot&, const NamedFill&);
    void fill(SiteCache, const Slot&, std::span<const ChainDependence>);
    void clear(SiteCache);

    void dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) const final;

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    void forEachValidatedSite(const auto& functor) const
    {
        for (const auto& [index, site] : m_validatedSites)
            functor(m_slots[index].get(), site.first, site.second);
    }

private:
    bool tryWrite(SiteCache, const Slot&);

    State& m_state;
    const CustomAccessorTable& m_customAccessors;
    std::span<RememberedSlot<Slot>> m_slots;
    SiteCacheState* m_states { nullptr };
    uint64_t* m_sitesThatMissed { nullptr };
    ChainDependentOwnerID m_ownerID;
    RememberedSlotSet m_writtenRegions;
    bool m_wasWrittenSinceLastSweep { false };
    UncheckedKeyHashSet<StructureID> m_structuresThatWait;
    uint32_t m_versionOfStructuresThatWait { 0 };
    UncheckedKeyHashMap<uint32_t, std::pair<UniquedStringImpl*, SlotAccess>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_validatedSites;
};

struct CachedName {
    UniquedStringImpl* uid;
    std::optional<uint32_t> identifierNumber;
};

class WatchedValues {
    WTF_MAKE_NONCOPYABLE(WatchedValues);
public:
    explicit WatchedValues(State&);

    bool tryWatchConstant(VM&, JSObject* holder, UniquedStringImpl*, JSValue expected, SiteCache);
    void stopWatchingConstant(SiteCache);
    void didCacheStore(UniquedStringImpl* uid) { m_namesWithCachedStore.add(uid); }

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    State& m_state;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<PropertyValueWatchpoint>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantsBySite;
    IdentifierSet m_namesWithCachedStore;
    RememberedSlotSet m_writtenSinceCollection;
};

class CustomAccessorTable {
    WTF_MAKE_NONCOPYABLE(CustomAccessorTable);
public:
    CustomAccessorTable(VM&, CacheRegistry&);
    ~CustomAccessorTable();

    const CustomAccessorRecord* records() const { return m_records; }
    const CustomAccessorRecord& at(uint32_t index) const
    {
        ASSERT(isInUse(index));
        return m_records[index];
    }
    bool isInUse(uint32_t index) const { return index < m_numberOfRecordsEverUsed && m_records[index].function; }

    std::optional<uint32_t> find(const CustomAccessorRecord&) const;
    std::optional<uint32_t> add(const CustomAccessorRecord&);
    size_t removeDeadEntries(VM&, CollectionScope);
    bool namesOnlyWhatIsLive(VM&, const Slot&) const;

private:
    using Key = std::tuple<void*, JSObject*, JSGlobalObject*, uint32_t>;
    static Key keyOf(const CustomAccessorRecord& record) { return { record.function, record.slotBase, record.globalObject, record.offsetOfCachedValue }; }

    VM& m_vm;
    CustomAccessorRecord* m_records { nullptr };
    uint32_t m_capacity { 0 };
    uint32_t m_numberOfRecordsEverUsed { 0 };
    Vector<uint32_t> m_freeIndexes;
    UncheckedKeyHashMap<Key, uint32_t> m_indexByKey;
    RememberedSlotSet m_writtenSinceCollection;
};

class HashedSlotTable {
    WTF_MAKE_NONCOPYABLE(HashedSlotTable);
    WTF_MAKE_TZONE_ALLOCATED(HashedSlotTable);
public:
    static constexpr uint32_t initialNumberOfEntries = 1u << 10;
    static constexpr unsigned regionShift = 7;

    HashedSlotTable(VM&, CacheRegistry&, const CustomAccessorTable&);
    ~HashedSlotTable();

    const HashedSlot* entries() const { return &m_entries->get(); }
    uint32_t numberOfEntries() const { return m_numberOfEntries; }
    uint32_t numberOfFilledEntries() const { return m_numberOfFilledEntries; }
    uint64_t offsetMask() const { return HashedSlot::offsetMaskFor(m_numberOfEntries); }

    enum class SetResult : uint8_t { Stored, StoredAndGrew };
    SetResult set(UniquedStringImpl*, SlotAccess, const Slot&);
    const Slot* find(StructureID, UniquedStringImpl*, SlotAccess) const;
    void remove(StructureID, UniquedStringImpl*, SlotAccess);

    bool hasChainDependentEntry(StructureID, UniquedStringImpl*) const;
    size_t removeChainDependentEntries(StructureID, UniquedStringImpl*);
    void clear();

    size_t removeDeadEntries(VM&, CollectionScope);

    template<typename Functor>
    void forEachEntry(const Functor& functor) const
    {
        for (const Entry& entry : allEntries()) {
            if (!entry.get().isEmpty())
                functor(entry.get());
        }
    }

private:
    using Entry = RememberedSlot<HashedSlot>;

    std::span<Entry> allEntries() const { return { m_entries, m_numberOfEntries }; }
    Entry* findEntry(StructureID, UniquedStringImpl*, uintptr_t key) const;
    Entry* findChainDependentEntry(StructureID, UniquedStringImpl*, SlotAccess) const;
    void erase(Entry&);
    void releaseNamesErasedDuringCollection();
    void grow();

    VM& m_vm;
    const CustomAccessorTable& m_customAccessors;
    uint32_t m_maxNumberOfEntries { 0 };
    uint32_t m_numberOfEntries { 0 };
    Entry* m_entries { nullptr };
    uint32_t m_numberOfFilledEntries { 0 };
    uint32_t m_evictionsWhileHalfFull { 0 };
    uint32_t m_evictionsSinceCollection { 0 };
    uint32_t m_nextVictimWay { 0 };
    Vector<UniquedStringImpl*> m_namesErasedDuringCollection;
    RememberedSlotSet m_writtenRegions;
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

    // No VM that runs the image reserves more for its tables when it is created.
    static size_t maxSizeOfPagesAtStart(const Image&);

    bool isSharedByInstances() const { return m_isSharedByInstances; }
    void didCreateInstance(Instance&);
    void publishWords(Instance&) const;

    void set(Structure*, const CachedName&, SlotAccess, const Slot&);
    std::optional<Slot> find(StructureID, const CachedName&, SlotAccess) const;
    void remove(StructureID, const CachedName&, SlotAccess);

    void fileAbsenceFilterForStores(JSGlobalObject*, JSObject*);
    void didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull);
    void didStartWatchingReplacements();
    uint32_t absenceFilterGenerationForStores() const { return m_absenceFilterGenerationForStores; }
    bool wasChangedOnSomeChain(uint32_t identifierNumber) const { return m_namesChangedOnChains[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    void didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull);
    bool mayBeNameOfObjectPrototype(uint32_t identifierNumber) const { return m_namesOfObjectPrototypes[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    const AbsenceFilter& absenceFilterFor(StructureID id) const { return m_absenceFilters[AbsenceFilter::indexFor(id.bits())]; }
    void rememberNameOfText(UniquedStringImpl*);

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) const final;

    size_t removeDeadEntries(VM&, CollectionScope);
    void validate(VM&) const;

    // An object of some chain that is changed first and reports behind that. A collection may end in between. No compiled code runs there, so validation leaves alone what the report is about to drop.
    class UnreportedChange {
        WTF_MAKE_NONCOPYABLE(UnreportedChange);
    public:
        UnreportedChange(VMPropertyCaches& caches, JSObject* changing)
            : m_caches(caches)
        {
            m_caches.m_objectsWithUnreportedChange.append(changing);
        }

        ~UnreportedChange() { m_caches.m_objectsWithUnreportedChange.removeLast(); }

    private:
        VMPropertyCaches& m_caches;
    };
    bool isBelowUnreportedChange(Structure*) const;

private:
    using BitPerName = std::array<uint64_t, (FieldTableEntry::maxName + 1) / 64>;

    VM& m_vm;
    CustomAccessorTable m_customAccessors;
    RowDisplacementTable m_front;
    HashedSlotTable m_hashedTable;
    UncheckedKeyHashMap<UniquedStringImpl*, uint16_t, WTF::PtrHash<UniquedStringImpl*>> m_columnsByNameAddress;
    std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> m_absenceFilters { };
    uint32_t m_absenceFilterGenerationForStores { 1 };
    BitPerName m_namesChangedOnChains { };
    BitPerName m_namesOfObjectPrototypes { };
    std::array<uint8_t, MissesOfPairs::numberOfCounters> m_missesOfPairs { };
    std::array<NameOfText, NameOfText::numberOfEntries> m_namesOfTexts { };
    Vector<JSObject*, 2> m_objectsWithUnreportedChange;
    bool m_hasInstance { false };
    bool m_isSharedByInstances { false };
};

struct CachedAccess {
    Structure* structure;
    CachedName name;
    SlotAccess access;
    SiteCache site { nullptr };
};

void initializePropertyCacheWords(Instance&);

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
    bool shouldFile(StructureID, UniquedStringImpl*, SlotAccess);
    // The look that followed the last yes of shouldFile() changed the Structure of the receiver, so nothing is filed under the Structure that it came with. It changes the next object of that Structure too: the misses of the pair count anew.
    void didFileNothing();
    bool countsNames() const { return !!m_countedFilings; }
    void countNamed(ASCIILiteral what, UniquedStringImpl* name);
    void countMiss(ASCIILiteral access, bool shouldFile, JSValue receiver, UniquedStringImpl* name);
    void dumpNamedCounts(PrintStream&) const;
    // A store by key is about to give an object of the Structure a name that no object of it has been given. A Structure is made for what comes again: the first such object becomes a dictionary,
    // and the next one that is given the same name there, and whatever it is given behind that, makes Structures.
    bool shouldMakeStructureForAdditionByKey(Structure*, UniquedStringImpl*);
    void didMakeStructureForAdditionByKey(Structure* made) { m_structureMadeForAdditionByKey = made->id(); }
    void didAddProperty(Structure* before, Structure* after);
    const Slot* findByKey(Structure*, UniquedStringImpl*, SlotAccess);
    void fillSiteCacheFromTables(SiteCache, JSCell* base, uint32_t identifierNumber, SlotAccess);
    // The getter that a read of the name calls on every object with the Structure of this one. Runs no code of the program and allocates no cell.
    JSValue findGetterAndFillSiteCache(SiteCache, JSCell* base, uint32_t identifierNumber);
    void cacheFreeName(SiteCache, UniquedStringImpl*, SlotAccess, const PropertyAnswer&);
    // The cell that a trap of a proxy is handed for the name. A slot names it and does not hold it. For a name of the program there is one, which is kept, so such a slot lasts as long as its Structure.
    JSCell* keyForTraps(UniquedStringImpl*);
    // The library keeps a field under the symbol in a form of its own. Whoever looks under the symbol with a key that is a value first calls the function on the compiled object that has the field, which puts the field in the form that every program may see.
    void prepareHoldersBeforeLookupsUnder(Symbol*, JSObject* function);
    JSObject* preparationBeforeLookupsUnder(UniquedStringImpl* uid) const { return m_preparationsBeforeLookups.isEmpty() ? nullptr : m_preparationsBeforeLookups.get(uid).function; }
    bool hasPreparationsBeforeLookups() const { return !m_preparationsBeforeLookups.isEmpty(); }
    bool isPreparingHolder() const { return m_isPreparingHolder; }
    void setIsPreparingHolder(bool isPreparing) { m_isPreparingHolder = isPreparing; }
    JSCell* holderLookedAtByName() const { return m_holderLookedAtByName; }
    JSCell* exchangeHolderLookedAtByName(JSCell* holder) { return std::exchange(m_holderLookedAtByName, holder); }

    template<typename Visitor> void visitRoots(Visitor&);
    void didFinishCollection(VM&, CollectionScope);

private:
    struct Addition {
        StructureID before;
        StructureID after;
    };

    static constexpr uint32_t numberOfMissCounters = 1 << 15;
    static constexpr uint32_t bitsOfMissCount = 2;
    static constexpr uint32_t missesBeforeFiling = 2;
    static constexpr uint32_t storeMissesBeforeFiling = 1;
    static constexpr uint32_t numberOfFirstAdditionsByKey = 1 << 12;
    static constexpr uint32_t firstAdditionsByKeyPerSet = 8;

    void ensureChangesAreReported(std::span<JSObject* const>);
    bool canCache(Structure*, const PropertyAnswer&) const;
    void countFiling(const CachedAccess&);

    State& m_state;
    VM& m_vm;
    VMPropertyCaches& m_tables;
    ChainDependentOwnerID m_tablesOwnerID;
    SiteCaches& m_siteCaches;
    WatchedValues m_watchedValues;
    StringKeyCache m_stringKeys;
    Vector<Addition> m_additionsSinceCollection;
    // Written under the lock of the caches.
    UncheckedKeyHashMap<UniquedStringImpl*, JSCell*> m_keysForTraps;
    struct PreparationBeforeLookups {
        Symbol* symbol { nullptr };
        JSObject* function { nullptr };
    };
    UncheckedKeyHashMap<UniquedStringImpl*, PreparationBeforeLookups> m_preparationsBeforeLookups;
    bool m_isPreparingHolder { false };
    // Compared, never read through.
    JSCell* m_holderLookedAtByName { nullptr };
    std::array<uint8_t, numberOfMissCounters> m_missCounters { };
    uint8_t* m_counterOfPairToFile { nullptr };
    std::array<uint32_t, numberOfFirstAdditionsByKey> m_firstAdditionsByKey { };
    uint32_t m_evictionSeed { 0 };
    StructureID m_structureMadeForAdditionByKey;
    std::atomic<bool> m_shouldForgetMisses { false };
    std::unique_ptr<UncheckedKeyHashSet<uint64_t>> m_countedFilings;
    UncheckedKeyHashMap<String, uint64_t> m_namedCounts;
};

static_assert(RowDisplacementTable::maxColumn <= UINT16_MAX);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
