/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMemberGuards.h"

#if ENABLE(AOT)

#include "AOTClassHierarchy.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/IterationStatus.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct Resolution {
    const LayoutData* declarer;
    JSObject* declarerHolder;
};

} // anonymous namespace

static_assert(numberOfClassSides == 2);

static uint32_t encodeClassSide(const LayoutData& layout, ClassSide side)
{
    return layout.index << 1 | static_cast<uint32_t>(side);
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
    auto found = std::ranges::find(fields, key, &FieldData::name);
    return found == fields.end() ? nullptr : &*found;
}

static UniquedStringImpl* uidOf(VM& vm, uint32_t key)
{
    return vm.aotState().identifiers->identifier(key).impl();
}

static bool isImplicitMember(State& state, ClassSide side, uint32_t key)
{
    VM& vm = *state.instance.vm;
    UniquedStringImpl* uid = uidOf(vm, key);
    const CommonIdentifiers& names = *vm.propertyNames;
    switch (side) {
    case ClassSide::Prototype:
        return uid == names.constructor.impl();
    case ClassSide::Static:
        return uid == names.prototype.impl() || uid == names.name.impl() || uid == names.length.impl();
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

static bool holdsDeclaredMember(VM& vm, JSObject* holder, UniquedStringImpl* uid, const ClassTable::DeclaredMember& member)
{
    unsigned attributes;
    JSValue value = holder->getDirect(vm, uid, attributes);
    if (!value || !value.isCell())
        return false;
    if (member.method)
        return !(attributes & PropertyAttribute::Accessor) && isClosureOfMethod(value.asCell(), *member.method);
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
    if (state.objects->classes.classHeldBy(holder, foundSide) != &layout || foundSide != side)
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

static bool isSet(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    return wordOf(state, layout, guard) & guard.mask();
}

static void set(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    wordOf(state, layout, guard) |= guard.mask();
}

static bool clear(State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    uint64_t& word = wordOf(state, layout, guard);
    if (!(word & guard.mask()))
        return false;
    word &= ~guard.mask();
    switch (guard.kind()) {
    case MemberGuardKind::Member:
        count(state.instance, RuntimeCounterID::MemberGuardsCleared);
        break;
    case MemberGuardKind::PrototypeLink:
        count(state.instance, RuntimeCounterID::PrototypeLinkGuardsCleared);
        break;
    case MemberGuardKind::EvaluatedOnce:
        break;
    }
    return true;
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
    const ClassHierarchy& hierarchy = state.objects->caches->classHierarchy;
    Vector<uint32_t, 16> stack;
    stack.append(hierarchy.definedSubclassesOf(layout));
    while (!stack.isEmpty()) {
        const LayoutData& descendant = *state.objects->classes.classWithIndex(stack.takeLast());
        if (functor(descendant) == IterationStatus::Continue)
            stack.append(hierarchy.definedSubclassesOf(descendant));
    }
}

static bool isDeclaredBetween(State& state, const LayoutData& from, const LayoutData* endExclusive, ClassSide side, uint32_t key)
{
    for (const LayoutData* next = &from; next && next != endExclusive; next = state.objects->classes.assumedBaseOf(*next)) {
        if (declares(state, *next, side, key))
            return true;
    }
    return false;
}

// Predicted bases may form a cycle. Classes defined with the base they predict do not: the base was defined first.
static bool isDefinedWithAssumedBase(State& state, const LayoutData& layout, JSObject* classObject)
{
    ClassTable& classes = state.objects->classes;
    if (classes.baseOf(layout))
        return true;
    const LayoutData* predicted = classes.assumedBaseOf(layout);
    return predicted && classObject->getPrototypeDirect() == JSValue(registeredClassObjectOf(state, *predicted));
}

static void clearInheritedGuards(State& state, const LayoutData& layout, const LayoutData* endExclusive, ClassSide side)
{
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::Member)) {
        if (isSet(state, layout, guard) && !isDeclaredBetween(state, layout, endExclusive, side, guard.name))
            clear(state, layout, guard);
    }
}

static bool holdsAsDeclared(VM& vm, State& state, const LayoutData& layout, ClassSide side, JSObject* holder, uint32_t key)
{
    UniquedStringImpl* uid = uidOf(vm, key);
    if (side == ClassSide::Static && findOwnStaticField(layout, key)) {
        unsigned attributes;
        return !isValidOffset(holder->structure()->get(vm, uid, attributes)) || (attributes & PropertyAttribute::CustomValue);
    }
    bool isLazy = isLazyHolder(holder, side);
    if (declaresMember(layout, side, key))
        return isLazy || holdsDeclaredMember(vm, holder, uid, declaredMember(layout, side, key));
    switch (side) {
    case ClassSide::Prototype:
        return isLazy || holder->getDirect(vm, uid) == JSValue(registeredClassObjectOf(state, layout));
    case ClassSide::Static:
        // The prototype property holds the object that the definition made the prototype of the instances.
        return uid == vm.propertyNames->prototype.impl() && holderOf(vm, state, layout, ClassSide::Prototype);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Compiled code fills an empty slot of a compiled object without a report, so a name of its layout counts as a property that it has.
static bool hasPropertyOrSlot(VM& vm, JSObject* holder, UniquedStringImpl* uid)
{
    if (!isCompiledObject(holder))
        return isValidOffset(holder->structure()->get(vm, uid));
    auto& object = *uncheckedDowncast<Object>(holder);
    if (object.fieldNamed(object.state(), uid))
        return true;
    JSObject* extraProperties = object.extraProperties();
    return extraProperties && isValidOffset(extraProperties->structure()->get(vm, uid));
}

// The assumed base of a constructor function is the class from whose prototype object its own inherits. The function itself inherits from no class object.
static const LayoutData* nextClassOnSide(const ClassTable& classes, const LayoutData& layout, ClassSide side)
{
    if (side == ClassSide::Static && !ClassTable::isDefinedByClass(layout))
        return nullptr;
    return classes.assumedBaseOf(layout);
}

static std::optional<Resolution> resolveAsDeclared(VM& vm, State& state, const LayoutData& start, ClassSide side, uint32_t key, UniquedStringImpl* uid)
{
    JSObject* previous = nullptr;
    ClassTable& classes = state.objects->classes;
    for (const LayoutData* next = &start; next; next = nextClassOnSide(classes, *next, side)) {
        JSObject* holder = holderOf(vm, state, *next, side);
        if (!holder || (previous && previous->getPrototypeDirect() != JSValue(holder)))
            return std::nullopt;
        if (isLazyHolder(holder, side) && hasComputedMemberName(*next, side))
            return std::nullopt;
        if (declares(state, *next, side, key)) {
            if (!holdsAsDeclared(vm, state, *next, side, holder, key))
                return std::nullopt;
            return Resolution { next, holder };
        }
        if (hasPropertyOrSlot(vm, holder, uid) || (next->hasUntypedBase && !classes.assumedBaseOf(*next)))
            return std::nullopt;
        previous = holder;
    }
    return Resolution { nullptr, nullptr };
}

static bool tryWatchMember(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    ClassSide side = guard.side();
    UniquedStringImpl* uid = uidOf(vm, guard.name);
    std::optional<Resolution> resolution = resolveAsDeclared(vm, state, layout, side, guard.name, uid);
    if (!resolution)
        return false;
    const LayoutData* declarer = resolution->declarer;
    if (!declarer)
        return true;
    JSObject* holder = resolution->declarerHolder;
    if (const FieldData* field = side == ClassSide::Static ? findOwnStaticField(*declarer, guard.name) : nullptr) {
        // A field without a value is not defined yet or was deleted, and only the bit of its own class records a deletion.
        return declarer == &layout || StaticFields::isInVariable(*uncheckedDowncast<Closure>(holder), *field);
    }
    if (isLazyHolder(holder, side))
        return true;
    return state.objects->caches->memberGuardWatchpoints.tryWatch(vm, *declarer, side, holder, uid);
}

static void updateMemberGuard(VM& vm, State& state, const LayoutData& layout, const MemberGuardData& guard)
{
    if (tryWatchMember(vm, state, layout, guard))
        set(state, layout, guard);
    else
        clear(state, layout, guard);
}

uint64_t* ClassTable::guardWords(const LayoutData& layout, ClassSide side) const
{
    std::optional<uint32_t> offset = layout.classData().guardWordsOffsetFor(side);
    return offset ? reinterpret_cast<uint64_t*>(m_state.instance.statics() + *offset) : nullptr;
}

const LayoutData* ClassTable::classHeldBy(JSObject* holder, ClassSide& side) const
{
    auto found = m_classSidesByHolder.find(holder);
    if (found == m_classSidesByHolder.end())
        return nullptr;
    side = static_cast<ClassSide>(found->value & 1);
    return classWithIndex(found->value >> 1);
}

void ClassTable::armMemberGuards(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder)
{
    ASSERT(holder);
    // The holders of every evaluation are entered: a constructor allocates an instance of the class whose prototype object it is handed. The guards describe the first evaluation.
    bool isFirstCall = m_classSidesByHolder.add(holder, encodeClassSide(layout, side)).isNewEntry;
    if (isFirstCall)
        m_writes.recordAll();
    if (layout.evaluation() == EvaluationState::SeenTwice || registeredHolderOf(vm, m_state, layout, side) != holder)
        return;
    switch (side) {
    case ClassSide::Static:
        if (isFirstCall && isDefinedWithAssumedBase(m_state, layout, holder))
            m_state.objects->caches->classHierarchy.didDefineClass(layout);
        // A class object reports what happens to it through its method table. A constructor function reports like a prototype object.
        if (!isDefinedByClass(layout))
            holder->didBecomePrototype(vm);
        break;
    case ClassSide::Prototype:
        // Only an object whose Structure may be a prototype reports an added name to VM::invalidateStructureChainIntegrity().
        holder->didBecomePrototype(vm);
        break;
    }
    if (isFirstCall) {
        for (MemberGuardKind kind : { MemberGuardKind::PrototypeLink, MemberGuardKind::EvaluatedOnce }) {
            for (const MemberGuardData& guard : guardsOf(layout, side, kind))
                set(m_state, layout, guard);
        }
    }
    bool mayArmCreatedMembers = !isFirstCall && isEvaluatedOnce(m_state, layout);
    for (const MemberGuardData& guard : guardsOf(layout, side, MemberGuardKind::Member)) {
        if (isFirstCall || isSet(m_state, layout, guard) || (mayArmCreatedMembers && isCreatedAtDefinition(layout, side, guard.name)))
            updateMemberGuard(vm, m_state, layout, guard);
    }
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

void ClassTable::didChangeMember(const LayoutData& layout, ClassSide side, UniquedStringImpl* uid)
{
    std::optional<uint32_t> key = m_state.instance.vm->aotState().identifiers->numberOf(uid);
    if (!key)
        return;
    if (side == ClassSide::Static) {
        const FieldData* field = findOwnStaticField(layout, *key);
        Closure* owner = field ? registeredClassObjectOf(m_state, layout) : nullptr;
        if (owner && StaticFields::isInVariable(*owner, *field))
            return;
    }
    if (const MemberGuardData* guard = findGuard(layout, side, MemberGuardKind::Member, *key))
        clear(m_state, layout, *guard);
    forEachDescendant(m_state, layout, [&](const LayoutData& descendant) {
        if (declares(m_state, descendant, side, *key))
            return IterationStatus::Done;
        if (const MemberGuardData* guard = findGuard(descendant, side, MemberGuardKind::Member, *key))
            clear(m_state, descendant, *guard);
        return IterationStatus::Continue;
    });
}

// A definition changes what its own name finds, in the class and below it. The watchpoints of the other members follow the holder to its next Structure by themselves.
void ClassTable::didDefineMember(VM& vm, const LayoutData& layout, ClassSide side, JSObject* holder, UniquedStringImpl* uid)
{
    didChangeMember(layout, side, uid);
    if (!m_classSidesByHolder.contains(holder)) {
        armMemberGuards(vm, layout, side, holder);
        return;
    }
    std::optional<uint32_t> key = vm.aotState().identifiers->numberOf(uid);
    if (!key || layout.evaluation() == EvaluationState::SeenTwice || registeredHolderOf(vm, m_state, layout, side) != holder)
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
    auto clearGuards = [&](const LayoutData& inheriting) {
        clearInheritedGuards(m_state, inheriting, &layout, side);
        return IterationStatus::Continue;
    };
    clearGuards(layout);
    forEachDescendant(m_state, layout, clearGuards);
}

void ClassTable::didChangePrototypeLink(const LayoutData& layout, ClassSide side)
{
    const LayoutData* base = assumedBaseOf(layout);
    auto clearGuards = [&](const LayoutData& inheriting) {
        for (const MemberGuardData& guard : guardsOf(inheriting, side, MemberGuardKind::PrototypeLink))
            clear(m_state, inheriting, guard);
        clearInheritedGuards(m_state, inheriting, base, side);
        return IterationStatus::Continue;
    };
    clearGuards(layout);
    forEachDescendant(m_state, layout, clearGuards);
}

void ClassTable::disarmMemberGuards(const LayoutData& classLayout)
{
    for (ClassSide side : allMembersOf<ClassSide>) {
        didChangeEveryMember(classLayout, side);
        didChangePrototypeLink(classLayout, side);
        for (const MemberGuardData& guard : guardsOf(classLayout, side, MemberGuardKind::EvaluatedOnce))
            clear(m_state, classLayout, guard);
    }
    m_state.objects->caches->memberGuardWatchpoints.stopWatching(classLayout);
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
{
}

MemberGuardWatchpoints::~MemberGuardWatchpoints() = default;

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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
