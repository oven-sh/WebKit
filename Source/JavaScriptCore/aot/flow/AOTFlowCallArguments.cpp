/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTFlowParts.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ArgumentListParts {
    std::span<const FlowNodeID> positions;
    FlowNodeID rest;

    Range restIndexes() const { return { static_cast<int64_t>(positions.size()), arrayIndexRange.maximum }; }
};

} // anonymous namespace

static void joinUnknownArguments(FlowSolver& solver, const FlowCallRequest& request, const ArgumentListParts& parts)
{
    Witness witness = solver.witnessAt(request.site, request.origin);
    for (FlowNodeID position : parts.positions)
        solver.joinUnknown(position, witness);
    solver.joinUnknown(parts.rest, witness);
}

static void expandElementParts(FlowSolver& solver, const FlowCallRequest& request, const ArgumentListParts& parts, FlowObjectID list)
{
    FlowPartAccess load { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load };
    for (uint32_t position = 0; position < parts.positions.size(); ++position) {
        load.value = parts.positions[position];
        loadElementParts(solver, load, list, Range::one(position));
    }
    load.value = parts.rest;
    loadElementParts(solver, load, list, parts.restIndexes());
}

static void expandIndexedProperties(FlowSolver& solver, const FlowCallRequest& request, const ArgumentListParts& parts, FlowObjectID arrayLike)
{
    FlowAccess access { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .thrown = request.operands.thrown };
    if (mayRunRepeatedly(request))
        access.flags.add(FlowAccessFlag::IsInCycle);
    FlowObjectSet receiver(arrayLike);
    for (uint32_t position = 0; position < parts.positions.size(); ++position) {
        access.value = parts.positions[position];
        accessProperties(solver, access, receiver, FlowKeySet::index(Range::one(position)));
    }
    access.value = parts.rest;
    accessProperties(solver, access, receiver, FlowKeySet::index(parts.restIndexes()));
}

static void expandList(FlowSolver& solver, const FlowCallRequest& request, const ArgumentListParts& parts, FlowObjectID list)
{
    if (list == unknownFlowObject) {
        joinUnknownArguments(solver, request, parts);
        return;
    }
    if (!numberOfFilledElements(solver, list)) {
        expandIndexedProperties(solver, request, parts, list);
        return;
    }

    expandElementParts(solver, request, parts, list);
}

// One site makes requests with several lists: its own, and those of the callbacks and protocols that a built-in runs.
static void appendListArguments(FlowSolver& solver, FlowStatementSite site, FlowNodeID lists, FlowPassedArguments& result)
{
    const FlowListArguments& arguments = solver.callState().links.listArguments.ensure(std::pair { site, lists }, [&] {
        FlowListArguments created;
        uint64_t numberOfPositions = solver.callState().limits.maxPositionalParts;
        for (uint64_t position = 0; position < numberOfPositions; ++position)
            created.positions.append(solver.graph().addTemporary(site.instance));
        created.rest = solver.graph().addTemporary(site.instance);
        return created;
    }).iterator->value;
    result.positions.appendVector(arguments.positions);
    result.rest = arguments.rest;
}

static void joinUndefinedArguments(FlowSolver& solver, const ArgumentListParts& parts)
{
    for (FlowNodeID position : parts.positions)
        solver.joinPrimitives(position, Primitive::Undefined);
    solver.joinPrimitives(parts.rest, Primitive::Undefined);
}

FlowPassedArguments expandArguments(FlowSolver& solver, const FlowCallRequest& request)
{
    FlowStatementSite site = request.site;

    FlowPassedArguments result;
    result.positions.append(request.operands.arguments);
    size_t explicitCount = result.positions.size();
    result.count = Range::one(static_cast<int64_t>(explicitCount));

    FlowNodeID lists = request.operands.argumentList;
    if (!lists)
        return result;

    result.count = { static_cast<int64_t>(explicitCount), Range::length().maximum };
    if (lists == unknownValueNode(solver)) {
        result.rest = lists;
        return result;
    }

    appendListArguments(solver, site, lists, result);
    ArgumentListParts parts { result.positions.span().subspan(explicitCount), result.rest };
    FlowTemporaryKey cursor = callTemporaryKey(site, FlowCallTemporaryRole::ArgumentLists, lists.index());
    solver.forEachMember(solver.takeNewObjects(cursor, lists), [&](FlowObjectID list) {
        expandList(solver, request, parts, list);
    });
    // Function.prototype.apply passes no argument for a list that is undefined or null.
    if (solver.takeNewPrimitives(cursor, lists) && (solver.valueOf(lists).primitives & Primitive::UndefinedOrNull))
        joinUndefinedArguments(solver, parts);
    return result;
}

FlowNodeID provenOperandNode(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand)
{
    TypeID type = solver.graph().typeAsRead(operand);
    if (type && !mayBeReference(solver.program().at(type)))
        return operand;

    FlowNodeID gate = solver.gateFor(callTemporaryKey({ site.instance, { } }, FlowCallTemporaryRole::ProvenOperand, operand.index()));
    solver.connect(operand, gate);
    solver.waitOn(operand, site, FlowWakeUp::ContinueAtUnknownObject);
    return gate;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
