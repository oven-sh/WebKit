/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTCheckedSpan.h"
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

class AddedExtraPropertiesBesideTable {
public:
    bool isEmpty() const { return !m_numberFilled; }
    NEVER_INLINE const AddedExtraProperty* find(StructureID structureID, UniquedStringImpl* name)
    {
        AddedExtraProperty& place = placeOf(structureID, name);
        return place.name ? &place : nullptr;
    }
    NEVER_INLINE void remember(const AddedExtraProperty& added)
    {
        if (m_places.isEmpty())
            m_places = emptyPlaces(fewestPlaces);
        AddedExtraProperty* place = &placeOf(added.structureBefore, added.name);
        if (!place->name && ++m_numberFilled * 2 > m_places.size()) {
            Vector<AddedExtraProperty> fewer = std::exchange(m_places, emptyPlaces(m_places.size() * 2));
            for (const AddedExtraProperty& kept : fewer) {
                if (kept.name)
                    placeOf(kept.structureBefore, kept.name) = kept;
            }
            place = &placeOf(added.structureBefore, added.name);
        }
        *place = added;
    }
    void clear()
    {
        m_places.clear();
        m_numberFilled = 0;
    }

private:
    static constexpr size_t fewestPlaces = 64;
    static Vector<AddedExtraProperty> emptyPlaces(size_t number)
    {
        Vector<AddedExtraProperty> places;
        places.fill(AddedExtraProperty { }, number);
        return places;
    }
    AddedExtraProperty& placeOf(StructureID structureID, UniquedStringImpl* name)
    {
        std::span<AddedExtraProperty> places = m_places.mutableSpan();
        size_t mask = places.size() - 1;
        for (size_t index = ((structureID.bits() >> StructureRowWords::granuleShift) * 0x9e3779b97f4a7c15ull + (std::bit_cast<uintptr_t>(name) >> 4) * 0xc2b2ae3d27d4eb4full) >> 32;; ++index) {
            AddedExtraProperty& place = places[index & mask];
            if (!place.name || (place.name == name && place.structureBefore == structureID))
                return place;
        }
    }

    Vector<AddedExtraProperty> m_places;
    size_t m_numberFilled { 0 };
};

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
    using Bytes = std::array<uint8_t, sizeof(Entry)>;
    static_assert(std::is_trivially_copyable_v<Entry>);

    UncheckedKeyHashMap<Key, Bytes> m_entries;
};

using SiteCache = RememberedSlot<Slot>*;

class SiteCaches final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(SiteCaches);
public:
    static constexpr unsigned regionShift = 8;
    static constexpr uint32_t numberOfMissBits = 1u << 17;

    SiteCaches(State&, const CustomAccessorTable&);

    uint32_t indexOf(SiteCache site) const
    {
        size_t offset = std::bit_cast<uintptr_t>(site) - std::bit_cast<uintptr_t>(m_slots.data());
        RELEASE_ASSERT(!(offset % sizeof(RememberedSlot<Slot>)) && offset / sizeof(RememberedSlot<Slot>) < m_slots.size());
        return static_cast<uint32_t>(offset / sizeof(RememberedSlot<Slot>));
    }
    SiteCache at(uint32_t index) const { return &checkedAt(m_slots, index); }

    bool didMissBefore(SiteCache);
    void didFailToFill(SiteCache);
    void fill(SiteCache, const Slot&, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    void clear(SiteCache);

    void dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull) const final;
    bool keepChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl* uidOrNull, const CreatedMembers&) final;

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    void waitLongerForNextFill(const void* site);
    unsigned takeSamplesToLeaveOutBeforeNextFill(const void* site);
    void forgetHowOftenItWasFilled(const void* site);
    bool didWaitForSamples(const void* site);

private:
    SiteCacheState& stateOf(const void* site) const { return *reinterpret_cast<SiteCacheState*>(reinterpret_cast<uint8_t*>(m_states) + SiteCacheState::offsetOfStateOf(std::bit_cast<uintptr_t>(site))); }
    uint8_t& refillCountOf(const void* site) const { return m_refillCounts[&stateOf(site) - m_states]; }

    State& m_state;
    const CustomAccessorTable& m_customAccessors;
    std::span<RememberedSlot<Slot>> m_slots;
    SiteCacheState* m_states { nullptr };
    uint8_t* m_refillCounts { nullptr };
    uint64_t* m_sitesThatMissed { nullptr };
    ChainDependentOwnerID m_ownerID;
    RememberedSlotSet m_writtenRegions;
    bool m_wasWrittenSinceLastSweep { false };
    UncheckedKeyHashSet<StructureID> m_structuresThatWait;
    uint32_t m_versionOfStructuresThatWait { 0 };
};

class StructuresBelowBuiltinClasses final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(StructuresBelowBuiltinClasses);
public:
    explicit StructuresBelowBuiltinClasses(State&);

    void dropChainDependent(VM&, uint32_t structureID, UniquedStringImpl*) final;
    bool hasChainDependent(VM&, uint32_t structureID, UniquedStringImpl*) const final;
    size_t removeDeadEntries(VM&, CollectionScope);
    BuiltinClassID classAbove(Structure*) const;
    bool holds(Structure*) const;
    void keep(Structure*, std::span<JSObject* const> changeableObjects);

private:
    State& m_state;
    ChainDependentOwnerID m_ownerID;
    Vector<std::pair<JSType, BuiltinClassID>, 2> m_classes;
};

class ErrorsWithMessage final {
    WTF_MAKE_NONCOPYABLE(ErrorsWithMessage);
public:
    explicit ErrorsWithMessage(State&);

    size_t removeDeadEntries(VM&, CollectionScope);
    void keepIfRead(Structure*, UniquedStringImpl* uid);

private:
    State& m_state;
};

class AnswersAtSites final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(AnswersAtSites);
public:
    AnswersAtSites(State&, SiteCaches&);

    uint32_t placeAtDistanceFromInstance(uint32_t) const;
    SiteAnswers* answersOf(uint32_t place) const
    {
        if (WideSiteAnswers* wide = wideAnswersOf(place))
            return &wide->answers;
        return m_indices[place] > indexOfAnswersThatHoldNothing ? &checkedAt(m_answers, m_indices[place] / wordsInAnswers) : nullptr;
    }

    bool isDueForFilling(uint32_t place);
    void beginPeriodOfAnotherLength(uint32_t place);
    void pointAtAnswersThatHoldNothingUnlessFilled(uint32_t place)
    {
        if (!m_indices[place])
            m_indices[place] = indexOfAnswersThatHoldNothing;
    }
    void didFailToFill(uint32_t place);
    struct AnswerByStructure {
        SiteAnswers::Answer answer;
        int32_t indexInStorageOfHolder { 0 };
    };
    void fill(uint32_t place, SiteUse, const AnswerByStructure&, UniquedStringImpl*, std::span<JSObject* const> changeableObjects, bool stubFollowsFurtherAnswers = false);
    void moveUpFurtherAnswer(uint32_t place, SiteUse, StructureID);
    void fillByShape(uint32_t place, SiteUse, SiteAnswers::Answer, std::optional<SiteAnswers::Answer> sameByStructure = std::nullopt);
    bool holdsAnotherStructure(uint32_t place, SiteUse, StructureID) const;
    void addStructureThatLacksName(uint32_t place, StructureID, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    struct WhatStandsAbove {
        uint32_t holderOrWord;
        int32_t indexInStorageOfHolder { 0 };
    };
    bool findsSlotOf(uint32_t place, SiteUse, const Object&) const;
    void fillAboveFieldLeftOut(uint32_t place, StructureID, WhatStandsAbove, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);

    void dropChainDependent(VM&, uint32_t place, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t place, UniquedStringImpl* uidOrNull) const final;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr uint32_t wordsInAnswers = sizeof(SiteAnswers) / sizeof(uint64_t);
    static constexpr uint32_t numberOfAnswersThatHoldNothing = 1;
    static constexpr uint16_t indexOfAnswersThatHoldNothing = numberOfAnswersThatHoldNothing * wordsInAnswers;
    static_assert(indexOfAnswersThatHoldNothing == indexOfSiteAnswersThatHoldNothing);
    static constexpr uint32_t wordsInWideAnswers = sizeof(WideSiteAnswers) / sizeof(uint64_t);

    uint8_t* missesAndSamplesLeftOutAt(uint32_t place);
    uint8_t& samplesLeftOutAt(uint32_t place) { return missesAndSamplesLeftOutAt(place)[1]; }
    void waitLongerForNextFill(uint32_t place);
    const void* keyOfState(uint32_t place) const { return std::bit_cast<const void*>(SiteCacheState::keyOfPlaceOfAnswers(place)); }
    SiteAnswers* ensureAnswersOf(uint32_t place, SiteUse);
    WideSiteAnswers& wideAnswersAt(uint32_t number) const { return m_endOfWideAnswers[-1 - static_cast<ptrdiff_t>(number)]; }
    uint32_t numberOf(const WideSiteAnswers& wide) const { return static_cast<uint32_t>(m_endOfWideAnswers - 1 - &wide); }
    uint32_t firstWordOfWideAnswersAt(uint32_t number) const { return static_cast<uint32_t>(m_answers.size() * wordsInAnswers - (number + 1) * wordsInWideAnswers); }
    WideSiteAnswers* wideAnswersOf(uint32_t place) const;
    WideSiteAnswers* ensureWideAnswersOf(uint32_t place);
    void addFurtherAnswer(uint32_t place, const AnswerByStructure&, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    bool wasPutOut(uint32_t place, StructureID id) const { return m_visitorsPutOut.contains(static_cast<uint64_t>(place) << 32 | id.bits()); }
    void giveBackIfEmpty(uint32_t numberOfAnswers);
    void giveBackWideAnswersIfEmpty(uint32_t numberOfWideAnswers);

    State& m_state;
    SiteCaches& m_states;
    std::span<uint16_t> m_indices;
    std::span<SiteAnswers> m_answers;
    WideSiteAnswers* m_endOfWideAnswers;
    Vector<uint32_t> m_places;
    Vector<uint16_t> m_numbersGivenBack;
    Vector<uint32_t> m_placesOfWideAnswers;
    Vector<uint16_t> m_numbersOfWideAnswersGivenBack;
    uint32_t m_nextStructureToForget { 0 };
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_visitorsPutOut;
    static constexpr uint8_t requestsThatOpenPage = 8;
    unsigned m_placesInPageShift { 0 };
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
    void forgetOrdinaryConstructorsWith(StructureID);
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

    static size_t maxSizeOfPagesAtStart(const Image&);

    bool isSharedByInstances() const { return m_isSharedByInstances; }
    void didCreateInstance(Instance&);
    void publishWords(Instance&) const;

    void set(Structure*, const CachedName&, SlotAccess, const Slot&);
    std::optional<Slot> find(StructureID, const CachedName&, SlotAccess) const;
    bool holdsInFrontRows(StructureID, const CachedName&, SlotAccess) const;
    void remove(StructureID, const CachedName&, SlotAccess);

    void fileAbsenceFilterForStores(JSGlobalObject*, JSObject*);
    bool addFilterAndCheckStoreCreatesOwnField(JSGlobalObject*, Object&, uint32_t identifierNumber);
    void didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull);
    void didReplaceValueOnObjectOfSomeChain(JSObject* changed, UniquedStringImpl*);
    void didStartWatchingReplacements();
    uint32_t absenceFilterGenerationForStores() const { return m_absenceFilterGenerationForStores; }
    bool wasChangedOnSomeChain(uint32_t identifierNumber) const { return m_namesChangedOnChains[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    bool storeCreatesOwnField(const Object&, uint32_t identifierNumber) const;
    void didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull);
    bool mayObjectPrototypeStopStore(uint32_t identifierNumber) const { return m_objectPrototypeStoreStoppers[identifierNumber / 64] >> identifierNumber % 64 & 1; }
    const AbsenceFilter& absenceFilterFor(StructureID id) const { return m_absenceFilters[AbsenceFilter::indexFor(id.bits())]; }
    void rememberNameOfText(UniquedStringImpl*);
    const JSCell*& lastKeyWithoutAtomLike(const JSCell* key) { return m_tables.keysThatCameWithoutAtom[std::bit_cast<uintptr_t>(key) / MarkedBlock::atomSize % std::tuple_size_v<decltype(Tables::keysThatCameWithoutAtom)>]; }

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull) const final;
    bool keepChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uidOrNull, const CreatedMembers&) final;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using BitPerName = std::array<uint64_t, (FieldTableEntry::maxName + 1) / 64>;

    void didChangeFilteredObject(JSObject* changed, UniquedStringImpl* uidOrNull);
    void forgetCreations();

    struct Tables {
        std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> absenceFilters;
        BitPerName namesChangedOnChains;
        BitPerName namesOfObjectPrototypes;
        BitPerName objectPrototypeStoreStoppers;
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
    uint32_t m_creationGeneration { 1 };
    ObjectsSeen m_filteredObjects;
    BitPerName& m_namesChangedOnChains { m_tables.namesChangedOnChains };
    BitPerName& m_namesOfObjectPrototypes { m_tables.namesOfObjectPrototypes };
    BitPerName& m_objectPrototypeStoreStoppers { m_tables.objectPrototypeStoreStoppers };
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
    uint32_t& stateOfPeriodsOfSamples() { return m_stateOfPeriodsOfSamples; }

    CachedName nameOf(uint32_t identifierNumber) const;
    CachedName nameOf(UniquedStringImpl*) const;

    void cache(const CachedAccess&, const PropertyAnswer&);
    bool shouldFile(StructureID, UniquedStringImpl*, SlotAccess);
    void didFileNothing();
    void remember(const FieldExit&);
    void forgetFieldExits()
    {
        m_fieldExits.fill({ });
        m_fieldExitsBesideTable.clear();
        m_fieldReturns.fill({ });
    }
    void remember(const FieldReturn& back) { placeOfReturn(back.structureBefore, back.name) = back; }
    const FieldReturn* findFieldReturn(StructureID structureID, uint32_t name)
    {
        const FieldReturn& back = placeOfReturn(structureID, name);
        return back.field && back.structureBefore == structureID && back.name == name ? &back : nullptr;
    }
    void forgetAddedExtraProperties()
    {
        m_addedExtraProperties.fill({ });
        m_addedExtraPropertiesBesideTable.clear();
    }
    FieldExit* findFieldExit(StructureID structureID, uint32_t name)
    {
        FieldExit& exit = placeOf(structureID, name);
        if (exit.field && exit.structureBefore == structureID && exit.name == name) [[likely]]
            return &exit;
        return m_fieldExitsBesideTable.isEmpty() ? nullptr : takeFieldExitFromBesideTable(structureID, name);
    }
    void remember(const AddedExtraProperty& added)
    {
        AddedExtraProperty& place = placeOf(added.structureBefore, added.name);
        if (!place.name || (place.name == added.name && place.structureBefore == added.structureBefore))
            place = added;
        else
            m_addedExtraPropertiesBesideTable.remember(added);
    }
    const AddedExtraProperty* findAddedExtraProperty(StructureID structureID, UniquedStringImpl* name)
    {
        AddedExtraProperty& added = placeOf(structureID, name);
        if (added.name == name && added.structureBefore == structureID) [[likely]]
            return &added;
        return m_addedExtraPropertiesBesideTable.isEmpty() ? nullptr : m_addedExtraPropertiesBesideTable.find(structureID, name);
    }
    void didAddProperty(Structure* before, Structure* after);
    const Slot* findByKey(Structure*, UniquedStringImpl*, SlotAccess);
    void fillSiteCacheFromTables(SiteCache, JSCell* base, uint32_t identifierNumber, SlotAccess);
    void fillSiteAnswers(uint32_t place, JSCell* base, uint32_t identifierNumber, SiteUse, bool stubFollowsFurtherAnswers = false, bool stubJumpsToArmOfAnswer = false);
    void fileOwnPlainValueUnlessFiled(JSObject*, uint32_t identifierNumber);
    static ReadGroupCache::Row* claimReadGroupRow(JSValue, ReadGroupCache&);
    JSValue fillReadGroupOffset(JSCell*, uint32_t identifierNumber, ReadGroupCache::Row&, uint32_t index);
    bool fillAboveFieldLeftOut(uint32_t place, SiteUse, Object&, const CachedName&);
    JSCell* keyForTraps(UniquedStringImpl*);
    void prepareHoldersBeforeLookupsUnder(Symbol*, JSObject* function);
    JSObject* preparationBeforeLookupsUnder(UniquedStringImpl* uid) const { return m_preparationsBeforeLookups.isEmpty() ? nullptr : m_preparationsBeforeLookups.get(uid).function; }
    bool hasPreparationsBeforeLookups() const { return !m_preparationsBeforeLookups.isEmpty(); }
    bool isPreparingHolder() const { return m_isPreparingHolder; }
    void setIsPreparingHolder(bool isPreparing) { m_isPreparingHolder = isPreparing; }
    bool isLookedAtByName(const JSCell* holder, const UniquedStringImpl* symbol) const { return m_holderLookedAtByName == holder && m_symbolLookedAtByName == symbol; }
    JSCell* exchangeHolderLookedAtByName(JSCell* holder) { return std::exchange(m_holderLookedAtByName, holder); }
    UniquedStringImpl* exchangeSymbolLookedAtByName(UniquedStringImpl* symbol) { return std::exchange(m_symbolLookedAtByName, symbol); }

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
    void keepIfBelowBuiltinClass(Structure*);
    bool canCache(Structure*, const PropertyAnswer&) const;

    State& m_state;
    VM& m_vm;
    VMPropertyCaches& m_tables;
    ChainDependentOwnerID m_tablesOwnerID;
    SiteCaches m_siteCaches;
    AnswersAtSites m_answersAtSites;
    StructuresBelowBuiltinClasses m_structuresBelowBuiltinClasses;
    ErrorsWithMessage m_errorsWithMessage;
    uint32_t m_stateOfPeriodsOfSamples { 0 };
    Vector<Addition> m_additionsSinceCollection;
    UncheckedKeyHashMap<UniquedStringImpl*, JSCell*> m_keysForTraps;
    struct PreparationBeforeLookups {
        Symbol* symbol { nullptr };
        JSObject* function { nullptr };
    };
    UncheckedKeyHashMap<UniquedStringImpl*, PreparationBeforeLookups> m_preparationsBeforeLookups;
    bool m_isPreparingHolder { false };
    JSCell* m_holderLookedAtByName { nullptr };
    UniquedStringImpl* m_symbolLookedAtByName { nullptr };
    std::array<uint8_t, numberOfMissCounters> m_missCounters { };
    uint8_t* m_counterOfPairToFile { nullptr };
    static uint64_t keyOfFieldExit(StructureID structureID, uint32_t name) { return static_cast<uint64_t>(structureID.bits()) << 32 | name; }
    static uint64_t keyOf(const FieldExit& exit) { return keyOfFieldExit(exit.structureBefore, exit.name); }
    static bool isFilled(const FieldExit& exit) { return !!exit.field; }
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
    FieldExit& placeOf(StructureID structureID, uint32_t name) { return m_fieldExits[((structureID.bits() >> StructureRowWords::granuleShift) + name) % FieldExit::numberOfPlaces]; }
    std::array<FieldExit, FieldExit::numberOfPlaces> m_fieldExits { };
    EntriesBesideTable<FieldExit, uint64_t> m_fieldExitsBesideTable;
    FieldReturn& placeOfReturn(StructureID structureID, uint32_t name) { return m_fieldReturns[((structureID.bits() >> StructureRowWords::granuleShift) + name) % FieldReturn::numberOfPlaces]; }
    std::array<FieldReturn, FieldReturn::numberOfPlaces> m_fieldReturns { };
    AddedExtraProperty& placeOf(StructureID structureID, UniquedStringImpl* name) { return m_addedExtraProperties[((structureID.bits() >> StructureRowWords::granuleShift) + (std::bit_cast<uintptr_t>(name) >> 4)) % AddedExtraProperty::numberOfPlaces]; }
    std::array<AddedExtraProperty, AddedExtraProperty::numberOfPlaces> m_addedExtraProperties { };
    AddedExtraPropertiesBesideTable m_addedExtraPropertiesBesideTable;
    std::atomic<bool> m_shouldForgetMisses { false };
};

static_assert(RowDisplacementTable::maxColumn <= UINT16_MAX);

class FirstAdditions {
    WTF_MAKE_NONCOPYABLE(FirstAdditions);
    WTF_MAKE_TZONE_ALLOCATED(FirstAdditions);
public:
    explicit FirstAdditions(State&);

    bool shouldMakeStructure(Structure*, UniquedStringImpl*);
    void didMakeStructure(Structure* made) { m_structureMade = made->id(); }
    void didBeginToFill(VM&, JSObject*, Structure* structureBefore);
    void finishFilling(VM&);
    void adoptSharedStructure(VM&, JSObject*);
    bool forget(JSObject*);
    bool mayBeginToFill(JSObject*) const;
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr uint32_t numberOfPairs = 1 << 12;
    static constexpr uint32_t pairsPerSet = 8;
    static constexpr uint32_t numberOfObjectsBeingFilled = 8;

    struct ObjectBeingFilled {
        JSObject* object { nullptr };
        StructureID sharedStructure;
    };
    static bool adoptSharedStructure(VM&, ObjectBeingFilled&);
    static void finishFilling(VM&, ObjectBeingFilled);

    std::span<uint32_t, numberOfPairs> m_pairs;
    uint32_t m_evictionSeed { 0 };
    StructureID m_structureMade;
    std::array<ObjectBeingFilled, numberOfObjectsBeingFilled> m_objectsBeingFilled { };
    uint32_t m_numberOfObjectsFilled { 0 };
    static constexpr uint32_t numberOfObjectsCounted = 64;
    static constexpr uintptr_t timesMask = 15;
    static uint32_t placeOfTimes(JSObject* object) { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(object) >> 4) * 0x9e3779b1u >> 26; }
    std::array<uintptr_t, numberOfObjectsCounted> m_timesObjectsBeganToFill { };
};

FirstAdditions& firstAdditionsOf(State&);
void didFinishFilling(Structure*);
bool hasFewEnoughNamesToBeFilled(Structure*);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
