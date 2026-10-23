/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowStamps.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowPositions.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallsOfFunction {
    const ValueFlowResultInputs& inputs;
    FunctionID id;
    const Function& function;
    TypeArena& types;
    ValueFlowPositions& positions;
    ValueSetMaker& maker;
    FlowBodyUpdate& update;
    FlowSiteTables& sites;
    Vector<AccessSiteID> accessSites;
    uint64_t numberOfUnknownCallees { 0 };
};

} // anonymous namespace

// The first operand of each is the callee.
static bool callsValue(const ValueFlowStatement& statement)
{
    if (statement.kind != ValueFlowStatementKind::Operation)
        return false;
    switch (statement.opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    default:
        return false;
    }
}

// The positions say what keeps the type of the callee from being proven. Where nothing does, something arrives that can be called and is neither a closure nor a built-in function: a bound function, a proxy.
static FlowSiteLoss lossOfCall(CallsOfFunction& calls, const ValueFlowStatement& call, const Type& family, ValueFlowNodeID callee, ValueSetID held)
{
    FlowSiteLoss loss = calls.positions.lossOf(calls.positions.resultAt(family, held), callee);
    if (loss.cause == UnprovenCause::Unknown) {
        loss.cause = UnprovenCause::UnknownCallee;
        calls.inputs.sets().forEachMember(calls.maker.intersect(held, calls.inputs.universe().otherCallables()), [&](ValueFlowObjectID object) {
            if (!hasFunction(loss.witness))
                loss.witness = witnessOfAllocation(calls.inputs, object);
        });
    }
    // What no earlier operation explains begins here.
    if (!hasFunction(loss.witness))
        loss.witness = { calls.id.index(), call.site.index(), calls.function.locationOf(call.site) };
    return loss;
}

// A set that is not complete still lists what the solve has followed to the call and no placeholder covers. Whatever else the call enters has escaped or is lost, and no function whose closure has is said to have known callers.
static void stampCall(CallsOfFunction& calls, const ValueFlowStatement& call)
{
    ValueFlowNodeID callee = calls.inputs.bodies().operandsOf(call)[0].node;
    const Type& family = calls.inputs.program.typeOf(calls.function, calls.function.at(call.site).operand(0));
    ValueSetID held = calls.inputs.valueOf(calls.maker, calls.inputs.bodies().operandsOf(call)[0]);
    std::optional<CalleeSet> callees = calls.positions.calleesAt(family, held, !!OperandPosition::newTarget(call.opcode));
    calls.numberOfUnknownCallees += calls.inputs.sets().contains(held, ValueFlowUniverse::unknownObject());

    AccessSiteID site = calls.accessSites[call.site.index()];
    if (!callees || !callees->isComplete)
        calls.sites.accessSiteLosses[site.index()] = lossOfCall(calls, call, family, callee, held);
    // A complete set without a member says that the call enters nothing: whatever arrives cannot be called.
    if (!callees || (!callees->isComplete && callees->functions.isEmpty() && callees->builtins.isEmpty()))
        return;

    calls.sites.callsOfBoundFunctions[site.index()] = false;
    calls.update.calls.append(FlowCallStamp { call.site, calls.types.intern(WTF::move(*callees)) });
}

ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    RELEASE_ASSERT(tables.sites.accessSiteLosses.size() == application.accessSites.numberOfSites());

    // The types count what lies outside them. Here the same sets are only looked at again.
    ValueFlowPositionCounts counts(nullptr);
    ValueSetMakers makers(inputs);
    auto phase = makeUnique<TypeInterningPhase>(types, numberOfFunctions);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(index);
        if (!inputs.isReached(id) || std::ranges::none_of(statements, callsValue))
            return;

        const Function& function = program.functions[index];
        Vector<AccessSiteID> accessSites = accessSitesByValue(function, application.accessSites, id);
        if (!inputs.build.isEnabled(FlowResultKind::CalleeSets, index)) {
            for (const ValueFlowStatement& statement : statements) {
                if (callsValue(statement))
                    tables.sites.accessSiteLosses[accessSites[statement.site.index()].index()] = FlowSiteLoss { .cause = UnprovenCause::ResultDisabled };
            }
            return;
        }

        TypeArena arena(*phase, index);
        ValueFlowPositions positions(inputs, arena, counts, index, &makers.of(worker));
        CallsOfFunction calls { inputs, id, function, arena, positions, makers.of(worker), application.bodies[index], tables.sites, WTF::move(accessSites) };
        for (const ValueFlowStatement& statement : statements) {
            if (callsValue(statement))
                stampCall(calls, statement);
        }
        AOT::sort(calls.update.calls, { }, &FlowCallStamp::value);
        inputs.build.statistics().count(StatisticID::FlowCallsOfUnknownCallees, calls.numberOfUnknownCallees);
    });
    TypeRemap remap = phase->finish(StringRemap { }, &inputs.loops());
    inputs.build.destroyLater(WTF::move(phase));
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (!remap.isIdentityFor(index))
            remapTypes(application.bodies[index], remap);
    });

    ValueFlowStampedCalls result;
    result.functionsInStamps = BitVector(numberOfFunctions);
    Vector<BitVector> functionsSeenBy(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        if (application.bodies[index].calls.isEmpty())
            return;
        BitVector& functions = functionsSeenBy[worker];
        functions.ensureSize(numberOfFunctions);
        for (const FlowCallStamp& stamp : application.bodies[index].calls) {
            for (FunctionID function : types.at(stamp.callees).functions)
                functions.quickSet(function.index());
        }
    });
    for (const BitVector& functions : functionsSeenBy)
        result.functionsInStamps.merge(functions);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
