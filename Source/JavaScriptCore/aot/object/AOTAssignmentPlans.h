/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTFieldMoves.h"
#include "AOTMappedCopy.h"
#include "AOTObject.h"
#include "AOTProgramStrings.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTThreshold.h"
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
#include <wtf/text/StringHash.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct State;

class ExcludedNameList {
    WTF_MAKE_NONCOPYABLE(ExcludedNameList);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ExcludedNameList);
public:
    ExcludedNameList(Vector<UniquedStringImpl*>&&, uint32_t runNumberPlusOne);

    std::span<UniquedStringImpl* const> names() const { return m_names.span(); }
    uint32_t runNumberPlusOne() const { return m_runNumberPlusOne; }
    bool contains(UniquedStringImpl* name) const { return m_names.size() <= maxNumberOfNamesCompared ? m_names.contains(name) : m_longList.contains(name); }

private:
    AOT_CLASS_THRESHOLD(size_t, maxNumberOfNamesCompared, 8);

    Vector<UniquedStringImpl*> m_names;
    UncheckedKeyHashSet<UniquedStringImpl*> m_longList;
    uint32_t m_runNumberPlusOne;
};

// A field that holds a literal-string word usually holds the same word as in the previous copy. The strings for such words live as long as the instance.
template<typename Store>
struct LiteralStringsReadByStore {
    // Null if the word is not a literal-string word.
    ALWAYS_INLINE JSString* stringForWordInSlot(uint64_t word) const
    {
        if (word == store.lastLiteralStringWord) [[likely]]
            return store.lastLiteralString;
        if (!strings || !isLiteralStringWord(word))
            return nullptr;
        store.lastLiteralString = strings->stringForLiteralStringWord(word);
        store.lastLiteralStringWord = static_cast<uint32_t>(word);
        return store.lastLiteralString;
    }

    const ProgramStrings* strings;
    const Store& store;
};

struct AssignmentSource {
    ALWAYS_INLINE JSValue read(const auto& store) const
    {
        if (!store.from)
            return propertyStorage->getDirect(store.sourceOffset);
        if (!store.from->isOutOfLine())
            return readInline(store);
        SlotRepresentation representation = store.from->slot.representation();
        const void* slot = extension ? extension->slotOrNull(store.from->slot.offset) : nullptr;
        if (slot && representation != SlotRepresentation::DeferredClosure) [[likely]] {
            if (JSValue value = readSlot(representation, slot, *strings)) [[likely]]
                return value;
        }
        return compiledObject->read(*store.from);
    }

    template<typename Store>
    ALWAYS_INLINE JSValue readInline(const Store& store) const
    {
        LiteralStringsReadByStore<Store> literalStrings { strings, store };
        return compiledObject->readInline(*store.from, &literalStrings);
    }

    StructureID propertyStorageStructureID() const { return propertyStorage ? propertyStorage->structureID() : StructureID(); }

    JSObject* object { nullptr };
    const Object* compiledObject { nullptr };
    JSObject* propertyStorage { nullptr };
    uint32_t orderWord { 0 };
    // The names of an ExcludedNameList, or a list of the caller's that lasts as long as the copy.
    std::span<UniquedStringImpl* const> excludedNames { };
    bool defines { false };
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
        OptionSet<ValueKind> acceptedKinds;
        // The int32s that the field takes. A field in a narrow slot takes all but a few, so Int32 is not among acceptedKinds.
        int32_t leastInt32 { 0 };
        int32_t greatestInt32 { -1 };
        // The slot holds an integer in two's complement, and each of those int32s fits it.
        bool writesInt32AsItIs { false };
        bool sourceAlwaysFits { false };
        bool sourceFitsUnlessUndefinedOrNull { false };
        bool isDirect { false };
        bool copiesBits { false };
        bool copiesBitsUntested { false };
        bool holdsCell { false };
        bool hasAbsent { false };
        // The mark of the target's cell answers for the field, and whoever stores undefined or null there sets the mark. A run does not.
        bool leavesUndefinedAndNullToOtherWay { false };
        uint8_t width { 0 };
        uint64_t absentBits { 0 };
        mutable uint32_t lastLiteralStringWord { 0 };
        mutable JSString* lastLiteralString { nullptr };
    };

    struct ExtraPropertyStore {
        const FieldData* from { nullptr };
        PropertyOffset sourceOffset { invalidOffset };
        PropertyOffset offset { invalidOffset };
        // extraPropertiesAfter keeps it.
        UniquedStringImpl* name { nullptr };
        bool addsName { false };
        mutable uint32_t lastLiteralStringWord { 0 };
        mutable JSString* lastLiteralString { nullptr };
    };
    // In storesInSourceOrder: the index of an ExtraPropertyStore. Without it, of a FieldStore.
    static constexpr uint16_t isExtraPropertyStore = 1 << 15;

    bool readsFrom(StructureID structure, uint32_t orderWord, const JSObject* propertyStorageOrNull) const
    {
        return source == structure && sourceOrderWord == orderWord && sourcePropertyStorage == (propertyStorageOrNull ? propertyStorageOrNull->structureID() : StructureID());
    }

    bool readsFrom(const Object& from) const
    {
        if (sourceStructureNamesPropertyStorage)
            return source == from.structureID() && sourceOrderWord == from.orderWord();
        return readsFrom(from.structureID(), from.orderWord(), from.extraProperties());
    }

    bool leavesOutRun(uint32_t numberPlusOne) const { return (excludedList ? excludedList->runNumberPlusOne() : 0) == numberPlusOne && excludedNames.isEmpty(); }

    bool isFor(StructureID targetID, StructureID extraPropertiesID, const AssignmentSource& from) const
    {
        return target == targetID && extraPropertiesBefore == extraPropertiesID && readsFrom(from.object->structureID(), from.orderWord, from.propertyStorage)
            && (!servesDefinitionsOnly || from.defines) && leavesOut(from.excludedNames);
    }

    bool fillsEmptySlotsOf(const Object& targetObject, const Object& sourceObject, uint32_t excludedRunNumberPlusOne) const
    {
        return orderWordBeforeFillingAll == targetObject.orderWord() && target == targetObject.structureID()
            && readsFrom(sourceObject) && leavesOutRun(excludedRunNumberPlusOne);
    }

    bool runsAsBeforeOn(const Object& targetObject, const AssignmentSource& from, uint32_t excludedRunNumberPlusOne) const
    {
        return isUsable && orderWordBeforeRun == targetObject.orderWord() && target == targetObject.structureID() && extraPropertiesBefore == StructureID()
            && readsFrom(from.object->structureID(), from.orderWord, from.propertyStorage) && leavesOutRun(excludedRunNumberPlusOne);
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
    const ExcludedNameList* excludedList { nullptr };
    Vector<RefPtr<UniquedStringImpl>, 2> excludedNames;
    bool isUsable { false };
    bool sourceStructureNamesPropertyStorage { false };
    Vector<uint16_t> storesInSourceOrder;
    bool servesDefinitionsOnly { false };
    bool addsExtraProperties { false };
    bool storesNameBehindSymbol { false };
    // Every store copies a field to a field whose type always accepts it.
    bool storesOnlyFieldsThatFit { false };
    bool mayStoreCells { false };
    bool readsOutOfLineFields { false };
    // In a row: runs in which a value has left its slot.
    uint8_t numberOfRunsInVain { 0 };
    std::optional<uint32_t> orderWordBeforeFillingAll { };
    uint32_t orderWordAfterFillingAll { 0 };
    std::optional<uint32_t> orderWordBeforeRun { };
    uint32_t orderWordAfterRun { 0 };
    uint64_t granuleBitsFilled { 0 };
    Structure* extraPropertiesAfter { nullptr };
    CompiledStructure* targetAfter { nullptr };
    Vector<FieldStore> fieldStores;
    Vector<ExtraPropertyStore> extraPropertyStores;
};

struct CopyOfTwoPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CopyOfTwoPlan);

    struct Source {
        bool isFor(const AssignmentSource* from) const
        {
            if (!from)
                return !structure;
            return structure == from->object->structureID() && propertyStorage == from->propertyStorageStructureID() && orderWord == from->orderWord;
        }

        StructureID structure;
        StructureID propertyStorage;
        uint32_t orderWord { 0 };
        FieldMovePlan fields;
        FieldMovePlan properties;
    };

    bool isFor(const AssignmentSource* first, const AssignmentSource* second) const { return sources[0].isFor(first) && sources[1].isFor(second); }

    std::array<Source, 2> sources;
    Structure* structure { nullptr };
    AOT_CLASS_THRESHOLD(uint8_t, maxVisitsInVain, 15);
    uint8_t visitsInVainLeft { maxVisitsInVain };
};

struct FieldMap {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FieldMap);

    static constexpr uint16_t isUnknown = 0;
    static constexpr uint16_t isLeftOut = 1;
    static constexpr uint16_t isNotServed = 2;
    static constexpr uint16_t firstField = 3;
    static constexpr uint16_t testsUndefinedAndNull = 1 << 15;

    // What is the same in every copy that moves the field.
    struct Entry {
        uint16_t fieldOfTarget() const { return (state & ~testsUndefinedAndNull) - firstField; }

        uint16_t state { isUnknown };
        MappedCopy::MoveKind kind { MappedCopy::MoveKind::Tested };
        uint8_t acceptedKinds { 0 };
    };

    uint32_t excludedRunNumberPlusOne { 0 };
    Vector<Entry> entries;
};

class AssignmentPlanCache final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(AssignmentPlanCache);
public:
    explicit AssignmentPlanCache(State&);
    ~AssignmentPlanCache() final;

    static constexpr unsigned numberOfSets = 1024;
    static constexpr unsigned waysPerSet = 4;

    // The target is a compiled object, or an ordinary object on which the copy defines the properties of the source: all stores of such a plan are extraPropertyStores, into the target itself, which ends with the Structure extraPropertiesAfter.
    ALWAYS_INLINE AssignmentPlan* planFor(VM&, JSObject* target, JSObject* extraProperties, const AssignmentSource&);
    // Either source may be null: a value without properties of its own. Null for the first pair of its kind: to make a plan costs several copies.
    CopyOfTwoPlan* planForCopyOfTwo(VM&, JSGlobalObject*, const AssignmentSource* first, const AssignmentSource* second);
    std::unique_ptr<FieldMovePlan> createPlanForOrdinaryCopy(VM&, JSGlobalObject*, JSObject* source, const ExcludedRun&);
    ALWAYS_INLINE void didRunAsBefore(AssignmentPlan&);
    ALWAYS_INLINE AssignmentPlan* recentPlan(StructureID target, StructureID source, uint32_t sourceOrderWord, uint32_t excludedRunNumberPlusOne) { return m_recentPlans[recentIndex(target, source, sourceOrderWord, excludedRunNumberPlusOne)]; }
    ALWAYS_INLINE MappedCopy& mappedCopyAt(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne);
    // Null where the names that the copy leaves out have not been listed yet.
    FieldMap* fieldMapFor(const LayoutData& target, const LayoutData& source, uint32_t excludedRunNumberPlusOne);
    FieldMap::Entry describeMappedField(VM&, const LayoutData& target, const FieldData& from, uint32_t excludedRunNumberPlusOne);
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
    // A plan keeps no Structure alive.
    size_t removeDeadEntries(VM&, CollectionScope);

    // In a run with --aotCountsPath: how many copies of properties went which way, and why plans were of no use.
    bool countsCopies() const { return !!m_copyCounts; }
    void noteCopy(ASCIILiteral way)
    {
        if (m_copyCounts) [[unlikely]]
            note("copies:"_s, way);
    }
    void notePlanWithoutUse(ASCIILiteral reason) { note("plans made without use:"_s, reason); }
    void dumpCopies(PrintStream&) const;

private:
    struct Set {
        std::array<std::unique_ptr<AssignmentPlan>, waysPerSet> ways;
        uint8_t nextVictim { 0 };
    };
    using Sets = std::array<Set, numberOfSets>;
    void note(ASCIILiteral kind, ASCIILiteral what);
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
    AssignmentPlan* makePlan(VM&, JSObject* target, JSObject* extraProperties, const AssignmentSource&, uint32_t hash);
    void fillPlan(VM&, AssignmentPlan&, unsigned index, JSObject* target, JSObject* extraProperties, const AssignmentSource&);
    void fillPlanForCopyOfTwo(VM&, CopyOfTwoPlan&, JSGlobalObject*, const AssignmentSource* first, const AssignmentSource* second, bool isOrdinaryCopy);
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
    AOT_CLASS_THRESHOLD(unsigned, minNumberOfPlacesForMappedCopies, 16);
    AOT_CLASS_THRESHOLD(unsigned, maxNumberOfPlacesForMappedCopies, 4096);
    AOT_CLASS_THRESHOLD(unsigned, maxProbesForMappedCopy, 8);
    static uint64_t hashOfMappedCopy(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne)
    {
        return (targetWords * 0x9e3779b97f4a7c15ull ^ std::rotl(sourceWords, 29) * 0xc2b2ae3d27d4eb4full) + excludedRunNumberPlusOne * 0x27d4eb2fu;
    }
    std::unique_ptr<MappedCopy>& placeForMappedCopy(uint64_t hash, unsigned probe) { return m_mappedCopies[((hash >> m_shiftForMappedCopies) + probe) & (m_mappedCopies.size() - 1)]; }
    MappedCopy& placeForNewMappedCopy(uint64_t hash);
    void growMappedCopies();

    State& m_state;
    ChainDependentOwnerID m_chainDependentOwner;
    MadeInArena<Sets> m_sets;
    // A plan stays where it is for as long as the cache does. One that has been made anew for another copy, or emptied, does not pass for the plan it was.
    std::array<AssignmentPlan*, numberOfRecentPlans> m_recentPlans { };
    static constexpr unsigned numberOfCopiesOfTwo = 64;
    static constexpr unsigned numberOfPlacesForCopyOfTwo = 4;
    std::array<std::unique_ptr<CopyOfTwoPlan>, numberOfCopiesOfTwo> m_copiesOfTwo;
    Vector<std::unique_ptr<MappedCopy>> m_mappedCopies;
    unsigned m_shiftForMappedCopies { 0 };
    unsigned m_numberOfMappedCopiesTaken { 0 };
    unsigned m_nextMappedCopyReplaced { 0 };
    UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<FieldMap>, 1>> m_fieldMaps;
    std::unique_ptr<RememberedCopies> m_copiesWithoutPlan;
    unsigned m_numberOfCopiesWithoutPlan { 0 };
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<ExcludedNameList>> m_excludedListsByRun;
    UncheckedKeyHashMap<UniquedStringImpl* const*, const ExcludedNameList*> m_excludedListsByAddress;
    RememberedSlotSet m_writes;
    static constexpr unsigned maxNumberOfWrittenPlansListed = 1024;
    Vector<AssignmentPlan*, 16> m_writtenPlans;
    bool m_listsEveryWrittenPlan { true };
    std::unique_ptr<UncheckedKeyHashMap<String, uint32_t>> m_copyCounts;
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

ALWAYS_INLINE AssignmentPlan* AssignmentPlanCache::planFor(VM& vm, JSObject* target, JSObject* extraProperties, const AssignmentSource& source)
{
    StructureID targetID = target->structureID();
    StructureID extraPropertiesID = extraProperties ? extraProperties->structureID() : StructureID();
    uint32_t hash = hashFor(targetID, extraPropertiesID, source);
    if (AssignmentPlan* plan = find(targetID, extraPropertiesID, source, hash)) [[likely]]
        return plan;
    return makePlan(vm, target, extraProperties, source, hash);
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

ALWAYS_INLINE void AssignmentPlanCache::didRunAsBefore(AssignmentPlan& plan)
{
    if (!plan.excludedNames.isEmpty())
        return;
    m_recentPlans[recentIndex(plan.target, plan.source, plan.sourceOrderWord, plan.excludedList ? plan.excludedList->runNumberPlusOne() : 0)] = &plan;
}

enum class RepeatedCopy : uint8_t { IsNotDone, ByWordsOrMoves, ByMap, ByMapForTheFirstTime, AsThePlanDidBefore };
RepeatedCopy tryRepeatAssignment(VM&, State&, JSObject* target, JSValue source, uint32_t excludedRunNumberPlusOne);
MappedCopy::MoveKind moveKindFor(SlotRepresentation from, SlotRepresentation to, SlotFit);
// For a copy that takes no frame, whose Structures are known.
void describeMovesForStub(MappedCopy&);
bool clearFieldsMovedInVain(const MappedCopy&, const MappedCopy::Move* end, JSCell* target);
// Defines plain properties one behind the other on an object without an integrity level, under any names but indexes: the way of a copy that has no plan.
bool definePropertiesInOrder(VM&, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values);
// The same for stores to an object on which every store writes or creates an own data property: a property that exists keeps its attributes.
bool storePropertiesInOrder(VM&, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values);
bool tryDefinePropertiesOf(VM&, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames);
bool tryDefinePropertiesOnOrdinaryObject(VM&, State&, JSObject* target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames);
// The whole of `{ ...first, ...second }` by a plan, which runs no code of the program. Null, with nothing done that anybody sees, where the two have to be copied one behind the other.
JSFinalObject* tryCopyTwoObjects(VM&, State&, JSGlobalObject*, JSValue first, JSValue second);
ALWAYS_INLINE const ExcludedNameList& AssignmentPlanCache::excludedNames(VM& vm, const ExcludedRun& run)
{
    ASSERT(run.numberPlusOne);
    if (const ExcludedNameList* list = m_excludedListsByRun.get(run.numberPlusOne)) [[likely]]
        return *list;
    return addExcludedNames(vm, run);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
