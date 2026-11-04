/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Known effects: what a call or an access does that stays as it is, by Link's tables and the proven types of its operands. Walker::build() takes the answer beside the draft. Nothing is built here, and no state is asked.
// At a suspension: an operation that may suspend keeps its declared bound, whatever it is handed here: effectsOf().
// At a throw: what a callee writes on the way to a throw counts wherever a handler of the unit stands round the call. What it reads there, and its marks, always count.
// Withheld: without DirectCalls a direct call has Link's two effects merged and no list of callees is read, without TypedOperations an access has its bound, without BuiltinCalls no row is asked.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallPlan.h"
#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet accessesWithFreeNamesForKnownEffects { Opcode::PropGet, Opcode::PropSet, Opcode::PropDelete };
static constexpr OpcodeSet callsOfListsForKnownEffects { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
static constexpr OpcodeSet constructionsForKnownEffects { Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet callsThatLinkMayListForKnownEffects = constructionsForKnownEffects | OpcodeSet { Opcode::Call, Opcode::CallList };

namespace {

struct HalvesOfKnownEffects {
    Effects onReturn;
    Effects onThrow;
    uint64_t layoutsWritten { objectsOfAnyLayout };
};

enum class AccessByNameForKnownEffects : uint8_t { Read, Has, Store, Define, Delete };

} // anonymous namespace

// What the state knows of a shape ends with the next write of a shape word, and the callee runs behind that point.
static TypeID ownTypeForKnownEffects(const Walker& walker, Reference reference)
{
    return reference && reference.isRecord() ? walker.code().at(reference.id()).type : TypeID();
}

void addEffectsOfEnteringByValue(Effects& effects)
{
    effects.reads.add(bookkeepingHeaps);
    effects.writes.add(bookkeepingHeaps);
    effects.mayThrow = true;
    effects.mayCollect = true;
}

// A variable that the plan has added has no place of its own among the 64, so a write at any place ends what is known of it.
void addVariablesThatStandForFields(const Plan& plan, Effects& effects)
{
    if (plan.promotedEnvironments().variables.isEmpty())
        return;
    effects.reads.add(AbstractHeap::ModuleVariables, effects.reads.maskOf(AbstractHeap::EnvironmentFields));
    effects.writes.add(AbstractHeap::ModuleVariables, effects.writes.maskOf(AbstractHeap::EnvironmentFields));
}

// Link speaks of a body as it is written. The body of a module that begins itself asks first whether the module has begun, which may run any code: Walker::beginModuleInBody().
static bool unitDoesMoreThanBodyForKnownEffects(const Plan& plan, FunctionID function)
{
    return !!plan.moduleEvaluation().moduleThatBeginsIn(function);
}

static Effects effectsOfCallByValueOfFunctionForKnownEffects(const LinkTables& linked, FunctionID function)
{
    const LinkedFunction& entered = linked.function(function);
    Effects effects = entered.effectsOnReturn;
    effects.merge(entered.effectsOnThrow);
    addEffectsOfEnteringByValue(effects);
    return effects;
}

// ---- Direct calls

// The record leaves out what holds only of other arguments. Nothing: it does not speak of these.
static std::optional<HalvesOfKnownEffects> findHalvesGivenArgumentsForKnownEffects(const Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedEffectsGivenArguments* given = linked.known.findEffectsGivenArguments(callee);
    if (!given)
        return std::nullopt;

    TypeArena& types = walker.types();
    std::span<const LinkedParameter> parameters = linked.parametersOf(callee);
    HalvesOfKnownEffects halves { given->onReturn, given->onThrow, given->layoutsWritten };
    for (uint32_t parameter = 0; parameter < 32; ++parameter) {
        bool isTakenAsExpected = given->parametersTakenAsExpected >> parameter & 1;
        bool isCalled = given->parametersCalled >> parameter & 1;
        if (!isTakenAsExpected && !isCalled)
            continue;
        TypeID arriving = parameter < entryArguments.size() ? ownTypeForKnownEffects(walker, entryArguments[parameter]) : TypeID();
        if (!arriving)
            return std::nullopt;
        if (isTakenAsExpected) {
            TypeID expected = parameter < parameters.size() ? parameters[parameter].expected : TypeID();
            if (!expected || !types.isSubset(arriving, expected))
                return std::nullopt;
        }
        if (!isCalled)
            continue;
        Type type = types.at(arriving);
        FunctionID called = type.isObject() && type.hasOnlyClosures() && !type.namesMergedObjects() ? singleFunctionOf(types, type) : FunctionID();
        if (!called || called.index() >= linked.functions.size())
            return std::nullopt;
        // The callee may catch what the function throws, so both halves of the function count on both ways out of the callee.
        Effects ofCall = effectsOfCallByValueOfFunctionForKnownEffects(linked, called);
        halves.onReturn.merge(ofCall);
        halves.onThrow.merge(ofCall);
        halves.layoutsWritten |= linked.known.layoutsWrittenBy(called);
    }
    return halves;
}

Effects effectsOfDirectCall(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    if (unitDoesMoreThanBodyForKnownEffects(walker.plan(), callee))
        return Effects::unknownCode();
    const LinkTables& linked = walker.facts().linked();
    const LinkedFunction& entered = linked.function(callee);
    Effects onReturn = entered.effectsOnReturn;
    Effects onThrow = entered.effectsOnThrow;
    if (walker.has(Mechanism::DirectCalls) && !linked.known.layoutsWrittenByFunction.isEmpty()) {
        if (std::optional<HalvesOfKnownEffects> given = findHalvesGivenArgumentsForKnownEffects(walker, callee, entryArguments)) {
            // The prologue of the unit polls and makes room by what Link says of the function, whoever calls it: prologueOfUnitMayCollect().
            bool mayCollect = onReturn.mayCollect || onThrow.mayCollect;
            bool mayBeTerminated = onReturn.mayBeTerminated || onThrow.mayBeTerminated;
            onReturn = given->onReturn;
            onThrow = given->onThrow;
            onReturn.mayCollect |= mayCollect;
            onReturn.mayBeTerminated |= mayBeTerminated;
        }
        // Where the throw leaves the unit, nothing of the unit runs behind it.
        if (!walker.standsUnderHandler())
            onThrow.writes = { };
    }
    Effects effects = onReturn;
    effects.merge(onThrow);
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    addVariablesThatStandForFields(walker.plan(), effects);
    return effects;
}

// An object of one of the engine's classes has every bit (bitsOfLayoutsOf()), and it may be written whatever Link's bits say (LinkKnownEffects::layoutsWrittenBy()). So no answer is empty: whoever holds the two together finds such an object written.
static uint64_t withBitForObjectsOfEngineForKnownEffects(uint64_t layouts)
{
    return layouts ? layouts : uint64_t { 1 } << 63;
}

uint64_t layoutsWrittenBy(Walker& walker, const Draft& draft)
{
    if (!walker.has(Mechanism::DirectCalls))
        return objectsOfAnyLayout;
    const LinkTables& linked = walker.facts().linked();
    if (draft.opcode == Opcode::CallDirect) {
        if (!returnsResultOfFunction(draft.role()) || unitDoesMoreThanBodyForKnownEffects(walker.plan(), draft.function()))
            return objectsOfAnyLayout;
        if (std::optional<HalvesOfKnownEffects> given = findHalvesGivenArgumentsForKnownEffects(walker, draft.function(), draft.variadicOperands()))
            return withBitForObjectsOfEngineForKnownEffects(given->layoutsWritten);
        return withBitForObjectsOfEngineForKnownEffects(linked.known.layoutsWrittenBy(draft.function()));
    }

    // Link speaks of the call as it is written.
    ValueID site = draft.inputValueGiven;
    if (!site || !callsThatLinkMayListForKnownEffects.contains(draft.opcode))
        return objectsOfAnyLayout;
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isParameter(site) || index.opcodeOf(site) != draft.opcode)
        return objectsOfAnyLayout;
    const LinkedCallOfSeveral* listed = linked.known.findCallOfSeveral(walker.currentFunction(), site);
    return listed ? withBitForObjectsOfEngineForKnownEffects(listed->layoutsWritten) : objectsOfAnyLayout;
}

uint64_t bitsOfLayoutsOf(Walker& walker, TypeID type)
{
    if (!type)
        return objectsOfAnyLayout;
    TypeArena& types = walker.types();
    Type held = types.at(type);
    if (!held.isJavaScriptValue())
        return objectsOfAnyLayout;
    if (!held.hasObjects())
        return 0;
    Type objects = types.at(types.withOnlyObjects(type));
    if (!objects.hasOnlyLayouts() || objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return objectsOfAnyLayout;
    uint64_t bits = 0;
    for (LayoutID layout : membersOf(types, objects.layouts))
        bits |= bitOfLayoutInKnownEffects(walker.program(), layout);
    return bits;
}

// ---- Accesses by name that stay closed

// An index and a number in print may be elements, `__proto__` is an accessor of Object.prototype, and in front of a lookup under a symbol the run time may call a function of the library.
static bool isLookedUpAsTextForKnownEffects(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key) || !program.types.hasStableNumber(key) || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

// The run time may make the closure of a member when it is first looked for, and it files what it finds.
static Effects effectsOfLookupThatRunsNoCodeForKnownEffects()
{
    Effects effects = Effects::unknownCode();
    effects.writes = effects.writes.restrictedTo(heapsWhoseWritesNobodySees);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    effects.mayCallUnknownCode = false;
    effects.mayBeTerminated = false;
    return effects;
}

// A frozen object refuses the store, and strict code throws then.
static Effects effectsOfStoreThatRunsNoCodeForKnownEffects(const Program& program, PropertyKeyID key)
{
    Effects effects = effectsOfLookupThatRunsNoCodeForKnownEffects();
    effects.writes.add(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
    effects.writes.add(program, MemoryLocation::objectField(key));
    effects.writes.add(program, MemoryLocation::at(AbstractHeap::NamedProperties, key.index()));
    effects.mayThrow = true;
    return effects;
}

// Whatever the key turns out to name.
static Effects effectsOfDeletionThatRunsNoCodeForKnownEffects()
{
    Effects effects = effectsOfLookupThatRunsNoCodeForKnownEffects();
    effects.writes.add(AbstractHeapSet {
        AbstractHeap::ObjectFields, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::MemberGuards, AbstractHeap::NamedProperties,
        AbstractHeap::Elements, AbstractHeap::ArrayStorage, AbstractHeap::AllocatorState, AbstractHeap::RuntimeState });
    effects.mayThrow = true;
    return effects;
}

// What the access runs on an object of the layout is added. False: nobody says what it runs.
static bool addCodeRunOnObjectsOfForKnownEffects(const Walker& walker, LayoutID id, PropertyKeyID key, AccessByNameForKnownEffects access, Effects& effects)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    const SlotPlan& slots = walker.plan().slots();
    const Layout& layout = program.at(id);
    // An instance of a class below a built-in class is a cell of that class, which may have traps of its own.
    if (!slots.holdsCompiledObjects(id) || !isFoundByShapeTest(layout))
        return false;
    // Neither looks at anything above the object, and neither calls what lies under the key.
    if (access == AccessByNameForKnownEffects::Define || access == AccessByNameForKnownEffects::Delete)
        return true;

    OptionSet<LayoutChange> possibleChanges = id.index() < linked.layouts.size() ? linked.layout(id).possibleChanges : LinkedLayout { }.possibleChanges;
    if (possibleChangesForbid(possibleChanges, TypedAccess::FindsMethodAbove) || layout.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (std::optional<FieldIndex> index = slots.findField(program, id, key)) {
        bool isWrittenAsPlainValue = *index >= layout.fields.size() || layout.fields[*index].form.isEmpty();
        if (!isWrittenAsPlainValue || slots.slot(id, *index).holdsAccessor)
            return false;
    } else if (findField(program, id, key))
        return false;

    // The slot may be empty, so the lookup may go on above the object.
    MemberFound found;
    found.kind = MemberFound::Kind::Nothing;
    bool standsBelowObjectPrototypeAlone = layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    if (!standsBelowObjectPrototypeAlone)
        found = linked.findMember(program, id, ClassSide::Prototype, key);
    switch (found.kind) {
    case MemberFound::Kind::Unknown:
        return false;
    case MemberFound::Kind::Nothing:
        return !isOnObjectPrototype(program.nameClassOf(key));
    case MemberFound::Kind::Method:
        return true;
    case MemberFound::Kind::Accessor: {
        // Whoever asks whether a property is there calls no getter.
        if (access == AccessByNameForKnownEffects::Has)
            return true;
        FunctionID entered = access == AccessByNameForKnownEffects::Read ? found.getter : found.setter;
        if (entered && entered.index() >= linked.functions.size())
            return false;
        if (entered)
            effects.merge(effectsOfCallByValueOfFunctionForKnownEffects(linked, entered));
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessByNameForKnownEffects kindOfAccessForKnownEffects(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropGet:
        return AccessByNameForKnownEffects::Read;
    case Opcode::PropHas:
        return AccessByNameForKnownEffects::Has;
    case Opcode::PropSet:
        return AccessByNameForKnownEffects::Store;
    case Opcode::PropDefine:
        return AccessByNameForKnownEffects::Define;
    case Opcode::PropDelete:
        return AccessByNameForKnownEffects::Delete;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A union over the parts of the receiver, so a wider type never gives less.
static std::optional<Effects> effectsOfAccessByNameForKnownEffects(Walker& walker, const BodyValue& value, const TypesOfInputs& typeOf)
{
    Opcode opcode = value.opcode();
    if (accessesWithFreeNamesForKnownEffects.contains(opcode) && value.isFreeName())
        return std::nullopt;
    const Program& program = walker.program();
    PropertyKeyID key = value.key();
    if (!isLookedUpAsTextForKnownEffects(program, key))
        return std::nullopt;

    TypeArena& types = walker.types();
    TypeID receiver = typeOf(value.objectOperand());
    if (!receiver)
        return std::nullopt;
    Type type = types.at(receiver);
    if (!type.isJavaScriptValue() || type.isNever())
        return std::nullopt;
    // `in` throws on whatever is no object. Every other access throws on undefined and null, and looks any other primitive up above its wrapper.
    AccessByNameForKnownEffects access = kindOfAccessForKnownEffects(opcode);
    if (access != AccessByNameForKnownEffects::Has && (type.primitives & ~Primitive::UndefinedOrNull))
        return std::nullopt;

    Effects effects = effectsOfLookupThatRunsNoCodeForKnownEffects();
    if (access == AccessByNameForKnownEffects::Store || access == AccessByNameForKnownEffects::Define)
        effects = effectsOfStoreThatRunsNoCodeForKnownEffects(program, key);
    else if (access == AccessByNameForKnownEffects::Delete)
        effects = effectsOfDeletionThatRunsNoCodeForKnownEffects();
    if (type.primitives)
        effects.mayThrow = true;
    if (!type.hasObjects())
        return effects;

    Type objects = types.at(types.withOnlyObjects(receiver));
    if (!objects.hasOnlyLayouts() || objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return std::nullopt;
    for (LayoutID member : membersOf(types, objects.layouts)) {
        if (!walker.plan().isInUse(member))
            continue;
        if (!addCodeRunOnObjectsOfForKnownEffects(walker, member, key, access, effects))
            return std::nullopt;
    }
    return effects;
}

// ---- Calls that stay by value

// The run time reads `prototype` off new.target for the object that it makes. A closure that can be new.target holds a plain value there. A proxy runs its trap.
static bool newTargetIsClosureForKnownEffects(Walker& walker, const BodyValue& construction, const TypesOfInputs& typeOf)
{
    ValueID newTarget = construction.newTargetOperand();
    if (!newTarget)
        return true;
    TypeID proven = typeOf(newTarget);
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    return type.isObject() && type.hasOnlyClosures();
}

// No function is mentioned: the call enters whatever it finds, and Link has listed all that it can find. Both halves count whole: whoever looks ahead stands in no block.
static std::optional<Effects> effectsOfCallThatLinkListsForKnownEffects(Walker& walker, FunctionID function, const BodyValue& call, const TypesOfInputs& typeOf)
{
    if (!walker.has(Mechanism::DirectCalls))
        return std::nullopt;
    const LinkKnownEffects& known = walker.facts().linked().known;
    const LinkedCallOfSeveral* listed = known.findCallOfSeveral(function, call.id());
    if (!listed || static_cast<size_t>(listed->effects) + 1 >= known.effectsOfCallsOfSeveral.size())
        return std::nullopt;
    if (constructionsForKnownEffects.contains(call.opcode()) && !newTargetIsClosureForKnownEffects(walker, call, typeOf))
        return std::nullopt;

    Effects effects = known.effectsOfCallsOfSeveral[listed->effects];
    effects.merge(known.effectsOfCallsOfSeveral[listed->effects + 1]);
    addEffectsOfEnteringByValue(effects);
    if (callsOfListsForKnownEffects.contains(call.opcode()))
        effects.reads.add(AbstractHeapSet { AbstractHeap::Elements, AbstractHeap::ArrayStorage });
    return effects;
}

static std::optional<Effects> effectsByTablesForKnownEffects(Walker& walker, FunctionID function, const BodyValue& value, const TypesOfInputs& typeOf)
{
    switch (value.opcode()) {
    case Opcode::PropGet:
    case Opcode::PropSet:
    case Opcode::PropDefine:
    case Opcode::PropHas:
    case Opcode::PropDelete:
        return walker.has(Mechanism::TypedOperations) ? effectsOfAccessByNameForKnownEffects(walker, value, typeOf) : std::nullopt;
    case Opcode::Call:
    case Opcode::SuperConstruct:
        if (std::optional<Effects> byRows = walker.has(Mechanism::BuiltinCalls) ? effectsOfCallOfBuiltinsByRows(walker, value, typeOf) : std::nullopt)
            return byRows;
        return effectsOfCallThatLinkListsForKnownEffects(walker, function, value, typeOf);
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        return effectsOfCallThatLinkListsForKnownEffects(walker, function, value, typeOf);
    default:
        return std::nullopt;
    }
}

std::optional<Effects> effectsOfOperationAsWritten(Walker& walker, FunctionID function, const BodyValue& value, const TypesOfInputs& typeOf)
{
    std::optional<Effects> effects = effectsByTablesForKnownEffects(walker, function, value, typeOf);
    if (effects)
        addVariablesThatStandForFields(walker.plan(), *effects);
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
