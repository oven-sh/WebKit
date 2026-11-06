/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassGuards.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTEffectsNet.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectPrototype.h"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

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
    return std::ranges::any_of(methodsInClassRecord(classLayout, side), [&](const MethodData& method) {
        return isNamedInClassRecord(method, key);
    });
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
    countEvent(*m_classes.m_state.instance.vm, RuntimeCounterID::MethodTableMadeWritable);
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
    return holds ? Typed::CountInEffectsNet::GuardsOfMembersArmedOrBegun : Typed::CountInEffectsNet::GuardsOfMembersEnded;
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
        uint32_t entry = guardHolds ? compiled[index] : lookups[dependent.selector];
        if (table[index] == entry)
            continue;
        update.willStoreEntry(index);
        table[index] = entry;
        state.countWriteForEffectsNet(countOfChangeOfClassGuard(guardHolds));
    }
}

static void countChangeOfClassGuard(State& state, uint32_t numberOfBit, bool holds)
{
    for (const ClassGuardCountData& count : std::ranges::equal_range(state.tables.classGuardCounts(), numberOfBit, { }, &ClassGuardCountData::guardBit)) {
        RELEASE_ASSERT(static_cast<uint64_t>(count.offsetOfCount) + sizeof(uint32_t) <= state.tables.instanceSizes.sizeOfStatics);
        uint32_t& numberOfGuardsThatDoNotHold = *reinterpret_cast<uint32_t*>(state.instance.statics() + count.offsetOfCount);
        RELEASE_ASSERT(!holds || numberOfGuardsThatDoNotHold);
        if (holds)
            --numberOfGuardsThatDoNotHold;
        else
            ++numberOfGuardsThatDoNotHold;
        state.countWriteForEffectsNet(countOfChangeOfClassGuard(holds));
    }
}

static void storeBitOfClassGuard(State& state, const LayoutData& classLayout, ClassSide side, uint32_t numberOfBit, bool holds)
{
    BitOfClassGuard bit = bitOfClassGuardWithNumber(state, numberOfBit);
    if (!bit.byte)
        return;
    uint8_t byteBefore = *bit.byte;
    if (holds != classLayout.classData().bitsAreClearWhileGuardsHold(side))
        *bit.byte |= bit.mask;
    else
        *bit.byte &= static_cast<uint8_t>(~bit.mask);
    if (*bit.byte != byteBefore) {
        bool isCountedFromBeginning = side == ClassSide::Prototype && classLayout.classData().guardsOfNamesAwaitBeginning && numberOfBit != classLayout.classData().guardBit[static_cast<size_t>(side)];
        RELEASE_ASSERT(!isCountedFromBeginning || state.objects->classes.guardsHaveBegun(classLayout));
        countChangeOfClassGuard(state, numberOfBit, holds);
        state.countWriteForEffectsNet(countOfChangeOfClassGuard(holds));
        if (!holds)
            countEvent(*state.instance.vm, RuntimeCounterID::ClassGuardBitsEnded);
    }

    auto dependents = std::ranges::equal_range(state.tables.methodTableDependents(), numberOfBit, { }, &MethodTableDependentData::guardBit);
    if (!dependents.empty())
        repointMethodTableEntries(state, dependents, holds && !state.objects->classes.hasInstanceBelowOtherPrototype(classLayout));
}

static bool bitOfClassGuardSaysThatItHolds(const LayoutData& classLayout, ClassSide side, BitOfClassGuard bit)
{
    return !!(*bit.byte & bit.mask) != classLayout.classData().bitsAreClearWhileGuardsHold(side);
}

void ClassTable::beginGuardsSlowCase(const LayoutData& classLayout)
{
    m_classesWhoseGuardsHaveBegun.set(m_tables.rankOfClass(classLayout.index));
    if (const LayoutData* above = assumedBaseOf(classLayout); above && !guardsHaveBegun(*above)) {
        if (isDefinedByClass(*above) || isClassOfCreatedObjects(*above))
            beginGuardsSlowCase(*above);
        else
            classPrototypeOfInstances(*m_state.instance.vm, *above);
    }
    if (BitOfClassGuard guardOfPrototypeSide = bitOfClassGuard(m_state, classLayout, ClassSide::Prototype); guardOfPrototypeSide.byte) {
        RELEASE_ASSERT(guardOfPrototypeSide.mask != maskOfClassGuardsBegun);
        *guardOfPrototypeSide.byte |= maskOfClassGuardsBegun;
        m_state.countWriteForEffectsNet(Typed::CountInEffectsNet::GuardsOfMembersArmedOrBegun);
    }
    if (!classLayout.classData().guardsOfNamesAwaitBeginning)
        return;
    Vector<uint32_t, 16> bits;
    for (const NamedClassGuardData& guard : guardsOfNamesInClassRecord(m_tables, classLayout, ClassSide::Prototype))
        bits.append(guard.numberOfBit());
    AOT::sort(bits);
    bits.shrink(bits.size() - std::ranges::unique(bits).size());
    for (uint32_t numberOfBit : bits) {
        if (!bitOfClassGuardSaysThatItHolds(classLayout, ClassSide::Prototype, bitOfClassGuardWithNumber(m_state, numberOfBit)))
            countChangeOfClassGuard(m_state, numberOfBit, false);
    }
}

static bool classGuardOfSideHolds(const State& state, const LayoutData& classLayout, ClassSide side)
{
    if (state.objects->classes.guardHasEnded(classLayout, side))
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

void endClassGuard(State& state, const LayoutData& classLayout, ClassSide side)
{
    ClassTable& classes = state.objects->classes;
    if (classes.m_endedGuards.get(classes.indexOfGuard(classLayout, side)))
        return;
    classes.m_endedGuards.ensureSize(static_cast<size_t>(state.tables.numberOfClasses()) * numberOfClassSides);
    ClassTable::MethodTableUpdate update(classes);
    Vector<uint32_t, 16> classesToEnd { classLayout.index };
    while (!classesToEnd.isEmpty()) {
        const LayoutData* current = classes.classWithIndex(classesToEnd.takeLast());
        RELEASE_ASSERT(current);
        if (classes.m_endedGuards.set(classes.indexOfGuard(*current, side)) || !current->classData().hasGuardsHereOrBelow(side))
            continue;
        storeBitOfClassGuard(state, *current, side, current->classData().guardBit[static_cast<size_t>(side)], false);
        for (const NamedClassGuardData& guard : guardsOfNamesInClassRecord(state.tables, *current, side))
            storeBitOfClassGuard(state, *current, side, guard.numberOfBit(), false);
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
        for (const NamedClassGuardData& guard : guardsOfNameInClassRecord(state.tables, current, side, *key)) {
            if (goesOnHolding(guard))
                continue;
            classes.m_endedGuardsOfNames.add(guard.numberOfBit());
            storeBitOfClassGuard(state, current, side, guard.numberOfBit(), false);
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
    countEvent(*state.instance.vm, RuntimeCounterID::ClassesLookedAtToEndGuardsOfNames, numberOfClassesLookedAt);
}

void didEvaluateClassAgain(State& state, const LayoutData& classLayout)
{
    state.objects->classes.disarmMemberGuards(classLayout);
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
    if (closures.hasCellFor(function) && !closures.closureIfCreated(function))
        closures.registerClosureOfMember(function, closure);
    return isClosureHeldByClosureTable(state, closure, method);
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

static StateOfClassHolder stateOfCompiledClassHolder(VM& vm, State& state, const LayoutData& classLayout, ClassSide side, Object& holder, NamesThatDifferOnClassHolders& namesThatDiffer)
{
    JSObject* extraProperties = extraPropertiesIfMade(holder);
    if (state.objects->classes.isDefinedByClass(classLayout) || holder.isInFrame() || &holder.state() != &state || (holder.hasExtraProperties() && !extraProperties))
        return StateOfClassHolder::Differs;
    for (const MethodData& method : methodsInClassRecord(classLayout, side)) {
        if (method.flags & methodHasComputedName)
            return StateOfClassHolder::Differs;
        namesThatDiffer.add(static_cast<uint32_t>(method.name));
    }
    for (const FieldData& field : holder.layout().fields())
        namesThatDiffer.add(static_cast<uint32_t>(field.name));
    if (extraProperties) {
        const ProgramIdentifiers& identifiers = identifiersOf(state.instance);
        extraProperties->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            namesThatDiffer.add(identifiers.numberOf(entry.key()));
            return true;
        });
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
            if (mayBeDefinedLater)
                isUnfinished = true;
            else
                namesThatDiffer.add(key);
            return true;
        }
        if (isImplicitMemberOfClassHolder(vm, side, entry.key()))
            return true;
        if (key && std::ranges::binary_search(namesOfStaticFields, *key))
            return true;
        size_t numberOfMembers = numberOfComputedNameMembersInProperty(state, methods, value, entry.attributes());
        numberOfMembersWithoutNameFound += numberOfMembers;
        if (numberOfMembers)
            return true;
        constexpr unsigned attributesThatTakeStores = PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue;
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

static bool isClassOfLibrary(const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    return constructor && functionKindData(constructor).isInRuntimeModule;
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
        if (stateOfClassHolder(vm, state, *current, ClassSide::Static, parent, namesOfFunctionsAbove) != StateOfClassHolder::AsDeclared)
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
    if (!parent || parent != registeredHolderOf(vm, state, *above, side))
        return false;
    if (!classGuardOfSideHolds(state, *above, side))
        return false;
    if (above->classData().guardsHoldWhileClear)
        return true;
    ClassSide sideOfParent;
    if (classes.classHeldBy(parent, sideOfParent) != above || sideOfParent != side)
        return false;
    if (bitOfClassGuard(state, *above, side).byte)
        return true;
    NamesThatDifferOnClassHolders namesFurtherUp;
    NamesThatDifferOnClassHolders namesOfParent;
    if (!hasParentNamedByClassRecord(vm, state, *above, side, parent, namesFurtherUp) || stateOfClassHolder(vm, state, *above, side, parent, namesOfParent) != StateOfClassHolder::AsDeclared)
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
    beginGuards(layout);
    if (classHolderReportsAsPrototype(*this, layout, side)) {
        holder->didBecomePrototype(vm);
        if (side == ClassSide::Static)
            m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
    }
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
    StateOfClassHolder stateOfHolder = StateOfClassHolder::Differs;
    NamesThatDifferOnClassHolders namesThatDifferAbove;
    if (hasParentNamedByClassRecord(vm, m_state, layout, side, holder, namesThatDifferAbove))
        stateOfHolder = isMadeAsDeclared ? StateOfClassHolder::AsDeclared : stateOfClassHolder(vm, m_state, layout, side, holder, namesThatDiffer);
    if (stateOfHolder == StateOfClassHolder::Differs) {
        endClassGuard(m_state, layout, side);
        return;
    }

    addNamesThatLookupsComeToAbove(layout, side, namesThatDifferAbove, namesThatDiffer);
    for (uint32_t name : namesThatDiffer.numbers)
        endClassGuardsOfName(m_state, layout, side, name);
    for (uint32_t name : namesThatDiffer.numbersOfPlainValues)
        endClassGuardsOfName(m_state, layout, side, name, GuardsOfNameToEnd::StoresStillPass);
    if (namesThatDiffer.someHasNoNumber)
        endClassGuardsOfName(m_state, layout, side, std::nullopt);
    if (stateOfHolder == StateOfClassHolder::MayStillBeDefined)
        return;

    storeBitOfClassGuard(m_state, layout, side, layout.classData().guardBit[static_cast<size_t>(side)], true);
    for (const NamedClassGuardData& guard : guardsOfNamesInClassRecord(m_tables, layout, side)) {
        if (!m_endedGuardsOfNames.contains(guard.numberOfBit()))
            storeBitOfClassGuard(m_state, layout, side, guard.numberOfBit(), true);
    }
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
    endClassGuardsOfName(m_state, layout, side, key);
}

void ClassTable::holdDefinitionToNameInRecord(const LayoutData& layout, ClassSide side, uint32_t index, UniquedStringImpl* uid, const JSObject* function) const
{
    const MethodData* row = findRowOfMemberInClassRecord(layout, side, index, function);
    if (!row || (row->flags & methodHasComputedName))
        return;
    if (std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid); !key || row->name != *key) {
        dataLogLn("AOT: a member of the class of layout L", layout.index, " is defined under another key than its record names: the compiler has proven that it never happens. This is a bug in Bun.");
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
        return "AddName"_s;
    case HolderMutation::ChangePrototype:
        return "ChangePrototype"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

[[noreturn]] static void crashOnFalseProofOfClassHolder(const LayoutData& classLayout, ClassSide side, HolderMutation mutation)
{
    dataLogLn("AOT: ", nameOfHolderMutation(mutation), " of the ", side == ClassSide::Prototype ? "prototype object"_s : "class object"_s, " of the class of layout L", classLayout.index, ": the compiler has proven that it never happens, and compiled code finds the members of the class without a look. This is a bug in Bun.");
    CRASH_WITH_INFO(classLayout.index, static_cast<uint32_t>(side), static_cast<uint32_t>(mutation));
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
    auto findRowsIn = [&](const LayoutData& classLayout, DeclaredMember& declared, bool& mayBeStoredOver) {
        bool isDeclared = false;
        for (const MethodData& method : methodsInClassRecord(classLayout, side)) {
            if (!key || !isNamedInClassRecord(method, *key))
                continue;
            isDeclared = true;
            declared.define(method);
            mayBeStoredOver |= !!(method.flags & methodMayBeStoredOver);
        }
        return isDeclared;
    };
    auto stopUnlessStaticFieldMayAnswer = [&](HolderMutation mutation) {
        for (const LayoutData* current = side == ClassSide::Static ? &layout : nullptr; current; current = assumedBaseOf(*current)) {
            if (current->classData().hasStaticFieldUnderComputedKey || staticField(*current, uid))
                return;
        }
        crashOnFalseProofOfClassHolder(layout, side, mutation);
    };
    auto isLaidHereOutsideRecord = [&] {
        if (side == ClassSide::Static)
            return false;
        return !layout.classData().namesAreLaidUnderWrittenKeysAlone || (key && std::ranges::binary_search(programTables().namesLaidOutsideClassRecords(), NameLaidOutsideClassRecordData { layout.index, *key }));
    };

    unsigned attributes = 0;
    JSValue value = holder->getDirect(vm, PropertyName(uid), attributes);
    DeclaredMember declared;
    bool mayBeStoredOver = false;
    bool isDeclaredHere = findRowsIn(layout, declared, mayBeStoredOver);
    if (!isDeclaredHere && (!value || isLaidHereOutsideRecord()))
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
        if (!isDefinedByClass(layout) || isLazyClassHolder(holder, side))
            return;
        value = findOnChainAbove(vm, holder, uid, attributes);
    }
    if (!value || !holdsFunctionsOfMemberInClassRecord(value, attributes, declared))
        stopUnlessStaticFieldMayAnswer(mutation);
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
    if (linkIsWrittenByClasses && (above ? !parent || !isHolderOfSideOfClass(*this, parent, *above, side) : parent != aboveEveryClass))
        crashOnFalseProofOfClassHolder(layout, side, HolderMutation::ChangePrototype);
}

void ClassTable::didDefineMember(VM& vm, const LayoutData& layout, ClassSide side, uint32_t index, JSObject* holder, UniquedStringImpl* uid, JSObject* function)
{
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
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuard(m_state, layout, side);
}

void ClassTable::didFreezeHolder(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
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

void ClassTable::didChangePrototypeLink(const LayoutData& layout, ClassSide side)
{
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuard(m_state, layout, side);
}

void ClassTable::didDefineWithOtherParent(const LayoutData& layout, ClassSide side)
{
    if (!hasNoGuardLeft(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuard(m_state, layout, side);
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

void ClassTable::didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    endGuardsOfShadowedMember(instanceLayout, uid);
}

void ClassTable::didShadowMemberSlowCase(const LayoutData& instanceLayout, const FieldData& field)
{
    if (endGuardsOfShadowedMember(instanceLayout, m_state.objects->layouts.nameOf(*m_state.instance.vm, field)))
        m_fieldsWhoseCreationEndsNoGuard.add(numberOfFieldRecord(field));
}

void ClassTable::SetOfFieldRecords::add(uint32_t number)
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
        return !m_endedGuardsOfNames.contains(guard.numberOfBit());
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
    for (const NamedClassGuardData& guard : guardsOfNamesInClassRecord(m_tables, layout, ClassSide::Prototype)) {
        auto dependents = std::ranges::equal_range(m_tables.methodTableDependents(), guard.numberOfBit(), { }, &MethodTableDependentData::guardBit);
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
        if (!object || object->type() != FinalObjectType || !flattenOnce(vm, object))
            return false;
        Structure* structure = object->structure();
        if (structure->realm() != realm || structure->isDictionary() || structure->hasPolyProto() || !structure->mayBePrototype() || hasIndexedProperties(structure->indexingType()))
            return false;
        between.append(object);
    }
    Vector<uint32_t, 16> namesBetween;
    bool hidesMember = false;
    for (JSObject* object : between) {
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(entry.key());
            if (!key)
                return true;
            namesBetween.append(*key);
            ChainWalk above = walkChain({ realm, between.last()->structure(), nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::UsedAtOnce }, entry.key());
            hidesMember |= above.result != ChainWalkResult::Absent;
            return !hidesMember;
        });
    }
    if (hidesMember)
        return false;
    for (uint32_t key : namesBetween)
        endClassGuardsOfName(m_state, layout, ClassSide::Prototype, key);
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
    for (const NamedClassGuardData& guard : guards) {
        if (!bitsThatDoNotBegin.contains(guard.numberOfBit()) && !m_endedGuardsOfNames.contains(guard.numberOfBit()))
            storeBitOfClassGuard(m_state, classLayout, ClassSide::Prototype, guard.numberOfBit(), true);
    }
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
