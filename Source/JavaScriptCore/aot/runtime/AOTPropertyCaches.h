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
#include "AOTRuntimeRecords.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <array>
#include <atomic>
#include <bit>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSObject;
class Symbol;

namespace AOT {

class CustomAccessorTable;
class Image;
struct Instance;
struct State;

// What falls on a place of a table that holds another entry waits here, so two entries of one place do not forget each other.
// It grows by an entry for each that met another on its place since the last collection, and is forgotten with the table.
template<typename Entry, typename Key>
class EntriesBesideTable {
public:
    bool isEmpty() const { return m_entries.isEmpty(); }
    void add(const Key& key, const Entry& entry) { m_entries.set(key, std::bit_cast<Bytes>(entry)); }
    bool take(const Key& key, Entry& entry)
    {
        auto found = m_entries.find(key);
        if (found == m_entries.end())
            return false;
        entry = std::bit_cast<Entry>(found->value);
        m_entries.remove(found);
        return true;
    }
    void clear() { m_entries.clear(); }

private:
    // The storage of a hash map is aligned for a word, and an entry may ask for more.
    using Bytes = std::array<uint8_t, sizeof(Entry)>;
    static_assert(std::is_trivially_copyable_v<Entry>);

    UncheckedKeyHashMap<Key, Bytes> m_entries;
};

using SiteCache = RememberedSlot<Slot>*;

// One Slot for each place of the program that reads or stores under a name and has a site: what the tables say of the Structure that came there.
// A slot names its cells and does not hold them.
class SiteCaches final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(SiteCaches);
public:
    static constexpr unsigned regionShift = 8;
    // Sites share the bits by number: a bit only decides whether a site is filled at its first sample or at its second.
    static constexpr uint32_t numberOfMissBits = 1u << 17;

    SiteCaches(State&, const CustomAccessorTable&);

    uint32_t indexOf(SiteCache site) const { return static_cast<uint32_t>(site - m_slots.data()); }
    SiteCache at(uint32_t index) const { return &m_slots[index]; }

    // A site is filled at the second sample that falls on it, so the page of a site that is seldom come by stays untouched.
    bool didMissBefore(SiteCache);
    void didFailToFill(SiteCache);
    // changeableObjects: those whose changes end what a slot with Slot::dependsOnChain says. They report their changes already.
    void fill(SiteCache, const Slot&, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    void clear(SiteCache);

    void dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) const final;
    bool keepChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull, const CreatedMembers&) final;

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    // The states are shared by address, with the places that have SiteAnswers too.
    void waitLongerForNextFill(const void* site);
    void forgetHowOftenItWasFilled(const void* site);
    // For a place whose misses are sampled: whether as many samples have fallen on it as it was to wait for.
    bool didWaitForSamples(const void* site);

private:
    SiteCacheState& stateOf(const void* site) const { return *reinterpret_cast<SiteCacheState*>(reinterpret_cast<uint8_t*>(m_states) + ((std::bit_cast<uintptr_t>(site) >> SiteCacheState::slotAddressShift) & SiteCacheState::offsetMask)); }
    uint8_t& refillCountOf(const void* site) const { return m_refillCounts[&stateOf(site) - m_states]; }

    State& m_state;
    const CustomAccessorTable& m_customAccessors;
    std::span<RememberedSlot<Slot>> m_slots;
    SiteCacheState* m_states { nullptr };
    // How often the sites of a state were filled again. The stubs do not read it.
    uint8_t* m_refillCounts { nullptr };
    uint64_t* m_sitesThatMissed { nullptr };
    ChainDependentOwnerID m_ownerID;
    RememberedSlotSet m_writtenRegions;
    bool m_wasWrittenSinceLastSweep { false };
    UncheckedKeyHashSet<StructureID> m_structuresThatWait;
    uint32_t m_versionOfStructuresThatWait { 0 };
};

// The SiteAnswers of every place of the program that has them. An answer names its cells and does not hold them.
// A place is known by its number. It has 16 bits of its own, which say where its answers lie in a table that is filled from the first on: so answers cost memory as they come to be, and a place without them reads what all such places read.
class AnswersAtSites final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(AnswersAtSites);
public:
    // Places share the bits by number: a bit only decides whether a place is filled at its first sample or at its second.
    static constexpr uint32_t numberOfMissBits = 1u << 17;

    AnswersAtSites(State&, SiteCaches&);

    uint32_t placeAtDistanceFromInstance(uint32_t) const;
    // Null: it has none.
    SiteAnswers* answersOf(uint32_t place) const { return m_indices[place] ? &m_answers[m_indices[place] / wordsInAnswers] : nullptr; }

    // One miss in SiteCacheState::missesOfEmptySitesPerSample comes here: a sample. A place is filled at the second sample that falls on it at the earliest, so a place that is seldom come by costs nothing.
    // It waits for more samples after each filling and after each look that found nothing to remember: one that goes on missing meets cells of many Structures, and no filling helps it.
    bool isDueForFilling(uint32_t place);
    void didFailToFill(uint32_t place);
    struct AnswerByStructure {
        SiteAnswers::Answer answer;
        // Of Form::ValueInStorageOfHolder.
        int32_t indexInStorageOfHolder { 0 };
    };
    // changeableObjects: those whose changes end what the answer says of the objects above the receiver. They report their changes already.
    void fill(uint32_t place, SiteUse, const AnswerByStructure&, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    // For a place that reads.
    void fillByShape(uint32_t place, SiteAnswers::Answer);

    void dropChainDependent(VM&, uint32_t place, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t place, UniquedStringImpl* uidOrNull) const final;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr uint32_t wordsInAnswers = sizeof(SiteAnswers) / sizeof(uint64_t);

    // The states are shared by address.
    const void* keyOfState(uint32_t place) const { return &m_indices[place]; }
    // Null: every one that an index reaches is taken.
    SiteAnswers* ensureAnswersOf(uint32_t place, SiteUse);
    void giveBackIfEmpty(uint32_t numberOfAnswers);

    State& m_state;
    SiteCaches& m_states;
    std::span<uint16_t> m_indices;
    std::span<SiteAnswers> m_answers;
    // By the number of the answers in the table.
    Vector<uint32_t> m_places;
    Vector<uint16_t> m_numbersGivenBack;
    uint64_t* m_placesThatMissed { nullptr };
    // The run time looked and found nothing to remember.
    uint64_t* m_placesThatFailed { nullptr };
    static constexpr uint8_t requestsThatOpenPage = 3;
    unsigned m_placesInPageShift { 0 };
    // By page of m_indices: how often a place of the page was due for its first answers, up to requestsThatOpenPage.
    uint8_t* m_requestsOfPages { nullptr };
    ChainDependentOwnerID m_ownerID;
    bool m_wasWrittenSinceLastSweep { false };
};

struct CachedName {
    UniquedStringImpl* uid;
    std::optional<uint32_t> identifierNumber;
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
    void forgetPlacesCopiedFrom(const HashedSlot&);
    void forgetPlacesCopiedFromSlotForStores(const Slot&);
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
    // For a store to a field that the object lacks. The filter covers all the names of a Structure: it decides this store, and lets the stubs create the fields after this one.
    bool addFilterAndCheckStoreCreatesOwnField(JSGlobalObject*, Object&, uint32_t identifierNumber);
    void didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull);
    void didStartWatchingReplacements();
    uint32_t absenceFilterGenerationForStores() const { return m_absenceFilterGenerationForStores; }
    bool wasChangedOnSomeChain(uint32_t identifierNumber) const { return m_namesChangedOnChains[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    // Like the stubs' branchIfChainMayHaveName(): no object on the prototype chain has an accessor or a read-only property with this name, so a store creates the object's own field.
    bool storeCreatesOwnField(const Object&, uint32_t identifierNumber) const;
    void didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull);
    bool mayBeNameOfObjectPrototype(uint32_t identifierNumber) const { return m_namesOfObjectPrototypes[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    const AbsenceFilter& absenceFilterFor(StructureID id) const { return m_absenceFilters[AbsenceFilter::indexFor(id.bits())]; }
    void rememberNameOfText(UniquedStringImpl*);
    // A string that names nothing anywhere is read under without an atom being made: most such keys come once. One that comes again gets its atom, and from then on the stubs answer.
    // The cells are only compared.
    const JSCell*& lastKeyWithoutAtomLike(const JSCell* key) { return m_tables.keysThatCameWithoutAtom[std::bit_cast<uintptr_t>(key) / MarkedBlock::atomSize % std::tuple_size_v<decltype(Tables::keysThatCameWithoutAtom)>]; }

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) const final;
    bool keepChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull, const CreatedMembers&) final;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using BitPerName = std::array<uint64_t, (FieldTableEntry::maxName + 1) / 64>;

    void didChangeFilteredObject(JSObject* changed, UniquedStringImpl* uidOrNull);

    // Pages of the VM: zero-filled, and untouched until first read or written.
    struct Tables {
        std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> absenceFilters;
        BitPerName namesChangedOnChains;
        BitPerName namesOfObjectPrototypes;
        std::array<uint8_t, MissesOfPairs::numberOfCounters> missesOfPairs;
        std::array<NameOfText, NameOfText::numberOfEntries> namesOfTexts;
        std::array<const JSCell*, 256> keysThatCameWithoutAtom;
    };

    VM& m_vm;
    CustomAccessorTable m_customAccessors;
    RowDisplacementTable m_front;
    HashedSlotTable m_hashedTable;
    UncheckedKeyHashMap<UniquedStringImpl*, uint16_t, WTF::PtrHash<UniquedStringImpl*>> m_columnsByNameAddress;
    Tables& m_tables;
    std::array<AbsenceFilter, AbsenceFilter::numberOfFilters>& m_absenceFilters { m_tables.absenceFilters };
    uint32_t m_absenceFilterGenerationForStores { 1 };
    uint32_t m_changesUnderNamesOnChains { 0 };
    // The objects that the filters and the Object.prototype name sets describe.
    ObjectsSeen m_filteredObjects;
    BitPerName& m_namesChangedOnChains { m_tables.namesChangedOnChains };
    BitPerName& m_namesOfObjectPrototypes { m_tables.namesOfObjectPrototypes };
    std::array<uint8_t, MissesOfPairs::numberOfCounters>& m_missesOfPairs { m_tables.missesOfPairs };
    std::array<NameOfText, NameOfText::numberOfEntries>& m_namesOfTexts { m_tables.namesOfTexts };
    bool m_hasInstance { false };
    bool m_isSharedByInstances { false };
};

struct CachedAccess {
    Structure* structure;
    CachedName name;
    SlotAccess access;
};

void initializePropertyCacheWords(Instance&);

class PropertyCaches {
    WTF_MAKE_NONCOPYABLE(PropertyCaches);
    WTF_MAKE_TZONE_ALLOCATED(PropertyCaches);
public:
    explicit PropertyCaches(State&);
    ~PropertyCaches();

    VMPropertyCaches& tables() { return m_tables; }
    SiteCaches& siteCaches() { return m_siteCaches; }
    AnswersAtSites& answersAtSites() { return m_answersAtSites; }
    // beginPeriodOfAnotherLength() of AOTPropertyCachesOperations.cpp.
    uint32_t& stateOfPeriodsOfSamples() { return m_stateOfPeriodsOfSamples; }

    CachedName nameOf(uint32_t identifierNumber) const;
    CachedName nameOf(UniquedStringImpl*) const;

    void cache(const CachedAccess&, const PropertyAnswer&);
    bool shouldFile(StructureID, UniquedStringImpl*, SlotAccess);
    // The look that followed the last yes of shouldFile() changed the Structure of the receiver, so nothing is filed under the Structure that it came with. It changes the next object of that Structure too: the misses of the pair count anew.
    void didFileNothing();
    void remember(const FieldExit& exit) { putAtPlace(placeOf(exit.structureBefore, exit.name), m_fieldExitsBesideTable, exit); }
    void forgetFieldExits()
    {
        m_fieldExits.fill({ });
        m_fieldExitsBesideTable.clear();
        m_addedExtraProperties.fill({ });
        m_addedExtraPropertiesBesideTable.clear();
    }
    // The stubs look at the place alone. What is found beside the table changes places with what lies there.
    FieldExit* findFieldExit(StructureID structureID, uint32_t name)
    {
        FieldExit& exit = placeOf(structureID, name);
        if (exit.field && exit.structureBefore == structureID && exit.name == name) [[likely]]
            return &exit;
        return m_fieldExitsBesideTable.isEmpty() ? nullptr : takeFieldExitFromBesideTable(structureID, name);
    }
    void remember(const AddedExtraProperty& added) { putAtPlace(placeOf(added.structureBefore, added.name), m_addedExtraPropertiesBesideTable, added); }
    const AddedExtraProperty* findAddedExtraProperty(StructureID structureID, UniquedStringImpl* name)
    {
        AddedExtraProperty& added = placeOf(structureID, name);
        if (added.name == name && added.structureBefore == structureID) [[likely]]
            return &added;
        return m_addedExtraPropertiesBesideTable.isEmpty() ? nullptr : takeAddedExtraPropertyFromBesideTable(structureID, name);
    }
    void didAddProperty(Structure* before, Structure* after);
    const Slot* findByKey(Structure*, UniquedStringImpl*, SlotAccess);
    // Copies what the tables hold for the Structure of the cell. Runs no code of the program and collects nothing.
    void fillSiteCacheFromTables(SiteCache, JSCell* base, uint32_t identifierNumber, SlotAccess);
    // The same for a place with SiteAnswers, which also remembers what the layout of a compiled object says.
    void fillSiteAnswers(uint32_t place, JSCell* base, uint32_t identifierNumber, SiteUse);
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

    void ensureChangesAreReported(std::span<JSObject* const>);
    bool canCache(Structure*, const PropertyAnswer&) const;

    State& m_state;
    VM& m_vm;
    VMPropertyCaches& m_tables;
    ChainDependentOwnerID m_tablesOwnerID;
    SiteCaches m_siteCaches;
    AnswersAtSites m_answersAtSites;
    uint32_t m_stateOfPeriodsOfSamples { 0 };
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
    using KeyOfAddedExtraProperty = std::pair<uint32_t, UniquedStringImpl*>;
    static uint64_t keyOfFieldExit(StructureID structureID, uint32_t name) { return static_cast<uint64_t>(structureID.bits()) << 32 | name; }
    static uint64_t keyOf(const FieldExit& exit) { return keyOfFieldExit(exit.structureBefore, exit.name); }
    static KeyOfAddedExtraProperty keyOf(const AddedExtraProperty& added) { return { added.structureBefore.bits(), added.name }; }
    static bool isFilled(const FieldExit& exit) { return !!exit.field; }
    static bool isFilled(const AddedExtraProperty& added) { return !!added.name; }
    template<typename Entry, typename Key>
    static NEVER_INLINE void putAtPlace(Entry& place, EntriesBesideTable<Entry, Key>& beside, const Entry& entry)
    {
        if (isFilled(place) && keyOf(place) != keyOf(entry))
            beside.add(keyOf(place), place);
        place = entry;
    }
    template<typename Entry, typename Key>
    static Entry* takeFromBesideTable(Entry& place, EntriesBesideTable<Entry, Key>& beside, const Key& key)
    {
        Entry found;
        if (!beside.take(key, found))
            return nullptr;
        putAtPlace(place, beside, found);
        return &place;
    }
    NEVER_INLINE FieldExit* takeFieldExitFromBesideTable(StructureID structureID, uint32_t name) { return takeFromBesideTable(placeOf(structureID, name), m_fieldExitsBesideTable, keyOfFieldExit(structureID, name)); }
    NEVER_INLINE AddedExtraProperty* takeAddedExtraPropertyFromBesideTable(StructureID structureID, UniquedStringImpl* name) { return takeFromBesideTable(placeOf(structureID, name), m_addedExtraPropertiesBesideTable, KeyOfAddedExtraProperty { structureID.bits(), name }); }
    FieldExit& placeOf(StructureID structureID, uint32_t name) { return m_fieldExits[((structureID.bits() >> StructureRowWords::granuleShift) + name) % FieldExit::numberOfPlaces]; }
    std::array<FieldExit, FieldExit::numberOfPlaces> m_fieldExits { };
    EntriesBesideTable<FieldExit, uint64_t> m_fieldExitsBesideTable;
    AddedExtraProperty& placeOf(StructureID structureID, UniquedStringImpl* name) { return m_addedExtraProperties[((structureID.bits() >> StructureRowWords::granuleShift) + (std::bit_cast<uintptr_t>(name) >> 4)) % AddedExtraProperty::numberOfPlaces]; }
    std::array<AddedExtraProperty, AddedExtraProperty::numberOfPlaces> m_addedExtraProperties { };
    EntriesBesideTable<AddedExtraProperty, KeyOfAddedExtraProperty> m_addedExtraPropertiesBesideTable;
    std::atomic<bool> m_shouldForgetMisses { false };
};

static_assert(RowDisplacementTable::maxColumn <= UINT16_MAX);

// Which names objects were given that no object of their Structure had been given before. A State makes it when it is first asked for.
class FirstAdditions {
    WTF_MAKE_NONCOPYABLE(FirstAdditions);
    WTF_MAKE_TZONE_ALLOCATED(FirstAdditions);
public:
    explicit FirstAdditions(State&);

    // An object of the Structure is about to be given a name that no object of it has been given. A Structure is made for what comes again: the first such object becomes a dictionary,
    // and the next one that is given the same name there, and whatever it is given behind that, makes Structures.
    bool shouldMakeStructure(Structure*, UniquedStringImpl*);
    void didMakeStructure(Structure* made) { m_structureMade = made->id(); }
    // The object has become a dictionary for the names that it is given now. It is made flat when as many other objects have followed it as are remembered, when the code that fills it has returned to the event loop,
    // or when somebody asks what its Structure says.
    void didBeginToFill(VM&, JSObject*);
    void finishFilling(VM&);
    // Whether the object was being filled. It is so no longer.
    bool forget(JSObject*);
    // An object that is given a name and loses it, or is looked at between any two names, begins to be filled again and again, and each time its names are copied and Structures are made that nothing finds again.
    // After a few times it makes Structures as ever, which come again.
    bool mayBeginToFill(JSObject*) const;
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr uint32_t numberOfPairs = 1 << 12;
    static constexpr uint32_t pairsPerSet = 8;
    static constexpr uint32_t numberOfObjectsBeingFilled = 8;

    // Pages that are zero until they are written.
    std::span<uint32_t, numberOfPairs> m_pairs;
    uint32_t m_evictionSeed { 0 };
    StructureID m_structureMade;
    std::array<JSObject*, numberOfObjectsBeingFilled> m_objectsBeingFilled { };
    uint32_t m_numberOfObjectsFilled { 0 };
    static constexpr uint32_t numberOfObjectsCounted = 64;
    static constexpr uintptr_t timesMask = 15;
    static uint32_t placeOfTimes(JSObject* object) { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(object) >> 4) * 0x9e3779b1u >> 26; }
    // The address of an object, which ends in four zeros, and in their place how often it began to be filled. The addresses are compared and never followed, and a collection forgets them.
    std::array<uintptr_t, numberOfObjectsCounted> m_timesObjectsBeganToFill { };
};

FirstAdditions& firstAdditionsOf(State&);
void didFinishFilling(Structure*);
// The names that an object has are copied into the table in which it is filled. An object that is given a name between two looks, again and again, is filled again and again: it may be so while it has few names.
bool hasFewEnoughNamesToBeFilled(Structure*);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
