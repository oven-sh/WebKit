/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowApply.h"
#include "AOTFlowStamps.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallsOfFunction {
    const ValueFlowResultInputs& inputs;
    FunctionID id;
    const Function& function;
    TypeArena& types;
    FlowBodyUpdate& update;
    FlowSiteTables& sites;
    Vector<AccessSiteID> accessSites;
    bool someCallEntersAnything { false };
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

static bool throwsBeforeEntry(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
}

// A bound function, a proxy and whatever else can be called enter code that nobody lists, so an object that is neither a closure nor a built-in function leaves the set open.
static std::optional<CalleeSet> calleesOf(const ValueFlowResultInputs& inputs, const ValueFlowStatement& call)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = inputs.program.builtins();
    bool isConstruction = !!OperandPosition::newTarget(call.opcode);

    CalleeSet callees;
    callees.isComplete = true;
    ValueFlowMembers members = forEachMemberOf(inputs, inputs.bodies().operandsOf(call)[0], [&](ValueFlowObjectID member) {
        if (!universe.isObject(member))
            return;
        if (FunctionID function = universe.closureFunction(member)) {
            if (!throwsBeforeEntry(inputs.program.at(function), isConstruction))
                callees.functions.append(function);
            return;
        }
        if (inputs.sets().contains(universe.builtinFunctions(), member)) {
            callees.builtins.append(builtins.canonicalID(universe.builtinOf(member)));
            return;
        }
        callees.isComplete = false;
    });
    if (members == ValueFlowMembers::AlmostEverything)
        return std::nullopt;

    canonicalize(callees);
    return callees;
}

static void stampCall(CallsOfFunction& calls, const ValueFlowStatement& call)
{
    std::optional<CalleeSet> callees = calleesOf(calls.inputs, call);
    if (!callees) {
        calls.someCallEntersAnything = true;
        return;
    }

    AccessSiteID site = calls.accessSites[call.site.index()];
    if (!callees->isComplete)
        calls.sites.accessSiteLosses[site.index()] = FlowSiteLoss { .cause = UnprovenCause::UnknownCallee, .witness = { calls.id.index(), call.site.index(), calls.function.locationOf(call.site) } };
    if (callees->functions.isEmpty() && callees->builtins.isEmpty())
        return;

    calls.sites.callsOfBoundFunctions[site.index()] = false;
    calls.update.calls.append(FlowCallStamp { call.site, calls.types.intern(WTF::move(*callees)), false });
}

ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    tables.sites.accessSiteLosses.grow(application.accessSites.numberOfSites());

    Vector<uint8_t> entersAnything(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0));
    TypeInterningPhase phase(types, numberOfFunctions);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(index);
        if (!inputs.isReached(id) || !inputs.build.isEnabled(FlowResultKind::CalleeSets, index) || std::ranges::none_of(statements, callsValue))
            return;

        const Function& function = program.functions[index];
        TypeArena arena(phase, index);
        CallsOfFunction calls { inputs, id, function, arena, application.bodies[index], tables.sites, accessSitesByValue(function, application.accessSites, id) };
        for (const ValueFlowStatement& statement : statements) {
            if (callsValue(statement))
                stampCall(calls, statement);
        }
        std::ranges::sort(calls.update.calls, { }, &FlowCallStamp::value);
        entersAnything[index] = calls.someCallEntersAnything;
    });
    TypeRemap remap = phase.finish(StringRemap { });
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (!remap.isIdentityFor(index))
            remapTypes(application.bodies[index], remap);
    });

    ValueFlowStampedCalls result;
    result.functionsInStamps = BitVector(numberOfFunctions);
    result.someCallEntersAnything = entersAnything.contains(static_cast<uint8_t>(1));
    for (const FlowBodyUpdate& update : application.bodies) {
        for (const FlowCallStamp& stamp : update.calls) {
            for (FunctionID function : types.at(stamp.callees).functions)
                result.functionsInStamps.quickSet(function.index());
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
