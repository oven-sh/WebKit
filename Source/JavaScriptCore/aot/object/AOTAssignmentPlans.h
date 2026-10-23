/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTFieldMoves.h"
#include "AOTObject.h"
#include "AOTObjectCopy.h"
#include "AOTSlotAccess.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include "StructureID.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/RefPtr.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

struct State;

// The names of one run of identifiers in the image, which a rest pattern leaves out.
class ExcludedNameList {
    WTF_MAKE_NONCOPYABLE(ExcludedNameList);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ExcludedNameList);
public:
    ExcludedNameList(Vector<UniquedStringImpl*>&&, uint32_t runNumberPlusOne);

    std::span<UniquedStringImpl* const> names() const { return m_names.span(); }
    uint32_t runNumberPlusOne() const { return m_runNumberPlusOne; }
    bool contains(UniquedStringImpl* name) const { return m_names.size() <= maxNumberOfNamesCompared ? m_names.contains(name) : m_longList.contains(name); }

private:
    static constexpr size_t maxNumberOfNamesCompared = 8;

    Vector<UniquedStringImpl*> m_names;
    UncheckedKeyHashSet<UniquedStringImpl*> m_longList;
    uint32_t m_runNumberPlusOne;
};

struct AssignmentSource {
    ALWAYS_INLINE JSValue read(const auto& store) const
    {
        if (!store.from)
            return propertyStorage->getDirect(store.sourceOffset);
        if (!store.from->isOutOfLine())
            return compiledObject->readInline(*store.from);
        // Most fields of a wide layout lie in the Extension, which is looked for once a copy.
        SlotRepresentation representation = store.from->slot.representation();
        const void* slot = extension ? extension->slotOrNull(store.from->slot.offset) : nullptr;
        if (slot && representation != SlotRepresentation::DeferredClosure) [[likely]] {
            if (JSValue value = readSlot(representation, slot, *strings)) [[likely]]
                return value;
        }
        return compiledObject->read(*store.from);
    }

    StructureID propertyStorageStructureID() const { return propertyStorage ? propertyStorage->structureID() : StructureID(); }

    JSObject* object { nullptr };
    const Object* compiledObject { nullptr };
    // An ordinary object itself, or the extra properties of a compiled object.
    JSObject* propertyStorage { nullptr };
    uint32_t orderWord { 0 };
    // The names of an ExcludedNameList, or a list of the caller's that lasts as long as the copy.
    std::span<UniquedStringImpl* const> excludedNames { };
    bool defines { false };
    // Of a compiled object.
    Extension* extension { nullptr };
    const ProgramStrings* strings { nullptr };
};

struct AssignmentPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(AssignmentPlan);

    struct FieldStore {
        const FieldData* from { nullptr };
        const FieldData* to { nullptr };
        PropertyOffset sourceOffset { invalidOffset };
        StructureID lastAcceptedStructure { };
        std::optional<uint32_t> orderWordBeforeCreation { };
        uint32_t orderWordAfterCreation { 0 };
        OptionSet<ValueKind> acceptedKinds;
        bool sourceAlwaysFits { false };
        // The slot holds a closure that may not be made yet, which only a definition by name knows how to replace.
        bool isDefinedByName { false };
        bool isAbsentWhenEmpty { false };
        bool isDirect { false };
        // Both slots have one representation, so what the one holds is what the other has to hold.
        bool copiesBits { false };
        // And whatever the source's field holds fits: the store is a load and a store, whatever the other stores of the plan are.
        bool copiesBitsUntested { false };
        bool holdsCell { false };
        // Of the slot that is stored to.
        bool hasAbsent { false };
        uint8_t width { 0 };
        uint64_t absentBits { 0 };
    };

    struct ExtraPropertyStore {
        const FieldData* from { nullptr };
        PropertyOffset sourceOffset { invalidOffset };
        PropertyOffset offset { invalidOffset };
        // extraPropertiesAfter keeps it.
        UniquedStringImpl* name { nullptr };
        bool addsName { false };
    };
    // In storesInSourceOrder: the index of an ExtraPropertyStore. Without it, of a FieldStore.
    static constexpr uint16_t isExtraPropertyStore = 1 << 15;

    bool isFor(StructureID targetID, StructureID extraPropertiesID, const AssignmentSource& from) const
    {
        return target == targetID && extraPropertiesBefore == extraPropertiesID && source == from.object->structureID()
            && sourcePropertyStorage == from.propertyStorageStructureID() && sourceOrderWord == from.orderWord
            && (!servesDefinitionsOnly || from.defines) && leavesOut(from.excludedNames);
    }

    // For a copy between two compiled objects that leaves out the names of a run or none. A Structure says whether its objects have extra properties.
    bool fillsEmptySlotsOf(const Object& targetObject, const Object& sourceObject, uint32_t excludedRunNumberPlusOne) const
    {
        return orderWordBeforeFillingAll == targetObject.orderWord() && target == targetObject.structureID() && source == sourceObject.structureID() && sourceOrderWord == sourceObject.orderWord()
            && (excludedList ? excludedList->runNumberPlusOne() : 0) == excludedRunNumberPlusOne && excludedNames.isEmpty();
    }

    // For a copy into a compiled object without extra properties that leaves out the names of a run or none. See tryRunAsBefore.
    bool runsAsBeforeOn(const Object& targetObject, const AssignmentSource& from, uint32_t excludedRunNumberPlusOne) const
    {
        return isUsable && orderWordBeforeRun == targetObject.orderWord() && target == targetObject.structureID() && extraPropertiesBefore == StructureID() && source == from.object->structureID()
            && sourcePropertyStorage == from.propertyStorageStructureID() && sourceOrderWord == from.orderWord
            && (excludedList ? excludedList->runNumberPlusOne() : 0) == excludedRunNumberPlusOne && excludedNames.isEmpty();
    }

    // Nothing but the list itself lies at the address of a list.
    bool leavesOut(std::span<UniquedStringImpl* const> names) const
    {
        if (excludedList)
            return excludedList->names().data() == names.data() && excludedList->names().size() == names.size();
        return std::ranges::equal(excludedNames, names, { }, &RefPtr<UniquedStringImpl>::get);
    }

    // For the collector, which may run in a thread of its own: a name is released by the thread whose table of names holds it, when the plan is made anew.
    void forgetCells()
    {
        Vector<RefPtr<UniquedStringImpl>, 2> names = WTF::move(excludedNames);
        *this = { };
        excludedNames = WTF::move(names);
    }

    StructureID target;
    StructureID source;
    StructureID sourcePropertyStorage;
    StructureID extraPropertiesBefore;
    uint32_t sourceOrderWord { 0 };
    // What the copy leaves out: the list of a run, or names that the plan keeps for itself.
    const ExcludedNameList* excludedList { nullptr };
    Vector<RefPtr<UniquedStringImpl>, 2> excludedNames;
    bool isUsable { false };
    // The run stores this many of fieldStores. From the first that is defined by name on, finishByName() goes on.
    size_t numberOfFieldStoresInRun { 0 };
    // For finishByName(). Empty where there are too many stores to number.
    Vector<uint16_t> storesInSourceOrder;
    // Nothing has looked whether a prototype intercepts a store under one of the names.
    bool servesDefinitionsOnly { false };
    // In a row. A copy whose source always holds a value that a field of the target does not take is not tried by its plan for ever.
    uint8_t numberOfRunsGivenUp { 0 };
    bool addsExtraProperties { false };
    bool mayCreateFields { false };
    // The plan serves definitions only, or no prototype intercepts a store under any of its names: a store and a definition do the same.
    bool storesAreDefinitions { false };
    // Every property comes from a field whose values always fit the field that takes them, and none becomes an extra property.
    bool storesOnlyFieldsThatFit { false };
    bool mayStoreCells { false };
    bool readsOutOfLineFields { false };
    std::optional<uint32_t> orderWordBeforeFillingAll { };
    uint32_t orderWordAfterFillingAll { 0 };
    // A run that has left every value in its slot or among the extra properties, however each field was created, has shown what the run makes of that order word: see tryRunAsBefore.
    std::optional<uint32_t> orderWordBeforeRun { };
    uint32_t orderWordAfterRun { 0 };
    uint64_t granuleBitsFilled { 0 };
    Structure* extraPropertiesAfter { nullptr };
    CompiledStructure* targetAfter { nullptr };
    // A field that the run creates is listed behind the names that the run has added by then, so the stores behind the first added name run after the extra properties.
    Vector<FieldStore> fieldStores;
    unsigned numberOfFieldStoresBeforeAddedNames { 0 };
    Vector<ExtraPropertyStore> extraPropertyStores;
};

// `{ ...first, ...second }` where the first is no compiled object: a new ordinary object that gets what the two hold at once, all of it in its cell.
struct CopyOfTwoPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CopyOfTwoPlan);

    struct Source {
        // Nothing is asked of a value without properties of its own, which has no Structure here.
        bool isFor(const AssignmentSource* from) const
        {
            if (!from)
                return !structure;
            return structure == from->object->structureID() && propertyStorage == from->propertyStorageStructureID() && orderWord == from->orderWord;
        }

        StructureID structure;
        StructureID propertyStorage;
        uint32_t orderWord { 0 };
        // Out of the slots of a compiled object. A property that the second source has too is not moved out of the first.
        FieldMovePlan fields;
        // Out of an ordinary object, or out of the extra properties of a compiled one.
        FieldMovePlan properties;
    };

    bool isFor(const AssignmentSource* first, const AssignmentSource* second) const { return sources[0].isFor(first) && sources[1].isFor(second); }

    std::array<Source, 2> sources;
    // Null where such a pair is copied one behind the other.
    Structure* structure { nullptr };
    // Several pairs that find no free place may come between two pairs that find the plan.
    static constexpr uint8_t maxVisitsInVain = 15;
    // Filled up by a pair that finds the plan, counted down by a pair of another kind that finds no free place. At zero that pair takes the place.
    uint8_t visitsInVainLeft { maxVisitsInVain };
};

// Where each field of one layout goes in an object of another, whichever of the fields a source holds and in whichever order: what the plans for all those orders have in common.
struct FieldMap {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FieldMap);

    static constexpr uint16_t isUnknown = 0;
    // The field has no key that a copy lists, or the copy leaves its name out.
    static constexpr uint16_t isLeftOut = 1;
    // There is more to the copy than a load and a store, with the value put into the form of the other slot between them: a plan does it.
    static constexpr uint16_t isNotServed = 2;
    // From here on: the index of the field in the layout of the target.
    static constexpr uint16_t firstField = 3;
    // Beside the index: the type of the source's field does not lie within the type of the target's, so each value is tested.
    static constexpr uint16_t isTested = 1 << 15;

    uint32_t excludedRunNumberPlusOne { 0 };
    Vector<uint16_t> entries;
};

// What a copy by a map makes of a target and a source with these order and shape words.
struct MappedCopy {
    // What is read, how an empty slot is told, and what becomes of the bits on their way.
    enum class MoveKind : uint8_t {
        WordUnlessZero,
        HalfWordUnlessZero,
        ByteUnlessZero,
        OptionalInt32,
        OptionalInt64,
        OptionalDouble,
        Word,
        HalfWord,
        QuarterWord,
        Byte,
        BoxedOptionalInt32,
        BoxedInt32,
        OptionalInt32AsDouble,
        Int32AsDouble,
        WideNarrowReference,
        // By readSlot() and tryWriteSlot().
        OtherForm,
        // And the value is asked whether the field of the target takes it.
        Tested,
    };
    // An offset counts from the cell, or from the Extension with the granules that it has in such a copy.
    struct Move {
        uint32_t from;
        uint32_t to;
        MoveKind kind;
        uint8_t widthInTarget;
        bool readsExtension;
        bool writesExtension;
        SlotRepresentation representationOfSource;
        SlotRepresentation representationOfTarget;
        // Of a tested move: the kinds of value, of ValueKind, that the field of the target takes whatever the value.
        uint8_t acceptedKinds;
        uint16_t fieldOfTarget;
    };
    enum class State : uint8_t { IsUnknown, IsServed, IsNotServed };

    bool isFor(uint64_t target, uint64_t source, uint32_t excludedRun) const { return state != State::IsUnknown && targetWords == target && sourceWords == source && excludedRunNumberPlusOne == excludedRun; }

    uint64_t targetWords { 0 };
    uint64_t sourceWords { 0 };
    uint64_t granuleBitsOfSource { 0 };
    uint64_t granuleBitsOfTargetBefore { 0 };
    uint64_t granuleBitsFilled { 0 };
    uint32_t excludedRunNumberPlusOne { 0 };
    uint32_t orderWordAfter { 0 };
    State state { State::IsUnknown };
    bool mayStoreCells { false };
    // In the order of the source's keys.
    Vector<Move> moves;
};

struct CopyDescription {
    String objects;
    uint64_t names { 0 };
    // What a plan is made for, and what a map for two layouts would be made for. Empty unless both objects are compiled.
    String structuresAndOrderWords;
    String layouts;
    // Of this thread, in user mode, when the description was complete. Zero where the system does not count them.
    uint64_t instructionsBefore { 0 };
};

class AssignmentPlanCache final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(AssignmentPlanCache);
public:
    explicit AssignmentPlanCache(State&);
    ~AssignmentPlanCache() final;

    // Five hundred plans in 256 sets left one plan in ten in a set of five or more, where each copy in turn made its plan anew.
    static constexpr unsigned numberOfSets = 1024;
    static constexpr unsigned waysPerSet = 4;

    ALWAYS_INLINE AssignmentPlan* planFor(VM&, Object& target, JSObject* extraProperties, const AssignmentSource&, Object::PlanCreation);
    ALWAYS_INLINE AssignmentPlan* existingPlanFor(Object& target, JSObject* extraProperties, const AssignmentSource&);
    // For a copy that defines the properties of the source on an ordinary object. All its stores are extraPropertyStores, into the target itself, which ends with the Structure extraPropertiesAfter.
    AssignmentPlan* planForOrdinaryTarget(VM&, JSObject* target, const AssignmentSource&, Object::PlanCreation);
    // Either source may be null: a value without properties of its own. Null for the first pair of its kind: to make a plan costs several copies.
    CopyOfTwoPlan* planForCopyOfTwo(VM&, JSGlobalObject*, const AssignmentSource* first, const AssignmentSource* second);
    ALWAYS_INLINE void didFillEmptySlots(AssignmentPlan&);
    // The plan of the last copy of its kind that has filled empty slots or run as before, whichever it was: the caller asks the plan.
    ALWAYS_INLINE AssignmentPlan* recentPlan(StructureID target, StructureID source, uint32_t sourceOrderWord, uint32_t excludedRunNumberPlusOne) { return m_recentPlans[recentIndex(target, source, sourceOrderWord, excludedRunNumberPlusOne)]; }
    ALWAYS_INLINE void didRunAsBefore(AssignmentPlan& plan) { didFillEmptySlots(plan); }
    // The place for what is known of such a copy, which may hold another copy's.
    ALWAYS_INLINE MappedCopy& mappedCopyAt(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne);
    // Null where the names that the copy leaves out have not been listed yet.
    FieldMap* fieldMapFor(const LayoutData& target, const LayoutData& source, uint32_t excludedRunNumberPlusOne);
    uint16_t describeMappedField(VM&, const LayoutData& target, const FieldData& from, uint32_t excludedRunNumberPlusOne);
    ALWAYS_INLINE const ExcludedNameList& excludedNames(VM&, const ExcludedRun&);
    void didWritePlan(AssignmentPlan& plan)
    {
        m_writes.recordAll();
        if (!m_writtenPlans.isEmpty() && m_writtenPlans.last() == &plan)
            return;
        if (m_writtenPlans.size() < maxNumberOfWrittenPlansListed) [[likely]]
            m_writtenPlans.append(&plan);
        else
            m_listsEveryWrittenPlan = false;
    }
    // A plan keeps no Structure alive: kept, they cost the large application 2.6 MB at the end of a session and saved it nothing.
    size_t removeDeadEntries(VM&, CollectionScope);

    // In a run with --aotCountsPath: which way each copy of properties went, into what and out of what.
    bool countsCopies() const { return !!m_copyCounts; }
    CopyDescription describeCopy(JSObject* target, JSValue source) const;
    void noteCopy(ASCIILiteral way, CopyDescription&&);
    void notePlanWithoutUse(ASCIILiteral reason);
    void dumpCopies(PrintStream&) const;

private:
    // A component that takes its properties apart meets them in dozens of sets and orders of keys, each with a plan of its own, and meets them all again at each turn.
    struct Set {
        std::array<std::unique_ptr<AssignmentPlan>, waysPerSet> ways;
        uint8_t nextVictim { 0 };
    };
    using Sets = std::array<Set, numberOfSets>;
    struct CopyCounts;
    static constexpr unsigned numberOfCopiesRemembered = 8192;
    using RememberedCopies = WTF::BitSet<numberOfCopiesRemembered>;

    static uint32_t hashFor(StructureID target, StructureID extraProperties, const AssignmentSource& source)
    {
        uint64_t objects = static_cast<uint64_t>(target.bits()) << 32 | source.object->structureID().bits();
        uint64_t propertyStorages = static_cast<uint64_t>(extraProperties.bits()) << 32 | source.propertyStorageStructureID().bits();
        uint64_t lists = static_cast<uint64_t>(source.orderWord) << 32;
        if (!source.excludedNames.empty())
            lists ^= std::bit_cast<uintptr_t>(source.excludedNames.front()) + source.excludedNames.size();
        return WTF::intHash(objects ^ std::rotl(propertyStorages, 23) ^ std::rotl(lists, 45));
    }
    ALWAYS_INLINE AssignmentPlan* find(StructureID target, StructureID extraProperties, const AssignmentSource&, uint32_t hash);
    struct ClaimedPlan {
        AssignmentPlan* plan { nullptr };
        unsigned index { 0 };
    };
    ClaimedPlan claimPlan(VM&, StructureID target, JSObject* extraProperties, const AssignmentSource&, uint32_t hash, Object::PlanCreation);
    AssignmentPlan* makePlan(VM&, Object& target, JSObject* extraProperties, const AssignmentSource&, uint32_t hash, Object::PlanCreation);
    void fillPlan(VM&, AssignmentPlan&, unsigned index, Object& target, JSObject* extraProperties, const AssignmentSource&);
    void fillPlanForOrdinaryTarget(VM&, AssignmentPlan&, JSObject* target, const AssignmentSource&);
    void fillPlanForCopyOfTwo(VM&, CopyOfTwoPlan&, JSGlobalObject*, const AssignmentSource* first, const AssignmentSource* second);
    bool isSecondCopy(uint32_t hash);
    AssignmentPlan* planAt(uint32_t index) const { return m_sets ? (*m_sets)[index / waysPerSet].ways[index % waysPerSet].get() : nullptr; }
    const ExcludedNameList& addExcludedNames(VM&, const ExcludedRun&);
    const ExcludedNameList* excludedListAt(std::span<UniquedStringImpl* const>) const;

    void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const final;

    static constexpr unsigned numberOfRecentPlans = 512;
    static unsigned recentIndex(StructureID target, StructureID source, uint32_t sourceOrderWord, uint32_t excludedRunNumberPlusOne)
    {
        return (target.bits() * 0x9e3779b1u ^ source.bits() * 0x85ebca6bu ^ sourceOrderWord * 0xc2b2ae35u ^ excludedRunNumberPlusOne * 0x27d4eb2fu) >> 23;
    }
    static_assert(numberOfRecentPlans == 1u << (32 - 23));
    static constexpr unsigned minNumberOfPlacesForMappedCopies = 16;
    static constexpr unsigned maxNumberOfPlacesForMappedCopies = 4096;
    static constexpr unsigned maxProbesForMappedCopy = 8;
    static uint64_t hashOfMappedCopy(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne)
    {
        return (targetWords * 0x9e3779b97f4a7c15ull ^ std::rotl(sourceWords, 29) * 0xc2b2ae3d27d4eb4full) + excludedRunNumberPlusOne * 0x27d4eb2fu;
    }
    std::unique_ptr<MappedCopy>& placeForMappedCopy(uint64_t hash, unsigned probe) { return m_mappedCopies[((hash >> m_shiftForMappedCopies) + probe) & (m_mappedCopies.size() - 1)]; }
    MappedCopy& placeForNewMappedCopy(uint64_t hash);
    void growMappedCopies();

    State& m_state;
    ChainDependentOwnerID m_chainDependentOwner;
    std::unique_ptr<Sets> m_sets;
    // A plan stays where it is for as long as the cache does. One that has been made anew for another copy, or emptied, does not pass for the plan it was.
    std::array<AssignmentPlan*, numberOfRecentPlans> m_recentPlans { };
    // A plan is in one of the first places from its hash on, and keeps its place for as long as pairs ask for it.
    static constexpr unsigned numberOfCopiesOfTwo = 64;
    static constexpr unsigned numberOfPlacesForCopyOfTwo = 4;
    std::array<std::unique_ptr<CopyOfTwoPlan>, numberOfCopiesOfTwo> m_copiesOfTwo;
    // No places, or a power of two of them, which doubles when half of them are taken.
    Vector<std::unique_ptr<MappedCopy>> m_mappedCopies;
    unsigned m_shiftForMappedCopies { 0 };
    unsigned m_numberOfMappedCopiesTaken { 0 };
    unsigned m_nextMappedCopyReplaced { 0 };
    // By the indexes of the two layouts. Most pairs have one map: a rest pattern has one for each list of names that it leaves out.
    UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<FieldMap>, 1>> m_fieldMaps;
    // A bit for a hash, which no other copy takes away: copies that come in turns all get to their second one. A bit that two copies share by chance makes a plan one copy early.
    std::unique_ptr<RememberedCopies> m_copiesWithoutPlan;
    unsigned m_numberOfCopiesWithoutPlan { 0 };
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<ExcludedNameList>> m_excludedListsByRun;
    UncheckedKeyHashMap<UniquedStringImpl* const*, const ExcludedNameList*> m_excludedListsByAddress;
    RememberedSlotSet m_writes;
    static constexpr unsigned maxNumberOfWrittenPlansListed = 1024;
    Vector<AssignmentPlan*, 16> m_writtenPlans;
    bool m_listsEveryWrittenPlan { true };
    std::unique_ptr<CopyCounts> m_copyCounts;
};

ALWAYS_INLINE AssignmentPlan* AssignmentPlanCache::find(StructureID targetID, StructureID extraPropertiesID, const AssignmentSource& source, uint32_t hash)
{
    if (!m_sets) [[unlikely]]
        return nullptr;
    for (auto& plan : (*m_sets)[hash % numberOfSets].ways) {
        if (plan && plan->isFor(targetID, extraPropertiesID, source))
            return plan.get();
    }
    return nullptr;
}

ALWAYS_INLINE AssignmentPlan* AssignmentPlanCache::planFor(VM& vm, Object& target, JSObject* extraProperties, const AssignmentSource& source, Object::PlanCreation creation)
{
    StructureID targetID = target.structureID();
    StructureID extraPropertiesID = extraProperties ? extraProperties->structureID() : StructureID();
    uint32_t hash = hashFor(targetID, extraPropertiesID, source);
    if (AssignmentPlan* plan = find(targetID, extraPropertiesID, source, hash)) [[likely]]
        return plan;
    return makePlan(vm, target, extraProperties, source, hash, creation);
}

ALWAYS_INLINE AssignmentPlan* AssignmentPlanCache::existingPlanFor(Object& target, JSObject* extraProperties, const AssignmentSource& source)
{
    StructureID targetID = target.structureID();
    StructureID extraPropertiesID = extraProperties ? extraProperties->structureID() : StructureID();
    return find(targetID, extraPropertiesID, source, hashFor(targetID, extraPropertiesID, source));
}

ALWAYS_INLINE MappedCopy& AssignmentPlanCache::mappedCopyAt(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne)
{
    uint64_t hash = hashOfMappedCopy(targetWords, sourceWords, excludedRunNumberPlusOne);
    if (!m_mappedCopies.isEmpty()) [[likely]] {
        for (unsigned probe = 0; probe < maxProbesForMappedCopy; ++probe) {
            MappedCopy* copy = placeForMappedCopy(hash, probe).get();
            if (!copy)
                break;
            if (copy->isFor(targetWords, sourceWords, excludedRunNumberPlusOne))
                return *copy;
        }
    }
    return placeForNewMappedCopy(hash);
}

ALWAYS_INLINE void AssignmentPlanCache::didFillEmptySlots(AssignmentPlan& plan)
{
    if (!plan.excludedNames.isEmpty())
        return;
    m_recentPlans[recentIndex(plan.target, plan.source, plan.sourceOrderWord, plan.excludedList ? plan.excludedList->runNumberPlusOne() : 0)] = &plan;
}

// The whole of a copy between two compiled objects that needs no plan, or does what an earlier one by a plan has done. Otherwise it does nothing.
enum class RepeatedCopy : uint8_t { IsNotDone, ByWordsOrMoves, ByMap, ByMapForTheFirstTime, AsThePlanDidBefore };
RepeatedCopy tryRepeatAssignment(VM&, State&, JSObject* target, JSValue source, uint32_t excludedRunNumberPlusOne);
// Defines plain properties one behind the other on an object without an integrity level, under any names but indexes: the way of a copy that has no plan.
bool definePropertiesInOrder(VM&, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values);
// As Object::tryAssignPropertiesOf for a copy that defines each property, as a spread and a rest pattern do. A definition asks no prototype.
bool tryDefinePropertiesOf(VM&, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames, Object::PlanCreation);
// The same for an ordinary object that takes plain properties directly (AOTPropertyCopy.h): its new Structure in one step, its storage grown once, a load and a store a property.
bool tryDefinePropertiesOnOrdinaryObject(VM&, State&, JSObject* target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames, Object::PlanCreation);
// The whole of `{ ...first, ...second }` by a plan, which runs no code of the program. Null, with nothing done that anybody sees, where the two have to be copied one behind the other.
JSFinalObject* tryCopyTwoObjects(VM&, State&, JSGlobalObject*, JSValue first, JSValue second);
// As tryDefinePropertiesOf where an earlier copy has made the plan.
enum class PlannedAssignment : uint8_t { HasNoPlan, IsDone, GaveUp };
PlannedAssignment tryAssignPropertiesByExistingPlan(VM&, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames);

ALWAYS_INLINE const ExcludedNameList& AssignmentPlanCache::excludedNames(VM& vm, const ExcludedRun& run)
{
    ASSERT(run.numberPlusOne);
    if (const ExcludedNameList* list = m_excludedListsByRun.get(run.numberPlusOne)) [[likely]]
        return *list;
    return addExcludedNames(vm, run);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
