/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTailCallPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTStackTestPlan.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubBodyConvention.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// By node: where its targets begin, and one more.
struct GraphOfTailCallPlan {
    std::span<const uint32_t> targetsOf(unsigned node) const { return targets.span().subspan(firstTargetOf[node], firstTargetOf[node + 1] - firstTargetOf[node]); }

    Vector<uint32_t> firstTargetOf;
    Vector<uint32_t> targets;
};

} // anonymous namespace

// ---- Rows of tail calls that end

// Null: native code that comes back, or nobody can tell. `comesBack` says which.
static FunctionID calleeOfTailCallInTailCallPlan(const Program& program, const LinkTables& linked, FunctionID caller, const SummaryCall& call, bool& comesBack)
{
    comesBack = false;
    switch (call.callee) {
    case SummaryCall::Callee::Builtin:
        comesBack = callsOfBuiltinComeBack(program, call.builtin);
        return { };
    case SummaryCall::Callee::Binding:
        return call.function;
    case SummaryCall::Callee::Name:
    case SummaryCall::Callee::Value:
        return linked.calleeAt(caller, call.site);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findFunctionsThatEndTheirTailCalls(const Program& program, const Facts& facts)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    BitVector functionsThatEnd(numberOfFunctions);
    const LinkTables& linked = facts.linked();
    if (linked.functions.size() != numberOfFunctions)
        return functionsThatEnd;

    BitVector isJudged(numberOfFunctions);
    BitVector makesTailCallThatNobodyFollows(numberOfFunctions);
    GraphOfTailCallPlan graph;
    graph.firstTargetOf.reserveInitialCapacity(numberOfFunctions + 1);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));
        FunctionID caller(index);
        if (!linked.function(caller).isReached || !program.at(caller).hasBody())
            continue;
        isJudged.quickSet(index);
        for (const SummaryCall& call : facts.summaries().at(caller).calls) {
            if (!call.isInTailPosition)
                continue;
            bool comesBack;
            if (FunctionID callee = calleeOfTailCallInTailCallPlan(program, linked, caller, call, comesBack))
                graph.targets.append(callee.index());
            else if (!comesBack)
                makesTailCallThatNobodyFollows.quickSet(index);
        }
    }
    graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));

    StronglyConnectedComponents rings = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned node) {
        return graph.targetsOf(node);
    });
    // A callee's component has the lower number, so whatever a function jumps to is settled in front of it.
    for (uint32_t member : rings.members) {
        if (!isJudged.quickGet(member) || makesTailCallThatNobodyFollows.quickGet(member) || rings.isInCycle.get(member))
            continue;
        bool everyCalleeEnds = std::ranges::all_of(graph.targetsOf(member), [&](uint32_t callee) {
            return functionsThatEnd.quickGet(callee);
        });
        if (everyCalleeEnds)
            functionsThatEnd.quickSet(member);
    }
    return functionsThatEnd;
}

// Only a call that is written as a direct one is sure to be one in every unit. A call of a value may be left to the word of the closure, whose entry returns in the form of entries.
bool makesOnlyTailCallsThatEnd(const Program& program, const Facts& facts, const BitVector& functionsThatEndTheirTailCalls, FunctionID function)
{
    const LinkTables& linked = facts.linked();
    if (function.index() >= linked.functions.size() || !linked.function(function).isReached || !program.at(function).hasBody())
        return false;
    return std::ranges::all_of(facts.summaries().at(function).calls, [&](const SummaryCall& call) {
        if (!call.isInTailPosition)
            return true;
        if (call.callee == SummaryCall::Callee::Binding && call.function == function)
            return true;
        bool comesBack;
        FunctionID callee = calleeOfTailCallInTailCallPlan(program, linked, function, call, comesBack);
        return callee ? functionsThatEndTheirTailCalls.get(callee.index()) : comesBack;
    });
}

void checkFormsRoundRingsOfTailCalls(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    const LinkTables& linked = facts.linked();
    if (linked.functions.size() != numberOfFunctions)
        return;

    struct TailCall {
        FunctionID caller;
        FunctionID callee;
        ValueID site;
        bool isWrittenDirect;
    };
    Vector<TailCall> tailCalls;
    GraphOfTailCallPlan graph;
    graph.firstTargetOf.reserveInitialCapacity(numberOfFunctions + 1);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));
        FunctionID caller(index);
        if (!linked.function(caller).isReached || !program.at(caller).hasBody())
            continue;
        for (const SummaryCall& call : facts.summaries().at(caller).calls) {
            bool comesBack;
            FunctionID callee = call.isInTailPosition ? calleeOfTailCallInTailCallPlan(program, linked, caller, call, comesBack) : FunctionID();
            if (!callee)
                continue;
            graph.targets.append(callee.index());
            tailCalls.append(TailCall { caller, callee, call.site, call.callee == SummaryCall::Callee::Binding });
        }
    }
    graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));
    StronglyConnectedComponents rings = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned node) {
        return graph.targetsOf(node);
    });

    const CallingConventions& conventions = plan.conventions();
    for (const TailCall& tailCall : tailCalls) {
        CodeUnit leaving { tailCall.caller, CodeRole::Body };
        CodeUnit entered { tailCall.callee, CodeRole::Body };
        if (rings.componentOf[tailCall.caller.index()] != rings.componentOf[tailCall.callee.index()] || !conventions.exists(leaving) || !conventions.exists(entered))
            continue;
        if (conventions.numberOfResults(leaving) != 1 || conventions.numberOfResults(entered) != 1)
            continue;
        Representation form = conventions.resultRepresentation(leaving);
        if (form == conventions.resultRepresentation(entered) && (tailCall.isWrittenDirect || form == Representation::JSValue))
            continue;
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .function = tailCall.caller,
            .value = tailCall.site,
            .detail = "a call in tail position on a ring of such calls cannot be a jump in every unit: the two bodies return in two forms, or in a raw one though the call is one of a value. Each turn of the ring would leave a frame"_s,
        });
    }
}

// ---- Registers

static bool hasNoUnitBesideBodyAndEntriesInTailCallPlan(FunctionConvention convention)
{
    for (unsigned role = 0; role < numberOfCodeRolesOfOneFunction; ++role) {
        if (convention.has(static_cast<CodeRole>(role)) && static_cast<CodeRole>(role) != CodeRole::Body && !isGenericEntry(static_cast<CodeRole>(role)))
            return false;
    }
    return true;
}

static bool liesInRegistersOfStubBodyInTailCallPlan(const ArgumentLocations& locations)
{
    if (locations.numberOfStackWords() || locations.results.size() != 1)
        return false;
    size_t numberOfGPRs = std::ranges::count(locations.parameters, ArgumentLocation::Kind::GPR, &ArgumentLocation::kind);
    size_t numberOfFPRs = std::ranges::count(locations.parameters, ArgumentLocation::Kind::FPR, &ArgumentLocation::kind);
    return numberOfGPRs <= StubBodyConvention::maxNumberOfOperandGPRs && numberOfFPRs <= StubBodyConvention::maxNumberOfOperandFPRs;
}

BitVector findFunctionsThatSaveRegistersOfCallers(const Program& program, const Facts& facts, const CallingConventions& conventions)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    BitVector functions(numberOfFunctions);

    // Whoever jumps to such a function has given up the frame in which it kept its registers for nothing.
    BitVector calleesOfTailCalls(numberOfFunctions);
    for (const LinkTables* tables : { &facts.linked(), &facts.held() }) {
        for (const LinkedCall& call : tables->calls) {
            if (call.isInTailPosition)
                calleesOfTailCalls.set(call.callee.index());
        }
    }

    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        const RuntimeFunctionData& data = program.builtins().at(entry.id);
        FunctionID id = entry.function;
        if (!id || !data.flags.contains(RuntimeFunctionFlag::UsesStubConvention) || data.implements.empty())
            continue;

        // The body is all the code that runs for a call of it: it leaves for no slow version, which would write what the callers keep.
        // An entry for calls by value jumps to the body like anybody else. Whoever comes that way takes it for an ordinary function, which may write more than this one does.
        FunctionConvention convention = conventions.of(id);
        if (!convention.has(CodeRole::Body) || !hasNoUnitBesideBodyAndEntriesInTailCallPlan(convention))
            continue;
        // What it jumped to would write registers that its callers keep.
        bool makesTailCall = std::ranges::any_of(facts.summaries().at(id).calls, [](const SummaryCall& call) {
            return call.isInTailPosition;
        });
        if (makesTailCall || calleesOfTailCalls.get(id.index()))
            continue;

        CodeUnit body { id, CodeRole::Body };
        if (convention.numberOfResults != 1 || conventions.stackWordsReservedByCallersOf(program, body))
            continue;
        if (liesInRegistersOfStubBodyInTailCallPlan(conventions.locationsOf(program, body)))
            functions.quickSet(id.index());
    }
    return functions;
}

// ---- The driver

void planTailCallsAndStackTests(const Program& program, const Facts& facts, Plan& plan, BuildContext&)
{
    if (!facts.mechanisms().contains(Mechanism::DirectCalls))
        return;

    TailCallPlan tailCalls;
    tailCalls.functionsThatSaveRegistersOfCallers = findFunctionsThatSaveRegistersOfCallers(program, facts, plan.conventions());
    plan.setTailCalls(WTF::move(tailCalls));

    plan.setStackTests(planStackTests(program, facts, plan));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
