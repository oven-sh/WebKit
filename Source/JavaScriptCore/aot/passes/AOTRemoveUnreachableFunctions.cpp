/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreachableFunctions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <numeric>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EntrypointLink {
    FunctionID target;
    FunctionID entrypoint;
};

// The entrypoints that have a body. Every reference to a closure asks for those of its function.
struct EntrypointsByTarget {
    // Ascending.
    std::span<const FunctionID> of(FunctionID target) const LIFETIME_BOUND
    {
        size_t index = target.index();
        if (index + 1 >= offsets.size())
            return { };
        return entrypoints.subspan(offsets[index], offsets[index + 1] - offsets[index]);
    }

    Vector<uint32_t> offsets;
    Vector<FunctionID> entrypoints;
};

// A byte a function, written by all threads.
class ReachedFunctions {
public:
    explicit ReachedFunctions(unsigned count)
        : m_isReached(new std::atomic<uint8_t>[count]())
        , m_count(count)
    {
    }

    // True for the one caller that reaches the function first.
    bool reach(FunctionID function)
    {
        if (function.index() >= m_count)
            return false;
        std::atomic<uint8_t>& isReached = m_isReached[function.index()];
        return !isReached.load(std::memory_order_relaxed) && !isReached.exchange(1, std::memory_order_relaxed);
    }

    BitVector take() const
    {
        BitVector result(m_count);
        for (unsigned index = 0; index < m_count; ++index) {
            if (m_isReached[index].load(std::memory_order_relaxed))
                result.quickSet(index);
        }
        return result;
    }

private:
    std::unique_ptr<std::atomic<uint8_t>[]> m_isReached;
    unsigned m_count;
};

struct ReachInputs {
    const Program& program;
    const CallGraph& callGraph;
    const EntrypointsByTarget& entrypointsByTarget;
    ReachedFunctions& reached;
};

} // anonymous namespace

static EntrypointsByTarget collectEntrypointsByTarget(const Program& program, const ParallelLoops& loops)
{
    unsigned count = program.functions.size();
    // In the order of the entrypoints, which the counting sort keeps.
    Vector<EntrypointLink> links = collectInParallel<EntrypointLink>(loops, count, [&](unsigned index, Vector<EntrypointLink>& found) {
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (record && function.hasBody() && record->target.index() < count)
            found.append({ record->target, FunctionID(index) });
    });

    EntrypointsByTarget result;
    result.offsets.fill(0u, count + 1);
    for (const EntrypointLink& link : links)
        ++result.offsets[link.target.index() + 1];
    std::partial_sum(result.offsets.begin(), result.offsets.end(), result.offsets.begin());
    Vector<uint32_t> cursors(result.offsets.subspan(0, count));
    result.entrypoints.grow(links.size());
    for (const EntrypointLink& link : links)
        result.entrypoints[cursors[link.target.index()]++] = link.entrypoint;
    return result;
}

// Whoever has the closure of a function enters one of its entrypoints, so a closure alone does not keep a body that the entrypoints hold a copy of.
static bool hasEntrypoint(const ReachInputs& inputs, FunctionID function)
{
    return !inputs.entrypointsByTarget.of(function).empty();
}

// Of all threads that reach a function in one round, one lists it.
static void reachReferencesOf(const ReachInputs& inputs, FunctionID function, Vector<FunctionID>& reachedBodies)
{
    auto add = [&](FunctionID reference) {
        if (inputs.reached.reach(reference) && hasBody(inputs.program, reference))
            reachedBodies.append(reference);
    };

    // An operation that may enter a function has the effects of that body, also where it enters through an entrypoint: a function without a body counts as unknown code.
    for (FunctionID callee : inputs.callGraph.calleesOf(function))
        add(callee);
    forEachFunctionReference(inputs.program, function, [&](FunctionID target, FunctionReferenceKind kind) {
        switch (kind) {
        case FunctionReferenceKind::Call:
        case FunctionReferenceKind::Dispatch:
            add(target);
            return;
        case FunctionReferenceKind::Closure:
        case FunctionReferenceKind::Constructor:
            break;
        }
        if (!hasEntrypoint(inputs, target))
            add(target);
        for (FunctionID entrypoint : inputs.entrypointsByTarget.of(target))
            add(entrypoint);
        // Asked about its instances, a bound function that captures no closure of its target has the run time make one.
        const auto* bound = inputs.program.at(target).creator.tryAs<CreatorRecord::BoundFunction>();
        if (!bound || bound->capturesTargetClosure)
            return;
        for (FunctionID entrypoint : inputs.entrypointsByTarget.of(bound->target))
            add(entrypoint);
    });
}

BitVector findReachableFunctions(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    ReachedFunctions reached(count);
    Vector<FunctionID> frontier;
    auto markReachable = [&](FunctionID function) {
        if (reached.reach(function) && hasBody(program, function))
            frontier.append(function);
    };

    EntrypointsByTarget entrypointsByTarget = collectEntrypointsByTarget(program, loops);
    ReachInputs inputs { program, callGraph, entrypointsByTarget, reached };
    // The tables take the body of a module or of a function that is registered for the host as the sign that it can run.
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    for (FunctionID root : callGraph.roots()) {
        if (isCalledByHost.get(root.index()) || !hasEntrypoint(inputs, root))
            markReachable(root);
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            markReachable(FunctionID(index));
    }

    while (!frontier.isEmpty()) {
        frontier = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& reachedBodies) {
            reachReferencesOf(inputs, frontier[position], reachedBodies);
        });
    }
    return reached.take();
}

void removeUnreachableFunctions(ProgramEditor& editor, const CallGraph& callGraph, BuildContext& build)
{
    if (callGraph.roots().empty())
        return;

    PhaseTimes times(build, PassID::RemoveUnreachableFunctions);
    const Program& program = editor.program();
    BitVector isReachable = findReachableFunctions(program, callGraph, build);
    times.didFinish("findReachableFunctions"_s);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> unreachableFunctions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return !isReachable.get(index) && hasBody(program, FunctionID(index));
    });
    forEachIndexInParallel(loops, unreachableFunctions.size(), [&](unsigned position) {
        editor.removeBody(FunctionID(unreachableFunctions[position]));
    });
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, unreachableFunctions.size());
    times.didFinish("remove bodies"_s);
}

// Every table names entrypoints, for closures and for the host alike.
static bool isEnteredOnlyByCalls(FunctionRole role)
{
    switch (role) {
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::CoroutineBody:
    case FunctionRole::StubBody:
    case FunctionRole::Model:
        return false;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Signature:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findBodiesWithoutCode(const Program& program, const BuildContext& build)
{
    return findBodiesWithoutCode(program, build, { });
}

BitVector findBodiesWithoutCode(const Program& program, const BuildContext& build, std::span<const Vector<FunctionID>> calledFunctions)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    BitVector hasNoCode(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        return facts.hasBody && isEnteredOnlyByCalls(facts.role) && !isCalledByHost.get(index) && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index));
    }))
        hasNoCode.quickSet(index);
    if (hasNoCode.isEmpty())
        return hasNoCode;

    Vector<FunctionID> frontier = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& found) {
        if (!hasNoCode.quickGet(index) && hasBody(program, FunctionID(index)))
            found.append(FunctionID(index));
    });
    while (!frontier.isEmpty()) {
        Vector<FunctionID> called = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            auto note = [&](FunctionID target) {
                if (hasNoCode.get(target.index()))
                    found.append(target);
            };
            if (calledFunctions.empty())
                forEachCalledFunction(program, frontier[position], note);
            else
                std::ranges::for_each(calledFunctions[frontier[position].index()], note);
        });
        frontier.shrink(0);
        for (FunctionID function : called) {
            if (hasNoCode.quickClear(function.index()))
                frontier.append(function);
        }
    }
    return hasNoCode;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
