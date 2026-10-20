/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummaries.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPointsToSolverInternal.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct RetainingObject {
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t entryParameter { 0 };

    friend bool operator==(const RetainingObject&, const RetainingObject&) = default;
};

static std::optional<uint32_t> entryParameterIndexOf(FunctionAnalyses& analyses, const TrackedObject& object)
{
    if (object.kind != ObjectKind::EntryParameter)
        return std::nullopt;
    ValueLocation location = analyses.useLists().locationOf(object.definition);
    if (location.placement != ValuePlacement::Parameter || location.block != entryBlock)
        return std::nullopt;
    return location.index;
}

static bool isConfinedToActivation(const TrackedObject& container)
{
    return container.kind == ObjectKind::Allocation && container.state == EscapeState::DoesNotEscape;
}

static RetainingObject retainingObjectFor(FunctionAnalyses& analyses, const TrackedObject& container)
{
    if (std::optional<uint32_t> entryParameter = entryParameterIndexOf(analyses, container))
        return { RetainedIn::Operand, *entryParameter };
    if (container.kind == ObjectKind::Allocation && container.reasons == EscapeReasons { EscapeReason::Returned })
        return { RetainedIn::Result, 0 };
    return { };
}

static OperandSummary summarizeRetention(FunctionAnalyses& analyses, OperandUses uses, std::span<const ObjectID> containers)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<RetainingObject> common;
    for (ObjectID id : containers) {
        const TrackedObject& container = escape.at(id);
        if (isConfinedToActivation(container))
            continue;
        RetainingObject retaining = retainingObjectFor(analyses, container);
        if (common && *common != retaining)
            retaining = { };
        common = retaining;
    }
    if (!common)
        return { uses - OperandUse::Retains, RetainedIn::UnknownMemory, 0 };
    return { uses | OperandUse::Retains, common->retainedIn, common->entryParameter };
}

static bool mayLoadReference(FunctionAnalyses& analyses, const ObjectUse& objectUse)
{
    return objectUse.readsContents && !analyses.escape().objectDefinedBy(objectUse.use.user);
}

static OperandUses usesOfLoadedReferences(FunctionAnalyses& analyses, uint32_t entryParameter)
{
    const AnalysisContext& context = analyses.context();
    OperandUses published = context.flow ? publishedParameterSummary(*context.flow, context.id, entryParameter).uses : OperandUses::all();
    if (published.contains(OperandUse::RetainsContents))
        return OperandUse::RetainsContents;
    return published - leakingUses;
}

std::unique_ptr<FunctionAnalyses> createAnalysesForSummary(BuildContext& build, const EffectContext& context, const FlowResults& flow, FunctionID id, const Function& function)
{
    return makeUnique<FunctionAnalyses>(AnalysisContext { build, context.program, &flow, &context.summaries, id }, function);
}

void forEachBaseObject(FunctionAnalyses& analyses, ValueID value, NOESCAPE const ScopedLambda<void(const BaseObject&)>& functor)
{
    if (!mayBeReference(analyses.program().typeOf(analyses.function(), value)))
        return;

    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp()) {
        functor(BaseObject { });
        return;
    }

    PointsToSet set = escape.pointsTo(value);
    bool mayBeUntracked = set.mayBeUntracked;
    for (ObjectID id : set.objects) {
        const TrackedObject& object = escape.at(id);
        switch (object.kind) {
        case ObjectKind::Allocation:
            functor(BaseObject { BaseObject::Kind::OwnAllocation, 0 });
            break;
        case ObjectKind::EntryParameter:
            if (std::optional<uint32_t> entryParameter = entryParameterIndexOf(analyses, object))
                functor(BaseObject { BaseObject::Kind::EntryParameter, *entryParameter });
            else
                mayBeUntracked = true;
            break;
        }
    }
    if (mayBeUntracked)
        functor(BaseObject { });
}

// A saved frame holds what is live in it, and the function goes on when its caller has long gone on: what it then does to a parameter happens at a time of which the caller knows nothing.
static bool maySaveFrame(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode);
        });
    });
}

OperandSummary summarizeEntryParameter(FunctionAnalyses& analyses, uint32_t entryParameter)
{
    const Function& function = analyses.function();
    ValueID parameter = function.at(entryBlock).parameters[entryParameter];
    if (!mayBeReference(analyses.program().typeOf(function, parameter)))
        return { .uses = OperandUse::Inspects };

    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> id = escape.objectDefinedBy(parameter);
    if (escape.gaveUp() || !id)
        return { };

    const TrackedObject& object = escape.at(*id);
    OperandUses uses = object.uses;
    auto loadsReference = [&](const ObjectUse& objectUse) {
        return mayLoadReference(analyses, objectUse);
    };
    if (std::ranges::any_of(escape.usesOf(*id), loadsReference))
        uses.add(usesOfLoadedReferences(analyses, entryParameter));
    auto callsInCycle = [&](const ObjectUse& objectUse) {
        return objectUse.uses.containsAny(callingUses) && analyses.blockCycles().isInCycle(objectUse.use.block);
    };
    if (std::ranges::any_of(escape.usesOf(*id), callsInCycle))
        uses.add(OperandUse::CallsRepeatedly);

    EscapeReasons direct = object.reasons - EscapeReason::StoredInEscapingObject;
    if (direct.contains(EscapeReason::Returned))
        uses.add(OperandUse::Returns);
    if (function.isUnsplitCoroutine() || maySaveFrame(function) || !(direct - EscapeReason::Returned).isEmpty())
        return { uses | OperandUse::Retains, RetainedIn::UnknownMemory, 0 };
    return summarizeRetention(analyses, uses, escape.containersOf(*id));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
