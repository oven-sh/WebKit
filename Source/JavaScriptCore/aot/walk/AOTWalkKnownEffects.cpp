/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static TypeID ownTypeForKnownEffects(Walker& walker, Reference reference)
{
    return reference && reference.isRecord() ? walker.typeShown(reference) : TypeID();
}

void addEffectsOfEnteringByValue(Effects& effects)
{
    effects.reads.add(bookkeepingHeaps);
    effects.writes.add(bookkeepingHeaps);
    effects.mayThrow = true;
    effects.mayCollect = true;
}

bool unitDoesMoreThanBodyForKnownEffects(const Plan& plan, FunctionID function)
{
    return !!plan.moduleEvaluation().moduleThatBeginsIn(function);
}

static Effects effectsOfCallByValueOfFunctionForKnownEffects(const LinkTables& linked, FunctionID function)
{
    const LinkedFunction& entered = linked.function(function);
    Effects effects = entered.effectsOnReturn();
    effects.merge(entered.effectsOnThrow());
    addEffectsOfEnteringByValue(effects);
    return effects;
}

static std::optional<HalvesOfKnownEffects> findHalvesGivenArgumentsForKnownEffects(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
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
            TypeID expected = parameter < parameters.size() ? parameters[parameter].writtenOrSeenToArrive() : TypeID();
            if (!expected || !types.isSubset(arriving, expected))
                return std::nullopt;
        }
        if (!isCalled)
            continue;
        Type type = types.at(arriving);
        FunctionID called = type.isObject() && type.hasOnlyClosures() && !type.namesMergedObjects() ? singleFunctionOf(types, type) : FunctionID();
        if (!called || called.index() >= linked.functions.size())
            return std::nullopt;
        Effects ofCall = effectsOfCallByValueOfFunctionForKnownEffects(linked, called);
        halves.onReturn.merge(ofCall);
        halves.onThrow.merge(ofCall);
        halves.layoutsWritten |= linked.known.layoutsWrittenBy(called);
    }
    return halves;
}

bool effectsOfDirectCallHangOnArguments(Walker& walker, FunctionID callee)
{
    const LinkedEffectsGivenArguments* given = walker.facts().linked().known.findEffectsGivenArguments(callee);
    return given && (given->parametersTakenAsExpected || given->parametersCalled);
}

Effects effectsOfDirectCall(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    if (unitDoesMoreThanBodyForKnownEffects(walker.plan(), callee))
        return Effects::unknownCode();
    const LinkTables& linked = walker.facts().linked();
    const LinkedFunction& entered = linked.function(callee);
    Effects onReturn = entered.effectsOnReturn();
    Effects onThrow = entered.effectsOnThrow();
    if (walker.has(Mechanism::DirectCalls) && !linked.known.layoutsWrittenByFunction.isEmpty()) {
        if (std::optional<HalvesOfKnownEffects> given = findHalvesGivenArgumentsForKnownEffects(walker, callee, entryArguments)) {
            bool mayCollect = onReturn.mayCollect || onThrow.mayCollect;
            bool mayBeTerminated = onReturn.mayBeTerminated || onThrow.mayBeTerminated;
            onReturn = given->onReturn;
            onThrow = given->onThrow;
            onReturn.mayCollect |= mayCollect;
            onReturn.mayBeTerminated |= mayBeTerminated;
        }
        if (!walker.standsUnderHandler())
            onThrow.writes = { };
    }
    Effects effects = onReturn;
    effects.merge(onThrow);
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    return effects;
}

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
