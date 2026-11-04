/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCompileOptionWords.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTHeatGraph.h"
#include "AOTIRParser.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr FunctionID moduleBody { 0u };
static constexpr FunctionID fetch { 1u };
static constexpr FunctionID route { 2u };
static constexpr FunctionID respond { 3u };
static constexpr FunctionID registerRoute { 4u };
static constexpr FunctionID onMessage { 5u };
static constexpr FunctionID job { 6u };
static constexpr FunctionID setUp { 7u };
static constexpr FunctionID escaped { 8u };

static constexpr ASCIILiteral serverText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
variable g0 name="setUp" module=m0 declared={type=function} initialization=Function function=f7
variable g1 name="log" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Boolean boolean=true
    v1 = CallDirect function=f7
    v2 = NewClosure function=f5
    v3 = CallBuiltin -, v2 overload=QueueMicrotask/0
    v4 = NewClosure function=f6
    v5 = CallBuiltin -, v4 overload=QueueMicrotask/0
    v6 = GetModuleVariable variable=g1
    v7 = Call v6
    Jump -> next b1()
  b1():
    LoopHint
    v9 = CallDirect function=f4
    Branch v0 -> taken b1(), notTaken b2()
  b2():
    Return
}
function f1 name="fetch" results=[{type=undefined}] {
    v0 = CallDirect function=f2
    Return v0
}
function f2 name="route" results=[{type=undefined}] {
    v0 = CallDirect function=f3
    Return v0
}
function f3 name="respond" results=[{type=undefined}] {
    Return
}
function f4 name="registerRoute" results=[{type=undefined}] {
    Return
}
function f5 name="onMessage" results=[{type=undefined}] {
    Return
}
function f6 name="job" results=[{type=undefined}] {
    Return
}
function f7 name="setUp" results=[{type=undefined}] {
    Return
}
function f8 name="escaped" results=[{type=undefined}] {
    Return
}
)IR"_s;

enum class UnseenCode : bool { CannotRun, MayRun };

static FlowResults createFlowResults(const Program& program, BuiltinCallbackTimes timesOfHandlers, UnseenCode unseenCode)
{
    FlowResultTables tables;
    tables.program = &program;
    if (unseenCode == UnseenCode::MayRun)
        tables.wholeProgram.unseenCode.append(UnseenCodeRecord { });
    tables.functions.functionCountAsRead = program.functions.size();
    tables.functions.functions.grow(program.functions.size());
    tables.functions.parameterSummaries.grow(program.functions.size());
    for (FunctionID function : { route, respond, registerRoute, job, setUp })
        tables.functions.functions[function.index()].callers = CallerState::AllKnown;
    for (FunctionID function : { moduleBody, setUp })
        tables.functions.functions[function.index()].executions = ExecutionCount::AtMostOnce;
    tables.functions.indirectCalls = {
        { moduleBody, AccessSiteID(0u), fetch, FlowIndirectCallKind::BuiltinCallback, timesOfHandlers, false },
        { moduleBody, AccessSiteID(0u), onMessage, FlowIndirectCallKind::BuiltinCallback, timesOfHandlers, false },
        { moduleBody, AccessSiteID(0u), job, FlowIndirectCallKind::BuiltinCallback, BuiltinCallbackTimes::Later, false },
    };
    return FlowResults(WTF::move(tables));
}

namespace {

struct Server {
    explicit Server(std::optional<BuiltinCallbackTimes> timesOfHandlers, UnseenCode unseenCode = UnseenCode::MayRun)
    {
        if (!program) {
            build.diagnostics().finish(nullptr);
            printDiagnostics(build.diagnostics(), WTF::dataFile());
        }
        AOT_TEST_CHECK(program);
        if (timesOfHandlers)
            flow = createFlowResults(*program, *timesOfHandlers, unseenCode);
        callGraph = buildCallGraph(*program, flow, build);
        graph = buildHeatGraph(*program, callGraph, flow, effects, build, nullptr, nullptr, nullptr, &graphOfClasses);
        activations = estimateActivations(graph);
    }

    double activationsOf(FunctionID function) const { return activations[function.index()]; }

    static std::optional<double> callsIn(const HeatGraph& graph, HeatNodeID caller, HeatNodeID callee)
    {
        std::span<const HeatNodeID> successors = graph.successors(caller);
        for (unsigned index = 0; index < successors.size(); ++index) {
            if (successors[index] == callee)
                return graph.multiplierOf(caller, index);
        }
        return std::nullopt;
    }

    std::optional<double> callsFrom(HeatNodeID caller, HeatNodeID callee) const { return callsIn(graph, caller, callee); }

    std::optional<double> callsFromFunction(FunctionID caller, FunctionID callee) const { return callsFrom(graph.nodeOf(caller), graph.nodeOf(callee)); }
    std::optional<double> callsFromHost(FunctionID callee) const { return callsFrom(graph.hostNode(), graph.nodeOf(callee)); }
    std::optional<double> callsFromUnknownCode(FunctionID callee) const { return callsFrom(graph.unknownCodeNode(), graph.nodeOf(callee)); }
    std::optional<double> callsFromHostWhereClassesAreEstimated(FunctionID callee) const { return callsIn(graphOfClasses, graphOfClasses.hostNode(), graphOfClasses.nodeOf(callee)); }

    BuildContext build { makeDefaultCompileOptions() };
    std::unique_ptr<Program> program { parseProgram(serverText, IRForm::Prepared, build) };
    FlowResults flow;
    EffectSummaries effects;
    CallGraph callGraph;
    HeatGraph graph;
    HeatGraph graphOfClasses;
    Vector<double> activations;
};

} // anonymous namespace

static void heatGraphLetsTheHostAloneCallHandlers()
{
    Server server(BuiltinCallbackTimes::PerEvent);

    for (FunctionID handler : { fetch, onMessage }) {
        AOT_TEST_CHECK(server.callsFromHost(handler) == HeatModel::eventsPerRun);
        AOT_TEST_CHECK(!server.callsFromFunction(moduleBody, handler));
        AOT_TEST_CHECK(!server.callsFromUnknownCode(handler));
    }
    for (FunctionID function : { fetch, route, respond, onMessage })
        AOT_TEST_CHECK(server.activationsOf(function) == HeatModel::eventsPerRun);

    AOT_TEST_CHECK(server.callsFromFunction(moduleBody, registerRoute) == 10.0);
    AOT_TEST_CHECK(server.activationsOf(registerRoute) == 10);

    AOT_TEST_CHECK(server.callsFromFunction(moduleBody, job) == 1.0 && !server.callsFromHost(job));
    AOT_TEST_CHECK(server.activationsOf(job) == 1);
}

static void heatGraphGivesCodeThatRunsOnceOneActivation()
{
    Server server(BuiltinCallbackTimes::PerEvent);

    AOT_TEST_CHECK(server.callsFrom(server.graph.nodeOf(moduleBody), server.graph.unknownCodeNode()) == 1.0);
    AOT_TEST_CHECK(server.graph.successors(server.graph.unknownCodeNode()).empty());
    AOT_TEST_CHECK(server.callsFromHost(escaped) == 1.0);
    AOT_TEST_CHECK(server.activationsOf(escaped) == 1);
    AOT_TEST_CHECK(server.callsFromHostWhereClassesAreEstimated(escaped) == frequencyConstants.iterationsPerLoop);
    AOT_TEST_CHECK(server.callsFromHostWhereClassesAreEstimated(moduleBody) == 1.0 && !server.callsFromHostWhereClassesAreEstimated(setUp));

    AOT_TEST_CHECK(server.callsFromHost(moduleBody) == 1.0);
    AOT_TEST_CHECK(server.activationsOf(moduleBody) == 1);

    AOT_TEST_CHECK(std::ranges::contains(server.callGraph.roots(), setUp));
    AOT_TEST_CHECK(server.callsFromFunction(moduleBody, setUp) == 1.0 && !server.callsFromHost(setUp));
    AOT_TEST_CHECK(server.activationsOf(setUp) == 1);
}

static void heatGraphLetsNothingCallFromUnknownCodeWhereNoUnseenCodeRuns()
{
    Server server(BuiltinCallbackTimes::PerEvent, UnseenCode::CannotRun);

    AOT_TEST_CHECK(server.callsFrom(server.graph.nodeOf(moduleBody), server.graph.unknownCodeNode()) == 1.0);
    AOT_TEST_CHECK(server.graph.successors(server.graph.unknownCodeNode()).empty());
    AOT_TEST_CHECK(server.callsFromHost(escaped) == 1.0);
    AOT_TEST_CHECK(server.activationsOf(escaped) == 1);
}

static void heatRanksHandlersAboveLoopsAtStart()
{
    Server server(BuiltinCallbackTimes::PerEvent);
    const Program& program = *server.program;
    Heat heat = estimateHeat(program, server.callGraph, server.flow, server.build);

    AOT_TEST_CHECK(hasElements(heat.hottestFirst().predicted().first(5), { fetch, route, respond, onMessage, registerRoute }));
    AOT_TEST_CHECK((heat.percentileOf(program, fetch) == heat.percentileOf(program, onMessage)).predicted());
    AOT_TEST_CHECK((heat.percentileOf(program, respond) > heat.percentileOf(program, registerRoute)).predicted());
    AOT_TEST_CHECK((heat.percentileOf(program, registerRoute) > heat.percentileOf(program, job)).predicted());
    AOT_TEST_CHECK(heat.of(program, moduleBody).predicted() == HeatClass::RunsOnce && heat.of(program, setUp).predicted() == HeatClass::RunsOnce);
}

static void heatGraphCountsOneCallOfCallbackCalledLater()
{
    Server server(BuiltinCallbackTimes::Later);

    for (FunctionID callback : { fetch, onMessage, job })
        AOT_TEST_CHECK(server.callsFromFunction(moduleBody, callback) == 1.0);
    for (FunctionID callback : { fetch, onMessage }) {
        AOT_TEST_CHECK(server.callsFromHost(callback) == 1.0);
        AOT_TEST_CHECK(!server.callsFromUnknownCode(callback));
    }
    for (FunctionID function : { fetch, route, respond, onMessage, job })
        AOT_TEST_CHECK(server.activationsOf(function) < server.activationsOf(registerRoute));
    AOT_TEST_CHECK(server.activationsOf(moduleBody) == 1);
}

static void heatGraphWithoutFlowResultsGoesByOperandUses()
{
    Server server(std::nullopt);

    AOT_TEST_CHECK(server.callsFromFunction(moduleBody, onMessage) == HeatModel::callsOfRepeatedCallback);
    AOT_TEST_CHECK(server.callsFromFunction(moduleBody, job) == HeatModel::callsOfRepeatedCallback);
    AOT_TEST_CHECK(!server.callsFromFunction(moduleBody, fetch));
    AOT_TEST_CHECK(server.callsFromHost(fetch) == 1.0 && server.callsFromHost(setUp) == 1.0);
    for (FunctionID function : { moduleBody, setUp })
        AOT_TEST_CHECK(server.callsFromHostWhereClassesAreEstimated(function) == frequencyConstants.iterationsPerLoop);
}

#define FOR_EACH_AOT_HEAT_GRAPH_TEST(v) \
    v(heatGraphLetsTheHostAloneCallHandlers) v(heatGraphGivesCodeThatRunsOnceOneActivation) v(heatGraphLetsNothingCallFromUnknownCodeWhereNoUnseenCodeRuns) v(heatRanksHandlersAboveLoopsAtStart) \
    v(heatGraphCountsOneCallOfCallbackCalledLater) v(heatGraphWithoutFlowResultsGoesByOperandUses)

std::span<const NativeTest> heatGraphTests()
{
#define AOT_HEAT_GRAPH_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_HEAT_GRAPH_TEST(AOT_HEAT_GRAPH_TEST_ENTRY) };
#undef AOT_HEAT_GRAPH_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
