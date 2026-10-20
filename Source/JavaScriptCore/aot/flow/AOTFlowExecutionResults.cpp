/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class SiteKind : uint8_t {
    Call,
    PropertyAccess,
    Other,
};

struct SiteStatement {
    ValueID origin;
    SiteKind kind { SiteKind::Other };
};

struct SingleCall {
    FunctionID caller;
    ValueID site;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };
    FunctionID callee;
};

} // anonymous namespace

// The loader forgets a CommonJS module whose body threw, and the next require() runs the body again, beside whatever the failed run has let out.
// Each site that requires the module gets what the body throws. An entry point that no site requires fails once: nobody is there to ask for it again.
static bool mayRunAgainAfterFailure(const FlowSolution& solution, ModuleID module)
{
    const FlowGraph& graph = solution.graph;
    FunctionID body = solution.program.at(module).body;
    FlowLocalID thrown = graph.summaryOf(body).thrown();
    bool isEntryPoint = std::ranges::any_of(solution.program.entries, [&](const Entry& entry) {
        return entry.module == module;
    });

    bool mayRunAgain = false;
    graph.forEachInstanceOf(body, [&](FlowInstanceID instance) {
        FlowNodeID node = graph.nodeOf(instance, thrown);
        if (graph.valueOf(node).isEmpty())
            return;
        mayRunAgain |= !isEntryPoint;
        graph.forEachSuccessor(node, [&](FlowNodeID target, TypeID) {
            mayRunAgain |= target != graph.nativeCode();
        });
    });
    return mayRunAgain;
}

static bool isBodyEvaluatedAtMostOnce(const FlowSolution& solution, ModuleID module)
{
    switch (solution.program.at(module).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::CommonJs:
        return cacheEntryStateOf(solution, module) == FlowCacheEntryState::NeverRemoved && !mayRunAgainAfterFailure(solution, module);
    case ModuleKind::Data:
        return cacheEntryStateOf(solution, module) == FlowCacheEntryState::NeverRemoved;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isModuleEvaluation(const FlowCallLink& link)
{
    return link.kind == FlowCallLinkKind::Host && link.times == BuiltinCallbackTimes::Once && !link.isInCycle;
}

static bool isEnteredOnlyByEvaluation(const FlowSolution& solution, const FlowCallIndex& calls, FunctionID body)
{
    std::span<const FlowCallLinkID> links = calls.linksTo(body);
    return links.size() == 1 && isModuleEvaluation(solution.records.at(links[0]));
}

static BitVector findSeeds(const FlowSolution& solution, const FlowCallIndex& calls)
{
    const Program& program = solution.program;
    const BitVector& evaluatedModules = solution.records.evaluatedModules();
    BitVector seeds(program.functions.size());
    for (size_t index = 0; index < program.modules.size(); ++index) {
        FunctionID body = program.modules[index].body;
        if (!body || !evaluatedModules.get(index) || !isEnteredOnlyByEvaluation(solution, calls, body))
            continue;
        if (isBodyEvaluatedAtMostOnce(solution, ModuleID(index)))
            seeds.quickSet(body.index());
    }
    return seeds;
}

static bool mayEnterCalleeRepeatedly(const FlowCallLink& link)
{
    // A built-in constructs with its callback too, and a function that a callback binds is called as often as the callback.
    if (link.isInCycle || link.times != BuiltinCallbackTimes::Once)
        return true;
    switch (link.kind) {
    case FlowCallLinkKind::Call:
    case FlowCallLinkKind::Construct:
    case FlowCallLinkKind::Accessor:
    case FlowCallLinkKind::BuiltinCallback:
    case FlowCallLinkKind::Protocol:
        return false;
    case FlowCallLinkKind::Host:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static SiteKind siteKindOf(FlowCallLinkKind kind)
{
    switch (kind) {
    case FlowCallLinkKind::Call:
    case FlowCallLinkKind::Construct:
    case FlowCallLinkKind::BuiltinCallback:
        return SiteKind::Call;
    case FlowCallLinkKind::Accessor:
        return SiteKind::PropertyAccess;
    case FlowCallLinkKind::Protocol:
    case FlowCallLinkKind::Host:
        return SiteKind::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<SingleCall> findSingleCall(const FlowSolution& solution, const FlowCallIndex& calls, FunctionID callee)
{
    std::optional<SingleCall> call;
    for (FlowCallLinkID id : calls.linksTo(callee)) {
        const FlowCallLink& link = solution.records.at(id);
        if (mayEnterCalleeRepeatedly(link))
            return std::nullopt;
        if (!call) {
            call = SingleCall { link.caller, link.site, link.kind, callee };
            continue;
        }
        if (call->caller != link.caller || call->site != link.site || call->kind != link.kind)
            return std::nullopt;
    }
    return call;
}

static bool mayBeEnteredOutsideCallLinks(const FlowSolution& solution, const BitVector& isCalledByHost, FunctionID function)
{
    if (isCalledByHost.get(function.index()) || solution.isPastWorkLimit(function))
        return true;
    return std::ranges::any_of(solution.objects.closuresOf(function), [&](FlowObjectID closure) {
        return solution.objectsInNativeCode().contains(closure)
            || solution.objectsInUnknownJavaScript().contains(closure)
            || solution.records.usesOf(closure).contains(FlowObjectUse::CalledRepeatedly);
    });
}

static Vector<SingleCall> collectSingleCallsByCaller(const FlowSolution& solution, const FlowCallIndex& calls, const BitVector& seeds)
{
    const ParallelLoops& loops = solution.build.parallelLoops();
    BitVector isCalledByHost = functionsCalledByHost(solution.program);
    Vector<SingleCall> singleCalls = collectInParallel<SingleCall>(loops, solution.program.functions.size(), [&](unsigned index, Vector<SingleCall>& found) {
        if (seeds.quickGet(index))
            return;
        std::optional<SingleCall> call = findSingleCall(solution, calls, FunctionID(index));
        if (call && !mayBeEnteredOutsideCallLinks(solution, isCalledByHost, call->callee))
            found.append(*call);
    });
    parallelSort(loops, singleCalls.mutableSpan(), [](const SingleCall& a, const SingleCall& b) {
        return std::tie(a.caller, a.callee) < std::tie(b.caller, b.callee);
    });
    return singleCalls;
}

static std::optional<SiteStatement> siteStatementOf(const FlowStatements& statements, FlowStatementReference reference)
{
    uint32_t index = reference.index;
    switch (reference.kind) {
    case FlowStatementKind::Call:
        return SiteStatement { statements.calls[index].origin, SiteKind::Call };
    case FlowStatementKind::DirectCall:
        return SiteStatement { statements.directCalls[index].origin, SiteKind::Call };
    case FlowStatementKind::FieldAccess:
        return SiteStatement { statements.fieldAccesses[index].origin, SiteKind::PropertyAccess };
    case FlowStatementKind::PropertyAccess:
        return SiteStatement { statements.propertyAccesses[index].origin, SiteKind::PropertyAccess };
    case FlowStatementKind::NaturalOperation:
        return SiteStatement { statements.naturalOperations[index].origin, SiteKind::Other };
    case FlowStatementKind::ElementAccess:
        return SiteStatement { statements.elementAccesses[index].origin, SiteKind::Other };
    case FlowStatementKind::SlotAccess:
        return SiteStatement { statements.slotAccesses[index].origin, SiteKind::Other };
    case FlowStatementKind::PrototypeAccess:
        return SiteStatement { statements.prototypeAccesses[index].origin, SiteKind::Other };
    case FlowStatementKind::PropertyCopy:
        return SiteStatement { statements.propertyCopies[index].origin, SiteKind::Other };
    case FlowStatementKind::ClassDefinition:
        return SiteStatement { statements.classDefinitions[index].origin, SiteKind::Other };
    case FlowStatementKind::MethodDefinition:
        return SiteStatement { statements.methodDefinitions[index].origin, SiteKind::Other };
    case FlowStatementKind::Await:
        return SiteStatement { statements.awaits[index].origin, SiteKind::Other };
    case FlowStatementKind::Iteration:
        return SiteStatement { statements.iterations[index].origin, SiteKind::Other };
    case FlowStatementKind::OperandUse: {
        const FlowOperandUse& use = statements.operandUses[index];
        if (!use.operand || !use.thrown)
            return std::nullopt;
        return SiteStatement { use.origin, SiteKind::Other };
    }
    case FlowStatementKind::ModuleOperation:
        return SiteStatement { statements.moduleOperations[index].origin, SiteKind::Other };
    case FlowStatementKind::Filter:
    case FlowStatementKind::ConditionalSeed:
    case FlowStatementKind::Allocation:
    case FlowStatementKind::LocationAccess:
    case FlowStatementKind::UnseenCodeEvent:
        return std::nullopt;
    case FlowStatementKind::BuiltinCall:
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Ascending by origin. A conversion enters one method once per operand, and an access under a key that is an object also converts the key.
static Vector<SiteStatement> findSingleEntrySites(const FlowStatements& statements)
{
    Vector<SiteStatement> siteStatements;
    statements.forEach([&](FlowStatementReference reference) {
        if (std::optional<SiteStatement> statement = siteStatementOf(statements, reference))
            siteStatements.append(*statement);
    });
    std::ranges::sort(siteStatements, { }, &SiteStatement::origin);

    Vector<SiteStatement> sites;
    for (size_t index = 0; index < siteStatements.size(); ++index) {
        const SiteStatement& statement = siteStatements[index];
        ValueID origin = statement.origin;
        // One function may be a callee of each statement of an operation, and the records keep one link per site and callee.
        bool sharesOrigin = (index && siteStatements[index - 1].origin == origin)
            || (index + 1 < siteStatements.size() && siteStatements[index + 1].origin == origin);
        if (origin && !sharesOrigin && statement.kind != SiteKind::Other)
            sites.append(statement);
    }
    return sites;
}

static BitVector findFunctionsEnteredAtMostOnce(const FlowSolution& solution, const FlowCallIndex& calls)
{
    BitVector seeds = findSeeds(solution, calls);
    Vector<SingleCall> singleCalls = collectSingleCallsByCaller(solution, calls, seeds);

    BitVector enteredAtMostOnce = seeds;
    Vector<FunctionID> worklist;
    for (size_t index : seeds)
        worklist.append(FunctionID(index));

    while (!worklist.isEmpty()) {
        FunctionID caller = worklist.takeLast();
        auto callsFromCaller = std::ranges::equal_range(singleCalls, caller, { }, &SingleCall::caller);
        if (callsFromCaller.empty())
            continue;
        Vector<SiteStatement> sites = findSingleEntrySites(solution.graph.summaryOf(caller).statements);
        for (const SingleCall& call : callsFromCaller) {
            auto site = std::ranges::lower_bound(sites, call.site, { }, &SiteStatement::origin);
            if (site == sites.end() || site->origin != call.site || site->kind != siteKindOf(call.kind))
                continue;
            enteredAtMostOnce.quickSet(call.callee.index());
            worklist.append(call.callee);
        }
    }
    return enteredAtMostOnce;
}

Vector<ExecutionCount> computeExecutionResults(const FlowSolution& solution, const FlowCallIndex& calls)
{
    Vector<ExecutionCount> counts(FillWith { }, solution.program.functions.size(), ExecutionCount::Unknown);
    for (size_t index : findFunctionsEnteredAtMostOnce(solution, calls)) {
        if (solution.build.isEnabled(FlowResultKind::ExecutionCounts, static_cast<uint32_t>(index)))
            counts[index] = ExecutionCount::AtMostOnce;
    }
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
