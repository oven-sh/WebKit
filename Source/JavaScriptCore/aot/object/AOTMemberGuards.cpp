/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMemberGuards.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeCounters.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"
#include "ObjectPrototype.h"
#include <algorithm>
#include <tuple>
#include <wtf/IterationStatus.h>
#include <wtf/OSAllocator.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct MemberName {
    UniquedStringImpl* uid()
    {
        if (!knownUid)
            knownUid = vm.aotState().identifiers->identifier(key).impl();
        return knownUid;
    }

    VM& vm;
    uint32_t key;
    UniquedStringImpl* knownUid { nullptr };
};

struct GuardResolution {
    const LayoutData* declarer { nullptr };
    JSObject* declarerHolder { nullptr };
    ChainObjects otherObjects;
};

class NameFilter {
public:
    void add(uint32_t key) { m_bits[key / bitsPerWord % numberOfWords] |= 1ull << key % bitsPerWord; }
    bool mayContain(uint32_t key) const { return m_bits[key / bitsPerWord % numberOfWords] >> key % bitsPerWord & 1; }

private:
    static constexpr uint32_t bitsPerWord = 64;
    static constexpr uint32_t numberOfWords = 4;
    std::array<uint64_t, numberOfWords> m_bits { };
};

struct ChainLevel {
    const LayoutData* classLayout;
    JSObject* holder;
    MemberGuardWatchpoints::NamesOnHolder* names;
    NameFilter namesToLookFor { };
};

} // anonymous namespace

static_assert(numberOfClassSides == 2);

static uint32_t encodeClassSide(const LayoutData& layout, ClassSide side)
{
    return layout.index << 1 | static_cast<uint32_t>(side);
}

// No guard on the static side says that no instance hides a member: that number stands for the guard that stores pass a member, which is on the prototype side like the other.
static uint32_t encodeDependent(const LayoutData& layout, const MemberGuardData& guard)
{
    ASSERT(guard.kind() == MemberGuardKind::Member || guard.kind() == MemberGuardKind::NotHidden || guard.kind() == MemberGuardKind::PassedByStores);
    ASSERT(guard.kind() == MemberGuardKind::Member || guard.side() == ClassSide::Prototype);
    if (guard.kind() == MemberGuardKind::PassedByStores)
        return encodeClassSide(layout, ClassSide::Static) << 1 | 1;
    return encodeClassSide(layout, guard.side()) << 1 | (guard.kind() == MemberGuardKind::NotHidden);
}

static MemberGuardWatchpoints& watchpointsOf(State& state)
{
    return state.objects->caches->memberGuardWatchpoints;
}

static std::span<const MethodData> methodsOf(const LayoutData& layout, ClassSide side)
{
    switch (side) {
    case ClassSide::Prototype:
        return layout.methods();
    case ClassSide::Static:
        return layout.staticMethods();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isNamed(const MethodData& method, uint32_t key)
{
    return !(method.flags & methodHasComputedName) && method.name == key;
}

static bool hasComputedMemberName(const LayoutData& layout, ClassSide side)
{
    return std::ranges::any_of(methodsOf(layout, side), [](const MethodData& method) {
        return !!(method.flags & methodHasComputedName);
    });
}

static bool isCreatedAtDefinition(const LayoutData& layout, ClassSide side, uint32_t key)
{
    return std::ranges::any_of(methodsOf(layout, side), [&](const MethodData& method) {
        return isNamed(method, key) && (method.flags & methodIsCreatedAtDefinition);
    });
}

static const FieldData* findOwnStaticField(const LayoutData& layout, uint32_t key)
{
    std::span<const FieldData> fields = layout.staticFields();
    auto found = std::ranges::find_if(fields, [&](const FieldData& field) {
        return field.name == key;
    });
    return found == fields.end() ? nullptr : &*found;
}

static bool isImplicitMember(State& state, ClassSide side, uint32_t key)
{
    const ImplicitMemberKeys& keys = watchpointsOf(state).implicitMemberKeys();
    switch (side) {
    case ClassSide::Prototype:
        return keys.constructor == key;
    case ClassSide::Static:
        return keys.prototype == key || keys.name == key || keys.length == key;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool declaresMember(const LayoutData& classLayout, ClassSide side, uint32_t key)
{
    bool declaresMethod = std::ranges::any_of(methodsOf(classLayout, side), [&](const MethodData& method) {
        return isNamed(method, key);
    });
    return declaresMethod || (side == ClassSide::Static && findOwnStaticField(classLayout, key));
}

static bool declares(State& state, const LayoutData& layout, ClassSide side, uint32_t key)
{
    return declaresMember(layout, side, key) || isImplicitMember(state, side, key);
}

static ClassTable::DeclaredMember declaredMember(const LayoutData& layout, ClassSide side, uint32_t key)
{
    ClassTable::DeclaredMember member;
    for (const MethodData& method : methodsOf(layout, side)) {
        if (isNamed(method, key))
            member.define(method);
    }
    return member;
}

static bool isClosureOfMethod(const JSCell* cell, const MethodData& method)
{
    return functionOf(cell) == FunctionID(method.function);
}

static bool isClosureThatTableHolds(State& state, JSCell* closure, const MethodData& method)
{
    ClosureTable& closures = state.objects->closures;
    FunctionID function(method.function);
    if (!closures.hasCellFor(function))
        return true;
    if (JSCell* held = closures.closureIfCreated(function))
        return held == closure;
    closures.registerClosureOfMember(function, closure);
    return true;
}

static bool holdsDeclaredMember(VM& vm, State& state, JSObject* holder, UniquedStringImpl* uid, const ClassTable::DeclaredMember& member)
{
    unsigned attributes;
    JSValue value = holder->getDirect(vm, uid, attributes);
    if (!value || !value.isCell())
        return false;
    if (member.method)
        return !(attributes & PropertyAttribute::Accessor) && isClosureOfMethod(value.asCell(), *member.method) && isClosureThatTableHolds(state, value.asCell(), *member.method);
    auto* accessor = dynamicDowncast<GetterSetter>(value.asCell());
    if (!accessor)
        return false;
    bool holdsGetter = member.getter ? isClosureOfMethod(accessor->getter(), *member.getter) : accessor->isGetterNull();
    bool holdsSetter = member.setter ? isClosureOfMethod(accessor->setter(), *member.setter) : accessor->isSetterNull();
    return holdsGetter && holdsSetter;
}

static bool isLazyHolder(JSObject* holder, ClassSide side)
{
    switch (side) {
    case ClassSide::Prototype:
        return holder->classInfo() == ClassPrototype::info();
    case ClassSide::Static:
        return uncheckedDowncast<Closure>(holder)->isLazyClassObject();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Closure* registeredClassObjectOf(State& state, const LayoutData& layout)
{
    FunctionID constructor(layout.classData().classConstructor);
    JSCell* registered = constructor ? state.objects->closures.closureIfCreated(constructor) : nullptr;
    return registered ? uncheckedDowncast<Closure>(registered) : nullptr;
}

static JSObject* registeredHolderOf(VM& vm, State& state, const LayoutData& layout, ClassSide side)
{
    if (ClassTable::isClassOfCreatedObjects(layout))
        return side == ClassSide::Prototype ? state.objects->classes.classPrototypeIfExists(layout) : nullptr;
    Closure* classObject = registeredClassObjectOf(state, layout);
    if (!classObject)
        return nullptr;
    switch (side) {
    case ClassSide::Static:
        return classObject;
    case ClassSide::Prototype: {
        JSValue prototype = classObject->prototypeProperty(vm);
        return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* holderOf(VM& vm, State& state, const LayoutData& layout, ClassSide side)
{
    JSObject* holder = registeredHolderOf(vm, state, layout, side);
    if (!holder)
        return nullptr;
    ClassSide foundSide;
    if (state.objects->classes.classWithGuardsHeldBy(holder, foundSide) != &layout || foundSide != side)
        return nullptr;
    return holder;
}

static std::span<const MemberGuardData> guardsOf(const LayoutData& layout, ClassSide side, MemberGuardKind kind)
{
    auto range = std::ranges::equal_range(layout.memberGuards(), std::pair { side, kind }, { }, [](const MemberGuardData& guard) {
        return std::pair { guard.side(), guard.kind() };
    });
    return { range.begin(), range.end() };
}

static const MemberGuardData* findGuard(const LayoutData& layout, ClassSide side, MemberGuardKind kind, uint32_t key)
{
    auto sortKeyOf = [](const MemberGuardData& guard) {
        return std::tuple { guard.side(), guard.kind(), static_cast<uint32_t>(guard.name) };
    };
    std::span<const MemberGuardData> guards = layout.memberGuards();
    std::tuple sought { side, kind, key };
    auto found = std::ranges::lower_bound(guards, sought, { }, sortKeyOf);
    return found != guards.end() && sortKeyOf(*found) == sought ? &*found : nullptr;
}

static uint64_t& wordOf(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    uint64_t* words = state.objects->classes.guardWords(layout, guard.side());
    RELEASE_ASSERT(words);
    return words[guard.wordIndex()];
}

static uint64_t* wordsOfBits(State& state)
{
    return reinterpret_cast<uint64_t*>(state.instance.statics());
}

static uint32_t bitNumberOf(const LayoutData& layout, const MemberGuardData& guard)
{
    return MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(guard.side()), guard.bit);
}

static bool holdsFromStart(const LayoutData& layout)
{
    return layout.classData().guardsHoldWhileClear;
}

static uint32_t listEntryOf(const LayoutData& layout, const MemberGuardData& guard)
{
    return bitNumberOf(layout, guard) | (holdsFromStart(layout) ? MemberGuardAggregates::holdsWhileClear : 0);
}

static ALWAYS_INLINE bool holds(const uint64_t* words, uint32_t listEntry)
{
    uint32_t number = MemberGuardAggregates::numberInListEntry(listEntry);
    bool isBitSet = words[MemberGuardAggregates::wordIndex(number)] & MemberGuardAggregates::mask(number);
    return isBitSet != !!(listEntry & MemberGuardAggregates::holdsWhileClear);
}

static ALWAYS_INLINE void setHolds(uint64_t* words, uint32_t listEntry, bool shouldHold)
{
    uint32_t number = MemberGuardAggregates::numberInListEntry(listEntry);
    if (shouldHold != !!(listEntry & MemberGuardAggregates::holdsWhileClear))
        words[MemberGuardAggregates::wordIndex(number)] |= MemberGuardAggregates::mask(number);
    else
        words[MemberGuardAggregates::wordIndex(number)] &= ~MemberGuardAggregates::mask(number);
}

static bool isSet(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    return holds(wordsOfBits(state), listEntryOf(layout, guard));
}

static void noteSets(State& state, MemberGuardAggregates aggregates)
{
    if (!aggregates.count)
        return;
    const uint32_t* sets = state.tables.at<uint32_t>(aggregates.offset);
    const uint64_t* words = wordsOfBits(state);
    MemberGuardWatchpoints& watchpoints = watchpointsOf(state);
    for (uint32_t index = 0; index < aggregates.count; ++index) {
        if (!holds(words, sets[index]))
            watchpoints.didSetMemberOf(sets[index], sets[aggregates.count + index]);
    }
}

enum class SetsThatHeld : bool { StayClear, AreReadAgain };

static void clearSetsContaining(State& state, const LayoutData& layout, const MemberGuardData& guard, SetsThatHeld setsThatHeld = SetsThatHeld::StayClear)
{
    MemberGuardAggregates aggregates = layout.aggregatesOf(guard);
    uint64_t* words = wordsOfBits(state);
    MemberGuardWatchpoints& watchpoints = watchpointsOf(state);
    const uint32_t* sets = state.tables.at<uint32_t>(aggregates.offset);
    for (uint32_t index = 0; index < aggregates.count; ++index) {
        uint32_t set = sets[index];
        if (setsThatHeld == SetsThatHeld::AreReadAgain && holds(words, set))
            watchpoints.didClearSetThatHeld(set, sets[aggregates.count + index]);
        setHolds(words, set, false);
        watchpoints.didChangeBit(set);
    }
    watchpoints.didChangeBit(listEntryOf(layout, guard));
}

static SetsThatHeld setsThatHeldBelowNewClass(const MemberGuardData& guard)
{
    return guard.kind() == MemberGuardKind::PassedByStores ? SetsThatHeld::AreReadAgain : SetsThatHeld::StayClear;
}

// Compiled code takes a test that passed to hold until something runs that may clear the bit, and a test that failed to fail from then on.
static void set(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    if (holdsFromStart(layout))
        return;
    uint64_t& word = wordOf(state, layout, guard);
    if ((word & guard.mask()) || watchpointsOf(state).hasEnded(guard))
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(state);
    word |= guard.mask();
    watchpointsOf(state).didChangeBit(listEntryOf(layout, guard));
    noteSets(state, layout.aggregatesOf(guard));
}

static bool clear(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    uint32_t listEntry = listEntryOf(layout, guard);
    if (!holds(wordsOfBits(state), listEntry))
        return false;
    setHolds(wordsOfBits(state), listEntry, false);
    clearSetsContaining(state, layout, guard);
    if (!holdsFromStart(layout))
        watchpointsOf(state).didEnd(guard);
    if (guard.kind() == MemberGuardKind::Member)
        count(state.instance, RuntimeCounterID::MemberGuardsCleared);
    else if (guard.kind() == MemberGuardKind::PrototypeLink)
        count(state.instance, RuntimeCounterID::PrototypeLinkGuardsCleared);
    return true;
}

static void end(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    if (clear(state, layout, guard) || holdsFromStart(layout))
        return;
    watchpointsOf(state).didEnd(guard);
    watchpointsOf(state).didCreateClassWith(MemberGuardAggregates::wordIndex(bitNumberOf(layout, guard)));
    clearSetsContaining(state, layout, guard);
}

// A store of a default is reported like any other change under the name, and the report may come before the change.
static void leaveForLater(State& state, const LayoutData& layout, const MemberGuardData& guard, SetsThatHeld setsThatHeld = SetsThatHeld::StayClear)
{
    ASSERT(guard.kind() == MemberGuardKind::PassedByStores);
    uint32_t listEntry = listEntryOf(layout, guard);
    if (holds(wordsOfBits(state), listEntry)) {
        MemberGuardWatchpoints::MethodTableUpdate update(state);
        setHolds(wordsOfBits(state), listEntry, false);
        clearSetsContaining(state, layout, guard, setsThatHeld);
    }
    watchpointsOf(state).mayHoldAgain(guard);
}

static bool isDeclaredBetween(State& state, const LayoutData& from, const LayoutData* endExclusive, ClassSide side, uint32_t key)
{
    for (const LayoutData* next = &from; next && next != endExclusive; next = state.objects->classes.assumedBaseOf(*next)) {
        if (declares(state, *next, side, key))
            return true;
    }
    return false;
}

static void leaveGuardsOfStoresForLater(State& state, const LayoutData& layout, ClassSide side)
{
    bool leavesGuardsClear = false;
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::PassedByStores)) {
        if (!isDeclaredBetween(state, layout, nullptr, side, guard.name))
            continue;
        if (!std::exchange(leavesGuardsClear, true) && side == ClassSide::Prototype)
            watchpointsOf(state).didLeaveGuardsOfStoresClear(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(side), 0), layout);
        leaveForLater(state, layout, guard, SetsThatHeld::AreReadAgain);
    }
}

static void beginSpeakingOfObjects(State& state, const LayoutData& layout, ClassSide side)
{
    if (holdsFromStart(layout) || !watchpointsOf(state).beginSpeakingOf(encodeClassSide(layout, side)))
        return;
    for (const MemberGuardData& guard : layout.memberGuards()) {
        if (guard.side() != side)
            continue;
        watchpointsOf(state).didCreateClassWith(MemberGuardAggregates::wordIndex(bitNumberOf(layout, guard)));
        if (!isSet(state, layout, guard))
            clearSetsContaining(state, layout, guard, setsThatHeldBelowNewClass(guard));
    }
}

static bool isEvaluatedOnce(State& state, const LayoutData& layout)
{
    switch (layout.evaluation()) {
    case EvaluationState::ProvenOnce:
        return true;
    case EvaluationState::PredictedOnce: {
        std::span<const MemberGuardData> guards = guardsOf(layout, ClassSide::Static, MemberGuardKind::EvaluatedOnce);
        return !guards.empty() && isSet(state, layout, guards.front());
    }
    case EvaluationState::SeenTwice:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachDescendant(State& state, const LayoutData& layout, const Invocable<IterationStatus(const LayoutData&)> auto& functor)
{
    auto subclassesOf = [&](const LayoutData& classLayout) -> std::span<const uint32_t> {
        uint32_t offset = classLayout.classData().subclassesOffset;
        if (!offset)
            return { };
        const uint32_t* list = state.tables.at<uint32_t>(offset);
        return { list + 1, list[0] };
    };
    Vector<uint32_t, 16> stack;
    stack.append(subclassesOf(layout));
    while (!stack.isEmpty()) {
        const LayoutData& descendant = *state.objects->classes.classWithIndex(stack.takeLast());
        if (functor(descendant) == IterationStatus::Continue)
            stack.append(subclassesOf(descendant));
    }
}

static void clearInheritedGuards(State& state, const LayoutData& layout, const LayoutData* endExclusive, ClassSide side)
{
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::Member)) {
        if (isSet(state, layout, guard) && !isDeclaredBetween(state, layout, endExclusive, side, guard.name))
            clear(state, layout, guard);
    }
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::PassedByStores)) {
        if (isSet(state, layout, guard))
            leaveForLater(state, layout, guard);
    }
}

static PropertyOffset offsetOfMember(VM& vm, Structure* structure, MemberName& name, unsigned& attributes)
{
    if (!isValidOffset(structure->maxOffset()))
        return invalidOffset;
    return structure->get(vm, name.uid(), attributes);
}

static bool holdsAsDeclared(VM& vm, State& state, const LayoutData& layout, ClassSide side, JSObject* holder, MemberName& name)
{
    if (side == ClassSide::Static && findOwnStaticField(layout, name.key)) {
        unsigned attributes = 0;
        return !isValidOffset(offsetOfMember(vm, holder->structure(), name, attributes)) || (attributes & PropertyAttribute::CustomValue);
    }
    bool isLazy = isLazyHolder(holder, side);
    if (declaresMember(layout, side, name.key))
        return isLazy || holdsDeclaredMember(vm, state, holder, name.uid(), declaredMember(layout, side, name.key));
    switch (side) {
    case ClassSide::Prototype:
        return isLazy || holder->getDirect(vm, name.uid()) == JSValue(registeredClassObjectOf(state, layout));
    case ClassSide::Static:
        return watchpointsOf(state).implicitMemberKeys().prototype == name.key && holderOf(vm, state, layout, ClassSide::Prototype);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Compiled code fills an empty slot of a compiled object without a report, so a name of its layout counts as a property that it has.
static bool hasPropertyOrSlot(VM& vm, JSObject* holder, MemberName& name)
{
    unsigned attributes = 0;
    if (!isCompiledObject(holder))
        return isValidOffset(offsetOfMember(vm, holder->structure(), name, attributes));
    auto& object = *uncheckedDowncast<Object>(holder);
    if (object.fieldNamed(object.state(), name.uid()))
        return true;
    JSObject* extraProperties = object.extraProperties();
    return extraProperties && isValidOffset(offsetOfMember(vm, extraProperties->structure(), name, attributes));
}

static const LayoutData* nextClassOnSide(const ClassTable& classes, const LayoutData& layout, ClassSide side)
{
    if (side == ClassSide::Static && !ClassTable::isDefinedByClass(layout))
        return nullptr;
    return classes.assumedBaseOf(layout);
}

static JSObject* objectAboveClasses(State& state, ClassSide side)
{
    JSGlobalObject* realm = state.instance.globalObject;
    switch (side) {
    case ClassSide::Prototype:
        return realm->objectPrototype();
    case ClassSide::Static:
        return realm->functionPrototype();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An object that may be a prototype reports each name that it gets and each change of its parent. Only an object that is looked up in its property table or in its layout is asked.
static bool lacksMember(VM& vm, State& state, JSObject* object, MemberName& name)
{
    Structure* structure = object->structure();
    if (!structure->mayBePrototype() || structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype())
        return false;
    // The stand-in for a prototype object reports the creation of its members to its own Instance.
    if (object->classInfo() == ClassPrototype::info())
        return Instance::of(structure->realm()) == &state.instance && !object->butterfly() && !ClassPrototype::hasMember(object, name.uid());
    if (isCompiledObject(object))
        return !uncheckedDowncast<Object>(object)->isInFrame() && !hasPropertyOrSlot(vm, object, name);
    if (object->type() != FinalObjectType || !flattenOnce(vm, object) || !structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence())
        return false;
    return !hasPropertyOrSlot(vm, object, name);
}

static bool passOtherObjects(VM& vm, State& state, JSValue first, JSObject* end, MemberName& name, ChainObjects& passed)
{
    for (JSValue next = first; next != JSValue(end); next = asObject(next)->getPrototypeDirect()) {
        if (!next.isObject() || !lacksMember(vm, state, asObject(next), name))
            return false;
        passed.append(asObject(next));
    }
    return true;
}

namespace {

class ClassesAbove {
public:
    struct Level {
        const LayoutData* classLayout { nullptr };
        JSObject* holder { nullptr };
        const LayoutData* next { nullptr };
        bool hasNameThatNobodyKnows { false };
        bool hasBaseThatNobodyKnows { false };
        bool hasFilter { false };
        bool wasAskedBefore { false };
        NameFilter declaredNames { };
    };

    ClassesAbove(VM& vm, State& state, const LayoutData& start, ClassSide side)
        : m_vm(vm)
        , m_state(state)
        , m_start(start)
        , m_side(side)
    {
    }

    ClassSide side() const { return m_side; }

    Level& at(size_t index)
    {
        while (index >= m_levels.size())
            append(m_levels.isEmpty() ? m_start : *m_levels.last().next);
        return m_levels[index];
    }

    bool declares(Level& level, uint32_t key)
    {
        if (!std::exchange(level.wasAskedBefore, true) || m_side == ClassSide::Static)
            return JSC::AOT::declares(m_state, *level.classLayout, m_side, key);
        if (!std::exchange(level.hasFilter, true)) {
            for (const MethodData& method : methodsOf(*level.classLayout, m_side)) {
                if (!(method.flags & methodHasComputedName))
                    level.declaredNames.add(method.name);
            }
            if (std::optional<uint32_t> constructor = watchpointsOf(m_state).implicitMemberKeys().constructor)
                level.declaredNames.add(*constructor);
        }
        return level.declaredNames.mayContain(key) && JSC::AOT::declares(m_state, *level.classLayout, m_side, key);
    }

private:
    void append(const LayoutData& classLayout)
    {
        ClassTable& classes = m_state.objects->classes;
        Level level;
        level.classLayout = &classLayout;
        level.holder = holderOf(m_vm, m_state, classLayout, m_side);
        if (level.holder) {
            level.hasNameThatNobodyKnows = isLazyHolder(level.holder, m_side) && hasComputedMemberName(classLayout, m_side);
            level.hasBaseThatNobodyKnows = classLayout.hasUntypedBase && !classes.assumedBaseOf(classLayout);
            level.next = nextClassOnSide(classes, classLayout, m_side);
        }
        m_levels.append(level);
    }

    VM& m_vm;
    State& m_state;
    const LayoutData& m_start;
    ClassSide m_side;
    Vector<Level, 4> m_levels;
};

} // anonymous namespace

static JSObject* prototypeOfBuiltinBase(VM& vm, State& state, const LayoutData& classLayout, ClassSide side)
{
    uint32_t builtinBase = classLayout.classData().builtinBase;
    if (side != ClassSide::Prototype || builtinBase == LayoutData::noBase)
        return nullptr;
    JSValue prototype = asObject(state.builtinCells->findObject(BuiltinID(builtinBase)))->getDirect(vm, vm.propertyNames->prototype);
    return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
}

static std::optional<GuardResolution> resolveAsDeclared(VM& vm, State& state, ClassesAbove& classes, MemberName& name)
{
    GuardResolution resolution;
    ClassSide side = classes.side();
    for (size_t index = 0;; ++index) {
        // A reference into the levels does not outlive the next one that is asked for.
        JSObject* holder = classes.at(index).holder;
        if (!holder || classes.at(index).hasNameThatNobodyKnows)
            return std::nullopt;
        const LayoutData* current = classes.at(index).classLayout;
        if (classes.declares(classes.at(index), name.key)) {
            if (!holdsAsDeclared(vm, state, *current, side, holder, name))
                return std::nullopt;
            resolution.declarer = current;
            resolution.declarerHolder = holder;
            return resolution;
        }
        if (hasPropertyOrSlot(vm, holder, name))
            return std::nullopt;
        if (JSObject* builtinPrototype = prototypeOfBuiltinBase(vm, state, *current, side); builtinPrototype && holder->getPrototypeDirect() == JSValue(builtinPrototype))
            return resolution;
        if (classes.at(index).hasBaseThatNobodyKnows)
            return std::nullopt;

        bool hasNext = classes.at(index).next;
        JSObject* nextHolder = hasNext ? classes.at(index + 1).holder : objectAboveClasses(state, side);
        if (!nextHolder || !passOtherObjects(vm, state, holder->getPrototypeDirect(), nextHolder, name, resolution.otherObjects))
            return std::nullopt;
        if (!hasNext)
            return resolution;
    }
}

static bool tryWatchDeclarer(VM& vm, State& state, const LayoutData& layout, ClassSide side, const LayoutData* declarer, JSObject* holder, MemberName& name)
{
    if (!declarer)
        return true;
    if (const FieldData* field = side == ClassSide::Static ? findOwnStaticField(*declarer, name.key) : nullptr) {
        // A field without a value is not defined yet or was deleted, and only the bit of its own class records a deletion.
        return declarer == &layout || StaticFields::isInVariable(*uncheckedDowncast<Closure>(holder), *field);
    }
    if (isLazyHolder(holder, side))
        return true;
    // Nothing can change or remove the prototype property of a class object. Class objects share their Structure, and a watchpoint on it would fire when another class leaves it.
    if (side == ClassSide::Static && ClassTable::isDefinedByClass(*declarer) && !declaresMember(*declarer, side, name.key))
        return true;
    // Whoever stores to the prototype property of a constructor function, defines it or deletes it comes through the functions of the closure, which report it and let no cache remember the store.
    if (side == ClassSide::Static && watchpointsOf(state).implicitMemberKeys().prototype == name.key && !declaresMember(*declarer, side, name.key))
        return true;
    return watchpointsOf(state).tryWatch(vm, *declarer, side, holder, name.uid());
}

static bool tryWatchMember(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard, ClassesAbove& classes)
{
    ASSERT(classes.side() == guard.side() && classes.at(0).classLayout == &layout);
    MemberName name { vm, guard.name };
    std::optional<GuardResolution> resolution = resolveAsDeclared(vm, state, classes, name);
    if (!resolution)
        return false;
    if (guard.kind() == MemberGuardKind::PassedByStores ? !!resolution->declarer : !tryWatchDeclarer(vm, state, layout, guard.side(), resolution->declarer, resolution->declarerHolder, name))
        return false;
    if (!resolution->otherObjects.isEmpty())
        watchpointsOf(state).watchOtherObjects(resolution->otherObjects.span(), layout, guard, name.uid());
    return true;
}

static bool tryWatchMemberPassedByStores(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    JSObject* holder = holderOf(vm, state, layout, ClassSide::Prototype);
    if (!holder)
        return false;
    JSGlobalObject* realm = state.instance.globalObject;
    Structure* structureBelow = realm->structureCache().emptyObjectStructureForPrototype(realm, holder, JSFinalObject::defaultInlineCapacity);
    MemberName name { vm, guard.name };
    ChainWalkStart start { realm, structureBelow, nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores };
    ChainWalk walk = walkChain(start, name.uid());
    // A walk that stops at a dictionary and flattens it has found nothing out about the name. An object is flattened once, so each further walk gets past one more.
    while (walk.didFlatten)
        walk = walkChain(start, name.uid());
    if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        return false;
    watchpointsOf(state).watchOtherObjects(walk.changeableObjects.span(), layout, guard, name.uid());
    return true;
}

static const MemberGuardData* findClearGuardOfStoresUnder(VM& vm, State& state, const LayoutData& layout, UniquedStringImpl* uid)
{
    if (!layout.classData().guardWordsOffsetFor(ClassSide::Prototype) || guardsOf(layout, ClassSide::Prototype, MemberGuardKind::PassedByStores).empty())
        return nullptr;
    std::optional<uint32_t> key = vm.aotState().identifiers->numberOf(uid);
    const MemberGuardData* guard = key ? findGuard(layout, ClassSide::Prototype, MemberGuardKind::PassedByStores, *key) : nullptr;
    if (!guard || isSet(state, layout, *guard) || watchpointsOf(state).hasEnded(*guard) || layout.evaluation() == EvaluationState::SeenTwice)
        return nullptr;
    return guard;
}

static void setGuardOfStores(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    SetForScope aStoreByNameHasAsked(watchpointsOf(state).aStoreByNameHasAsked(), true);
    MemberGuardWatchpoints::MethodTableUpdate update(state);
    uint32_t listEntry = listEntryOf(layout, guard);
    setHolds(wordsOfBits(state), listEntry, true);
    watchpointsOf(state).didChangeBit(listEntry);
    noteSets(state, layout.aggregatesOf(guard));
}

// The guard of the class that stores is set, and the store came by name all the same: a set that holds the guard is clear. A class that has stored nothing yet, an abstract base for one, may be all that stands in its way.
static void completeSetsOfGuardOfStores(VM& vm, State& state, const LayoutData& layout, UniquedStringImpl* uid)
{
    if (!layout.classData().guardWordsOffsetFor(ClassSide::Prototype))
        return;
    std::optional<uint32_t> key = vm.aotState().identifiers->numberOf(uid);
    const MemberGuardData* guard = key ? findGuard(layout, ClassSide::Prototype, MemberGuardKind::PassedByStores, *key) : nullptr;
    if (!guard || !isSet(state, layout, *guard))
        return;
    SetForScope aStoreByNameHasAsked(watchpointsOf(state).aStoreByNameHasAsked(), true);
    MemberGuardWatchpoints::MethodTableUpdate update(state);
    noteSets(state, layout.aggregatesOf(*guard));
}

void ClassTable::didStoreByName(VM& vm, const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    const MemberGuardData* guard = findClearGuardOfStoresUnder(vm, m_state, layout, uid);
    MemberGuardWatchpoints& watchpoints = watchpointsOf(m_state);
    if (!guard || watchpoints.wasFoundNotToHold(*guard) || !watchpoints.wasAskedBefore(*guard))
        return;
    if (tryWatchMemberPassedByStores(vm, m_state, layout, *guard))
        setGuardOfStores(m_state, layout, *guard);
    else
        watchpoints.didFindNotToHold(*guard);
}

void ClassTable::didFindThatStoresPass(VM& vm, const LayoutData& instanceLayout, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    const MemberGuardData* guard = findClearGuardOfStoresUnder(vm, m_state, layout, uid);
    if (!guard) {
        completeSetsOfGuardOfStores(vm, m_state, layout, uid);
        return;
    }
    watchpointsOf(m_state).watchOtherObjects(changeableObjects, layout, *guard, uid);
    setGuardOfStores(m_state, layout, *guard);
}

static void updateMemberGuard(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard, ClassesAbove& classes)
{
    if (tryWatchMember(vm, state, layout, guard, classes))
        set(state, layout, guard);
    else
        clear(state, layout, guard);
}

static void updateMemberGuard(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    ClassesAbove classes(vm, state, layout, guard.side());
    updateMemberGuard(vm, state, layout, guard, classes);
}

// A member that the program assigns to the prototype object is not there yet when the class is defined. An access by name that has been answered looks again.
void ClassTable::didFindMemberByName(VM& vm, const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    // The guards of a constructor function begin with its first object, behind what the program assigns.
    if (!isDefinedByClass(layout))
        return;
    if (holdsFromStart(layout) || !layout.numberOfMemberGuards() || layout.evaluation() == EvaluationState::SeenTwice || !classPrototypeIfExists(layout))
        return;
    std::optional<uint32_t> key = vm.aotState().identifiers->numberOf(uid);
    const MemberGuardData* guard = key ? findGuard(layout, ClassSide::Prototype, MemberGuardKind::Member, *key) : nullptr;
    MemberGuardWatchpoints& watchpoints = watchpointsOf(m_state);
    if (!guard || isSet(m_state, layout, *guard) || watchpoints.hasEnded(*guard) || watchpoints.wasFoundNotToHold(*guard))
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    updateMemberGuard(vm, m_state, layout, *guard);
    if (!isSet(m_state, layout, *guard))
        watchpoints.didFindNotToHold(*guard);
}

uint64_t* ClassTable::guardWords(const LayoutData& layout, ClassSide side) const
{
    std::optional<uint32_t> offset = layout.classData().guardWordsOffsetFor(side);
    return offset ? reinterpret_cast<uint64_t*>(m_state.instance.statics() + *offset) : nullptr;
}

const LayoutData* ClassTable::classWithGuardsHeldBy(JSObject* holder, ClassSide& side) const
{
    auto found = m_classSidesByHolder.find(holder);
    if (found == m_classSidesByHolder.end())
        return classLeftToRecordHeldBy(holder, side);
    side = static_cast<ClassSide>(found->value & 1);
    return classWithIndex(found->value >> 1);
}

const LayoutData* ClassTable::classHeldBy(JSObject* holder, ClassSide& side) const
{
    const LayoutData* layout = classWithGuardsHeldBy(holder, side);
    return layout || !m_hasHoldersLeftOut ? layout : classOfHolderLeftOut(holder, side);
}

// Nothing can change or remove the prototype property of a class object, so a class object that names the holder as its prototype proves what the holder is.
const LayoutData* ClassTable::classOfHolderLeftOut(JSObject* holder, ClassSide& side) const
{
    VM& vm = *m_state.instance.vm;
    auto* classObject = dynamicDowncast<Closure>(holder);
    side = classObject ? ClassSide::Static : ClassSide::Prototype;
    if (!classObject) {
        if (holder->type() != FinalObjectType)
            return nullptr;
        JSValue constructor = holder->getDirect(vm, vm.propertyNames->constructor);
        classObject = constructor && constructor.isCell() ? dynamicDowncast<Closure>(constructor.asCell()) : nullptr;
        if (!classObject || &classObject->state() != &m_state)
            return nullptr;
    }
    const LayoutData* layout = classObject->classData();
    if (!layout || !isDefinedByClass(*layout) || !watchpointsOf(m_state).hasNoGuardLeft(*layout))
        return nullptr;
    JSValue prototype = classObject->getDirect(vm, vm.propertyNames->prototype);
    if (!prototype || (side == ClassSide::Prototype && prototype != JSValue(holder)))
        return nullptr;
    return layout;
}

bool ClassTable::tryLeaveHoldersOut(VM& vm, const LayoutData& layout, JSObject* prototype)
{
    if (!watchpointsOf(m_state).hasNoGuardLeft(layout))
        return false;
    prototype->didBecomePrototype(vm);
    m_hasHoldersLeftOut = true;
    return true;
}

void ClassTable::didCreatePrototypeLeftToRecord(const LayoutData& layout)
{
    ASSERT(leavesDefinitionToRecord(layout) && guardsOf(layout, ClassSide::Static, MemberGuardKind::PassedByStores).empty());
    leaveGuardsOfStoresForLater(m_state, layout, ClassSide::Prototype);
}

const LayoutData* ClassTable::classLeftToRecordHeldBy(JSObject* holder, ClassSide& side) const
{
    if (auto* prototype = dynamicDowncast<ClassPrototype>(holder)) {
        const LayoutData* layout = classWithIndex(prototype->classIndex());
        if (!layout || !leavesDefinitionToRecord(*layout) || classPrototypeIfExists(*layout) != holder)
            return nullptr;
        side = ClassSide::Prototype;
        return layout;
    }
    auto* classObject = dynamicDowncast<Closure>(holder);
    const LayoutData* layout = classObject ? classObject->classData() : nullptr;
    if (!layout || !leavesDefinitionToRecord(*layout) || registeredClassObjectOf(m_state, *layout) != classObject)
        return nullptr;
    side = ClassSide::Static;
    return layout;
}

static std::span<const MemberGuardData> guardsOn(const LayoutData& layout, ClassSide side)
{
    auto range = std::ranges::equal_range(layout.memberGuards(), side, { }, &MemberGuardData::side);
    return { range.begin(), range.end() };
}

static bool hasSpokenOfObjects(State& state, const LayoutData& layout, ClassSide side, size_t numberOfGuards)
{
    uint32_t firstWord = MemberGuardAggregates::wordIndex(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(side), 0));
    for (size_t word = 0; word * MemberGuardData::bitsPerWord < numberOfGuards; ++word) {
        if (watchpointsOf(state).isAboutExistingClass(firstWord + word))
            return true;
    }
    return false;
}

static bool appendNamesOfHolder(VM& vm, State& state, const LayoutData& layout, JSObject* holder, MemberGuardWatchpoints::Names& names)
{
    constexpr ClassSide side = ClassSide::Prototype;
    bool isLazy = isLazyHolder(holder, side);
    if (isLazy && hasComputedMemberName(layout, side))
        return false;
    const ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    auto appendPropertiesOf = [&](JSObject* object) {
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            std::optional<uint32_t> key = identifiers.numberOf(entry.key());
            if (key && !(isLazy && declares(state, layout, side, *key)))
                names.append(*key);
            return true;
        });
    };
    if (isCompiledObject(holder)) {
        auto& object = *uncheckedDowncast<Object>(holder);
        if (object.isInFrame())
            return false;
        for (const FieldData& field : object.layout().fields())
            names.append(field.name);
        if (JSObject* extraProperties = object.extraProperties())
            appendPropertiesOf(extraProperties);
    } else
        appendPropertiesOf(holder);
    if (isLazy)
        return true;
    for (const MethodData& method : methodsOf(layout, side)) {
        if (!(method.flags & methodHasComputedName))
            names.append(method.name);
    }
    if (std::optional<uint32_t> constructor = watchpointsOf(state).implicitMemberKeys().constructor)
        names.append(*constructor);
    return true;
}

static bool isAsDeclaredOn(VM& vm, State& state, const ChainLevel& level, uint32_t key)
{
    constexpr ClassSide side = ClassSide::Prototype;
    if (!declares(state, *level.classLayout, side, key))
        return false;
    MemberName name { vm, key };
    return holdsAsDeclared(vm, state, *level.classLayout, side, level.holder, name) && tryWatchDeclarer(vm, state, *level.classLayout, side, level.classLayout, level.holder, name);
}

static bool findsMemberAsDeclared(VM& vm, State& state, std::span<const ChainLevel> chain, uint32_t key)
{
    using MemberVerdict = MemberGuardWatchpoints::MemberVerdict;
    constexpr ClassSide side = ClassSide::Prototype;
    for (const ChainLevel& level : chain) {
        if (!level.namesToLookFor.mayContain(key))
            continue;
        std::optional<size_t> index = level.names->find(key);
        if (!index) {
            if (isLazyHolder(level.holder, side) && declares(state, *level.classLayout, side, key))
                return true;
            continue;
        }
        MemberVerdict& verdict = level.names->verdicts[*index];
        if (verdict == MemberVerdict::NotLookedAt)
            verdict = isAsDeclaredOn(vm, state, level, key) ? MemberVerdict::IsAsDeclared : MemberVerdict::IsNotAsDeclared;
        return verdict == MemberVerdict::IsAsDeclared;
    }
    return true;
}

static bool isOnNoLevel(State& state, std::span<const ChainLevel> chain, uint32_t key)
{
    return std::ranges::none_of(chain, [&](const ChainLevel& level) {
        if (!level.namesToLookFor.mayContain(key))
            return false;
        return level.names->find(key) || (isLazyHolder(level.holder, ClassSide::Prototype) && declares(state, *level.classLayout, ClassSide::Prototype, key));
    });
}

static bool tryArmWholePrototypeSide(VM& vm, State& state, const LayoutData& layout, JSObject* holder)
{
    constexpr ClassSide side = ClassSide::Prototype;
    ClassTable& classes = state.objects->classes;
    MemberGuardWatchpoints& watchpoints = watchpointsOf(state);
    std::span<const MemberGuardData> guards = guardsOn(layout, side);
    if (!guards.empty() && hasSpokenOfObjects(state, layout, side, guards.size()))
        return false;
    if (holderOf(vm, state, layout, side) != holder || watchpoints.hasChangedPrototypeSide(layout))
        return false;

    MemberGuardWatchpoints::Names ownNames;
    if (!appendNamesOfHolder(vm, state, layout, holder, ownNames))
        return false;
    Vector<ChainLevel, 8> chain;
    chain.append({ &layout, holder, nullptr });
    while (true) {
        const LayoutData* current = chain.last().classLayout;
        if (current->hasUntypedBase && !classes.assumedBaseOf(*current))
            return false;
        const LayoutData* next = nextClassOnSide(classes, *current, side);
        JSObject* nextHolder = next ? holderOf(vm, state, *next, side) : objectAboveClasses(state, side);
        if (!nextHolder || chain.last().holder->getPrototypeDirect() != JSValue(nextHolder))
            return false;
        if (!next)
            break;
        if (!watchpoints.namesLookedUpBelow(*next)) {
            MemberGuardWatchpoints::Names namesOfNext;
            if (watchpoints.hasChangedPrototypeSide(*next) || !appendNamesOfHolder(vm, state, *next, nextHolder, namesOfNext))
                return false;
            watchpoints.didArmWholePrototypeSide(*next, WTF::move(namesOfNext));
        }
        chain.append({ next, nextHolder, nullptr });
    }
    watchpoints.didArmWholePrototypeSide(layout, WTF::move(ownNames));
    if (guards.empty())
        return true;
    for (ChainLevel& level : chain) {
        level.names = watchpoints.namesLookedUpBelow(*level.classLayout);
        for (uint32_t key : level.names->keys)
            level.namesToLookFor.add(key);
        if (!isLazyHolder(level.holder, side))
            continue;
        for (const MethodData& method : methodsOf(*level.classLayout, side)) {
            if (!(method.flags & methodHasComputedName))
                level.namesToLookFor.add(method.name);
        }
        if (std::optional<uint32_t> constructor = watchpoints.implicitMemberKeys().constructor)
            level.namesToLookFor.add(*constructor);
    }

    Vector<const MemberGuardData*, 16> guardsOfOtherMembers;
    bool leavesGuardsOfStoresClear = false;
    for (const MemberGuardData& guard : guards) {
        if (guard.kind() == MemberGuardKind::Member && !findsMemberAsDeclared(vm, state, chain.span(), guard.name))
            guardsOfOtherMembers.append(&guard);
        if (guard.kind() == MemberGuardKind::PassedByStores && !isOnNoLevel(state, chain.span(), guard.name)) {
            guardsOfOtherMembers.append(&guard);
            leavesGuardsOfStoresClear = true;
        }
    }
    if (leavesGuardsOfStoresClear)
        watchpoints.didLeaveGuardsOfStoresClear(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(side), 0), layout);
    watchpoints.beginSpeakingOf(encodeClassSide(layout, side));
    uint64_t* words = classes.guardWords(layout, side);
    uint32_t firstWord = MemberGuardAggregates::wordIndex(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(side), 0));
    for (size_t first = 0; first < guards.size(); first += MemberGuardData::bitsPerWord) {
        size_t count = std::min<size_t>(guards.size() - first, MemberGuardData::bitsPerWord);
        words[first / MemberGuardData::bitsPerWord] = count == MemberGuardData::bitsPerWord ? ~0ull : (1ull << count) - 1;
        watchpoints.didCreateClassWith(firstWord + first / MemberGuardData::bitsPerWord);
    }
    for (const MemberGuardData* guard : guardsOfOtherMembers)
        words[guard->wordIndex()] &= ~guard->mask();
    if (std::span<const MemberGuardAggregates> aggregates = layout.memberGuardAggregates(); !aggregates.empty()) {
        const MemberGuardData* firstGuardOfClass = layout.memberGuards().data();
        for (const MemberGuardData& guard : guards) {
            if (words[guard.wordIndex()] & guard.mask())
                noteSets(state, aggregates[&guard - firstGuardOfClass]);
        }
    }
    for (const MemberGuardData* guard : guardsOfOtherMembers)
        clearSetsContaining(state, layout, *guard, setsThatHeldBelowNewClass(*guard));
    return true;
}

static void watchOwnMember(VM& vm, State& state, const LayoutData& layout, ClassSide side, JSObject* holder, uint32_t key, ClassTable::HolderState holderState = ClassTable::HolderState::Unknown)
{
    if (!layout.classData().subclassesOffset && !findGuard(layout, side, MemberGuardKind::Member, key))
        return;
    MemberName name { vm, key };
    bool isAsDeclared = (holderState == ClassTable::HolderState::AsDefinitionMadeIt && ClassTable::hasOneClassObject(layout)) || holdsAsDeclared(vm, state, layout, side, holder, name);
    ASSERT(isAsDeclared == holdsAsDeclared(vm, state, layout, side, holder, name));
    if (!isAsDeclared || !tryWatchDeclarer(vm, state, layout, side, &layout, holder, name))
        state.objects->classes.didChangeMember(layout, side, name.uid());
}

static void watchMembersOfHolder(VM& vm, State& state, const LayoutData& layout, ClassSide side, JSObject* holder, ClassTable::HolderState holderState)
{
    if (isLazyHolder(holder, side))
        return;
    const ImplicitMemberKeys& implicit = watchpointsOf(state).implicitMemberKeys();
    for (const MethodData& method : methodsOf(layout, side)) {
        if (!(method.flags & (methodHasComputedName | methodIsCreatedAtDefinition)))
            watchOwnMember(vm, state, layout, side, holder, method.name, holderState);
    }
    if (side == ClassSide::Prototype && implicit.constructor)
        watchOwnMember(vm, state, layout, side, holder, *implicit.constructor, holderState);
    if (holderState == ClassTable::HolderState::AsDefinitionMadeIt)
        return;
    const ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    Vector<UniquedStringImpl*, 4> otherNames;
    holder->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        std::optional<uint32_t> key = identifiers.numberOf(entry.key());
        if (key && !declares(state, layout, side, *key))
            otherNames.append(entry.key());
        return true;
    });
    for (UniquedStringImpl* uid : otherNames)
        state.objects->classes.didChangeMember(layout, side, uid);
}

void ClassTable::armMemberGuards(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, HolderState holderState)
{
    ASSERT(holder);
    // The holders of every evaluation are entered: a constructor allocates an instance of the class whose prototype object it is handed. The guards describe the first evaluation.
    bool isFirstCall = m_classSidesByHolder.add(holder, encodeClassSide(layout, side)).isNewEntry;
    if (!isFirstCall && !m_sidesEnteredWithoutGuards.isEmpty())
        isFirstCall = m_sidesEnteredWithoutGuards.remove(encodeClassSide(layout, side));
    if (holdsFromStart(layout)) {
        if (!isFirstCall && holderState == HolderState::NotTouched)
            return;
        if (!layout.numberOfMemberGuards() && !layout.classData().subclassesOffset) {
            if (isFirstCall)
                m_writes.recordAll();
            if (side == ClassSide::Prototype)
                holder->didBecomePrototype(vm);
            return;
        }
    }
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    bool describesHolder = layout.evaluation() != EvaluationState::SeenTwice && registeredHolderOf(vm, m_state, layout, side) == holder;
    if (holdsFromStart(layout)) {
        if (isFirstCall) {
            leaveGuardsOfStoresForLater(m_state, layout, side);
            m_writes.recordAll();
        }
        if (side == ClassSide::Prototype)
            holder->didBecomePrototype(vm);
        if (describesHolder)
            watchMembersOfHolder(vm, m_state, layout, side, holder, holderState);
        else
            disarmMemberGuards(layout);
        return;
    }
    if (isFirstCall) {
        m_writes.recordAll();
        watchpointsOf(m_state).didArmHolder(layout, side);
        if (describesHolder && side == ClassSide::Prototype) {
            holder->didBecomePrototype(vm);
            if (tryArmWholePrototypeSide(vm, m_state, layout, holder))
                return;
        }
    }
    if (!describesHolder) {
        beginSpeakingOfObjects(m_state, layout, side);
        return;
    }
    // Only an object whose Structure may be a prototype reports an added name to VM::invalidateStructureChainIntegrity().
    // A class object reports what happens to it through its method table. A constructor function reports like a prototype object.
    if (side == ClassSide::Prototype || !isDefinedByClass(layout)) {
        holder->didBecomePrototype(vm);
        if (side == ClassSide::Static)
            m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
    }
    if (isFirstCall) {
        bool isKnownWhenEvaluatedAgain = isDefinedByClass(layout) || layout.evaluation() == EvaluationState::ProvenOnce;
        for (MemberGuardKind kind : { MemberGuardKind::PrototypeLink, MemberGuardKind::EvaluatedOnce, MemberGuardKind::InstancePrototype, MemberGuardKind::NotHidden }) {
            if (kind == MemberGuardKind::EvaluatedOnce && !isKnownWhenEvaluatedAgain)
                continue;
            for (const MemberGuardData& guard : guardsOf(layout, side, kind))
                set(m_state, layout, guard);
        }
    }
    if (isFirstCall && side == ClassSide::Prototype && isLazyHolder(holder, side) && hasComputedMemberName(layout, side))
        watchpointsOf(m_state).didLeaveMembersUnarmed(layout);
    bool mayArmCreatedMembers = !isFirstCall && isEvaluatedOnce(m_state, layout);
    ClassesAbove classesAbove(vm, m_state, layout, side);
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::Member)) {
        if (isFirstCall || isSet(m_state, layout, guard) || (mayArmCreatedMembers && isCreatedAtDefinition(layout, side, guard.name)))
            updateMemberGuard(vm, m_state, layout, guard, classesAbove);
    }
    if (isFirstCall) {
        bool leavesGuardsOfStoresClear = false;
        for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::PassedByStores)) {
            if (tryWatchMember(vm, m_state, layout, guard, classesAbove))
                set(m_state, layout, guard);
            else {
                leaveForLater(m_state, layout, guard);
                leavesGuardsOfStoresClear = true;
            }
        }
        if (leavesGuardsOfStoresClear)
            watchpointsOf(m_state).didLeaveGuardsOfStoresClear(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(side), 0), layout);
    }
    beginSpeakingOfObjects(m_state, layout, side);
    if (isFirstCall || isLazyHolder(holder, side))
        return;
    forEachDescendant(m_state, layout, [&](const LayoutData& descendant) {
        for (const MemberGuardData& guard : guardsOf(descendant, side, MemberGuardKind::Member)) {
            if (!isSet(m_state, descendant, guard) || isDeclaredBetween(m_state, descendant, &layout, side, guard.name))
                continue;
            if (declares(m_state, layout, side, guard.name))
                updateMemberGuard(vm, m_state, descendant, guard);
        }
        return IterationStatus::Continue;
    });
}

void ClassTable::enterHolderWithoutGuards(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    ASSERT(!holdsFromStart(layout) && !isDefinedByClass(layout));
    if (!m_classSidesByHolder.add(holder, encodeClassSide(layout, side)).isNewEntry)
        return;
    m_writes.recordAll();
    m_sidesEnteredWithoutGuards.add(encodeClassSide(layout, side));
    watchpointsOf(m_state).didArmHolder(layout, side);
    holder->didBecomePrototype(vm);
    if (side == ClassSide::Static)
        m_state.objects->closures.didGivePrototypePropertyTo(vm, holder);
}

void ClassTable::didChangeMember(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid)
{
    if (m_memberBeingDefined.classLayout == &layout && m_memberBeingDefined.side == side && m_memberBeingDefined.uid == uid)
        return;
    if (!layout.numberOfMemberGuards() && !layout.classData().subclassesOffset)
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    std::optional<uint32_t> key = m_state.instance.vm->aotState().identifiers->numberOf(uid);
    if (!key)
        return;
    if (side == ClassSide::Static) {
        const FieldData* field = findOwnStaticField(layout, *key);
        Closure* owner = field ? registeredClassObjectOf(m_state, layout) : nullptr;
        if (owner && StaticFields::isInVariable(*owner, *field))
            return;
    }
    if (side == ClassSide::Prototype)
        watchpointsOf(m_state).didChangeNameOnPrototypeSide(layout, *key);
    auto endGuardsOf = [&](const LayoutData& inheriting) {
        if (const MemberGuardData* guard = findGuard(inheriting, side, MemberGuardKind::Member, *key)) {
            clear(m_state, inheriting, *guard);
            watchpointsOf(m_state).mayHoldAgain(*guard);
        }
        if (const MemberGuardData* guard = findGuard(inheriting, side, MemberGuardKind::PassedByStores, *key))
            leaveForLater(m_state, inheriting, *guard);
    };
    endGuardsOf(layout);
    if (!layout.classData().subclassesOffset)
        return;
    NamedGuardsBelow& below = m_namedGuardsBelow.add(encodeClassSide(layout, side), NamedGuardsBelow { }).iterator->value;
    if (!std::exchange(below.hasWalked, true)) {
        forEachDescendant(m_state, layout, [&](const LayoutData& descendant) {
            if (declares(m_state, descendant, side, *key))
                return IterationStatus::Done;
            endGuardsOf(descendant);
            return IterationStatus::Continue;
        });
        return;
    }
    for (uint64_t nameAndClass : classesBelowWithGuardsNamed(below, layout, side, *key)) {
        const LayoutData& descendant = *classWithIndex(static_cast<uint32_t>(nameAndClass));
        if (!isDeclaredBetween(m_state, descendant, &layout, side, *key))
            endGuardsOf(descendant);
    }
}

std::span<const uint64_t> ClassTable::classesBelowWithGuardsNamed(NamedGuardsBelow& below, const LayoutData& layout, ClassSide side, uint32_t key)
{
    Vector<uint64_t>& list = below.namesAndClasses;
    if (!std::exchange(below.isListed, true)) {
        forEachDescendant(m_state, layout, [&](const LayoutData& descendant) {
            for (MemberGuardKind kind : { MemberGuardKind::Member, MemberGuardKind::PassedByStores }) {
                for (const MemberGuardData& guard : guardsOf(descendant, side, kind))
                    list.append(static_cast<uint64_t>(guard.name) << 32 | descendant.index);
            }
            return IterationStatus::Continue;
        });
        AOT::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
        list.shrinkToFit();
    }
    auto named = std::ranges::equal_range(list, key, { }, [](uint64_t nameAndClass) {
        return static_cast<uint32_t>(nameAndClass >> 32);
    });
    return { named.begin(), named.end() };
}

void ClassTable::didDefineMember(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid, JSObject* function)
{
    std::optional<FunctionID> defined = functionOf(function);
    bool isNamedByClosureTable = defined && m_state.objects->closures.hasCellFor(*defined);
    if (!isNamedByClosureTable && holdsFromStart(layout) && !layout.classData().subclassesOffset && m_classSidesByHolder.contains(holder)) {
        std::optional<uint32_t> key = layout.numberOfMemberGuards() ? vm.aotState().identifiers->numberOf(uid) : std::nullopt;
        if (!key || (!findGuard(layout, side, MemberGuardKind::Member, *key) && !findGuard(layout, side, MemberGuardKind::PassedByStores, *key)))
            return;
    }
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    if (holdsFromStart(layout)) {
        if (!m_classSidesByHolder.contains(holder))
            armMemberGuards(vm, layout, side, holder);
        std::optional<uint32_t> definedKey = vm.aotState().identifiers->numberOf(uid);
        if (definedKey && isCreatedAtDefinition(layout, side, *definedKey) && registeredHolderOf(vm, m_state, layout, side) == holder) {
            if (holdsDeclaredMember(vm, m_state, holder, uid, declaredMember(layout, side, *definedKey)))
                watchOwnMember(vm, m_state, layout, side, holder, *definedKey);
            return;
        }
        didChangeMember(layout, side, uid);
        return;
    }
    didChangeMember(layout, side, uid);
    if (!m_classSidesByHolder.contains(holder)) {
        armMemberGuards(vm, layout, side, holder);
        return;
    }
    std::optional<uint32_t> key = vm.aotState().identifiers->numberOf(uid);
    if (layout.evaluation() == EvaluationState::SeenTwice || registeredHolderOf(vm, m_state, layout, side) != holder)
        return;
    if (side == ClassSide::Prototype && !isLazyHolder(holder, side) && watchpointsOf(m_state).takeMembersLeftUnarmed(layout)) {
        ClassesAbove classesAbove(vm, m_state, layout, side);
        for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::Member)) {
            if (!isSet(m_state, layout, guard))
                updateMemberGuard(vm, m_state, layout, guard, classesAbove);
        }
    }
    if (!key)
        return;

    if (const MemberGuardData* guard = findGuard(layout, side, MemberGuardKind::Member, *key)) {
        if (isSet(m_state, layout, *guard) || (isEvaluatedOnce(m_state, layout) && isCreatedAtDefinition(layout, side, *key)))
            updateMemberGuard(vm, m_state, layout, *guard);
    }
    if (isLazyHolder(holder, side) || !declares(m_state, layout, side, *key))
        return;
    forEachDescendant(m_state, layout, [&](const LayoutData& descendant) {
        const MemberGuardData* guard = findGuard(descendant, side, MemberGuardKind::Member, *key);
        if (guard && isSet(m_state, descendant, *guard) && !isDeclaredBetween(m_state, descendant, &layout, side, *key))
            updateMemberGuard(vm, m_state, descendant, *guard);
        return IterationStatus::Continue;
    });
}

void ClassTable::didChangeEveryMember(const LayoutData& layout, ClassSide side)
{
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    if (side == ClassSide::Prototype)
        watchpointsOf(m_state).didChangePrototypeSide(layout);
    auto clearGuards = [&](const LayoutData& inheriting) {
        clearInheritedGuards(m_state, inheriting, &layout, side);
        return IterationStatus::Continue;
    };
    clearGuards(layout);
    forEachDescendant(m_state, layout, clearGuards);
}

// Some callers report the change before they make it, so nothing is looked up again here.
void ClassTable::didChangePrototypeLink(const LayoutData& layout, ClassSide side)
{
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    const LayoutData* base = assumedBaseOf(layout);
    if (side == ClassSide::Prototype)
        watchpointsOf(m_state).didChangePrototypeSide(layout);
    auto clearGuards = [&](const LayoutData& inheriting) {
        for (const MemberGuardData& guard : guardsOf(inheriting, side, MemberGuardKind::PrototypeLink))
            clear(m_state, inheriting, guard);
        clearInheritedGuards(m_state, inheriting, base, side);
        return IterationStatus::Continue;
    };
    clearGuards(layout);
    forEachDescendant(m_state, layout, clearGuards);
}

void ClassTable::didDefineWithOtherParent(const LayoutData& layout, ClassSide side)
{
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    if (side == ClassSide::Prototype)
        watchpointsOf(m_state).didChangePrototypeSide(layout);
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::PrototypeLink))
        end(m_state, layout, guard);
}

void ClassTable::didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl* uid)
{
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (!layout.classData().guardWordsOffsetFor(ClassSide::Prototype))
        return;
    std::optional<uint32_t> key = m_state.instance.vm->aotState().identifiers->numberOf(uid);
    const MemberGuardData* guard = key ? findGuard(layout, ClassSide::Prototype, MemberGuardKind::NotHidden, *key) : nullptr;
    if (!guard)
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    end(m_state, layout, *guard);
}

void ClassTable::didChangeInstancePrototype(VM& vm, const LayoutData& instanceLayout, JSValue prototype)
{
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    if (!instanceLayout.isClass())
        return;
    const LayoutData& layout = classOfInstances(instanceLayout);
    if (!layout.classData().guardWordsOffsetFor(ClassSide::Prototype))
        return;
    JSObject* holder = holderOf(vm, m_state, layout, ClassSide::Prototype);
    if (holder && prototype == JSValue(holder))
        return;

    for (const MemberGuardData& guard : guardsOf(layout, ClassSide::Prototype, MemberGuardKind::InstancePrototype))
        end(m_state, layout, guard);
    for (const MemberGuardData& guard : guardsOf(layout, ClassSide::Prototype, MemberGuardKind::NotHidden)) {
        MemberName name { vm, guard.name };
        ChainObjects passed;
        if (holder && isSet(m_state, layout, guard) && passOtherObjects(vm, m_state, prototype, holder, name, passed))
            watchpointsOf(m_state).watchOtherObjects(passed.span(), layout, guard, name.uid());
        else
            end(m_state, layout, guard);
    }
}

void ClassTable::disarmMemberGuards(const LayoutData& classLayout)
{
    if (!watchpointsOf(m_state).beginDisarming(classLayout))
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    for (ClassSide side : allMembersOf<ClassSide>) {
        m_sidesEnteredWithoutGuards.remove(encodeClassSide(classLayout, side));
        didChangeEveryMember(classLayout, side);
        didChangePrototypeLink(classLayout, side);
    }
    for (const MemberGuardData& guard : classLayout.memberGuards())
        end(m_state, classLayout, guard);
    watchpointsOf(m_state).stopWatching(classLayout);

    // A validating run looks at what every holder gets.
    if (m_state.instance.vm->aotState().validatesRuntime || !m_state.tables.classPrototypeNames().empty() || !isDefinedByClass(classLayout))
        return;
    bool hasGuardOfStores = false;
    auto lookForGuardOfStores = [&](const LayoutData& inheriting) {
        hasGuardOfStores |= std::ranges::any_of(inheriting.memberGuards(), [](const MemberGuardData& guard) {
            return guard.kind() == MemberGuardKind::PassedByStores;
        });
        return hasGuardOfStores ? IterationStatus::Done : IterationStatus::Continue;
    };
    if (lookForGuardOfStores(classLayout) == IterationStatus::Continue)
        forEachDescendant(m_state, classLayout, lookForGuardOfStores);
    if (!hasGuardOfStores)
        watchpointsOf(m_state).didLeaveNoGuard(classLayout);
}

void didEvaluateClassAgain(State& state, const LayoutData& classLayout)
{
    state.objects->classes.disarmMemberGuards(classLayout);
    count(state.instance, RuntimeCounterID::ClassesEvaluatedAgain);
}

MemberGuardWatchpoint::MemberGuardWatchpoint(const ObjectPropertyCondition& condition, ClassTable& classes, const LayoutData& classLayout, ClassSide side)
    : AdaptiveInferredPropertyValueWatchpointBase(condition)
    , m_classes(classes)
    , m_classLayout(classLayout)
    , m_side(side)
{
}

void MemberGuardWatchpoint::handleFire(VM&, const FireDetail&)
{
    m_classes.didChangeMember(m_classLayout, m_side, key().uid());
}

MemberGuardWatchpoints::MemberGuardWatchpoints(State& state)
    : m_state(state)
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
{
    if (std::span<const MethodTableDependentData> dependents = state.tables.methodTableDependents(); !dependents.empty()) {
        m_firstBitWithDependents = dependents.front().guardBit;
        m_numberOfBitsAmongDependents = dependents.back().guardBit - dependents.front().guardBit + 1;
    }
}

MemberGuardWatchpoints::~MemberGuardWatchpoints() = default;

MemberGuardWatchpoints::MethodTableUpdate::MethodTableUpdate(State& state)
    : m_watchpoints(watchpointsOf(state))
{
    ++m_watchpoints.m_numberOfOpenUpdates;
}

MemberGuardWatchpoints::MethodTableUpdate::~MethodTableUpdate()
{
    if (m_watchpoints.m_numberOfOpenUpdates == 1) {
        m_watchpoints.speakOfCreatedHolders();
        m_watchpoints.setCompleteSets();
    }
    if (!--m_watchpoints.m_numberOfOpenUpdates)
        m_watchpoints.updateMethodTable();
}

bool MemberGuardWatchpoints::tryArmGuardOfStoresInTheWay(uint32_t listEntry, bool setHeldBefore)
{
    if ((!m_aStoreByNameHasAsked && !setHeldBefore) || m_classesWithClearGuardsOfStores.isEmpty())
        return false;
    if (!std::exchange(m_classesWithClearGuardsOfStoresAreSorted, true))
        AOT::sort(m_classesWithClearGuardsOfStores);
    uint32_t number = MemberGuardAggregates::numberInListEntry(listEntry);
    auto above = std::ranges::upper_bound(m_classesWithClearGuardsOfStores, number, { }, [](const auto& entry) {
        return entry.first;
    });
    if (above == m_classesWithClearGuardsOfStores.begin())
        return false;
    auto [firstBitNumber, layout] = *std::prev(above);
    for (const MemberGuardData& guard : guardsOf(*layout, ClassSide::Prototype, MemberGuardKind::PassedByStores)) {
        if (guard.bit != number - firstBitNumber)
            continue;
        if (hasEnded(guard) || wasFoundNotToHold(guard) || layout->evaluation() == EvaluationState::SeenTwice)
            return false;
        if (!tryWatchMemberPassedByStores(*m_state.instance.vm, m_state, *layout, guard)) {
            didFindNotToHold(guard);
            return false;
        }
        setGuardOfStores(m_state, *layout, guard);
        return true;
    }
    return false;
}

void MemberGuardWatchpoints::setCompleteSets()
{
    while (!m_setsToComplete.isEmpty()) {
        m_recentSetsToComplete.clear();
        uint64_t* words = wordsOfBits(m_state);
        auto standsInNobodysWay = [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            if (holds(words, entry))
                return true;
            if (entry & (MemberGuardAggregates::isAwaitedBeforeClassExists | MemberGuardAggregates::holdsWhileClear))
                return false;
            return !isAboutExistingClass(MemberGuardAggregates::wordIndex(MemberGuardAggregates::numberInListEntry(entry)));
        };
        for (const GuardSet& set : std::exchange(m_setsToComplete, { })) {
            if (holds(words, set.bit))
                continue;
            const uint32_t* list = m_state.tables.at<uint32_t>(set.offsetOfMembers);
            std::span members { list + 1, list[0] };
            if (uint32_t remembered = memberLastInTheWayOf(set.bit); remembered && !standsInNobodysWay(members[remembered]) && !tryArmGuardOfStoresInTheWay(members[remembered], set.heldBefore))
                continue;
            auto inTheWay = std::ranges::find_if_not(members, standsInNobodysWay);
            while (inTheWay != members.end() && tryArmGuardOfStoresInTheWay(*inTheWay, set.heldBefore))
                inTheWay = std::find_if_not(inTheWay, members.end(), standsInNobodysWay);
            if (inTheWay != members.end()) {
                if (size_t index = inTheWay - members.begin(); index >= fewestMembersPassedToRemember)
                    didFindInTheWay(set.bit, static_cast<uint32_t>(index));
                continue;
            }
            setHolds(words, set.bit, true);
            didChangeBit(set.bit);
        }
    }
}

void MemberGuardWatchpoints::speakOfCreatedHolders()
{
    if (m_createdHolders.isEmpty())
        return;
    for (auto [classLayout, side] : std::exchange(m_createdHolders, { }))
        beginSpeakingOfObjects(m_state, *classLayout, side);
}

void MemberGuardWatchpoints::didArmHolder(const LayoutData& classLayout, ClassSide side)
{
    m_createdHolders.removeAllMatching([&](auto& created) {
        return created.first == &classLayout && created.second == side;
    });
}

auto MemberGuardWatchpoints::namesLookedUpBelow(const LayoutData& classLayout) -> NamesOnHolder*
{
    auto found = m_namesLookedUpBelowClass.find(classLayout.index);
    return found == m_namesLookedUpBelowClass.end() ? nullptr : &found->value;
}

void MemberGuardWatchpoints::didArmWholePrototypeSide(const LayoutData& classLayout, Names&& names)
{
    AOT::sort(names);
    names.shrink(std::ranges::unique(names).begin() - names.begin());
    NamesOnHolder namesOnHolder;
    namesOnHolder.verdicts.fill(MemberVerdict::NotLookedAt, names.size());
    namesOnHolder.keys = WTF::move(names);
    m_namesLookedUpBelowClass.set(classLayout.index, WTF::move(namesOnHolder));
}

void MemberGuardWatchpoints::didChangeNameOnPrototypeSide(const LayoutData& classLayout, uint32_t key)
{
    auto found = m_namesLookedUpBelowClass.find(classLayout.index);
    if (found == m_namesLookedUpBelowClass.end())
        return;
    NamesOnHolder& names = found->value;
    if (std::optional<size_t> index = names.find(key)) {
        names.verdicts[*index] = MemberVerdict::NotLookedAt;
        return;
    }
    size_t index = std::ranges::lower_bound(names.keys, key) - names.keys.begin();
    names.keys.insert(index, key);
    names.verdicts.insert(index, MemberVerdict::NotLookedAt);
}

void MemberGuardWatchpoints::didChangeBit(uint32_t guardBit)
{
    uint32_t number = MemberGuardAggregates::numberInListEntry(guardBit);
    if (number - m_firstBitWithDependents >= m_numberOfBitsAmongDependents)
        return;
    std::span<const MethodTableDependentData> dependents = m_state.tables.methodTableDependents();
    if (!std::ranges::binary_search(dependents, number, { }, &MethodTableDependentData::guardBit) || !m_recentChangedBits.add(guardBit))
        return;
    m_changedBits.append(guardBit);
    if (!m_numberOfOpenUpdates)
        updateMethodTable();
}

void MemberGuardWatchpoints::updateMethodTable()
{
    if (m_changedBits.isEmpty())
        return;
    const Image& image = *m_state.instance.vm->aotState().image;
    std::span<const uint32_t> compiled = image.methodTable();
    std::span<const uint32_t> lookups = image.methodLookups();
    std::span<uint32_t> table = m_state.instance.methodTable();
    bool isWritable = false;
    size_t reservedSize = InstanceLayout::bytesReservedForMethodTable(m_state.tables.instanceSizes);
    m_recentChangedBits.clear();
    for (uint32_t listEntry : std::exchange(m_changedBits, { })) {
        bool isSet = holds(wordsOfBits(m_state), listEntry);
        uint32_t guardBit = MemberGuardAggregates::numberInListEntry(listEntry);
        for (const MethodTableDependentData& dependent : std::ranges::equal_range(m_state.tables.methodTableDependents(), guardBit, { }, &MethodTableDependentData::guardBit)) {
            uint32_t entry = isSet ? compiled[dependent.entry] : lookups[dependent.selector];
            if (table[dependent.entry] == entry)
                continue;
            if (!std::exchange(isWritable, true))
                OSAllocator::protect(table.data(), reservedSize, true, true);
            table[dependent.entry] = entry;
        }
    }
    if (isWritable)
        OSAllocator::protect(table.data(), reservedSize, true, false);
}

bool MemberGuardWatchpoints::tryWatch(VM& vm, const LayoutData& classLayout, ClassSide side, JSObject* holder, UniquedStringImpl* uid)
{
    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, uid);
    if (!condition || !condition.isWatchable(PropertyCondition::EnsureWatchability))
        return false;
    Watchpoints& watchpoints = m_watchpointsByClass.add(classLayout.index, Watchpoints()).iterator->value;
    watchpoints.removeAllMatching([&](const std::unique_ptr<MemberGuardWatchpoint>& watchpoint) {
        return watchpoint->key().object() == holder && watchpoint->key().uid() == uid;
    });
    watchpoints.append(makeUnique<MemberGuardWatchpoint>(condition, m_state.objects->classes, classLayout, side));
    watchpoints.last()->install(vm);
    return true;
}

void MemberGuardWatchpoints::stopWatching(const LayoutData& classLayout)
{
    m_watchpointsByClass.remove(classLayout.index);
}

void MemberGuardWatchpoints::watchOtherObjects(std::span<JSObject* const> objects, const LayoutData& classLayout, const MemberGuardData& guard, UniquedStringImpl* uid)
{
    m_state.chainDependents->add(objects, { uid, encodeDependent(classLayout, guard), m_chainDependentOwner });
}

static std::pair<const LayoutData&, const MemberGuardData*> findGuardOfDependent(State& state, uint32_t subject, UniquedStringImpl* uid)
{
    const LayoutData& layout = *state.objects->classes.classWithIndex(subject >> 2);
    std::optional<uint32_t> key = state.instance.vm->aotState().identifiers->numberOf(uid);
    ClassSide side = static_cast<ClassSide>(subject >> 1 & 1);
    if (!key)
        return { layout, nullptr };
    if (!(subject & 1))
        return { layout, findGuard(layout, side, MemberGuardKind::Member, *key) };
    return { layout, findGuard(layout, ClassSide::Prototype, side == ClassSide::Static ? MemberGuardKind::PassedByStores : MemberGuardKind::NotHidden, *key) };
}

// The report may come before the change. Each class below has dependents of its own for the guards that it has.
void MemberGuardWatchpoints::dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uid)
{
    auto [layout, guard] = findGuardOfDependent(m_state, subject, uid);
    if (!guard)
        return;
    if (guard->kind() == MemberGuardKind::PassedByStores)
        AOT::leaveForLater(m_state, layout, *guard);
    else
        end(m_state, layout, *guard);
}

bool MemberGuardWatchpoints::hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uid) const
{
    auto [layout, guard] = findGuardOfDependent(m_state, subject, uid);
    return guard && isSet(m_state, layout, *guard);
}

const ImplicitMemberKeys& MemberGuardWatchpoints::implicitMemberKeys()
{
    if (!m_implicitMemberKeys) {
        VM& vm = *m_state.instance.vm;
        const ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
        const CommonIdentifiers& names = *vm.propertyNames;
        m_implicitMemberKeys = ImplicitMemberKeys {
            .constructor = identifiers.numberOf(names.constructor.impl()),
            .prototype = identifiers.numberOf(names.prototype.impl()),
            .name = identifiers.numberOf(names.name.impl()),
            .length = identifiers.numberOf(names.length.impl()),
        };
    }
    return *m_implicitMemberKeys;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
