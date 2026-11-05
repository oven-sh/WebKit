/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Known effects: what a direct call does, by Link's tables and the proven types of its arguments. Walker::build() takes the answer beside the draft. Nothing is built here, and no state is asked.
// At a suspension: an operation that may suspend keeps its declared bound, whatever it is handed here: effectsOf().
// At a throw: what a callee writes on the way to a throw counts wherever a handler of the unit stands round the call. What it reads there, and its marks, always count.
// Withheld: without DirectCalls a direct call has Link's two effects merged.

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

namespace {

struct HalvesOfKnownEffects {
    Effects onReturn;
    Effects onThrow;
    uint64_t layoutsWritten { objectsOfAnyLayout };
};

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
    return objectsOfAnyLayout;
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
