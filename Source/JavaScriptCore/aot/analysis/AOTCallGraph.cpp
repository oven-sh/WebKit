/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallGraph.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTFlowResults.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include <numeric>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BodyReferences {
    explicit BodyReferences(unsigned numberOfFunctions)
        : calleeOffsets(FillWith { }, numberOfFunctions + 1, 0u)
        , directCallSites(FillWith { }, numberOfFunctions, 0u)
        , linkedCallSites(FillWith { }, numberOfFunctions, 0u)
        , hasOtherReference(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0))
        , callsUnknownCode(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0))
    {
    }

    Vector<uint32_t> calleeOffsets;
    Vector<FunctionID> callees;
    Vector<uint32_t> directCallSites;
    Vector<uint32_t> linkedCallSites;
    Vector<uint8_t> hasOtherReference;
    Vector<uint8_t> callsUnknownCode;
};

struct CalledCode {
    Vector<FunctionID>& functions;
    bool includesUnknownCode { false };
};

class SuccessorList {
public:
    SuccessorList(std::span<const FunctionID> functions, std::optional<unsigned> unknownCodeNode)
        : m_functions(functions)
        , m_unknownCodeNode(unknownCodeNode)
    {
    }

    size_t size() const { return m_functions.size() + (m_unknownCodeNode ? 1 : 0); }
    unsigned operator[](size_t position) const { return position < m_functions.size() ? m_functions[position].index() : *m_unknownCodeNode; }

private:
    std::span<const FunctionID> m_functions;
    std::optional<unsigned> m_unknownCodeNode;
};

} // anonymous namespace

static void countReference(FunctionID target, FunctionReferenceKind kind, BodyReferences& references)
{
    switch (kind) {
    case FunctionReferenceKind::Call:
        WTF::atomicExchangeAdd(&references.directCallSites[target.index()], 1u, std::memory_order_relaxed);
        return;
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
        WTF::atomicStore(&references.hasOtherReference[target.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        return;
    case FunctionReferenceKind::Dispatch:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void collectCalledCode(const Program& program, const Function& function, const auto& instruction, BodyReferences& references, CalledCode& calledCode)
{
    auto addFunction = [&](FunctionID callee) {
        if (program.at(callee).hasBody())
            calledCode.functions.append(callee);
    };
    forEachFunctionReference(instruction, [&](FunctionID target, FunctionReferenceKind kind) {
        countReference(target, kind, references);
        if (kind == FunctionReferenceKind::Call)
            addFunction(target);
    });
    forEachCalleeSet(instruction, [&](CalleeSetID set) {
        for (FunctionID callee : program.at(set).functions) {
            WTF::atomicExchangeAdd(&references.linkedCallSites[callee.index()], 1u, std::memory_order_relaxed);
            addFunction(callee);
        }
    });
    EffectDescription description = describeEffects(program, function, instruction);
    for (FunctionID callee : description.functions)
        addFunction(callee);
    for (FunctionID callee : description.slowPathFunctions)
        addFunction(callee);
    calledCode.includesUnknownCode |= description.callsUnknownCode;
}

static void appendIndirectCallees(const Program& program, const FlowResults& flow, FunctionID id, Vector<FunctionID>& callees)
{
    Vector<FunctionID, 8> sources;
    auto addSource = [&](FunctionID source) {
        if (FunctionID asRead = findFunctionAsRead(&program, source))
            sources.append(asRead);
    };
    addSource(id);
    for (const InlineFrame& frame : program.at(id).inlineFrames)
        addSource(frame.callee);
    std::ranges::sort(sources);
    removeRepeatedElements(sources);
    for (FunctionID source : sources) {
        for (const FlowIndirectCall& call : flow.indirectCallsFrom(source)) {
            if (program.at(call.callee).hasBody())
                callees.append(call.callee);
        }
    }
}

static BodyReferences collectBodyReferences(const Program& program, const FlowResults& flow, const ParallelLoops& loops)
{
    unsigned count = program.functions.size();
    BodyReferences references(count);
    references.callees = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& callees) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        size_t first = callees.size();
        CalledCode calledCode { callees };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                collectCalledCode(program, function, function.at(id), references, calledCode);
            collectCalledCode(program, function, block.terminator, references, calledCode);
        }
        references.callsUnknownCode[index] = calledCode.includesUnknownCode;
        appendIndirectCallees(program, flow, FunctionID(index), callees);
        auto added = callees.mutableSpan().subspan(first);
        std::ranges::sort(added);
        callees.shrink(callees.size() - std::ranges::unique(added).size());
        references.calleeOffsets[index + 1] = static_cast<uint32_t>(callees.size() - first);
    });
    std::partial_sum(references.calleeOffsets.begin(), references.calleeOffsets.end(), references.calleeOffsets.begin());
    return references;
}

static void markRecordReferences(const Program& program, Vector<uint8_t>& hasOtherReference, const ParallelLoops& loops)
{
    auto mark = [&](FunctionID function) {
        std::atomic_ref(hasOtherReference[function.index()]).store(1, std::memory_order_relaxed);
    };
    auto markReference = [&](FunctionID function, FunctionReferenceKind kind) {
        switch (kind) {
        case FunctionReferenceKind::Closure:
        case FunctionReferenceKind::Constructor:
            mark(function);
            return;
        case FunctionReferenceKind::Call:
        case FunctionReferenceKind::Dispatch:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        forEachFunctionReference(layout, markReference);
        for (const Vector<ClassMember>* members : { &layout.classFlow.members, &layout.classFlow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.function && member.closureUse != ClosureUse::CalledOnly)
                    mark(member.function);
            }
        }
    });
    // Nobody takes the closure out of a variable that is never read: a function declaration that is only called has its direct callers and no other.
    forEachIndexInParallel(loops, program.variables.size(), [&](unsigned index) {
        const Variable& variable = program.variables[index];
        if (variable.flow.reads != ReadState::NeverRead)
            forEachFunctionReference(variable, markReference);
    });
    forEachIndexInParallel(loops, program.modules.size(), [&](unsigned index) {
        forEachFunctionReference(program.modules[index], markReference);
    });
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        const CreatorRecord& creator = program.functions[index].creator;
        forEachFunctionReference(creator, markReference);
        if (creator.is<CreatorRecord::Entrypoint>())
            mark(FunctionID(index));
    });
}

static BitVector findRoots(const Program& program, const FlowResults& flow, const BitVector& isEnteredFromOutside)
{
    BitVector isRoot = isEnteredFromOutside;
    for (const Variable& variable : program.variables) {
        if (variable.function)
            isRoot.set(variable.function.index());
    }
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.kind == FlowIndirectCallKind::Host)
            isRoot.set(call.callee.index());
    }
    return isRoot;
}

static BitVector createBitVector(const Vector<uint8_t>& flags)
{
    BitVector bits(flags.size());
    for (size_t index = 0; index < flags.size(); ++index) {
        if (flags[index])
            bits.quickSet(index);
    }
    return bits;
}

static StronglyConnectedComponents computeComponents(const CallGraph& graph)
{
    unsigned unknownCodeNode = graph.numberOfFunctions();
    return computeStronglyConnectedComponents(unknownCodeNode + 1, [&](unsigned node) -> SuccessorList {
        if (node == unknownCodeNode)
            return { graph.functionsCalledByUnknownCode(), std::nullopt };
        FunctionID function(node);
        if (graph.calleeSet(function) == CalleeSetState::Complete)
            return { graph.calleesOf(function), std::nullopt };
        return { graph.calleesOf(function), unknownCodeNode };
    });
}

CallGraph::Adjacency CallGraph::Adjacency::inverse() const
{
    Adjacency result;
    result.offsets.fill(0, offsets.size());
    for (FunctionID target : targets)
        ++result.offsets[target.index() + 1];
    std::partial_sum(result.offsets.begin(), result.offsets.end(), result.offsets.begin());
    Vector<uint32_t> nextPositions = result.offsets;
    result.targets.grow(targets.size());
    for (unsigned source = 0; source + 1 < offsets.size(); ++source) {
        for (FunctionID target : of(FunctionID(source)))
            result.targets[nextPositions[target.index()]++] = FunctionID(source);
    }
    return result;
}

CallGraph buildCallGraph(const Program& program, const FlowResults& flow, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    PhaseTimes times(build, PassID::BuildCallGraph);
    unsigned count = program.functions.size();
    BodyReferences references = collectBodyReferences(program, flow, loops);
    times.didFinish("collectBodyReferences"_s);
    markRecordReferences(program, references.hasOtherReference, loops);
    times.didFinish("markRecordReferences"_s);
    BitVector isEnteredFromOutside = functionsCalledByHost(program, loops);
    BitVector isRoot = findRoots(program, flow, isEnteredFromOutside);
    times.didFinish("findRoots"_s);

    Vector<uint8_t> hasCompleteCallerSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCalleeSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasOnlyDirectCallers(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCallSiteSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint32_t> callSitesOfHost(FillWith { }, count, 0u);
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        ++references.linkedCallSites[call.callee.index()];
        if (call.kind == FlowIndirectCallKind::Host)
            ++callSitesOfHost[call.callee.index()];
    }
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        hasCompleteCallerSet[index] = !isRoot.get(index)
            && (function.hasOnlyDirectCallers() || callerStateOf(flow, FunctionID(index)) == CallerState::AllKnown);
        hasCompleteCalleeSet[index] = !references.callsUnknownCode[index];
        hasOnlyDirectCallers[index] = !isEnteredFromOutside.get(index)
            && (function.hasOnlyDirectCallers() || !references.hasOtherReference[index]);
        // Flow has followed every closure of the function to its calls, and none is kept as a value: a reference to a closure adds no way in that is not counted.
        const FunctionFlow& functionFlow = flow.function(FunctionID(index));
        hasCompleteCallSiteSet[index] = hasOnlyDirectCallers[index]
            || (!isEnteredFromOutside.get(index) && functionFlow.callers == CallerState::AllKnown && functionFlow.closureUse == ClosureUse::CalledOnly);
    });

    CallGraph graph;
    graph.m_callees.offsets = WTF::move(references.calleeOffsets);
    graph.m_callees.targets = WTF::move(references.callees);
    graph.m_calledByUnknownCode = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& called) {
        if (program.functions[index].hasBody() && !hasCompleteCallerSet[index])
            called.append(FunctionID(index));
    });
    graph.m_hasCompleteCalleeSet = createBitVector(hasCompleteCalleeSet);
    graph.m_callSites = WTF::move(references.linkedCallSites);
    graph.m_callSitesOfHost = WTF::move(callSitesOfHost);
    for (unsigned index = 0; index < count; ++index)
        graph.m_callSites[index] += references.directCallSites[index];
    graph.m_directCallSites = WTF::move(references.directCallSites);
    times.didFinish("describeFunctions"_s);

    // Each runs on one thread.
    runTasksInParallel(loops, {
        [&] {
            graph.m_components = computeComponents(graph);
        },
        [&] {
            graph.m_callers = graph.m_callees.inverse();
        },
        [&] {
            graph.m_roots.reserveInitialCapacity(isRoot.bitCount());
            for (size_t index : isRoot)
                graph.m_roots.append(FunctionID(index));
            graph.m_hasCompleteCallerSet = createBitVector(hasCompleteCallerSet);
            graph.m_hasOnlyDirectCallers = createBitVector(hasOnlyDirectCallers);
            graph.m_hasCompleteCallSiteSet = createBitVector(hasCompleteCallSiteSet);
        },
    });
    times.didFinish("computeComponents"_s);
    return graph;
}

void CallGraph::dump(PrintStream& out, const Program& program) const
{
    auto printFunctions = [&](std::span<const FunctionID> functions) {
        for (FunctionID function : functions)
            out.print(" f", function.index());
    };
    out.print("called by unknown code");
    printFunctions(functionsCalledByUnknownCode());
    out.println();
    out.print("roots");
    printFunctions(roots());
    out.println();
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        FunctionID function(index);
        out.print("f", index, ": callees");
        printFunctions(calleesOf(function));
        out.print(" | callers");
        printFunctions(callersOf(function));
        out.println(" | sites ", numberOfDirectCallSites(function), " | callers ", enumName(callerSet(function)), " | callees ", enumName(calleeSet(function)), " | entries ", enumName(entrySet(function)), " | ", enumName(recursion(function)), " | rank ", bottomUpRank(function), " | call sites ", numberOfCallSites(function), " ", enumName(callSiteSet(function)));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
