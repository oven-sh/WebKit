/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassGuards.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTCheckedSpan.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTEffectsNet.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectPrototype.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct BitOfClassGuard {
    uint8_t* byte { nullptr };
    uint8_t mask { 0 };
};

enum class StateOfClassHolder : uint8_t { AsDeclared, MayStillBeDefined, Differs };

enum class NameOnPrototypeHolder : uint8_t { Found, AbsentUntilReported, AbsentForNow };

struct NamesThatDifferOnClassHolders {
    Vector<uint32_t, 4> numbers;
    Vector<uint32_t, 4> numbersOfPlainValues;
    bool someHasNoNumber { false };
    bool someSeesAccesses { false };

    void add(std::optional<uint32_t> key)
    {
        if (key)
            numbers.append(*key);
        else
            someHasNoNumber = true;
    }
};

} // anonymous namespace

static_assert(numberOfClassSides == 2);

static uint32_t encodeSideOfClassHolder(const LayoutData& classLayout, ClassSide side)
{
    return classLayout.index << 1 | static_cast<uint32_t>(side);
}

static std::span<const MethodData> methodsInClassRecord(const LayoutData& classLayout, ClassSide side)
{
    switch (side) {
    case ClassSide::Prototype:
        return classLayout.methods();
    case ClassSide::Static:
        return classLayout.staticMethods();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isNamedInClassRecord(const MethodData& method, uint32_t key)
{
    return !(method.flags & methodHasComputedName) && method.name == key;
}

bool declaresMember(const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    bool isDeclared = false;
    for (const MethodData& method : methodsInClassRecord(classLayout, side))
        isDeclared |= isNamedInClassRecord(method, key);
    return isDeclared;
}

static BitOfClassGuard bitOfClassGuardWithNumber(const State& state, uint32_t bit)
{
    if (bit == noClassGuardBit)
        return { };
    RELEASE_ASSERT(byteOffsetOfClassGuardBit(bit) < state.tables.instanceSizes.sizeOfStatics);
    return { state.instance.statics() + byteOffsetOfClassGuardBit(bit), maskOfClassGuardBit(bit) };
}

static BitOfClassGuard bitOfClassGuard(const State& state, const LayoutData& classLayout, ClassSide side)
{
    return bitOfClassGuardWithNumber(state, classLayout.classData().guardBit[static_cast<size_t>(side)]);
}

static std::span<const NamedClassGuardData> guardsOfNamesInClassRecord(const ProgramTables& tables, const LayoutData& classLayout, ClassSide side)
{
    uint32_t offset = classLayout.classData().namedGuardsOffset[static_cast<size_t>(side)];
    if (!offset)
        return { };
    const uint32_t* list = tables.at<uint32_t>(offset);
    return { reinterpret_cast<const NamedClassGuardData*>(list + 1), list[0] };
}

static auto guardsOfNameInClassRecord(const ProgramTables& tables, const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    return std::ranges::equal_range(guardsOfNamesInClassRecord(tables, classLayout, side), key, { }, &NamedClassGuardData::name);
}

static void setPagesOfMethodTableWritableForClassGuards(State& state, size_t firstPage, size_t numberOfPages, bool isWritable)
{
    OSAllocator::protect(reinterpret_cast<uint8_t*>(state.instance.methodTable().data()) + firstPage * pageSize(), numberOfPages * pageSize(), true, isWritable);
}

void ClassTable::MethodTableUpdate::willStoreEntry(size_t index)
{
    size_t page = index * sizeof(uint32_t) / pageSize();
    if (m_classes.m_writablePagesOfMethodTable.set(page))
        return;
    countEvent(*m_classes.m_state.instance.vm, RuntimeCounterID::MethodTablePagesMadeWritable);
    setPagesOfMethodTableWritableForClassGuards(m_classes.m_state, page, 1, true);
}

ClassTable::MethodTableUpdate::~MethodTableUpdate()
{
    if (--m_classes.m_numberOfOpenMethodTableUpdates)
        return;
    BitVector& pages = m_classes.m_writablePagesOfMethodTable;
    for (size_t first = pages.findBit(0, true); first < pages.size();) {
        size_t end = pages.findBit(first, false);
        setPagesOfMethodTableWritableForClassGuards(m_classes.m_state, first, end - first, false);
        first = pages.findBit(end, true);
    }
    pages.clearAll();
}

static constexpr Typed::CountInEffectsNet countOfChangeOfClassGuard(bool holds)
{
    return holds ? Typed::CountInEffectsNet::MemberGuardsArmedOrBegun : Typed::CountInEffectsNet::MemberGuardsEnded;
}

template<typename Dependents>
static void repointMethodTableEntries(State& state, Dependents&& dependents, bool guardHolds)
{
    const Image& image = *state.instance.program;
    std::span<const uint32_t> compiled = image.methodTable();
    std::span<const uint32_t> lookups = image.methodLookups();
    std::span<uint32_t> table = state.instance.methodTable();
    ClassTable::MethodTableUpdate update(state.objects->classes);
    for (const MethodTableDependentData& dependent : dependents) {
        size_t index = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&state.instance) + static_cast<int32_t>(dependent.distanceToEntry)) - table.data();
        uint32_t entry = guardHolds ? checkedAt(compiled, index) : checkedAt(lookups, dependent.selector);
        uint32_t& stored = checkedAt(table, index);
        if (stored == entry)
            continue;
        update.willStoreEntry(index);
        stored = entry;
        state.countWriteForEffectsNet(countOfChangeOfClassGuard(guardHolds));
    }
}

template<typename Row>
static std::span<const Row> rowsFrom(std::span<const Row> rows, uint32_t numberOfBit)
{
    return { std::ranges::lower_bound(rows, numberOfBit, { }, &Row::guardBit), rows.end() };
}

template<typename Row>
static std::span<const Row> rowsThrough(std::span<const Row> rows, uint32_t numberOfBit)
{
    if (rows.empty() || rows.front().guardBit > numberOfBit)
        return { };
    return { rows.begin(), std::ranges::upper_bound(rows, numberOfBit, { }, &Row::guardBit) };
}

struct ClassGuardRows {
    ClassGuardRows() = default;
    ClassGuardRows(const State& state, uint32_t firstBit, uint32_t lastBit)
        : firstBit(firstBit)
        , lastBit(lastBit)
        , counts(rowsThrough(rowsFrom(state.tables.classGuardCounts(), firstBit), lastBit))
        , methodTableDependents(rowsThrough(rowsFrom(state.tables.methodTableDependents(), firstBit), lastBit))
    {
    }

    uint32_t firstBit { 0 };
    uint32_t lastBit { 0 };
    std::span<const ClassGuardCountData> counts;
    std::span<const MethodTableDependentData> methodTableDependents;
};

static ClassGuardRows findClassGuardRows(const State& state, std::span<const NamedClassGuardData> guards, uint32_t sideBit = noClassGuardBit)
{
    bool hasSideBit = sideBit != noClassGuardBit;
    if (guards.empty() && !hasSideBit)
        return { };
    uint32_t firstBit = hasSideBit ? sideBit : UINT32_MAX;
    uint32_t lastBit = hasSideBit ? sideBit : 0;
    for (const NamedClassGuardData& guard : guards) {
        firstBit = std::min(firstBit, guard.numberOfBit());
        lastBit = std::max(lastBit, guard.numberOfBit());
    }
    return { state, firstBit, lastBit };
}

class ClassGuardSet {
public:
    explicit ClassGuardSet(const ClassGuardRows& rows)
        : m_firstByte(byteOffsetOfClassGuardBit(rows.firstBit))
    {
        m_words.fill(0, (byteOffsetOfClassGuardBit(rows.lastBit) - m_firstByte) / bitsPerWord + 1);
    }

    bool isEmpty() const { return m_isEmpty; }
    ALWAYS_INLINE bool contains(uint32_t numberOfBit) const
    {
        uint32_t place = byteOffsetOfClassGuardBit(numberOfBit) - m_firstByte;
        return m_words[place / bitsPerWord] >> (place % bitsPerWord) & 1;
    }
    ALWAYS_INLINE void add(uint32_t numberOfBit)
    {
        uint32_t place = byteOffsetOfClassGuardBit(numberOfBit) - m_firstByte;
        m_words[place / bitsPerWord] |= 1ull << (place % bitsPerWord);
        m_isEmpty = false;
    }

private:
    static constexpr uint32_t bitsPerWord = 64;

    uint32_t m_firstByte;
    bool m_isEmpty { true };
    Vector<uint64_t, 8> m_words;
};

static ALWAYS_INLINE void countChangeOfClassGuard(State& state, const ClassGuardCountData& count, bool holds)
{
    RELEASE_ASSERT(static_cast<uint64_t>(count.offsetOfCount) + sizeof(uint32_t) <= state.tables.instanceSizes.sizeOfStatics);
    uint32_t& numberOfGuardsThatDoNotHold = *reinterpret_cast<uint32_t*>(state.instance.statics() + count.offsetOfCount);
    RELEASE_ASSERT(!holds || numberOfGuardsThatDoNotHold);
    if (holds)
        --numberOfGuardsThatDoNotHold;
    else
        ++numberOfGuardsThatDoNotHold;
    state.countWriteForEffectsNet(countOfChangeOfClassGuard(holds));
}

static void countChangeOfClassGuard(State& state, std::span<const ClassGuardCountData> counts, uint32_t numberOfBit, bool holds)
{
    for (const ClassGuardCountData& count : counts) {
        if (count.guardBit != numberOfBit)
            return;
        countChangeOfClassGuard(state, count, holds);
    }
}

static void countChangeOfClassGuards(State& state, std::span<const ClassGuardCountData> counts, const ClassGuardSet& changed, bool holds)
{
    if (changed.isEmpty())
        return;
    for (const ClassGuardCountData& count : counts) {
        if (changed.contains(count.guardBit))
            countChangeOfClassGuard(state, count, holds);
    }
}

class ClassGuardWriter {
public:
    ClassGuardWriter(State& state, const LayoutData& classLayout, ClassSide side, const ClassGuardRows& rows)
        : m_state(state)
        , m_rows(rows)
        , m_isInverted(classLayout.classData().bitsAreClearWhileGuardsHold(side))
        , m_usesMethodTable(!state.objects->classes.hasInstanceBelowOtherPrototype(classLayout))
    {
        if (side != ClassSide::Prototype || !classLayout.classData().guardsOfNamesAwaitBeginning)
            return;
        m_namesHaveBegun = state.objects->classes.guardsHaveBegun(classLayout);
        m_countsNames = !state.objects->classes.countsGuardsOfNamesBehindArming(classLayout);
    }

    void storeNameGuard(uint32_t numberOfBit, bool holds) { store(numberOfBit, holds, m_namesHaveBegun, m_countsNames); }
    void storeSideGuard(uint32_t numberOfBit, bool holds)
    {
        if (numberOfBit != noClassGuardBit)
            store(numberOfBit, holds, true, true);
    }

    void storeNameGuards(std::span<const NamedClassGuardData> guards, bool holds, const Invocable<bool(uint32_t)> auto& isStored)
    {
        if (guards.empty())
            return;
        RELEASE_ASSERT(byteOffsetOfClassGuardBit(m_rows.lastBit) < m_state.tables.instanceSizes.sizeOfStatics);
        uint8_t* statics = m_state.instance.statics();
        ClassGuardSet stored(m_rows);
        ClassGuardSet changed(m_rows);
        bool isSet = holds != m_isInverted;
        for (const NamedClassGuardData& guard : guards) {
            uint32_t numberOfBit = guard.numberOfBit();
            if (!isStored(numberOfBit))
                continue;
            stored.add(numberOfBit);
            uint8_t& byte = statics[byteOffsetOfClassGuardBit(numberOfBit)];
            uint8_t mask = maskOfClassGuardBit(numberOfBit);
            if (!!(byte & mask) == isSet)
                continue;
            byte ^= mask;
            changed.add(numberOfBit);
            didChangeBit(holds);
        }
        RELEASE_ASSERT(changed.isEmpty() || m_namesHaveBegun);
        if (m_countsNames)
            countChangeOfClassGuards(m_state, m_rows.counts, changed, holds);
        for (auto dependents = m_rows.methodTableDependents; !dependents.empty();) {
            uint32_t numberOfBit = dependents.front().guardBit;
            size_t count = 1;
            while (count < dependents.size() && dependents[count].guardBit == numberOfBit)
                ++count;
            if (stored.contains(numberOfBit))
                repointMethodTableEntries(m_state, dependents.first(count), holds && m_usesMethodTable);
            dependents = dependents.subspan(count);
        }
    }

private:
    bool storeBit(uint32_t numberOfBit, bool holds)
    {
        BitOfClassGuard bit = bitOfClassGuardWithNumber(m_state, numberOfBit);
        uint8_t byteBefore = *bit.byte;
        if (holds != m_isInverted)
            *bit.byte |= bit.mask;
        else
            *bit.byte &= static_cast<uint8_t>(~bit.mask);
        if (*bit.byte == byteBefore)
            return false;
        didChangeBit(holds);
        return true;
    }

    void didChangeBit(bool holds)
    {
        m_state.countWriteForEffectsNet(countOfChangeOfClassGuard(holds));
        if (!holds)
            countEvent(*m_state.instance.vm, RuntimeCounterID::ClassGuardBitsInvalidated);
    }

    void store(uint32_t numberOfBit, bool holds, bool hasBegun, bool counts)
    {
        if (storeBit(numberOfBit, holds)) {
            RELEASE_ASSERT(hasBegun);
            if (counts && !m_rows.counts.empty())
                countChangeOfClassGuard(m_state, rowsFrom(m_rows.counts, numberOfBit), numberOfBit, holds);
        }
        if (m_rows.methodTableDependents.empty())
            return;
        auto dependents = rowsThrough(rowsFrom(m_rows.methodTableDependents, numberOfBit), numberOfBit);
        if (!dependents.empty())
            repointMethodTableEntries(m_state, dependents, holds && m_usesMethodTable);
    }

    State& m_state;
    ClassGuardRows m_rows;
    bool m_isInverted;
    bool m_usesMethodTable;
    bool m_namesHaveBegun { true };
    bool m_countsNames { true };
};

static void storeGuards(State& state, const LayoutData& classLayout, ClassSide side, uint32_t sideBit, bool holds, const Invocable<bool(uint32_t)> auto& isStored)
{
    auto guards = guardsOfNamesInClassRecord(state.tables, classLayout, side);
    ClassGuardWriter writer(state, classLayout, side, findClassGuardRows(state, guards, sideBit));
    writer.storeSideGuard(sideBit, holds);
    writer.storeNameGuards(guards, holds, isStored);
}

static void storeSideGuards(State& state, const LayoutData& classLayout, ClassSide side, bool holds, const Invocable<bool(uint32_t)> auto& isStored)
{
    storeGuards(state, classLayout, side, classLayout.classData().guardBit[static_cast<size_t>(side)], holds, isStored);
}

static bool bitOfClassGuardSaysThatItHolds(const LayoutData& classLayout, ClassSide side, BitOfClassGuard bit)
{
    return !!(*bit.byte & bit.mask) != classLayout.classData().bitsAreClearWhileGuardsHold(side);
}

void ClassTable::beginGuardsSlowCase(const LayoutData& classLayout)
{
    m_state.instance.propertyCaches.forgetReplacedMembers();
    m_classesWhoseGuardsHaveBegun.set(m_tables.rankOfClass(classLayout.index));
    if (!classPrototypeIfExists(classLayout))
        clearAssignedMembers(classLayout, ClassSide::Prototype);
    if (const LayoutData* above = assumedBaseOf(classLayout); above && !guardsHaveBegun(*above)) {
        if (isDefinedByClass(*above) || isClassOfCreatedObjects(*above) || m_beginsGuardsOfFunctionsNotMadeYet)
            beginGuardsSlowCase(*above);
        else
            classPrototypeOfInstances(*m_state.instance.vm, *above);
    }
    if (BitOfClassGuard guardOfPrototypeSide = bitOfClassGuard(m_state, classLayout, ClassSide::Prototype); guardOfPrototypeSide.byte) {
        RELEASE_ASSERT(guardOfPrototypeSide.mask != maskOfClassGuardsBegun);
        if (!m_beginsGuardsOfFunctionsNotMadeYet)
            *guardOfPrototypeSide.byte |= maskOfClassGuardsBegun;
        m_state.countWriteForEffectsNet(Typed::CountInEffectsNet::MemberGuardsArmedOrBegun);
    }
    if (classLayout.classData().guardsOfNamesAwaitBeginning && !countsGuardsOfNamesBehindArming(classLayout))
        countGuardsOfNamesThatDoNotHold(classLayout);
}

void ClassTable::countGuardsOfNamesThatDoNotHold(const LayoutData& classLayout)
{
    auto guards = guardsOfNamesInClassRecord(m_tables, classLayout, ClassSide::Prototype);
    ClassGuardRows rows = findClassGuardRows(m_state, guards);
    if (rows.counts.empty())
        return;
    RELEASE_ASSERT(byteOffsetOfClassGuardBit(rows.lastBit) < m_tables.instanceSizes.sizeOfStatics);
    const uint8_t* statics = m_state.instance.statics();
    bool isSetWhileEnded = classLayout.classData().bitsAreClearWhileGuardsHold(ClassSide::Prototype);
    ClassGuardSet endedGuards(rows);
    for (const NamedClassGuardData& guard : guards) {
        uint32_t numberOfBit = guard.numberOfBit();
        if (!!(statics[byteOffsetOfClassGuardBit(numberOfBit)] & maskOfClassGuardBit(numberOfBit)) == isSetWhileEnded)
            endedGuards.add(numberOfBit);
    }
    countChangeOfClassGuards(m_state, rows.counts, endedGuards, false);
}

bool ClassTable::standsBelowOtherParent(const LayoutData& classLayout, ClassSide side) const
{
    for (const LayoutData* current = &classLayout; current; current = assumedBaseOf(*current)) {
        if (hasOtherParent(*current, side))
            return true;
    }
    return false;
}

static bool classGuardOfSideHolds(const State& state, const LayoutData& classLayout, ClassSide side)
{
    if (state.objects->classes.guardHasEnded(classLayout, side) || state.objects->classes.hasOtherParent(classLayout, side))
        return false;
    BitOfClassGuard bit = bitOfClassGuard(state, classLayout, side);
    return !bit.byte || bitOfClassGuardSaysThatItHolds(classLayout, side, bit);
}

bool classGuardHolds(const State& state, const LayoutData& classLayout, ClassSide side)
{
    const ClassTable& classes = state.objects->classes;
    for (const LayoutData* current = &classLayout; current; current = classes.assumedBaseOf(*current)) {
        if (classes.m_sidesWhereNamesEnded.get(classes.indexOfGuard(*current, side)))
            return false;
    }
    return classGuardOfSideHolds(state, classLayout, side);
}

bool classGuardOfNameHolds(const State& state, const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    auto guards = guardsOfNameInClassRecord(state.tables, classLayout, side, key);
    if (std::ranges::none_of(guards, &NamedClassGuardData::answersReadsOffEveryObject))
        return classGuardHolds(state, classLayout, side);
    if (state.objects->classes.guardHasEnded(classLayout, side))
        return false;
    return std::ranges::any_of(guards, [&](const NamedClassGuardData& guard) {
        return guard.answersReadsOffEveryObject() && bitOfClassGuardSaysThatItHolds(classLayout, side, bitOfClassGuardWithNumber(state, guard.numberOfBit()));
    });
}

static std::span<const uint32_t> subclassesInClassRecord(const ProgramTables& tables, const LayoutData& classLayout)
{
    uint32_t offset = classLayout.classData().subclassesOffset;
    if (!offset)
        return { };
    const uint32_t* list = tables.at<uint32_t>(offset);
    return { list + 1, list[0] };
}

static auto assignedMembersOf(const ProgramTables& tables, const LayoutData& classLayout, ClassSide side)
{
    return std::ranges::equal_range(tables.assignedMembers(), encodeSideOfClassHolder(classLayout, side), { }, &AssignedMemberData::classAndSide);
}

static void countAssignedMember(State& state, uint32_t guardBit, bool holds)
{
    for (const ClassGuardCountData& count : std::ranges::equal_range(state.tables.classGuardCounts(), guardBit, { }, &ClassGuardCountData::guardBit))
        countChangeOfClassGuard(state, count, holds);
}

static void storeAssignedMember(State& state, const AssignedMemberData& member, JSValue found)
{
    uint32_t offset = byteOffsetOfClassGuardBit(member.guardBit);
    EncodedJSValue& word = state.statics->at<EncodedJSValue>(offset);
    if (word == JSValue::encode(found))
        return;
    bool wasLost = JSValue::decode(word).isUndefined();
    word = JSValue::encode(found);
    if (wasLost != found.isUndefined())
        countAssignedMember(state, member.guardBit, !found.isUndefined());
    if (found.isCell())
        state.objects->classes.didStoreAssignedMember(member.guardBit);
    state.countWriteForEffectsNet(countOfChangeOfClassGuard(!found.isUndefined()));
    if (found.isUndefined())
        countEvent(*state.instance.vm, RuntimeCounterID::ClassGuardBitsInvalidated);
}

void endClassGuard(State& state, const LayoutData& classLayout, ClassSide side)
{
    ClassTable& classes = state.objects->classes;
    if (classes.m_endedGuards.get(classes.indexOfGuard(classLayout, side)))
        return;
    classes.m_endedGuards.ensureSize(static_cast<size_t>(state.tables.numberOfClasses()) * numberOfClassSides);
    ClassTable::MethodTableUpdate update(classes);
    classes.m_endedGuards.set(classes.indexOfGuard(classLayout, side));
    if (!classLayout.classData().hasGuardsHereOrBelow(side))
        return;
    storeSideGuards(state, classLayout, side, false, [](uint32_t) {
        return true;
    });
    classes.clearAssignedMembers(classLayout, side);
    for (uint32_t subclass : subclassesInClassRecord(state.tables, classLayout)) {
        const LayoutData* below = classes.classWithIndex(subclass);
        RELEASE_ASSERT(below);
        endGuardsOfInheritedMembers(state, *below, side);
    }
}

static Closure* registeredClassObjectOf(State& state, const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    JSCell* registered = constructor ? state.objects->closures.closureIfCreated(constructor) : nullptr;
    return registered ? dynamicDowncast<Closure>(registered) : nullptr;
}

static JSObject* prototypeHolderIfMade(VM& vm, State& state, const LayoutData& classLayout)
{
    if (JSObject* prototype = state.objects->classes.classPrototypeIfExists(classLayout))
        return prototype;
    Closure* classObject = registeredClassObjectOf(state, classLayout);
    JSValue stored = classObject ? classObject->getDirect(vm, vm.propertyNames->prototype) : JSValue();
    return stored && stored.isObject() ? asObject(stored) : nullptr;
}

bool ClassTable::inheritsFromAbove(const LayoutData& classLayout, const LayoutData& relinkedClass, ClassSide side, uint32_t name) const
{
    std::optional<uint32_t> constructorKey = side == ClassSide::Prototype ? identifiersOf(m_state.instance).numberOf(m_state.instance.vm->propertyNames->constructor.impl()) : std::nullopt;
    for (const LayoutData* declaring = &classLayout;; declaring = assumedBaseOf(*declaring)) {
        RELEASE_ASSERT(declaring);
        if (declaresMember(*declaring, side, name) || (name == constructorKey && isDefinedByClass(*declaring)))
            return false;
        if (declaring->index == relinkedClass.index)
            return true;
    }
}

void ClassTable::listGuardsOfPassingStores(const LayoutData& classLayout, const LayoutData& relinkedClass, GuardBits& guardBits)
{
    VM& vm = *m_state.instance.vm;
    JSGlobalObject* realm = m_state.instance.globalObject;
    if (!m_state.chainDependents || m_state.chainDependents->realm() != realm)
        return;
    JSObject* holder = prototypeHolderIfMade(vm, m_state, classLayout);
    if (!holder || isCompiledObject(holder) || !holder->mayBePrototype() || holder->structure()->realm() != realm)
        return;
    ClassSide side;
    const LayoutData* held = classHeldBy(holder, side);
    if (!held || held->index != classLayout.index || side != ClassSide::Prototype)
        return;
    uint32_t nameAsked = 0;
    bool passes = false;
    auto guards = guardsOfNamesInClassRecord(m_tables, classLayout, ClassSide::Prototype);
    for (size_t index = 0; index < guards.size(); ++index) {
        const NamedClassGuardData& guard = guards[index];
        if (!guard.answersStoresAlone() || m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(guard.numberOfBit())) || !inheritsFromAbove(classLayout, relinkedClass, ClassSide::Prototype, guard.name))
            continue;
        if (!passes || guard.name != nameAsked) {
            nameAsked = guard.name;
            UniquedStringImpl* uid = identifiersOf(m_state.instance).identifier(guard.name).impl();
            ChainWalk walk = walkChain({ realm, holder->structure(), nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, uid);
            passes = storeThroughChain(walk) == StoreThroughChain::CreatesOwnProperty;
            if (passes) {
                walk.changeableObjects.append(holder);
                m_state.chainDependents->add(walk.changeableObjects.span(), ChainDependent { uid, classLayout.index, ownerOfGuardsThatHangOnChains() });
            }
        }
        if (passes)
            guardBits.add(guard.numberOfBit());
    }
}

void endGuardsOfInheritedMembers(State& state, const LayoutData& classLayout, ClassSide side)
{
    ClassTable& classes = state.objects->classes;
    if (classes.hasNoGuardLeft(classLayout, side) || classes.hasOtherParent(classLayout, side))
        return;
    classes.m_sidesWithOtherParent.ensureSize(static_cast<size_t>(state.tables.numberOfClasses()) * numberOfClassSides);
    classes.m_sidesWithOtherParent.set(classes.indexOfGuard(classLayout, side));
    classes.m_sidesWhereNamesEnded.set(classes.indexOfGuard(classLayout, side));
    ClassTable::GuardBits guardsOfPassingStores;
    if (side == ClassSide::Prototype) {
        DeferGC deferGC(*state.instance.vm);
        Vector<uint32_t, 16> classesToAsk { classLayout.index };
        while (!classesToAsk.isEmpty()) {
            const LayoutData* current = classes.classWithIndex(classesToAsk.takeLast());
            RELEASE_ASSERT(current);
            if (classes.hasNoGuardLeft(*current, side) || (current->index != classLayout.index && classes.hasOtherParent(*current, side)))
                continue;
            classes.listGuardsOfPassingStores(*current, classLayout, guardsOfPassingStores);
            classesToAsk.append(subclassesInClassRecord(state.tables, *current));
        }
    }
    ClassTable::MethodTableUpdate update(classes);
    Vector<uint32_t, 16> classesToEnd { classLayout.index };
    while (!classesToEnd.isEmpty()) {
        const LayoutData* current = classes.classWithIndex(classesToEnd.takeLast());
        RELEASE_ASSERT(current);
        if (classes.hasNoGuardLeft(*current, side) || (current->index != classLayout.index && classes.hasOtherParent(*current, side)))
            continue;
        auto guards = guardsOfNamesInClassRecord(state.tables, *current, side);
        for (const NamedClassGuardData& guard : guards) {
            if (guardsOfPassingStores.contains(guard.numberOfBit()) || !classes.inheritsFromAbove(*current, classLayout, side, guard.name))
                continue;
            classes.m_endedGuardsOfNames.add(byteOffsetOfClassGuardBit(guard.numberOfBit()));
            classes.m_lateMembersOfGuards.remove(guard.numberOfBit());
        }
        storeSideGuards(state, *current, side, false, [&](uint32_t numberOfBit) {
            return classes.m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(numberOfBit));
        });
        for (const AssignedMemberData& member : assignedMembersOf(state.tables, *current, side))
            classes.updateAssignedMember(member);
        classesToEnd.append(subclassesInClassRecord(state.tables, *current));
    }
}

void endClassGuardsOfName(State& state, const LayoutData& classLayout, ClassSide side, std::optional<uint32_t> key, GuardsOfNameToEnd toEnd)
{
    ClassTable& classes = state.objects->classes;
    if (classes.hasNoGuardLeft(classLayout, side))
        return;
    if (toEnd != GuardsOfNameToEnd::ReadsStillFind)
        classes.m_sidesWhereNamesEnded.set(classes.indexOfGuard(classLayout, side));
    if (!key || !state.tables.someClassGuardNames(*key))
        return;
    classes.updateAssignedMembers(classLayout, side, *key);

    ClassTable::MethodTableUpdate update(classes);
    auto goesOnHolding = [&](const NamedClassGuardData& guard) {
        switch (toEnd) {
        case GuardsOfNameToEnd::All:
            return false;
        case GuardsOfNameToEnd::StoresStillPass:
            return guard.answersStoresAlone();
        case GuardsOfNameToEnd::ReadsStillFind:
            return !guard.answersStoresAlone();
        case GuardsOfNameToEnd::OneObjectHidesName:
            return !guard.answersReadsOffEveryObject();
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto endGuardsIn = [&](const LayoutData& current) {
        if (!classes.m_lateMembers.isEmpty() && side == ClassSide::Prototype) [[unlikely]]
            classes.m_lateMembers.remove(ClassTable::lateMemberKey(current, *key));
        for (const NamedClassGuardData& guard : guardsOfNameInClassRecord(state.tables, current, side, *key)) {
            if (goesOnHolding(guard))
                continue;
            classes.m_endedGuardsOfNames.add(byteOffsetOfClassGuardBit(guard.numberOfBit()));
            if (!classes.m_lateMembersOfGuards.isEmpty()) [[unlikely]]
                classes.m_lateMembersOfGuards.remove(guard.numberOfBit());
            ClassGuardWriter(state, current, side, ClassGuardRows(state, guard.numberOfBit(), guard.numberOfBit())).storeNameGuard(guard.numberOfBit(), false);
        }
    };
    if (toEnd == GuardsOfNameToEnd::OneObjectHidesName || subclassesInClassRecord(state.tables, classLayout).empty()) {
        endGuardsIn(classLayout);
        return;
    }

    uint64_t part = 0;
    if (toEnd == GuardsOfNameToEnd::StoresStillPass)
        part = ClassTable::storesStillPassUnderEndedName;
    else if (toEnd == GuardsOfNameToEnd::ReadsStillFind)
        part = ClassTable::readsStillFindUnderEndedName;
    auto everyGuardEndedAt = [&](const LayoutData& current) {
        return static_cast<uint64_t>(classes.indexOfGuard(current, side)) << 32 | *key;
    };
    auto endedNameAt = [&](const LayoutData& current) {
        return everyGuardEndedAt(current) | part;
    };
    auto hasEndedAt = [&](const LayoutData& current) {
        return classes.m_endedNames.contains(everyGuardEndedAt(current)) || (part && classes.m_endedNames.contains(endedNameAt(current)));
    };
    if (hasEndedAt(classLayout))
        return;
    classes.m_endedNames.add(endedNameAt(classLayout));
    auto listedBelow = std::ranges::equal_range(state.tables.classesBelowWithGuardsOfNames(), *key, { }, &ClassBelowWithGuardOfNameData::name);
    size_t numberOfClassesLookedAt = 1;
    endGuardsIn(classLayout);
    Vector<uint32_t, 16> classesToEnd;
    classesToEnd.append(subclassesInClassRecord(state.tables, classLayout));
    auto entry = listedBelow.begin();
    const LayoutData* listed = nullptr;
    const LayoutData* reached = nullptr;
    while (entry != listedBelow.end()) {
        ++numberOfClassesLookedAt;
        const LayoutData* current = classes.classWithIndex(classesToEnd.takeLast());
        RELEASE_ASSERT(current);
        std::span<const uint32_t> subclasses = subclassesInClassRecord(state.tables, *current);
        if (!classes.hasNoGuardLeft(*current, side) && (subclasses.empty() || !hasEndedAt(*current))) {
            endGuardsIn(*current);
            classesToEnd.append(subclasses);
        }
        if (classesToEnd.isEmpty())
            break;

        ++numberOfClassesLookedAt;
        if (!listed) {
            listed = classes.classWithIndex(entry->classLayout());
            RELEASE_ASSERT(listed);
            reached = entry->side() == side && !classes.hasNoGuardLeft(*listed, side) ? listed : nullptr;
        }
        if (reached)
            reached = classes.assumedBaseOf(*reached);
        if (reached && reached->index != classLayout.index)
            continue;
        if (reached)
            endGuardsIn(*listed);
        listed = nullptr;
        ++entry;
    }
    countEvent(*state.instance.vm, RuntimeCounterID::ClassesVisitedForGuardInvalidation, numberOfClassesLookedAt);
}

static bool isLazyClassHolder(JSObject* holder, ClassSide side)
{
    switch (side) {
    case ClassSide::Prototype:
        return holder->classInfo() == ClassPrototype::info();
    case ClassSide::Static:
        return uncheckedDowncast<Closure>(holder)->isLazyClassObject();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* registeredHolderOf(VM& vm, State& state, const LayoutData& classLayout, ClassSide side)
{
    Closure* classObject = registeredClassObjectOf(state, classLayout);
    switch (side) {
    case ClassSide::Static:
        return classObject;
    case ClassSide::Prototype: {
        if (JSObject* made = prototypeHolderIfMade(vm, state, classLayout))
            return made;
        JSValue prototype = classObject ? classObject->prototypeProperty(vm) : JSValue();
        return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isClosureOfMethodInClassRecord(const JSCell* closure, const MethodData& method)
{
    return functionOf(closure) == FunctionID(method.function);
}

static bool isClosureHeldByClosureTable(State& state, const JSCell* closure, const MethodData& method)
{
    ClosureTable& closures = state.objects->closures;
    FunctionID function(method.function);
    return !closures.hasCellFor(function) || closures.closureIfCreated(function) == closure;
}

static bool isMethodOfClassRecord(State& state, const JSCell* closure, const MethodData& method)
{
    return isClosureOfMethodInClassRecord(closure, method) && (method.kind() != MethodKind::Method || isClosureHeldByClosureTable(state, closure, method));
}

static bool holdsMethodOfClassRecord(State& state, JSCell* closure, const MethodData& method)
{
    if (!isClosureOfMethodInClassRecord(closure, method))
        return false;
    ClosureTable& closures = state.objects->closures;
    FunctionID function(method.function);
    if (!closures.hasCellFor(function))
        return true;
    if (JSCell* created = closures.closureIfCreated(function))
        return created == closure;
    closures.registerClosureOfMember(function, closure);
    return closures.closureIfCreated(function) == closure;
}

static bool isOnlyClosureOfFunction(State& state, JSCell* closure, FunctionID function)
{
    if (functionOf(closure) != function)
        return false;
    ClosureTable& closures = state.objects->closures;
    if (!closures.hasCellFor(function))
        return true;
    if (!closures.closureIfCreated(function))
        closures.registerClosureOfMember(function, closure);
    return closures.closureIfCreated(function) == closure;
}

static bool holdsAssignedMember(State& state, JSValue value, unsigned attributes, const AssignedMemberData& member)
{
    if (member.holdsValue)
        return !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue));
    if (!value.isCell() || attributes & (PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
        return false;
    if (FunctionID method(member.method); method)
        return !(attributes & PropertyAttribute::Accessor) && isOnlyClosureOfFunction(state, value.asCell(), method);
    auto* accessor = dynamicDowncast<GetterSetter>(value.asCell());
    if (!accessor)
        return false;
    auto holdsHalf = [&](bool isNull, JSObject* held, FunctionID expected) {
        return expected ? !isNull && functionOf(held) == expected : isNull;
    };
    return holdsHalf(accessor->isGetterNull(), accessor->getter(), FunctionID(member.getter)) && holdsHalf(accessor->isSetterNull(), accessor->setter(), FunctionID(member.setter));
}

JSValue ClassTable::findAssignedMember(const AssignedMemberData& member) const
{
    VM& vm = *m_state.instance.vm;
    const LayoutData* classLayout = classWithIndex(member.classLayout());
    if (!classLayout || hasNoGuardLeft(*classLayout, member.side()) || (!FunctionID(member.method) && !FunctionID(member.getter) && !FunctionID(member.setter) && !member.holdsValue))
        return jsUndefined();
    if (!m_namesBelowPrototypes.isEmpty() && m_namesBelowPrototypes.contains(static_cast<uint64_t>(indexOfGuard(*classLayout, member.side())) << 32 | member.name)) [[unlikely]]
        return jsUndefined();
    UniquedStringImpl* uid = identifiersOf(m_state.instance).uid(member.name);
    JSObject* below = nullptr;
    for (const LayoutData* current = classLayout; current; current = assumedBaseOf(*current)) {
        if (!m_membersReplacedAgain.isEmpty() && m_membersReplacedAgain.contains(static_cast<uint64_t>(indexOfGuard(*current, member.side())) << 32 | member.name)) [[unlikely]]
            return jsUndefined();
        JSObject* holder = prototypeHolderIfMade(vm, m_state, *current);
        if (current->evaluation() == EvaluationState::SeenTwice || (below && below->getPrototypeDirect() != (holder ? JSValue(holder) : JSValue())))
            return jsUndefined();
        if (holder) {
            if (isCompiledObject(holder))
                return jsUndefined();
            unsigned attributes = 0;
            if (JSValue value = holder->getDirect(vm, uid, attributes))
                return !holdsAssignedMember(m_state, value, attributes, member) ? jsUndefined() : FunctionID(member.getter) ? JSValue(uncheckedDowncast<GetterSetter>(value.asCell())->getter()) : value;
        } else if (!baseOf(*current) && !prototypeAwaitsFirstTouch(*current))
            return jsUndefined();
        if (declaresMember(*current, member.side(), member.name) || hasOtherParent(*current, member.side()))
            return jsUndefined();
        below = holder;
    }
    return jsUndefined();
}

void ClassTable::clearAssignedMemberIfDead(VM& vm, uint32_t guardBit)
{
    EncodedJSValue& word = m_state.statics->at<EncodedJSValue>(byteOffsetOfClassGuardBit(guardBit));
    JSValue held = JSValue::decode(word);
    if (!held.isCell() || vm.heap.isMarked(held.asCell()))
        return;
    word = JSValue::encode(jsUndefined());
    countAssignedMember(m_state, guardBit, false);
}

void ClassTable::updateAssignedMember(const AssignedMemberData& member)
{
    storeAssignedMember(m_state, member, findAssignedMember(member));
}

void ClassTable::clearAssignedMembers(const LayoutData& classLayout, ClassSide side)
{
    for (const AssignedMemberData& member : assignedMembersOf(m_tables, classLayout, side))
        storeAssignedMember(m_state, member, jsUndefined());
}

void ClassTable::updateAssignedMembers(const LayoutData& classLayout, ClassSide side)
{
    for (const AssignedMemberData& member : assignedMembersOf(m_tables, classLayout, side))
        updateAssignedMember(member);
}

void ClassTable::updateAssignedMembersOfHeirs(const LayoutData& defined)
{
    if (m_tables.assignedMembers().empty())
        return;
    for (const LayoutData* base = &defined; base; base = assumedBaseOf(*base)) {
        std::span<const uint32_t> subclasses = subclassesInClassRecord(m_tables, *base);
        if (subclasses.empty() || !std::has_single_bit(++m_definedHeirCounts.add(base->index, 0).iterator->value))
            continue;
        Vector<uint32_t, 16> heirs(subclasses);
        while (!heirs.isEmpty()) {
            const LayoutData* heir = classWithIndex(heirs.takeLast());
            RELEASE_ASSERT(heir);
            if (!guardsHaveBegun(*heir))
                updateAssignedMembers(*heir, ClassSide::Prototype);
            heirs.append(subclassesInClassRecord(m_tables, *heir));
        }
    }
}

bool ClassTable::isAssignedMemberName(uint32_t key)
{
    std::span<const AssignedMemberData> members = m_tables.assignedMembers();
    if (members.empty())
        return false;
    if (m_assignedMemberNames.isEmpty()) {
        for (const AssignedMemberData& member : members)
            m_assignedMemberNames.add(member.name);
    }
    return m_assignedMemberNames.contains(key);
}

void ClassTable::updateAssignedMembers(const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    if (!isAssignedMemberName(key))
        return;
    std::span<const AssignedMemberData> members = m_tables.assignedMembers();
    Vector<uint32_t, 16> classes { classLayout.index };
    while (!classes.isEmpty()) {
        const LayoutData* current = classWithIndex(classes.takeLast());
        RELEASE_ASSERT(current);
        if (!current->classData().hasGuardsHereOrBelow(side))
            continue;
        uint64_t place = static_cast<uint64_t>(encodeSideOfClassHolder(*current, side)) << 32 | key;
        for (const AssignedMemberData& member : std::ranges::equal_range(members, place, { }, &AssignedMemberData::place))
            updateAssignedMember(member);
        classes.append(subclassesInClassRecord(m_tables, *current));
    }
}

static bool holdsMemberOfClassRecord(State& state, JSValue value, unsigned attributes, const ClassTable::DeclaredMember& member)
{
    if (!value || !value.isCell())
        return false;
    if (member.method)
        return !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) && holdsMethodOfClassRecord(state, value.asCell(), *member.method);
    auto* accessor = dynamicDowncast<GetterSetter>(value.asCell());
    if (!accessor)
        return false;
    bool holdsGetter = member.getter ? isMethodOfClassRecord(state, accessor->getter(), *member.getter) : accessor->isGetterNull();
    bool holdsSetter = member.setter ? isMethodOfClassRecord(state, accessor->setter(), *member.setter) : accessor->isSetterNull();
    return holdsGetter && holdsSetter;
}

static size_t numberOfComputedNameMembersInProperty(State& state, std::span<const MethodData> methods, JSValue value, unsigned attributes)
{
    auto isDeclared = [&](const JSCell* closure, MethodKind kind) {
        return std::ranges::any_of(methods, [&](const MethodData& method) {
            return (method.flags & methodHasComputedName) && method.kind() == kind && isMethodOfClassRecord(state, closure, method);
        });
    };
    if (!value || !value.isCell())
        return 0;
    if (!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return !(attributes & PropertyAttribute::ReadOnly) && isDeclared(value.asCell(), MethodKind::Method);
    auto* accessor = dynamicDowncast<GetterSetter>(value.asCell());
    if (!accessor)
        return 0;
    bool hasGetter = !accessor->isGetterNull();
    bool hasSetter = !accessor->isSetterNull();
    if ((hasGetter && !isDeclared(accessor->getter(), MethodKind::Getter)) || (hasSetter && !isDeclared(accessor->setter(), MethodKind::Setter)))
        return 0;
    return static_cast<size_t>(hasGetter) + hasSetter;
}

static bool isImplicitMemberOfClassHolder(VM& vm, ClassSide side, UniquedStringImpl* uid)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    switch (side) {
    case ClassSide::Prototype:
        return uid == names.constructor.impl();
    case ClassSide::Static:
        return uid == names.prototype.impl() || uid == names.name.impl() || uid == names.length.impl();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool classHolderReportsAsPrototype(const ClassTable& classes, const LayoutData& classLayout, ClassSide side)
{
    return side == ClassSide::Prototype || !classes.isDefinedByClass(classLayout);
}

static bool isClassOfLibrary(const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    return constructor && functionKindData(constructor).isInRuntimeModule;
}

bool ClassTable::isAccessorOfProgram(const LayoutData& classLayout, JSValue value)
{
    auto* accessor = value && value.isCell() ? dynamicDowncast<GetterSetter>(value.asCell()) : nullptr;
    if (!accessor || !isClassOfLibrary(classLayout))
        return false;
    auto isOfProgram = [](const JSCell* half) {
        std::optional<FunctionID> function = functionOf(half);
        return !function || !functionKindData(*function).isInRuntimeModule;
    };
    return (!accessor->isGetterNull() && isOfProgram(accessor->getter())) || (!accessor->isSetterNull() && isOfProgram(accessor->setter()));
}

static StateOfClassHolder stateOfCompiledClassHolder(VM& vm, State& state, const LayoutData& classLayout, ClassSide side, Object& holder, NamesThatDifferOnClassHolders& namesThatDiffer)
{
    JSObject* extraProperties = extraPropertiesIfCreated(holder);
    if (state.objects->classes.isDefinedByClass(classLayout) || holder.isInFrame() || &holder.state() != &state || (holder.hasExtraProperties() && !extraProperties))
        return StateOfClassHolder::Differs;
    std::span<const MethodData> methods = methodsInClassRecord(classLayout, side);
    for (const MethodData& method : methods) {
        if (method.flags & methodHasComputedName)
            return StateOfClassHolder::Differs;
    }
    constexpr unsigned attributesThatTakeStores = PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue;
    for (const FieldData& field : holder.layout().fields()) {
        if (field.isPrivate() || field.isAccessor() || holder.attributesOf(field) & attributesThatTakeStores) {
            namesThatDiffer.add(static_cast<uint32_t>(field.name));
            namesThatDiffer.someSeesAccesses |= !field.isPrivate();
        } else
            namesThatDiffer.numbersOfPlainValues.append(static_cast<uint32_t>(field.name));
    }
    Vector<uint32_t, 8> namesHeldAsDeclared;
    if (extraProperties) {
        const ProgramIdentifiers& identifiers = identifiersOf(state.instance);
        extraProperties->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            std::optional<uint32_t> key = identifiers.numberOf(entry.key());
            ClassTable::DeclaredMember member;
            bool isDeclared = false;
            for (const MethodData& method : methods) {
                if (key && method.name == *key) {
                    member.define(method);
                    isDeclared = true;
                }
            }
            if (isDeclared && holdsMemberOfClassRecord(state, extraProperties->getDirect(entry.offset()), entry.attributes(), member)) {
                namesThatDiffer.someSeesAccesses |= ClassTable::isAccessorOfProgram(classLayout, extraProperties->getDirect(entry.offset()));
                namesHeldAsDeclared.append(*key);
                return true;
            }
            namesThatDiffer.add(key);
            namesThatDiffer.someSeesAccesses |= ClassTable::seesAccesses(vm, entry.key(), entry.attributes());
            return true;
        });
    }
    for (const MethodData& method : methods) {
        if (!namesHeldAsDeclared.contains(static_cast<uint32_t>(method.name)))
            namesThatDiffer.add(static_cast<uint32_t>(method.name));
    }
    return StateOfClassHolder::AsDeclared;
}

static StateOfClassHolder stateOfClassHolder(VM& vm, State& state, const LayoutData& classLayout, ClassSide side, JSObject* holder, NamesThatDifferOnClassHolders& namesThatDiffer)
{
    if (isLazyClassHolder(holder, side))
        return StateOfClassHolder::AsDeclared;
    if (Object* compiledHolder = compiledObjectOrNull(holder))
        return stateOfCompiledClassHolder(vm, state, classLayout, side, *compiledHolder, namesThatDiffer);

    static_assert(MethodData::nameBits < 32);
    constexpr uint32_t noName = UINT32_MAX;
    auto nameOfRow = [](uint64_t row) {
        return static_cast<uint32_t>(row >> 32);
    };
    std::span<const MethodData> methods = methodsInClassRecord(classLayout, side);
    Vector<uint64_t, 32> rowsByName;
    for (size_t index = 0; index < methods.size(); ++index) {
        if (!(methods[index].flags & methodHasComputedName))
            rowsByName.append(static_cast<uint64_t>(methods[index].name) << 32 | index);
    }
    size_t numberOfMembersWithoutNameToFind = methods.size() - rowsByName.size();
    AOT::sort(rowsByName);

    Vector<uint32_t, 16> namesOfStaticFields;
    if (side == ClassSide::Static) {
        for (const FieldData& field : classLayout.staticFields())
            namesOfStaticFields.append(static_cast<uint32_t>(field.name));
        AOT::sort(namesOfStaticFields);
    }

    bool isDefinedByClass = state.objects->classes.isDefinedByClass(classLayout);
    const ProgramIdentifiers& identifiers = identifiersOf(state.instance);
    size_t numberOfMembersWithoutNameFound = 0;
    Vector<uint32_t, 32> declaredNamesFound;
    bool isUnfinished = false;
    holder->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        JSValue value = holder->getDirect(entry.offset());
        namesThatDiffer.someSeesAccesses |= ClassTable::isAccessorOfProgram(classLayout, value);
        std::optional<uint32_t> key = identifiers.numberOf(entry.key());
        auto rows = std::ranges::equal_range(rowsByName, key.value_or(noName), { }, nameOfRow);
        if (!rows.empty()) {
            ClassTable::DeclaredMember member;
            bool mayBeDefinedLater = false;
            for (uint64_t row : rows) {
                const MethodData& method = methods[static_cast<uint32_t>(row)];
                member.define(method);
                mayBeDefinedLater |= isDefinedByClass && (method.flags & methodIsCreatedAtDefinition);
            }
            declaredNamesFound.append(*key);
            if (holdsMemberOfClassRecord(state, value, entry.attributes(), member))
                return true;
            namesThatDiffer.someSeesAccesses |= ClassTable::seesAccesses(vm, entry.key(), entry.attributes());
            if (mayBeDefinedLater)
                isUnfinished = true;
            else
                namesThatDiffer.add(key);
            return true;
        }
        if (isImplicitMemberOfClassHolder(vm, side, entry.key())) {
            namesThatDiffer.someSeesAccesses |= side == ClassSide::Prototype && ClassTable::seesAccesses(vm, entry.key(), entry.attributes());
            return true;
        }
        if (key && std::ranges::binary_search(namesOfStaticFields, *key))
            return true;
        size_t numberOfMembers = numberOfComputedNameMembersInProperty(state, methods, value, entry.attributes());
        numberOfMembersWithoutNameFound += numberOfMembers;
        if (numberOfMembers)
            return true;
        constexpr unsigned attributesThatTakeStores = PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue;
        namesThatDiffer.someSeesAccesses |= ClassTable::seesAccesses(vm, entry.key(), entry.attributes());
        if (key && !(entry.attributes() & attributesThatTakeStores))
            namesThatDiffer.numbersOfPlainValues.append(*key);
        else
            namesThatDiffer.add(key);
        return true;
    });

    AOT::sort(declaredNamesFound);
    bool lacksDeclaredName = false;
    for (size_t index = 0; index < rowsByName.size(); ++index) {
        uint32_t name = nameOfRow(rowsByName[index]);
        if ((index && name == nameOfRow(rowsByName[index - 1])) || std::ranges::binary_search(declaredNamesFound, name))
            continue;
        lacksDeclaredName = true;
        if (!isDefinedByClass)
            namesThatDiffer.add(name);
    }
    bool lacksMemberWithoutName = numberOfMembersWithoutNameFound != numberOfMembersWithoutNameToFind;
    if (isDefinedByClass)
        return isUnfinished || lacksDeclaredName || lacksMemberWithoutName ? StateOfClassHolder::MayStillBeDefined : StateOfClassHolder::AsDeclared;
    return lacksMemberWithoutName ? StateOfClassHolder::Differs : StateOfClassHolder::AsDeclared;
}

const ClassTable::CheckedHolder* ClassTable::findCheckedHolder(const LayoutData& classLayout, ClassSide side, JSObject* holder) const
{
    auto found = m_checkedHolders.find(encodeSideOfClassHolder(classLayout, side));
    bool isCurrent = found != m_checkedHolders.end() && found->value.holder == holder && found->value.structure == holder->structureID();
    return isCurrent ? &found->value : nullptr;
}

void ClassTable::addCheckedHolder(const LayoutData& classLayout, ClassSide side, CheckedHolder&& checked)
{
    m_state.instance.propertyCaches.forgetReplacedMembersOf(checked.holder);
    m_checkedHolders.set(encodeSideOfClassHolder(classLayout, side), WTF::move(checked));
}

void ClassTable::removeCheckedHolder(const LayoutData& classLayout, ClassSide side)
{
    if (!m_checkedHolders.isEmpty())
        m_checkedHolders.remove(encodeSideOfClassHolder(classLayout, side));
}

static bool isParentAsDeclared(VM& vm, State& state, const LayoutData& classLayout, ClassSide side, JSObject* parent, NamesThatDifferOnClassHolders& namesThatDiffer)
{
    ClassTable& classes = state.objects->classes;
    if (isLazyClassHolder(parent, side) || compiledObjectOrNull(parent))
        return stateOfClassHolder(vm, state, classLayout, side, parent, namesThatDiffer) == StateOfClassHolder::AsDeclared;
    const ClassTable::CheckedHolder* checked = classes.findCheckedHolder(classLayout, side, parent);
    if (!checked) {
        NamesThatDifferOnClassHolders names;
        if (stateOfClassHolder(vm, state, classLayout, side, parent, names) != StateOfClassHolder::AsDeclared)
            return false;
        classes.addCheckedHolder(classLayout, side, { parent, parent->structureID(), WTF::move(names.numbers), WTF::move(names.numbersOfPlainValues), names.someHasNoNumber, names.someSeesAccesses });
        checked = classes.findCheckedHolder(classLayout, side, parent);
        RELEASE_ASSERT(checked);
    }
    namesThatDiffer.numbers.appendVector(checked->differingNames);
    namesThatDiffer.numbersOfPlainValues.appendVector(checked->plainValueNames);
    namesThatDiffer.someHasNoNumber |= checked->someHasNoNumber;
    namesThatDiffer.someSeesAccesses |= checked->someSeesAccesses;
    return true;
}

static bool holdsClassObjectUnderConstructor(VM& vm, State& state, const LayoutData& classLayout, JSObject* prototype)
{
    if (isLazyClassHolder(prototype, ClassSide::Prototype))
        return true;
    if (compiledObjectOrNull(prototype))
        return false;
    constexpr unsigned attributesOfNoPlainValue = PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue;
    unsigned attributes = 0;
    JSValue held = prototype->getDirect(vm, vm.propertyNames->constructor, attributes);
    return held && !(attributes & attributesOfNoPlainValue) && held.isCell() && held.asCell() == registeredClassObjectOf(state, classLayout);
}

static const LayoutData* classAboveOnSide(const ClassTable& classes, const LayoutData& classLayout, ClassSide side)
{
    if (side == ClassSide::Static && !classes.isDefinedByClass(classLayout))
        return nullptr;
    return classes.assumedBaseOf(classLayout);
}

static const LayoutData* classWithHolder(const ClassTable& classes, JSObject* object)
{
    ClassSide side;
    if (const LayoutData* held = classes.classHeldBy(object, side))
        return held;
    if (auto* lazyPrototype = dynamicDowncast<ClassPrototype>(object))
        return classes.classOf(*lazyPrototype);
    auto* closure = dynamicDowncast<Closure>(object);
    return closure ? closure->classData() : nullptr;
}

static bool standsBelowFunctionsNamedByClassRecords(VM& vm, State& state, const LayoutData& classLayout, JSObject* function, NamesThatDifferOnClassHolders& namesOfFunctionsAbove)
{
    ClassTable& classes = state.objects->classes;
    JSObject* functionPrototype = state.instance.globalObject->functionPrototype();
    const LayoutData* current = &classLayout;
    for (JSObject* parent = function->getPrototypeDirect().getObject(); parent != functionPrototype; parent = parent->getPrototypeDirect().getObject()) {
        current = classes.assumedBaseOf(*current);
        if (!parent || !current || classes.isDefinedByClass(*current) || parent != registeredClassObjectOf(state, *current) || classes.guardHasEnded(*current, ClassSide::Static))
            return false;
        ClassSide sideOfParent;
        if (classes.classHeldBy(parent, sideOfParent) != current || sideOfParent != ClassSide::Static)
            return false;
        if (!isParentAsDeclared(vm, state, *current, ClassSide::Static, parent, namesOfFunctionsAbove))
            return false;
    }
    while ((current = classes.assumedBaseOf(*current))) {
        for (const MethodData& method : methodsInClassRecord(*current, ClassSide::Static))
            namesOfFunctionsAbove.add(method.flags & methodHasComputedName ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(method.name)));
    }
    return true;
}

static void addNamesThatLookupsComeToAbove(const LayoutData& classLayout, ClassSide side, const NamesThatDifferOnClassHolders& above, NamesThatDifferOnClassHolders& ofHolder)
{
    size_t numberOfOwnNumbers = ofHolder.numbers.size();
    size_t numberOfOwnPlainValues = ofHolder.numbersOfPlainValues.size();
    auto isAnsweredByHolder = [&](uint32_t name) {
        return declaresMember(classLayout, side, name) && !std::ranges::contains(ofHolder.numbers.span().first(numberOfOwnNumbers), name) && !std::ranges::contains(ofHolder.numbersOfPlainValues.span().first(numberOfOwnPlainValues), name);
    };
    for (uint32_t name : above.numbers) {
        if (!isAnsweredByHolder(name))
            ofHolder.numbers.append(name);
    }
    for (uint32_t name : above.numbersOfPlainValues) {
        if (!isAnsweredByHolder(name))
            ofHolder.numbersOfPlainValues.append(name);
    }
    ofHolder.someHasNoNumber |= above.someHasNoNumber;
}

static bool hasParentNamedByClassRecord(VM& vm, State& state, const LayoutData& classLayout, ClassSide side, JSObject* holder, NamesThatDifferOnClassHolders& namesThatDifferAbove)
{
    ClassTable& classes = state.objects->classes;
    JSObject* parent = holder->getPrototypeDirect().getObject();
    const LayoutData* above = classAboveOnSide(classes, classLayout, side);
    if (!above && side == ClassSide::Static && !classes.isDefinedByClass(classLayout))
        return standsBelowFunctionsNamedByClassRecords(vm, state, classLayout, holder, namesThatDifferAbove);
    if (!above) {
        const LayoutData* classOfParent = parent ? classWithHolder(classes, parent) : nullptr;
        return !classOfParent || isClassOfLibrary(*classOfParent);
    }
    if (!parent || parent != registeredHolderOf(vm, state, *above, side) || classes.guardHasEnded(*above, side))
        return false;
    bool otherParentStandsAbove = classes.standsBelowOtherParent(*above, side);
    if (!otherParentStandsAbove) {
        if (!classGuardOfSideHolds(state, *above, side))
            return false;
        if (above->classData().guardsHoldWhileClear)
            return true;
    }
    ClassSide sideOfParent;
    if (classes.classHeldBy(parent, sideOfParent) != above || sideOfParent != side)
        return false;
    if (!otherParentStandsAbove && bitOfClassGuard(state, *above, side).byte)
        return true;
    NamesThatDifferOnClassHolders namesFurtherUp;
    NamesThatDifferOnClassHolders namesOfParent;
    if (!classes.hasOtherParent(*above, side) && !hasParentNamedByClassRecord(vm, state, *above, side, parent, namesFurtherUp))
        return false;
    if (!isParentAsDeclared(vm, state, *above, side, parent, namesOfParent))
        return false;
    addNamesThatLookupsComeToAbove(*above, side, namesFurtherUp, namesOfParent);
    namesThatDifferAbove.numbers.appendVector(namesOfParent.numbers);
    namesThatDifferAbove.numbersOfPlainValues.appendVector(namesOfParent.numbersOfPlainValues);
    namesThatDifferAbove.someHasNoNumber |= namesOfParent.someHasNoNumber;
    return true;
}

static const MethodData* findRowOfMemberInClassRecord(const LayoutData& classLayout, ClassSide side, uint32_t index, const JSCell* function)
{
    std::span<const MethodData> methods = methodsInClassRecord(classLayout, side);
    size_t numberOnOtherSide = methodsInClassRecord(classLayout, side == ClassSide::Static ? ClassSide::Prototype : ClassSide::Static).size();
    size_t lowest = index > numberOnOtherSide ? index - numberOnOtherSide : 0;
    auto defined = functionOf(function);
    for (size_t row = std::min<size_t>(static_cast<size_t>(index) + 1, methods.size()); row-- > lowest;) {
        if (defined == FunctionID(methods[row].function))
            return &methods[row];
    }
    return nullptr;
}

static bool definesMemberOfClassRecord(State& state, const LayoutData& classLayout, ClassSide side, uint32_t index, UniquedStringImpl* uid, JSCell* function)
{
    std::optional<uint32_t> key = identifiersOf(state.instance).numberOf(uid);
    if (const MethodData* row = findRowOfMemberInClassRecord(classLayout, side, index, function); row && key && isNamedInClassRecord(*row, *key))
        return row->kind() == MethodKind::Method ? holdsMethodOfClassRecord(state, function, *row) : isMethodOfClassRecord(state, function, *row);
    bool recordNamesKey = key && declaresMember(classLayout, side, *key);
    return std::ranges::any_of(methodsInClassRecord(classLayout, side), [&](const MethodData& method) {
        bool isRowOfKey = recordNamesKey ? isNamedInClassRecord(method, *key) : !!(method.flags & methodHasComputedName);
        if (!isRowOfKey)
            return false;
        return method.kind() == MethodKind::Method ? holdsMethodOfClassRecord(state, function, method) : isMethodOfClassRecord(state, function, method);
    });
}

static bool isLastMemberCreatedAtDefinition(const LayoutData& classLayout, ClassSide side, const JSCell* function)
{
    std::span<const MethodData> methods = methodsInClassRecord(classLayout, side);
    for (size_t index = methods.size(); index--;) {
        if (methods[index].flags & methodIsCreatedAtDefinition)
            return isClosureOfMethodInClassRecord(function, methods[index]);
    }
    return false;
}

static NameOnPrototypeHolder findNameOnPrototypeHolder(VM& vm, State& state, const LayoutData& classLayout, UniquedStringImpl* uid, std::optional<uint32_t> key)
{
    if (uid == vm.propertyNames->constructor.impl() || (key && declaresMember(classLayout, ClassSide::Prototype, *key)))
        return NameOnPrototypeHolder::Found;
    const ClassTable& classes = state.objects->classes;
    JSObject* holder = prototypeHolderIfMade(vm, state, classLayout);
    if (!holder)
        return NameOnPrototypeHolder::AbsentForNow;
    if (isLazyClassHolder(holder, ClassSide::Prototype))
        return classes.classPrototypeIfExists(classLayout) == holder ? NameOnPrototypeHolder::AbsentUntilReported : NameOnPrototypeHolder::AbsentForNow;
    if (isCompiledObject(holder) ? !Object::structureGuaranteesAbsence(holder->structure(), uid) : isValidOffset(holder->structure()->get(vm, uid)))
        return NameOnPrototypeHolder::Found;
    ClassSide side;
    const LayoutData* held = classes.classHeldBy(holder, side);
    bool isEntered = held && held->index == classLayout.index && side == ClassSide::Prototype;
    return isEntered ? NameOnPrototypeHolder::AbsentUntilReported : NameOnPrototypeHolder::AbsentForNow;
}

void ClassTable::armMemberGuards(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, HolderState holderState)
{
    ASSERT(holder);
    m_state.instance.propertyCaches.forgetReplacedMembers();
    removeCheckedHolder(layout, side);
    bool countsBehindArming = side == ClassSide::Prototype && layout.classData().guardsOfNamesAwaitBeginning && !guardsHaveBegun(layout);
    if (countsBehindArming)
        m_classesThatCountGuardsOfNamesBehindArming.set(m_tables.rankOfClass(layout.index));
    auto countWhatDoesNotHold = makeScopeExit([&] {
        if (!countsBehindArming)
            return;
        m_classesThatCountGuardsOfNamesBehindArming.clear(m_tables.rankOfClass(layout.index));
        countGuardsOfNamesThatDoNotHold(layout);
    });
    beginGuards(layout);
    if (classHolderReportsAsPrototype(*this, layout, side)) {
        holder->didBecomePrototype(vm);
        if (side == ClassSide::Static)
            m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
    }
    ASSERT(!isDefinedByClass(layout) || !holder->structure()->hasPolyProto());
    reportReplacementsOnHolder(vm, layout, side, holder, nullptr);
    auto entry = m_classSidesByHolder.add(holder, encodeSideOfClassHolder(layout, side));
    bool isEnteredForThisSide = entry.iterator->value == encodeSideOfClassHolder(layout, side);
    if (entry.isNewEntry)
        m_writes.recordAll();
    if (hasNoGuardLeft(layout))
        return;
    forgetWhatCreatedFieldsHide();
    if (!entry.isNewEntry && isEnteredForThisSide && holderState == HolderState::NotTouched)
        return;

    MethodTableUpdate update(*this);
    bool runTimeSeesEveryEvaluation = isDefinedByClass(layout) || layout.evaluation() == EvaluationState::ProvenOnce || side == ClassSide::Prototype;
    bool isOfFirstEvaluation = layout.evaluation() != EvaluationState::SeenTwice && registeredHolderOf(vm, m_state, layout, side) == holder;
    if (isEnteredForThisSide && isOfFirstEvaluation && !runTimeSeesEveryEvaluation) {
        endClassGuard(m_state, layout, side);
        return;
    }
    if (!isEnteredForThisSide || !isOfFirstEvaluation) {
        disarmMemberGuards(layout);
        return;
    }
    if (hasNoGuardLeft(layout, side))
        return;

    bool isMadeAsDeclared = holderState == HolderState::AsDefinitionMadeIt && hasOneClassObject(layout);
#if ASSERT_ENABLED
    if (isMadeAsDeclared) {
        NamesThatDifferOnClassHolders namesOfNewHolder;
        ASSERT(stateOfClassHolder(vm, m_state, layout, side, holder, namesOfNewHolder) == StateOfClassHolder::AsDeclared);
        ASSERT(namesOfNewHolder.numbers.isEmpty() && !namesOfNewHolder.someHasNoNumber);
    }
#endif
    NamesThatDifferOnClassHolders namesThatDiffer;
    StateOfClassHolder stateOfHolder = isMadeAsDeclared ? StateOfClassHolder::AsDeclared : stateOfClassHolder(vm, m_state, layout, side, holder, namesThatDiffer);
    if (stateOfHolder == StateOfClassHolder::Differs) {
        endClassGuard(m_state, layout, side);
        return;
    }
    NamesThatDifferOnClassHolders namesThatDifferAbove;
    if (!hasOtherParent(layout, side) && !hasParentNamedByClassRecord(vm, m_state, layout, side, holder, namesThatDifferAbove)) {
        endGuardsOfInheritedMembers(m_state, layout, side);
        namesThatDifferAbove = { };
    }

    if (std::optional<uint32_t> constructorKey = identifiersOf(m_state.instance).numberOf(vm.propertyNames->constructor.impl()); constructorKey && side == ClassSide::Prototype && !isMadeAsDeclared && !isDefinedByClass(layout) && !holdsClassObjectUnderConstructor(vm, m_state, layout, holder))
        namesThatDiffer.add(constructorKey);
    addNamesThatLookupsComeToAbove(layout, side, namesThatDifferAbove, namesThatDiffer);
    Vector<uint32_t, 4> lateMembers;
    Vector<uint32_t, 4> namesThatEnd;
    for (uint32_t name : namesThatDiffer.numbers)
        (isLateMember(vm, layout, side, holder, name) ? lateMembers : namesThatEnd).append(name);
    for (uint32_t name : lateMembers)
        endClassGuardsOfName(m_state, layout, side, name);
    addLateMembers(layout, lateMembers.span());
    for (uint32_t name : namesThatEnd)
        endClassGuardsOfName(m_state, layout, side, name);
    for (uint32_t name : namesThatDiffer.numbersOfPlainValues)
        endClassGuardsOfName(m_state, layout, side, name, GuardsOfNameToEnd::StoresStillPass);
    if (namesThatDiffer.someHasNoNumber)
        endClassGuardsOfName(m_state, layout, side, std::nullopt);
    if (namesThatDiffer.someSeesAccesses && m_classWhoseModuleEnds != &layout)
        didLayWhatSeesAccesses(vm, layout, side);
    if (stateOfHolder == StateOfClassHolder::MayStillBeDefined) {
        clearAssignedMembers(layout, side);
        return;
    }

    updateAssignedMembers(layout, side);
    storeGuards(m_state, layout, side, standsBelowOtherParent(layout, side) ? noClassGuardBit : layout.classData().guardBit[static_cast<size_t>(side)], true, [&](uint32_t numberOfBit) {
        return !m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(numberOfBit));
    });
}

bool ClassTable::holdersAreLeftOut(const LayoutData& layout)
{
    ASSERT(isDefinedByClass(layout));
    if (layout.evaluation() == EvaluationState::SeenTwice)
        disarmMemberGuards(layout);
    return everyGuardHasEnded(layout);
}

bool ClassTable::tryLeaveHoldersOut(VM& vm, const LayoutData& layout, Closure* classObject, JSObject* prototype)
{
    if (!holdersAreLeftOut(layout) || prototype->type() != FinalObjectType)
        return false;
    if (!m_classObjectOfHolderLeftOut)
        m_classObjectOfHolderLeftOut = PrivateSymbolImpl::create(*StringImpl::create("class"_span8).ptr());
    prototype->putDirect(vm, PropertyName(m_classObjectOfHolderLeftOut.get()), classObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    prototype->didBecomePrototype(vm);
    m_hasHoldersLeftOut = true;
    return true;
}

void ClassTable::nameClassObjectOnPrototypeMadeLeftOut(VM& vm, const LayoutData& layout, Closure* classObject, JSObject* prototype)
{
    PropertyOffset offset = prototype->structure()->get(vm, PropertyName(m_classObjectOfHolderLeftOut.get()));
    RELEASE_ASSERT(isValidOffset(offset));
    prototype->putDirectOffset(vm, offset, classObject);
    didLeaveOutHoldersOfEvaluation(layout, prototype);
}

const LayoutData* ClassTable::classOfHolderLeftOut(JSObject* holder, ClassSide& side) const
{
    VM& vm = *m_state.instance.vm;
    auto* classObject = dynamicDowncast<Closure>(holder);
    side = classObject ? ClassSide::Static : ClassSide::Prototype;
    if (!classObject) {
        if (holder->type() != FinalObjectType)
            return nullptr;
        JSValue named = holder->getDirect(vm, PropertyName(m_classObjectOfHolderLeftOut.get()));
        classObject = named ? dynamicDowncast<Closure>(named.asCell()) : nullptr;
        if (!classObject)
            return nullptr;
    }
    if (&classObject->state() != &m_state)
        return nullptr;
    const LayoutData* layout = classObject->classData();
    if (!layout || !isDefinedByClass(*layout) || !everyGuardHasEnded(*layout))
        return nullptr;
    JSValue prototype = classObject->getDirect(vm, vm.propertyNames->prototype);
    if (!prototype || (side == ClassSide::Prototype && prototype != JSValue(holder)))
        return nullptr;
    return layout;
}

void ClassTable::disarmMemberGuards(const LayoutData& classLayout)
{
    MethodTableUpdate update(*this);
    endClassGuard(m_state, classLayout, ClassSide::Prototype);
    endClassGuard(m_state, classLayout, ClassSide::Static);
}

void ClassTable::didChangeMember(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid)
{
    removeCheckedHolder(layout, side);
    if (hasNoGuardLeft(layout, side))
        return;
    if (side == ClassSide::Prototype || uid == m_state.instance.vm->propertyNames->prototype.impl())
        forgetWhatCreatedFieldsHide();
    if (m_memberBeingDefined.classLayout && m_memberBeingDefined.classLayout->index == layout.index && m_memberBeingDefined.side == side && m_memberBeingDefined.uid == uid)
        return;
    if (!layout.classData().guardsHoldWhileClear && !isDefinedByClass(layout) && !classPrototypeIfExists(layout))
        return;
    const CommonIdentifiers& names = *m_state.instance.vm->propertyNames;
    if (side == ClassSide::Static && (uid == names.name.impl() || uid == names.length.impl()) && !hasStaticMethodNamed(layout, uid))
        return;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (side == ClassSide::Static && key && m_state.objects->layouts.staticField(layout, *key) && !m_state.objects->layouts.staticMethod(layout, *key))
        return;
    if (side == ClassSide::Prototype && key && !m_lateMembers.isEmpty() && tryArmLateMember(*m_state.instance.vm, layout, uid, *key)) [[unlikely]]
        return;
    endClassGuardsOfName(m_state, layout, side, key);
}

bool ClassTable::isLateMember(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, uint32_t key) const
{
    if (side != ClassSide::Prototype || isDefinedByClass(layout) || isClassOfCreatedObjects(layout) || isCompiledObject(holder))
        return false;
    if (!declaresMember(layout, side, key) || !subclassesInClassRecord(m_tables, layout).empty() || holder->getDirect(vm, identifiersOf(m_state.instance).uid(key)))
        return false;
    return std::ranges::none_of(guardsOfNameInClassRecord(m_tables, layout, side, key), [&](const NamedClassGuardData& guard) {
        return m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(guard.numberOfBit()));
    });
}

void ClassTable::addLateMembers(const LayoutData& layout, std::span<const uint32_t> names)
{
    for (uint32_t name : names) {
        if (!m_lateMembers.add(lateMemberKey(layout, name)).isNewEntry)
            continue;
        for (const NamedClassGuardData& guard : guardsOfNameInClassRecord(m_tables, layout, ClassSide::Prototype, name))
            ++m_lateMembersOfGuards.add(guard.numberOfBit(), 0).iterator->value;
    }
}

bool ClassTable::tryArmLateMember(VM& vm, const LayoutData& layout, UniquedStringImpl* uid, uint32_t key)
{
    JSObject* holder = registeredHolderOf(vm, m_state, layout, ClassSide::Prototype);
    if (!m_lateMembers.remove(lateMemberKey(layout, key)) || !holder || !holdsMemberOfRecordUnder(vm, layout, ClassSide::Prototype, holder, uid))
        return false;
    MethodTableUpdate update(*this);
    m_state.instance.propertyCaches.forgetReplacedMembers();
    for (const NamedClassGuardData& guard : guardsOfNameInClassRecord(m_tables, layout, ClassSide::Prototype, key)) {
        auto awaited = m_lateMembersOfGuards.find(guard.numberOfBit());
        if (awaited == m_lateMembersOfGuards.end() || --awaited->value)
            continue;
        m_lateMembersOfGuards.remove(awaited);
        m_endedGuardsOfNames.remove(byteOffsetOfClassGuardBit(guard.numberOfBit()));
        ClassGuardWriter(m_state, layout, ClassSide::Prototype, ClassGuardRows(m_state, guard.numberOfBit(), guard.numberOfBit())).storeNameGuard(guard.numberOfBit(), true);
    }
    return true;
}

void ClassTable::holdDefinitionToNameInRecord(const LayoutData& layout, ClassSide side, uint32_t index, UniquedStringImpl* uid, const JSObject* function) const
{
    const MethodData* row = findRowOfMemberInClassRecord(layout, side, index, function);
    if (!row || (row->flags & methodHasComputedName))
        return;
    if (std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid); !key || row->name != *key) {
        RuntimeCheckFailure failure("object mutation check failed"_s);
        failure.detail("a member of ", reportedClassName(layout), " was defined under another key than the compiler recorded");
        failure.report();
        CRASH_WITH_INFO(layout.index, row->name);
    }
}

namespace {

enum class HolderMutation : uint8_t { ReplaceMember, DeleteMember, AddName, ChangePrototype };

} // anonymous namespace

static ASCIILiteral nameOfHolderMutation(HolderMutation mutation)
{
    switch (mutation) {
    case HolderMutation::ReplaceMember:
        return "ReplaceMember"_s;
    case HolderMutation::DeleteMember:
        return "DeleteMember"_s;
    case HolderMutation::AddName:
        return "AddProperty"_s;
    case HolderMutation::ChangePrototype:
        return "ChangePrototype"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

[[noreturn]] static void crashOnFalseProofOfClassHolder(const LayoutData& classLayout, ClassSide side, HolderMutation mutation)
{
    RuntimeCheckFailure failure("object mutation check failed"_s);
    failure.detail(nameOfHolderMutation(mutation), " on the ", side == ClassSide::Prototype ? "prototype"_s : "constructor"_s, " of ", reportedClassName(classLayout), ", but the compiler proved that this never happens");
    failure.report();
    CRASH_WITH_INFO(classLayout.index, static_cast<uint32_t>(side), static_cast<uint32_t>(mutation));
}

static void throwChangeOfClassHolderTakenBack(VM& vm, State& state, HolderMutation mutation)
{
    auto change = [&]() -> ASCIILiteral {
        switch (mutation) {
        case HolderMutation::ReplaceMember:
            return "replace this member of the class"_s;
        case HolderMutation::DeleteMember:
            return "delete this member of the class"_s;
        case HolderMutation::AddName:
            return "add this name to the class"_s;
        case HolderMutation::ChangePrototype:
            return "change what the class extends"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto owesNoCheck = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    {
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!scope.exception())
            throwTypeError(state.instance.globalObject, scope, changeThatCompiledCodeRulesOutMessage(change()));
    }
    RELEASE_ASSERT(owesNoCheck.exception());
}

static bool takeBackChangeOfMember(VM& vm, State& state, JSObject* holder, UniquedStringImpl* uid, HolderMutation mutation, const ClassTable::DeclaredMember& declared)
{
    if (!isAskedByCodeThatInspectorBrought(vm, state.instance))
        return false;
    JSGlobalObject* realm = state.instance.globalObject;
    if (mutation == HolderMutation::AddName) {
        VM::DeletePropertyModeScope removesWhatIsNotConfigurable(vm, VM::DeletePropertyMode::IgnoreConfigurable);
        DeletePropertySlot slot;
        if (!holder->methodTable()->deleteProperty(holder, realm, PropertyName(uid), slot))
            return false;
        throwChangeOfClassHolderTakenBack(vm, state, mutation);
        return true;
    }
    for (const MethodData* row : { declared.method, declared.getter, declared.setter }) {
        if (row && !FunctionID(row->function))
            return false;
    }
    auto closureOf = [&](const MethodData* row) -> JSObject* {
        return row ? asObject(state.objects->closures.closure(vm, FunctionID(row->function))) : nullptr;
    };
    if (declared.method)
        holder->putDirect(vm, PropertyName(uid), closureOf(declared.method), static_cast<unsigned>(PropertyAttribute::DontEnum));
    else
        holder->putDirectAccessor(realm, PropertyName(uid), GetterSetter::create(vm, realm, closureOf(declared.getter), closureOf(declared.setter)), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
    throwChangeOfClassHolderTakenBack(vm, state, mutation);
    return true;
}

static bool holdsFunctionsOfMemberInClassRecord(JSValue value, unsigned attributes, const ClassTable::DeclaredMember& member)
{
    auto isFunctionOfRow = [](const JSCell* closure, const MethodData& row) {
        return !FunctionID(row.function) || isClosureOfMethodInClassRecord(closure, row);
    };
    if (!value.isCell())
        return false;
    if (member.method)
        return !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && isFunctionOfRow(value.asCell(), *member.method);
    auto* accessor = dynamicDowncast<GetterSetter>(value.asCell());
    if (!accessor)
        return false;
    bool holdsGetter = member.getter ? isFunctionOfRow(accessor->getter(), *member.getter) : accessor->isGetterNull();
    bool holdsSetter = member.setter ? isFunctionOfRow(accessor->setter(), *member.setter) : accessor->isSetterNull();
    return holdsGetter && holdsSetter;
}

static bool isHolderOfSideOfClass(const ClassTable& classes, JSObject* object, const LayoutData& classLayout, ClassSide side)
{
    ClassSide sideHeld;
    if (const LayoutData* held = classes.classHeldBy(object, sideHeld))
        return held->index == classLayout.index && sideHeld == side;
    const LayoutData* classOfLazyHolder = nullptr;
    switch (side) {
    case ClassSide::Prototype:
        if (auto* prototype = dynamicDowncast<ClassPrototype>(object))
            classOfLazyHolder = classes.classOf(*prototype);
        break;
    case ClassSide::Static:
        if (auto* classObject = dynamicDowncast<Closure>(object))
            classOfLazyHolder = classObject->classData();
        break;
    }
    return classOfLazyHolder && classOfLazyHolder->index == classLayout.index;
}

bool ClassTable::nobodyHearsOfValueUnder(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid) const
{
    const CommonIdentifiers& names = *m_state.instance.vm->propertyNames;
    if (uid->isSymbol() || uid == names.prototype.impl() || uid == names.name.impl() || uid == names.length.impl() || parseIndex(PropertyName(uid)))
        return false;
    if (!hasNoGuardLeft(layout, side) && !m_sidesWhereNamesEnded.get(indexOfGuard(layout, side)))
        return false;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (key && someGuardMayName(layout, side, *key))
        return false;
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        if (hasComputedMemberNames(*current) || (key && declaresMember(*current, side, *key)))
            return false;
    }
    return true;
}

bool ClassTable::everyGuardOfStaticNameHasEnded(const LayoutData& layout, uint32_t key) const
{
    if (!subclassesInClassRecord(m_tables, layout).empty())
        return m_endedNames.contains(static_cast<uint64_t>(indexOfGuard(layout, ClassSide::Static)) << 32 | key);
    return std::ranges::all_of(guardsOfNameInClassRecord(m_tables, layout, ClassSide::Static, key), [&](const NamedClassGuardData& guard) {
        return m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(guard.numberOfBit()));
    });
}

bool ClassTable::someGuardMayName(const LayoutData& layout, ClassSide side, uint32_t key) const
{
    if (!m_state.tables.someClassGuardNames(key))
        return false;
    bool mayBeNamed = !subclassesInClassRecord(m_tables, layout).empty() || !guardsOfNameInClassRecord(m_tables, layout, side, key).empty();
    if (mayBeNamed && (side != ClassSide::Static || !everyGuardOfStaticNameHasEnded(layout, key)))
        return true;
    return std::ranges::contains(assignedMembersOf(m_tables, layout, side), key, &AssignedMemberData::name);
}

bool ClassTable::dependsOnValueUnder(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid) const
{
    const CommonIdentifiers& names = *m_state.instance.vm->propertyNames;
    if (uid->isSymbol() || uid == names.constructor.impl() || uid == names.underscoreProto.impl() || parseIndex(PropertyName(uid)))
        return true;
    if (side == ClassSide::Static && (uid == names.prototype.impl() || uid == names.name.impl() || uid == names.length.impl()))
        return true;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (key && m_state.tables.someClassGuardNames(*key) && (!subclassesInClassRecord(m_tables, layout).empty() || !guardsOfNameInClassRecord(m_tables, layout, side, *key).empty()))
        return true;
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        if (hasComputedMemberNames(*current) || (key && declaresMember(*current, side, *key)))
            return true;
    }
    return false;
}

void ClassTable::reportReplacementsOnHolder(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uidOrNull)
{
    Object* compiled = compiledObjectOrNull(holder);
    JSObject* extra = compiled && compiled->mayBePrototype() ? extraPropertiesIfCreated(*compiled) : nullptr;
    if (!extra || compiled->compiledStructure().reportsReplacements())
        return;
    bool isReported = false;
    if (uidOrNull)
        isReported = isValidOffset(extra->structure()->get(vm, uidOrNull)) && dependsOnValueUnder(layout, side, uidOrNull);
    else {
        extra->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            isReported |= dependsOnValueUnder(layout, side, entry.key());
            return !isReported;
        });
    }
    if (isReported)
        startReportingReplacements(vm, holder);
}

bool ClassTable::addsStaticUnobserved(VM& vm, const Closure& classObject, const LayoutData& layout, Structure* before, UniquedStringImpl* uid) const
{
    if (!hasNoGuardLeft(layout, ClassSide::Static) || uid->isSymbol() || isImplicitMemberOfClassHolder(vm, ClassSide::Static, uid) || parseIndex(PropertyName(uid)) || !closureAdmitsName(vm, &classObject, uid))
        return false;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        if (hasComputedMemberNames(*current) || (key && declaresMember(*current, ClassSide::Static, *key)))
            return false;
    }
    JSValue prototype = classObject.getDirect(vm, vm.propertyNames->prototype);
    if (!prototype || !prototype.isObject() || !before->storedPrototype().isObject())
        return false;
    JSObject* parent = asObject(before->storedPrototype());
    JSValue parentOfPrototype = asObject(prototype)->getPrototypeDirect();
    JSCell* parentOfPrototypeCell = parentOfPrototype.isCell() ? parentOfPrototype.asCell() : nullptr;
    const ClassStructureCache& cache = m_state.objects->caches->classStructures;
    ClassStructures kept = cache.find(layout.index, parent, parentOfPrototypeCell);
    if (!kept && parent == m_state.instance.globalObject->functionPrototype())
        kept = cache.find(layout.index, nullptr, parentOfPrototypeCell);
    if (!kept || sharesStructureOfRootClasses(kept.classObjectStructure))
        return false;
    for (Structure* structure = before; structure; structure = structure->previousID()) {
        if (structure == kept.classObjectStructure)
            return true;
    }
    return false;
}

template<typename Functor>
void ClassTable::forEachMethodNamed(const LayoutData& layout, ClassSide side, uint32_t key, const Functor& functor) const
{
    std::span<const MethodData> methods = methodsInClassRecord(layout, side);
    constexpr size_t minMethodsToIndex = 16;
    if (methods.size() < minMethodsToIndex) {
        for (const MethodData& method : methods) {
            if (isNamedInClassRecord(method, key))
                functor(method);
        }
        return;
    }
    auto nameOf = [&](uint32_t index) {
        return static_cast<uint32_t>(methods[index].name);
    };
    const Vector<uint32_t>& indices = m_methodsByName.ensure(encodeSideOfClassHolder(layout, side), [&] {
        Vector<uint32_t> sorted;
        for (uint32_t index = 0; index < methods.size(); ++index) {
            if (!(methods[index].flags & methodHasComputedName))
                sorted.append(index);
        }
        sortKeepingEqualElementsInOrder(sorted, std::ranges::less { }, nameOf);
        return sorted;
    }).iterator->value;
    for (uint32_t index : std::ranges::equal_range(indices, key, { }, nameOf))
        functor(methods[index]);
}

bool ClassTable::memberMayBeOverwritten(const LayoutData& layout, ClassSide side, uint32_t key) const
{
    bool mayBeStoredOver = false;
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        forEachMethodNamed(*current, side, key, [&](const MethodData& method) {
            mayBeStoredOver |= !!(method.flags & methodMayBeStoredOver);
        });
    }
    return mayBeStoredOver;
}

void ClassTable::noteReplacedMember(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid)
{
    if (side != ClassSide::Prototype || vm.aotState().instances.size() != 1 || isCompiledObject(holder) || uid->isSymbol() || parseIndex(PropertyName(uid)) || isImplicitMemberOfClassHolder(vm, side, uid) || !m_lateMembers.isEmpty())
        return;
    if ((!hasNoGuardLeft(layout, side) && !m_sidesWhereNamesEnded.get(indexOfGuard(layout, side))) || m_memberBeingDefined.classLayout || m_classDefinedFromRecord)
        return;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (!key || hasComputedMemberNames(layout) || (layout.classData().isChangedInSightAlone(side) && !memberMayBeOverwritten(layout, side, *key)))
        return;
    Structure* structure = holder->structure();
    if (structure->isDictionary() || structure->isCompiledStructure() || structure->isImmutableProperty(uid) || isWriteJudgedByWriter(structure, uid))
        return;
    auto put = structure->classInfoForCells()->methodTable.put;
    if (put != ClassPrototype::info()->methodTable.put && put != JSObject::info()->methodTable.put)
        return;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset) || seesAccesses(vm, uid, attributes))
        return;
    uint64_t member = static_cast<uint64_t>(indexOfGuard(layout, side)) << 32 | *key;
    if (m_membersReplacedOnce.add(member).isNewEntry)
        return;
    if (m_membersReplacedAgain.add(member).isNewEntry)
        updateAssignedMembers(layout, side, *key);
    ReplacedMember& place = m_state.instance.propertyCaches.replacedMembers[*key & (ReplacedMember::numberOfPlaces - 1)];
    place = { holder, offsetRelativeToBase(offset), structure->id().bits(), *key, 0 };
}

void ClassTable::holdMemberToProof(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid)
{
    if (!layout.classData().isChangedInSightAlone(side) || vm.aotState().nativeTestChangesObjectsUnseen)
        return;
    if (m_classDefinedFromRecord && m_classDefinedFromRecord->index == layout.index)
        return;
    if (m_memberBeingDefined.classLayout && m_memberBeingDefined.classLayout->index == layout.index && m_memberBeingDefined.side == side && m_memberBeingDefined.uid == uid)
        return;
    if ((uid->isSymbol() && static_cast<SymbolImpl*>(uid)->isPrivate()) || parseIndex(PropertyName(uid)) || isImplicitMemberOfClassHolder(vm, side, uid) || isCompiledObject(holder))
        return;

    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    bool isLaidFirst = false;
    auto findRowsIn = [&](const LayoutData& classLayout, DeclaredMember& declared, bool& mayBeStoredOver) {
        bool isDeclared = false;
        if (!key)
            return isDeclared;
        forEachMethodNamed(classLayout, side, *key, [&](const MethodData& method) {
            isDeclared = true;
            declared.define(method);
            mayBeStoredOver |= !!(method.flags & methodMayBeStoredOver);
            isLaidFirst |= &classLayout == &layout && method.flags & methodIsStoredFirst;
        });
        return isDeclared;
    };
    DeclaredMember declared;
    auto stopUnlessStaticFieldMayAnswer = [&](HolderMutation mutation) {
        for (const LayoutData* current = side == ClassSide::Static ? &layout : nullptr; current; current = assumedBaseOf(*current)) {
            if (staticField(*current, uid))
                return;
        }
        if (takeBackChangeOfMember(vm, m_state, holder, uid, mutation, declared))
            return;
        crashOnFalseProofOfClassHolder(layout, side, mutation);
    };
    auto isAddedToThisPrototype = [&] {
        if (side == ClassSide::Static)
            return false;
        return !layout.classData().namesAreLaidUnderWrittenKeysAlone || (key && std::ranges::binary_search(programTables().addedPrototypeNames(), AddedPrototypeNameData { layout.index, *key }));
    };

    unsigned attributes = 0;
    JSValue value = holder->getDirect(vm, PropertyName(uid), attributes);
    bool mayBeStoredOver = false;
    bool isDeclaredHere = findRowsIn(layout, declared, mayBeStoredOver);
    if (!isDeclaredHere && (!value || isAddedToThisPrototype()))
        return;
    bool isDeclaredInRow = isDeclaredHere;
    for (const LayoutData* above = assumedBaseOf(layout); above && !isDeclaredInRow; above = assumedBaseOf(*above))
        isDeclaredInRow = findRowsIn(*above, declared, mayBeStoredOver);
    if (mayBeStoredOver)
        return;

    if (!isDeclaredInRow) {
        bool isOwnHasInstance = layout.classData().classObjectLacksHasInstance && uid == vm.propertyNames->hasInstanceSymbol.impl();
        if ((side == ClassSide::Prototype || isOwnHasInstance) && !holdsWhatIsFoundAbove(vm, holder, uid, value, attributes))
            stopUnlessStaticFieldMayAnswer(HolderMutation::AddName);
        return;
    }
    HolderMutation mutation = !isDeclaredHere ? HolderMutation::AddName : value ? HolderMutation::ReplaceMember : HolderMutation::DeleteMember;
    if (!value) {
        if (isLaidFirst && m_classesWithObjects.get(m_tables.rankOfClass(layout.index)))
            stopUnlessStaticFieldMayAnswer(mutation);
        if (!isDefinedByClass(layout) || isLazyClassHolder(holder, side))
            return;
        value = findOnChainAbove(vm, holder, uid, attributes);
    }
    if (!value || !holdsFunctionsOfMemberInClassRecord(value, attributes, declared))
        stopUnlessStaticFieldMayAnswer(mutation);
}

void ClassTable::holdMethodsStoredFirst(VM& vm, const LayoutData& instanceLayout, JSValue prototypeOfInstances)
{
    if (!instanceLayout.isClass() || vm.aotState().nativeTestChangesObjectsUnseen)
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    JSObject* prototype = isDefinedByClass(layout) ? nullptr : classPrototypeIfExists(layout);
    if (!prototype || prototypeOfInstances != JSValue(prototype) || !m_classesWithObjects.add(m_tables.rankOfClass(layout.index)))
        return;
    for (const MethodData& method : methodsInClassRecord(layout, ClassSide::Prototype)) {
        if (!(method.flags & methodIsStoredFirst))
            continue;
        DeclaredMember declared;
        declared.define(method);
        unsigned attributes = 0;
        JSValue value = prototype->getDirect(vm, PropertyName(identifiersOf(m_state.instance).uid(method.name)), attributes);
        if (!value || !holdsFunctionsOfMemberInClassRecord(value, attributes, declared))
            crashOnFalseProofOfClassHolder(layout, ClassSide::Prototype, value ? HolderMutation::ReplaceMember : HolderMutation::DeleteMember);
    }
}

void ClassTable::holdLinkToProof(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    if (!layout.classData().isChangedInSightAlone(side) || vm.aotState().nativeTestChangesObjectsUnseen)
        return;
    if ((m_classDefinedFromRecord && m_classDefinedFromRecord->index == layout.index) || isCompiledObject(holder))
        return;
    JSGlobalObject* realm = m_state.instance.globalObject;
    JSObject* aboveEveryClass = side == ClassSide::Prototype ? static_cast<JSObject*>(realm->objectPrototype()) : static_cast<JSObject*>(realm->functionPrototype());
    JSObject* parent = holder->getPrototypeDirect().getObject();
    const LayoutData* above = assumedBaseOf(layout);
    bool linkIsWrittenByClasses = isDefinedByClass(layout) && (above ? isDefinedByClass(*above) : layout.assumedBase() == LayoutData::noBase);
    if (linkIsWrittenByClasses && (above ? !parent || !isHolderOfSideOfClass(*this, parent, *above, side) : parent != aboveEveryClass)) {
        JSObject* written = above ? registeredHolderOf(vm, m_state, *above, side) : aboveEveryClass;
        if (written && isAskedByCodeThatInspectorBrought(vm, m_state.instance)) {
            holder->setPrototypeDirect(vm, written);
            throwChangeOfClassHolderTakenBack(vm, m_state, HolderMutation::ChangePrototype);
            return;
        }
        crashOnFalseProofOfClassHolder(layout, side, HolderMutation::ChangePrototype);
    }
}

void ClassTable::didDefineMember(VM& vm, const LayoutData& layout, ClassSide side, uint32_t index, JSObject* holder, UniquedStringImpl* uid, JSObject* function)
{
    removeCheckedHolder(layout, side);
    if (hasNoGuardLeft(layout))
        return;
    forgetWhatCreatedFieldsHide();
    if (!m_classSidesByHolder.contains(holder))
        armMemberGuards(vm, layout, side, holder);
    if (hasNoGuardLeft(layout, side))
        return;
    if (registeredHolderOf(vm, m_state, layout, side) != holder) {
        endClassGuard(m_state, layout, side);
        return;
    }
    if (!definesMemberOfClassRecord(m_state, layout, side, index, uid, function)) {
        endClassGuardsOfName(m_state, layout, side, identifiersOf(m_state.instance).numberOf(uid));
        return;
    }
    if (!layout.classData().guardsHoldWhileClear && isLastMemberCreatedAtDefinition(layout, side, function))
        armMemberGuards(vm, layout, side, holder);
}

void ClassTable::didChangeEveryMember(const LayoutData& layout, ClassSide side)
{
    removeCheckedHolder(layout, side);
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuard(m_state, layout, side);
}

void ClassTable::didFreezeHolder(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    removeCheckedHolder(layout, side);
    if (hasNoGuardLeft(layout, side))
        return;
    forgetWhatCreatedFieldsHide();
    if (!layout.classData().guardsHoldWhileClear && !isDefinedByClass(layout) && !classPrototypeIfExists(layout))
        return;
    if (isCompiledObject(holder)) {
        endClassGuard(m_state, layout, side);
        return;
    }
    MethodTableUpdate update(*this);
    holder->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(entry.key()))
            endClassGuardsOfName(m_state, layout, side, key, GuardsOfNameToEnd::ReadsStillFind);
        return true;
    });
}

bool ClassTable::seesAccesses(VM& vm, UniquedStringImpl* uid, unsigned attributes)
{
    return (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) || uid == vm.propertyNames->underscoreProto.impl();
}

bool ClassTable::holdsMemberOfRecordUnder(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid) const
{
    if (m_memberBeingDefined.classLayout && m_memberBeingDefined.classLayout->index == layout.index && m_memberBeingDefined.side == side && m_memberBeingDefined.uid == uid)
        return true;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (!key || compiledObjectOrNull(holder))
        return false;
    DeclaredMember member;
    forEachMethodNamed(layout, side, *key, [&](const MethodData& method) {
        member.define(method);
    });
    unsigned attributes = 0;
    JSValue value = holder->getDirect(vm, uid, attributes);
    return (member.method || member.isAccessor()) && holdsMemberOfClassRecord(m_state, value, attributes, member);
}

static std::optional<uint32_t> numberOfWordOnWhatSeesAccesses(VM& vm, State& state)
{
    Identifier word = Identifier::fromString(vm, "what sees a read or a store above"_s);
    return identifiersOf(state.instance).numberOf(word.impl());
}

static FunctionID functionOfRowThatIsNotMadeYet(State& state, const LayoutData& layout)
{
    const ClassTable& classes = state.objects->classes;
    for (const LayoutData* current = &layout; current && !classes.isDefinedByClass(*current) && !ClassTable::isClassOfCreatedObjects(*current) && !classes.classPrototypeIfExists(*current); current = classes.assumedBaseOf(*current)) {
        FunctionID constructor(current->classData().classConstructor);
        ASSERT(!state.tables.kindOfFunction(constructor.index()).isSlim);
        bool statementsSetItUp = !methodsInClassRecord(*current, ClassSide::Prototype).empty() || classes.assumedBaseOf(*current);
        if (statementsSetItUp && classes.hasOneClassObject(*current) && !state.objects->closures.closureIfCreated(constructor))
            return constructor;
    }
    return { };
}

void ClassTable::letGuardsBeAskedForAgain(FunctionID made)
{
    for (const LayoutData* layout : m_classesThatWaitForFunctions.take(made.index())) {
        if (BitOfClassGuard guardOfPrototypeSide = bitOfClassGuard(m_state, *layout, ClassSide::Prototype); guardOfPrototypeSide.byte)
            *guardOfPrototypeSide.byte &= static_cast<uint8_t>(~maskOfClassGuardsBegun);
    }
}

void ClassTable::beginGuardsAheadOfFirstObject(VM& vm, const LayoutData& layout)
{
    BitOfClassGuard guardOfPrototypeSide = bitOfClassGuard(m_state, layout, ClassSide::Prototype);
    if (FunctionID awaited = functionOfRowThatIsNotMadeYet(m_state, layout)) {
        SetForScope beginsGuardsOfFunctionsNotMadeYet(m_beginsGuardsOfFunctionsNotMadeYet, true);
        beginGuards(layout);
        if (!guardOfPrototypeSide.byte || m_tables.kindOfFunction(FunctionID(layout.classData().classConstructor).index()).isInRuntimeModule)
            return;
        if (!(*guardOfPrototypeSide.byte & maskOfClassGuardsBegun))
            m_classesThatWaitForFunctions.add(awaited.index(), Vector<const LayoutData*, 1> { }).iterator->value.append(&layout);
    } else
        classPrototypeOfInstances(vm, layout);
    if (guardOfPrototypeSide.byte)
        *guardOfPrototypeSide.byte |= maskOfClassGuardsBegun;
}

void ClassTable::beginGuardsAtEndOfModule(VM& vm, const LayoutData& layout)
{
    if (!m_classesAtOrAboveGuardOfWordOnWhatSeesAccesses) {
        BitVector& classes = m_classesAtOrAboveGuardOfWordOnWhatSeesAccesses.emplace();
        std::optional<uint32_t> word = numberOfWordOnWhatSeesAccesses(vm, m_state);
        if (word && m_state.tables.someClassGuardNames(*word)) {
            m_numberOfWordOnWhatSeesAccesses = word;
            for (const ClassBelowWithGuardOfNameData& listed : std::ranges::equal_range(m_state.tables.classesBelowWithGuardsOfNames(), *word, { }, &ClassBelowWithGuardOfNameData::name)) {
                for (const LayoutData* current = listed.side() == ClassSide::Prototype ? classWithIndex(listed.classLayout()) : nullptr; current; current = assumedBaseOf(*current))
                    classes.set(m_tables.rankOfClass(current->index));
            }
        }
    }
    if (!m_numberOfWordOnWhatSeesAccesses)
        return;
    if (!m_classesAtOrAboveGuardOfWordOnWhatSeesAccesses->get(m_tables.rankOfClass(layout.index)) && guardsOfNameInClassRecord(m_state.tables, layout, ClassSide::Prototype, *m_numberOfWordOnWhatSeesAccesses).empty())
        return;
    SetForScope classWhoseModuleEnds(m_classWhoseModuleEnds, &layout);
    classPrototypeOfInstances(vm, layout);
}

void ClassTable::didLayWhatSeesAccesses(VM& vm, const LayoutData& layout, ClassSide side)
{
    if (hasNoGuardLeft(layout, side))
        return;
    endClassGuardsOfName(m_state, layout, side, numberOfWordOnWhatSeesAccesses(vm, m_state));
}

void ClassTable::didChangePrototypeLink(const LayoutData& layout, ClassSide side)
{
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endGuardsOfInheritedMembers(m_state, layout, side);
}

void ClassTable::didDefineWithOtherParent(const LayoutData& layout, ClassSide side)
{
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endGuardsOfInheritedMembers(m_state, layout, side);
}

bool ClassTable::endGuardsOfShadowedMember(const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    if (!instanceLayout.isClass())
        return true;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (hasNoGuardLeft(layout, ClassSide::Prototype))
        return true;
    VM& vm = *m_state.instance.vm;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    ++m_numberOfWalksOverPrototypeHolders;
    bool everyHolderReportsNewNames = true;
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        switch (findNameOnPrototypeHolder(vm, m_state, *current, uid, key)) {
        case NameOnPrototypeHolder::Found:
            endClassGuardsOfName(m_state, layout, ClassSide::Prototype, key, GuardsOfNameToEnd::OneObjectHidesName);
            return true;
        case NameOnPrototypeHolder::AbsentUntilReported:
            break;
        case NameOnPrototypeHolder::AbsentForNow:
            everyHolderReportsNewNames = false;
            break;
        }
    }
    return everyHolderReportsNewNames;
}

bool ClassTable::mayNameMemberByIndex(const LayoutData& instanceLayout)
{
    if (m_classesWithoutIndexedMembers.contains(instanceLayout.index) || !instanceLayout.isClass())
        return false;
    const LayoutData& layout = classOfInstances(instanceLayout);
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        for (const MethodData& method : methodsInClassRecord(*current, ClassSide::Prototype)) {
            if (method.flags & methodHasComputedName || parseIndex(PropertyName(identifiersOf(m_state.instance).uid(method.name))))
                return true;
        }
    }
    m_classesWithoutIndexedMembers.add(instanceLayout.index);
    return false;
}

void ClassTable::didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    endGuardsOfShadowedMember(instanceLayout, uid);
}

void ClassTable::didShadowMemberSlowCase(const LayoutData& instanceLayout, const FieldData& field)
{
    if (endGuardsOfShadowedMember(instanceLayout, m_state.objects->layouts.nameOf(*m_state.instance.vm, field)))
        m_fieldsWhoseCreationEndsNoGuard.add(numberOfFieldRecord(field));
}

void ClassTable::PagedNumberSet::add(uint32_t number)
{
    uint32_t page = number >> PagedBitSet::logBitsPerPage;
    size_t group = page >> logPagesPerGroup;
    if (group >= m_groups.size())
        m_groups.grow(group + 1);
    if (!m_groups[group])
        m_groups[group] = makeUniqueWithoutFastMallocCheck<Group>();
    uint64_t& bits = (*m_groups[group])[page % pagesPerGroup];
    if (!bits)
        m_pagesInUse.append(page);
    bits |= 1ull << number % PagedBitSet::bitsPerPage;
}

void ClassTable::PagedNumberSet::remove(uint32_t number)
{
    if (contains(number)) {
        uint32_t page = number >> PagedBitSet::logBitsPerPage;
        (*m_groups[page >> logPagesPerGroup])[page % pagesPerGroup] &= ~(1ull << number % PagedBitSet::bitsPerPage);
    }
}

struct ClassTable::OwnerOfGuardsThatHangOnChains final : ChainDependentOwner {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OwnerOfGuardsThatHangOnChains);

    explicit OwnerOfGuardsThatHangOnChains(ClassTable& classes)
        : classes(classes)
    {
    }

    void dropChainDependent(VM&, uint32_t classIndex, UniquedStringImpl* uidOrNull) final { classes.endGuardThatHangsOnChain(classIndex, uidOrNull); }
    bool hasChainDependent(VM&, uint32_t classIndex, UniquedStringImpl* uidOrNull) const final { return classes.guardThatHangsOnChainHolds(classIndex, uidOrNull); }

    ClassTable& classes;
};

ChainDependentOwnerID ClassTable::ownerOfGuardsThatHangOnChains()
{
    if (!m_ownerOfGuardsThatHangOnChains) {
        m_ownerOfGuardsThatHangOnChains = makeUnique<OwnerOfGuardsThatHangOnChains>(*this);
        m_numberOfOwnerOfGuardsThatHangOnChains = m_state.chainDependents->addOwner(*m_ownerOfGuardsThatHangOnChains);
    }
    return m_numberOfOwnerOfGuardsThatHangOnChains;
}

void ClassTable::endGuardThatHangsOnChain(uint32_t classIndex, UniquedStringImpl* uidOrNull)
{
    const LayoutData* classLayout = classWithIndex(classIndex);
    RELEASE_ASSERT(classLayout);
    if (!uidOrNull) {
        endClassGuard(m_state, *classLayout, ClassSide::Prototype);
        return;
    }
    endClassGuardsOfName(m_state, *classLayout, ClassSide::Prototype, identifiersOf(m_state.instance).numberOf(uidOrNull));
}

bool ClassTable::guardThatHangsOnChainHolds(uint32_t classIndex, UniquedStringImpl* uidOrNull) const
{
    const LayoutData* classLayout = classWithIndex(classIndex);
    if (!classLayout || guardHasEnded(*classLayout, ClassSide::Prototype))
        return false;
    std::optional<uint32_t> key = uidOrNull ? identifiersOf(m_state.instance).numberOf(uidOrNull) : std::nullopt;
    if (!key)
        return true;
    return std::ranges::any_of(guardsOfNameInClassRecord(m_tables, *classLayout, ClassSide::Prototype, *key), [&](const NamedClassGuardData& guard) {
        return !m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(guard.numberOfBit()));
    });
}

void ClassTable::didChangeInstancePrototype(VM& vm, const LayoutData& instanceLayout, JSValue prototype)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (hasNoGuardLeft(layout, ClassSide::Prototype))
        return;
    JSObject* holder = registeredHolderOf(vm, m_state, layout, ClassSide::Prototype);
    if (holder && prototype == JSValue(holder))
        return;
    if (!holder && isClassOfCreatedObjects(layout))
        return;
    if (!holder) {
        endClassGuard(m_state, layout, ClassSide::Prototype);
        return;
    }
    if (!m_classesWithInstanceBelowOtherPrototype.add(layout.index).isNewEntry)
        return;
    forgetWhatCreatedFieldsHide();
    MethodTableUpdate update(*this);
    auto guards = guardsOfNamesInClassRecord(m_tables, layout, ClassSide::Prototype);
    auto classDependents = findClassGuardRows(m_state, guards).methodTableDependents;
    for (const NamedClassGuardData& guard : guards) {
        auto dependents = rowsThrough(rowsFrom(classDependents, guard.numberOfBit()), guard.numberOfBit());
        if (!dependents.empty())
            repointMethodTableEntries(m_state, dependents, false);
    }
}

bool ClassTable::takesObjectsLaidBelowPrototype(VM& vm, const LayoutData& instanceLayout, JSObject* firstAbove)
{
    if (!instanceLayout.isClass() || !m_state.chainDependents)
        return false;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (isClassOfCreatedObjects(layout) || guardHasEnded(layout, ClassSide::Prototype))
        return false;
    JSObject* holder = registeredHolderOf(vm, m_state, layout, ClassSide::Prototype);
    if (!holder)
        return false;
    JSGlobalObject* realm = m_state.instance.globalObject;
    firstAbove->didBecomePrototype(vm);
    ChainObjects between;
    for (JSObject* object = firstAbove; object != holder; object = object->getPrototypeDirect().getObject()) {
        if (Object* compiled = object ? compiledObjectOrNull(object) : nullptr) {
            JSObject* extra = extraPropertiesIfCreated(*compiled);
            if (compiled->isInFrame() || compiled->structure()->realm() != realm || !compiled->structure()->mayBePrototype() || (extra && (!flattenOnce(vm, extra) || extra->structure()->isDictionary())))
                return false;
            between.append(object);
            continue;
        }
        if (!object || object->type() != FinalObjectType || !flattenOnce(vm, object))
            return false;
        Structure* structure = object->structure();
        if (structure->realm() != realm || structure->isDictionary() || structure->hasPolyProto() || !structure->mayBePrototype() || hasIndexedProperties(structure->indexingType()))
            return false;
        between.append(object);
    }
    bool mayHideMembers = std::ranges::all_of(between, [&](JSObject* object) {
        unsigned attributes = 0;
        Object* compiled = compiledObjectOrNull(object);
        JSValue constructor = compiled ? compiled->getDirectFromNativeCode(vm, vm.propertyNames->constructor.impl(), &attributes) : object->getDirect(vm, vm.propertyNames->constructor);
        auto* classObject = constructor && constructor.isObject() ? dynamicDowncast<Closure>(asObject(constructor)) : nullptr;
        if (!classObject || Instance::owning(classObject) != &m_state.instance)
            return false;
        const LayoutData* evaluated = classConstructedBy(functionOfOwnClosure(*classObject));
        return !evaluated || (evaluated->hasUntypedBase && (evaluated->assumedBase() == LayoutData::noBase || evaluated->assumedBase() == layout.index));
    });
    Vector<uint32_t, 16> namesBetween;
    Vector<uint32_t, 16> namesOfFieldsBetween;
    bool hidesMember = false;
    auto noteName = [&](UniquedStringImpl* name, Vector<uint32_t, 16>& names) {
        std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(name);
        if (!key)
            return;
        names.append(*key);
        if (mayHideMembers || name == vm.propertyNames->constructor.impl())
            return;
        ChainWalk above = walkChain({ realm, between.last()->structure(), nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::UsedAtOnce }, name);
        hidesMember |= above.result != ChainWalkResult::Absent;
    };
    for (JSObject* object : between) {
        JSObject* listed = object;
        if (Object* compiled = compiledObjectOrNull(object)) {
            for (const FieldData& field : compiled->layout().fields()) {
                if (field.isPrivate() || field.isBrand())
                    continue;
                bool isPlain = !(compiled->attributesOf(field) & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) && !(compiled->orderWord() & orderWordHasFieldOutsideSlot);
                noteName(propertyNameOf(vm, *compiled, field), isPlain ? namesOfFieldsBetween : namesBetween);
            }
            listed = extraPropertiesIfCreated(*compiled);
        }
        if (!listed)
            continue;
        listed->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            noteName(entry.key(), namesBetween);
            return !hidesMember;
        });
    }
    if (hidesMember)
        return false;
    for (uint32_t key : namesOfFieldsBetween) {
        for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current))
            endClassGuardsOfName(m_state, *current, ClassSide::Prototype, key, GuardsOfNameToEnd::StoresStillPass);
    }
    for (uint32_t key : namesBetween) {
        bool mayBeAssigned = isAssignedMemberName(key);
        for (const LayoutData* current = &layout; current; current = mayBeAssigned ? assumedBaseOf(*current) : nullptr) {
            m_namesBelowPrototypes.add(static_cast<uint64_t>(indexOfGuard(*current, ClassSide::Prototype)) << 32 | key);
            updateAssignedMembers(*current, ClassSide::Prototype, key);
            endClassGuardsOfName(m_state, *current, ClassSide::Prototype, key);
        }
    }
    m_state.chainDependents->add(between.span(), ChainDependent { nullptr, layout.index, ownerOfGuardsThatHangOnChains() });
    forgetWhatCreatedFieldsHide();
    return true;
}

bool ClassTable::accessorAboveCreatedObjectsIsWhatRecordNames(VM& vm, const LayoutData& classLayout, Structure* structure, uint32_t name)
{
    const MethodData* getter = nullptr;
    const MethodData* setter = nullptr;
    for (const MethodData& method : methodsInClassRecord(classLayout, ClassSide::Prototype)) {
        if (!isNamedInClassRecord(method, name))
            continue;
        if (method.kind() == MethodKind::Method)
            return false;
        (method.kind() == MethodKind::Getter ? getter : setter) = &method;
    }
    JSGlobalObject* realm = m_state.instance.globalObject;
    const Identifier& identifier = identifiersOf(m_state.instance).identifier(name);
    ChainWalk walk = walkChain({ realm, structure, nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForReads }, identifier.impl());
    if (!walk.dependsOnChain() || walk.passedEmptySlot)
        return false;
    if (!getter && !setter) {
        if (walk.result != ChainWalkResult::Absent)
            return false;
        m_state.chainDependents->add(walk.changeableObjects.span(), ChainDependent { identifier.impl(), classLayout.index, ownerOfGuardsThatHangOnChains() });
        return true;
    }
    if (walk.result != ChainWalkResult::Found || !walk.holder)
        return false;
    PropertySlot slot(walk.holder, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!PropertyWalk::ordinaryGetOwnPropertySlot(realm, walk.holder, identifier, slot) || !slot.isAccessor())
        return false;
    GetterSetter* held = slot.getterSetter();
    auto isHalfOfRecord = [&](bool isNull, JSObject* half, const MethodData* row) {
        return row ? !isNull && functionOf(half) == FunctionID(row->function) : isNull;
    };
    if (!isHalfOfRecord(held->isGetterNull(), held->getter(), getter) || !isHalfOfRecord(held->isSetterNull(), held->setter(), setter))
        return false;
    m_state.chainDependents->add(walk.changeableObjects.span(), ChainDependent { identifier.impl(), classLayout.index, ownerOfGuardsThatHangOnChains() });
    return true;
}

void ClassTable::defineClassOfCreatedObjects(VM& vm, const LayoutData& classLayout, JSObject* parent)
{
    ASSERT(isClassOfCreatedObjects(classLayout) && !classPrototypeIfExists(classLayout));
    registerClassPrototype(classLayout, parent);
    m_state.objects->structures.forgetStructuresOf(classLayout);
    if (!classLayout.shape || guardHasEnded(classLayout, ClassSide::Prototype) || !m_state.chainDependents)
        return;
    std::span<const NamedClassGuardData> guards = guardsOfNamesInClassRecord(m_tables, classLayout, ClassSide::Prototype);
    if (guards.empty())
        return;
    Structure* structure = m_state.objects->structures.objectStructure(vm, classLayout.shape);
    JSGlobalObject* realm = m_state.instance.globalObject;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bitsThatDoNotBegin;
    uint32_t nameAsked = 0;
    bool passes = false;
    for (size_t index = 0; index < guards.size(); ++index) {
        const NamedClassGuardData& guard = guards[index];
        if (!guard.answersStoresAlone()) {
            if (!accessorAboveCreatedObjectsIsWhatRecordNames(vm, classLayout, structure, guard.name))
                bitsThatDoNotBegin.add(guard.numberOfBit());
            continue;
        }
        if (!index || guard.name != nameAsked || !guards[index - 1].answersStoresAlone()) {
            nameAsked = guard.name;
            UniquedStringImpl* uid = identifiersOf(m_state.instance).identifier(guard.name).impl();
            passes = storeCreatesOwnProperty(realm, structure, nullptr, uid, *m_state.chainDependents, ChainDependent { uid, classLayout.index, ownerOfGuardsThatHangOnChains() });
        }
        if (!passes)
            bitsThatDoNotBegin.add(guard.numberOfBit());
    }
    ClassGuardWriter writer(m_state, classLayout, ClassSide::Prototype, findClassGuardRows(m_state, guards));
    writer.storeNameGuards(guards, true, [&](uint32_t numberOfBit) {
        return !bitsThatDoNotBegin.contains(numberOfBit) && !m_endedGuardsOfNames.contains(byteOffsetOfClassGuardBit(numberOfBit));
    });
}

const LayoutData* ClassTable::classHeldBy(JSObject* holder, ClassSide& side) const
{
    auto found = m_classSidesByHolder.find(holder);
    if (found == m_classSidesByHolder.end())
        return m_hasHoldersLeftOut ? classOfHolderLeftOut(holder, side) : nullptr;
    side = static_cast<ClassSide>(found->value & 1);
    return classWithIndex(found->value >> 1);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
