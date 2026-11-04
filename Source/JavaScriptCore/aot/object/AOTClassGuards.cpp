/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassGuards.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/OSAllocator.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct BitOfClassGuard {
    uint8_t* byte { nullptr };
    uint8_t mask { 0 };
};

// AsDeclared and MayStillBeDefined: but for the names that are listed beside the answer. Differs: in a way that no name stands for.
enum class StateOfClassHolder : uint8_t { AsDeclared, MayStillBeDefined, Differs };

// AbsentUntilReported: the table of classes hears of it when the holder gets the name. AbsentForNow: there is no holder yet, or one that tells the table nothing.
enum class NameOnPrototypeHolder : uint8_t { Found, AbsentUntilReported, AbsentForNow };

// Under these a holder has something else than the record of its class says, or lacks what the record says.
struct NamesThatDifferOnClassHolders {
    Vector<uint32_t, 4> numbers;
    // Where the record declares nothing and the holder has a plain value that may be written: a store into an instance passes it by.
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

static bool isStaticFieldInClassRecord(const LayoutData& classLayout, uint32_t key)
{
    return std::ranges::any_of(classLayout.staticFields(), [&](const FieldData& field) {
        return field.name == key;
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

// Ascending by name. A name may come several times, and so may the bit of a guard for several names.
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

// The pages of the table are read-only but for the time of an update.
static void setMethodTableWritableForClassGuards(State& state, bool isWritable)
{
    OSAllocator::protect(state.instance.methodTable().data(), InstanceLayout::bytesReservedForMethodTable(state.tables.instanceSizes), true, isWritable);
}

void ClassTable::MethodTableUpdate::willStoreEntry()
{
    if (!std::exchange(m_classes.m_methodTableIsWritable, true))
    {
        countEvent(*m_classes.m_state.instance.vm, RuntimeCounterID::MethodTableMadeWritable);
        setMethodTableWritableForClassGuards(m_classes.m_state, true);
    }
}

ClassTable::MethodTableUpdate::~MethodTableUpdate()
{
    if (!--m_classes.m_numberOfOpenMethodTableUpdates && std::exchange(m_classes.m_methodTableIsWritable, false))
        setMethodTableWritableForClassGuards(m_classes.m_state, false);
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
        update.willStoreEntry();
        table[index] = entry;
    }
}

// The bit has just changed.
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
        countChangeOfClassGuard(state, numberOfBit, holds);
        if (!holds)
            countEvent(*state.instance.vm, RuntimeCounterID::ClassGuardBitsEnded);
    }

    // A call through the method table tests no guard and asks no object.
    auto dependents = std::ranges::equal_range(state.tables.methodTableDependents(), numberOfBit, { }, &MethodTableDependentData::guardBit);
    if (!dependents.empty())
        repointMethodTableEntries(state, dependents, holds && !state.objects->classes.hasInstanceBelowOtherPrototype(classLayout));
}

static bool bitOfClassGuardSaysThatItHolds(const LayoutData& classLayout, ClassSide side, BitOfClassGuard bit)
{
    return !!(*bit.byte & bit.mask) != classLayout.classData().bitsAreClearWhileGuardsHold(side);
}

// Whatever has become of its names.
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
    return !classes.m_sidesWithEndedNames.get(classes.indexOfGuard(classLayout, side)) && classGuardOfSideHolds(state, classLayout, side);
}

bool classGuardOfNameHolds(const State& state, const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    auto guards = guardsOfNameInClassRecord(state.tables, classLayout, side, key);
    if (std::ranges::all_of(guards, &NamedClassGuardData::answersStoresAlone))
        return classGuardHolds(state, classLayout, side);
    if (state.objects->classes.guardHasEnded(classLayout, side))
        return false;
    // What ends the guard of the name ends every guard that is listed under it. A guard for several names may have ended for another of them.
    return std::ranges::any_of(guards, [&](const NamedClassGuardData& guard) {
        return !guard.answersStoresAlone() && bitOfClassGuardSaysThatItHolds(classLayout, side, bitOfClassGuardWithNumber(state, guard.numberOfBit()));
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
        // Whatever ended the guard of a class has ended those of the classes below it.
        if (classes.m_endedGuards.set(classes.indexOfGuard(*current, side)))
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
    uint64_t part = 0;
    if (toEnd == GuardsOfNameToEnd::StoresStillPass)
        part = ClassTable::storesStillPassUnderEndedName;
    else if (toEnd == GuardsOfNameToEnd::ReadsStillFind)
        part = ClassTable::readsStillFindUnderEndedName;
    auto everyGuardEndedAt = [&](const LayoutData& current) {
        return static_cast<uint64_t>(classes.indexOfGuard(current, side)) << 32 | key.value_or(ClassTable::nameWithoutNumber);
    };
    auto endedNameAt = [&](const LayoutData& current) {
        return everyGuardEndedAt(current) | part;
    };
    auto hasEndedAt = [&](const LayoutData& current) {
        return classes.m_endedNames.contains(everyGuardEndedAt(current)) || (part && classes.m_endedNames.contains(endedNameAt(current)));
    };
    if (classes.guardHasEnded(classLayout, side) || hasEndedAt(classLayout))
        return;
    classes.m_endedNames.add(endedNameAt(classLayout));
    classes.m_sidesWithEndedNames.ensureSize(static_cast<size_t>(state.tables.numberOfClasses()) * numberOfClassSides);
    ClassTable::MethodTableUpdate update(classes);
    Vector<uint32_t, 16> classesToEnd { classLayout.index };
    while (!classesToEnd.isEmpty()) {
        const LayoutData* current = classes.classWithIndex(classesToEnd.takeLast());
        RELEASE_ASSERT(current);
        // Whatever ended a guard of a class has ended the same guard of the classes below it.
        if (classes.guardHasEnded(*current, side) || (current->index != classLayout.index && hasEndedAt(*current)))
            continue;
        if (toEnd != GuardsOfNameToEnd::ReadsStillFind)
            classes.m_sidesWithEndedNames.quickSet(classes.indexOfGuard(*current, side));
        if (key) {
            for (const NamedClassGuardData& guard : guardsOfNameInClassRecord(state.tables, *current, side, *key)) {
                if (toEnd != GuardsOfNameToEnd::All && guard.answersStoresAlone() == (toEnd == GuardsOfNameToEnd::StoresStillPass))
                    continue;
                classes.m_endedGuardsOfNames.add(guard.numberOfBit());
                storeBitOfClassGuard(state, *current, side, guard.numberOfBit(), false);
            }
        }
        classesToEnd.append(subclassesInClassRecord(state.tables, *current));
    }
}

// What happens to a holder before the guards of its class begin is seen when they begin. The classes below have looked at the holder already.
static void endClassGuardsThatHaveBegun(State& state, const LayoutData& classLayout, ClassSide side)
{
    ClassTable& classes = state.objects->classes;
    if (!classes.guardsAwaitFirstInstance(classLayout)) {
        endClassGuard(state, classLayout, side);
        return;
    }
    ClassTable::MethodTableUpdate update(classes);
    for (uint32_t index : subclassesInClassRecord(state.tables, classLayout)) {
        const LayoutData* below = classes.classWithIndex(index);
        RELEASE_ASSERT(below);
        endClassGuardsThatHaveBegun(state, *below, side);
    }
}

static void endClassGuardsOfNameThatHaveBegun(State& state, const LayoutData& classLayout, ClassSide side, std::optional<uint32_t> key, GuardsOfNameToEnd toEnd = GuardsOfNameToEnd::All)
{
    ClassTable& classes = state.objects->classes;
    if (!classes.guardsAwaitFirstInstance(classLayout)) {
        endClassGuardsOfName(state, classLayout, side, key, toEnd);
        return;
    }
    ClassTable::MethodTableUpdate update(classes);
    for (uint32_t index : subclassesInClassRecord(state.tables, classLayout)) {
        const LayoutData* below = classes.classWithIndex(index);
        RELEASE_ASSERT(below);
        endClassGuardsOfNameThatHaveBegun(state, *below, side, key, toEnd);
    }
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

// Allocates nothing: a store that creates a field asks. The prototype object of a class may be made before its class object.
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

// Compiled code that reads a method behind the guard takes the closure that the table holds, or has the table make one.
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

// Asked of what a holder has. Where the table has made no closure of the function yet, the one on the holder becomes the table's: what is read behind the guard is the same object.
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

// How many members that the record declares without a name make up the property. 0 where something else has a part in it.
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

// A class object reports what happens to it through its method table. A prototype object reports through VM::invalidateStructureChainIntegrity(), and so does a constructor function.
static bool classHolderReportsAsPrototype(const ClassTable& classes, const LayoutData& classLayout, ClassSide side)
{
    return side == ClassSide::Prototype || !classes.isDefinedByClass(classLayout);
}

// `Below.prototype = new Above()` (AOTClassGuards.h). No closure that lies on the object is looked at: whatever it has differs.
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

    // Until the definition of a class ends, no code of the program can name the class. What a constructor function lacks at its first allocation, the program sees to be missing.
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

// The class that the object is a holder of, entered or not.
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

// `Object.setPrototypeOf(Readable, Stream)` (AOTClassGuards.h). Each function above is that of the class which the record of the one below names, so what changes it ends the guards below.
static bool standsBelowFunctionsNamedByClassRecords(VM& vm, State& state, const LayoutData& classLayout, JSObject* function, NamesThatDifferOnClassHolders& namesOfFunctionsAbove)
{
    ClassTable& classes = state.objects->classes;
    const ProgramIdentifiers& identifiers = identifiersOf(state.instance);
    JSObject* functionPrototype = state.instance.globalObject->functionPrototype();
    const LayoutData* current = &classLayout;
    for (JSObject* parent = function->getPrototypeDirect().getObject(); parent != functionPrototype; parent = parent->getPrototypeDirect().getObject()) {
        current = classes.assumedBaseOf(*current);
        if (!parent || !current || classes.isDefinedByClass(*current) || parent != registeredClassObjectOf(state, *current) || classes.guardHasEnded(*current, ClassSide::Static))
            return false;
        ClassSide sideOfParent;
        if (classes.classHeldBy(parent, sideOfParent) != current || sideOfParent != ClassSide::Static)
            return false;
        parent->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (!isImplicitMemberOfClassHolder(vm, ClassSide::Static, entry.key()))
                namesOfFunctionsAbove.add(identifiers.numberOf(entry.key()));
            return true;
        });
    }
    return true;
}

// A lookup that passes an instance stops at the first holder that has the name: what stands under it further up is never come to. `ofHolder`: what differs on the holder of the class itself.
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

// A change to a class ends the guards of the classes that its record lists below it. So a holder may stand below the holder of the class that its own record names, and below that of no other class of the program.
// What has ended a guard of a name at a class whose guards have begun has ended it below. Of a holder above that nobody has looked at yet, the names that differ are listed.
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
    bool guardAboveHasBegun = !classes.guardsAwaitFirstInstance(*above);
    if (guardAboveHasBegun ? !classGuardOfSideHolds(state, *above, side) : classes.guardHasEnded(*above, side))
        return false;
    // The holders of a class that is left to its record are entered before their first change.
    if (above->classData().guardsHoldWhileClear)
        return true;
    ClassSide sideOfParent;
    if (classes.classHeldBy(parent, sideOfParent) != above || sideOfParent != side)
        return false;
    // A side that no code tests has no bit to say that its guard has begun.
    if (guardAboveHasBegun && bitOfClassGuard(state, *above, side).byte)
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

static bool definesMemberOfClassRecord(State& state, const LayoutData& classLayout, ClassSide side, UniquedStringImpl* uid, JSCell* function)
{
    std::optional<uint32_t> key = identifiersOf(state.instance).numberOf(uid);
    // A member with a computed name that lands on a name of the record replaces what the record declares there.
    bool recordNamesKey = key && declaresMember(classLayout, side, *key);
    return std::ranges::any_of(methodsInClassRecord(classLayout, side), [&](const MethodData& method) {
        bool isRowOfKey = recordNamesKey ? isNamedInClassRecord(method, *key) : !!(method.flags & methodHasComputedName);
        if (!isRowOfKey)
            return false;
        // As where a holder is looked at: what the definition lays there is what is read behind the guard.
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

// What a lookup that passes an instance of the class finds under the name on the prototype object of the class.
// A compiled holder counts as having each name of its layout, whatever its slots hold: compiled code fills them without a word.
static NameOnPrototypeHolder findNameOnPrototypeHolder(VM& vm, State& state, const LayoutData& classLayout, UniquedStringImpl* uid, std::optional<uint32_t> key)
{
    if (uid == vm.propertyNames->constructor.impl() || (key && declaresMember(classLayout, ClassSide::Prototype, *key)))
        return NameOnPrototypeHolder::Found;
    const ClassTable& classes = state.objects->classes;
    JSObject* holder = prototypeHolderIfMade(vm, state, classLayout);
    if (!holder)
        return NameOnPrototypeHolder::AbsentForNow;
    // didChangeClassHolder() knows such an object by the class that it names.
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
    MethodTableUpdate update(*this);
    forgetWhatCreatedFieldsHide();
    // Only an object whose Structure may be a prototype reports an added name or a replaced value.
    if (classHolderReportsAsPrototype(*this, layout, side)) {
        holder->didBecomePrototype(vm);
        if (side == ClassSide::Static)
            m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
    }
    // A constructor allocates an instance of the class whose prototype object it is handed, so the holders of every evaluation say which class they are of: classHeldBy(). The guards describe the first evaluation.
    auto entry = m_classSidesByHolder.add(holder, encodeSideOfClassHolder(layout, side));
    bool isEnteredForThisSide = entry.iterator->value == encodeSideOfClassHolder(layout, side);
    if (entry.isNewEntry)
        m_writes.recordAll();
    else if (isEnteredForThisSide && holderState == HolderState::NotTouched)
        return;

    // Compiled code makes a second closure of a function that is no class without a word to the run time.
    bool runTimeSeesEveryEvaluation = isDefinedByClass(layout) || layout.evaluation() == EvaluationState::ProvenOnce;
    bool isOfFirstEvaluation = layout.evaluation() != EvaluationState::SeenTwice && registeredHolderOf(vm, m_state, layout, side) == holder;
    if (!isEnteredForThisSide || !runTimeSeesEveryEvaluation || !isOfFirstEvaluation) {
        disarmMemberGuards(layout);
        return;
    }
    if (m_endedGuards.get(indexOfGuard(layout, side)))
        return;

    // With one class object every member that the record creates is the closure that the table holds.
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

// The classes below look at the holder when their own guards begin, and hear of every change to it from here on.
void ClassTable::enterHolderWithoutGuards(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    ASSERT(!isDefinedByClass(layout) && !layout.classData().guardsHoldWhileClear);
    forgetWhatCreatedFieldsHide();
    holder->didBecomePrototype(vm);
    if (side == ClassSide::Static)
        m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
    if (m_classSidesByHolder.add(holder, encodeSideOfClassHolder(layout, side)).isNewEntry)
        m_writes.recordAll();
}

bool ClassTable::tryLeaveHoldersOut(VM& vm, const LayoutData& layout, Closure* classObject, JSObject* prototype)
{
    ASSERT(isDefinedByClass(layout));
    if (layout.evaluation() == EvaluationState::SeenTwice)
        disarmMemberGuards(layout);
    // A prototype object that has not made its members says by itself which class it is of, and makes them once it is given a property.
    if (!hasNoGuardLeft(layout) || prototype->type() != FinalObjectType)
        return false;
    if (!m_classObjectOfHolderLeftOut)
        m_classObjectOfHolderLeftOut = PrivateSymbolImpl::create(*StringImpl::create("class"_span8).ptr());
    prototype->putDirect(vm, PropertyName(m_classObjectOfHolderLeftOut.get()), classObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    prototype->didBecomePrototype(vm);
    m_hasHoldersLeftOut = true;
    return true;
}

// Nothing can change or remove the prototype property of a class object, so a class object that names the holder as its prototype proves what the holder is.
// The holder's own `constructor` is the program's to delete, redefine or overwrite with that of another class, as a mixin does: it says nothing.
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
    if (!layout || !isDefinedByClass(*layout) || !hasNoGuardLeft(*layout))
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

// Some callers report a change before they make it, so didChangeMember, didChangeEveryMember and didChangePrototypeLink never read the holder.
void ClassTable::didChangeMember(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid)
{
    if (m_endedGuards.get(indexOfGuard(layout, side)))
        return;
    // `Counter.made++` says nothing about a prototype object.
    if (side == ClassSide::Prototype || uid == m_state.instance.vm->propertyNames->prototype.impl())
        forgetWhatCreatedFieldsHide();
    if (m_memberBeingDefined.classLayout && m_memberBeingDefined.classLayout->index == layout.index && m_memberBeingDefined.side == side && m_memberBeingDefined.uid == uid)
        return;
    // The class of a constructor function is defined at its first allocation, out of what the program has assigned until then. Its guards begin there.
    if (!layout.classData().guardsHoldWhileClear && !isDefinedByClass(layout) && !classPrototypeIfExists(layout))
        return;
    // Every function has a name and a length of its own from the start, and the first look at one makes the property. So no lookup passes a class object under either, and no guard speaks of them.
    const CommonIdentifiers& names = *m_state.instance.vm->propertyNames;
    if (side == ClassSide::Static && (uid == names.name.impl() || uid == names.length.impl()) && !hasStaticMethodNamed(layout, uid))
        return;
    // Compiled code resolves nothing under the name of a static field, at the class or below it, so no guard speaks of one.
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    if (side == ClassSide::Static && key && isStaticFieldInClassRecord(layout, *key) && !declaresMember(layout, side, *key))
        return;
    endClassGuardsOfNameThatHaveBegun(m_state, layout, side, key);
}

void ClassTable::didDefineMember(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid, JSObject* function)
{
    // The holders may have been left out, and stay out.
    if (hasNoGuardLeft(layout))
        return;
    forgetWhatCreatedFieldsHide();
    if (!m_classSidesByHolder.contains(holder))
        armMemberGuards(vm, layout, side, holder);
    if (m_endedGuards.get(indexOfGuard(layout, side)))
        return;
    if (registeredHolderOf(vm, m_state, layout, side) != holder) {
        endClassGuard(m_state, layout, side);
        return;
    }
    if (!definesMemberOfClassRecord(m_state, layout, side, uid, function)) {
        endClassGuardsOfName(m_state, layout, side, identifiersOf(m_state.instance).numberOf(uid));
        return;
    }
    if (!layout.classData().guardsHoldWhileClear && isLastMemberCreatedAtDefinition(layout, side, function))
        armMemberGuards(vm, layout, side, holder);
}

void ClassTable::didChangeEveryMember(const LayoutData& layout, ClassSide side)
{
    if (!guardHasEnded(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuardsThatHaveBegun(m_state, layout, side);
}

void ClassTable::didFreezeHolder(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    if (m_endedGuards.get(indexOfGuard(layout, side)))
        return;
    forgetWhatCreatedFieldsHide();
    // As in didChangeMember(): whoever begins the guards looks at the holder.
    if (!layout.classData().guardsHoldWhileClear && !isDefinedByClass(layout) && !classPrototypeIfExists(layout))
        return;
    // The Structure of a compiled object lists none of its properties.
    if (isCompiledObject(holder)) {
        endClassGuardsThatHaveBegun(m_state, layout, side);
        return;
    }
    MethodTableUpdate update(*this);
    holder->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(entry.key()))
            endClassGuardsOfNameThatHaveBegun(m_state, layout, side, key, GuardsOfNameToEnd::ReadsStillFind);
        return true;
    });
}

void ClassTable::didChangePrototypeLink(const LayoutData& layout, ClassSide side)
{
    if (!guardHasEnded(layout, side))
        forgetWhatCreatedFieldsHide();
    endClassGuardsThatHaveBegun(m_state, layout, side);
}

void ClassTable::didDefineWithOtherParent(const LayoutData& layout, ClassSide side)
{
    forgetWhatCreatedFieldsHide();
    endClassGuard(m_state, layout, side);
}

// A guard that has ended stays ended, so an answer that rests on one stands for good.
bool ClassTable::endGuardsOfShadowedMember(const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    if (!instanceLayout.isClass())
        return true;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (m_endedGuards.get(indexOfGuard(layout, ClassSide::Prototype)))
        return true;
    VM& vm = *m_state.instance.vm;
    std::optional<uint32_t> key = identifiersOf(m_state.instance).numberOf(uid);
    ++m_numberOfWalksOverPrototypeHolders;
    bool everyHolderReportsNewNames = true;
    for (const LayoutData* current = &layout; current; current = assumedBaseOf(*current)) {
        switch (findNameOnPrototypeHolder(vm, m_state, *current, uid, key)) {
        case NameOnPrototypeHolder::Found:
            endClassGuardsOfName(m_state, layout, ClassSide::Prototype, key, GuardsOfNameToEnd::StoresStillPass);
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

void ClassTable::didChangeInstancePrototype(VM& vm, const LayoutData& instanceLayout, JSValue prototype)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (m_endedGuards.get(indexOfGuard(layout, ClassSide::Prototype)))
        return;
    JSObject* holder = registeredHolderOf(vm, m_state, layout, ClassSide::Prototype);
    if (holder && prototype == JSValue(holder))
        return;
    // An object of a class that nobody has defined: its guards have not begun, and hold for no object alone.
    if (!holder) {
        endClassGuard(m_state, layout, ClassSide::Prototype);
        return;
    }
    // A call through the method table goes by the shape, which the object keeps. Each row of the class hangs on a guard of a name of the class (MethodTablePlanner::rowOf()).
    if (!m_classesWithInstanceBelowOtherPrototype.add(layout.index).isNewEntry)
        return;
    forgetWhatCreatedFieldsHide();
    for (const NamedClassGuardData& guard : guardsOfNamesInClassRecord(m_tables, layout, ClassSide::Prototype)) {
        auto dependents = std::ranges::equal_range(m_tables.methodTableDependents(), guard.numberOfBit(), { }, &MethodTableDependentData::guardBit);
        if (!dependents.empty())
            repointMethodTableEntries(m_state, dependents, false);
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
