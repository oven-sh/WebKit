/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreachableFunctions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTThreads.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EntrypointLink {
    FunctionID target;
    FunctionID entrypoint;

    friend auto operator<=>(const EntrypointLink&, const EntrypointLink&) = default;
};

struct ReachInputs {
    const Program& program;
    const CallGraph& callGraph;
    std::span<const EntrypointLink> entrypointsByTarget;
    const BitVector& isReachable;
};

} // anonymous namespace

static Vector<EntrypointLink> collectEntrypointsByTarget(const Program& program, const ParallelLoops& loops)
{
    Vector<EntrypointLink> links = collectInParallel<EntrypointLink>(loops, program.functions.size(), [&](unsigned index, Vector<EntrypointLink>& found) {
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (record && function.hasBody())
            found.append({ record->target, FunctionID(index) });
    });
    parallelSort(loops, links.mutableSpan(), std::less<EntrypointLink> { });
    return links;
}

static auto entrypointsOf(const ReachInputs& inputs, FunctionID target)
{
    return std::ranges::equal_range(inputs.entrypointsByTarget, target, { }, &EntrypointLink::target);
}

// Whoever has the closure of a function enters one of its entrypoints, so a closure alone does not keep a body that the entrypoints hold a copy of.
static bool hasEntrypoint(const ReachInputs& inputs, FunctionID function)
{
    return !entrypointsOf(inputs, function).empty();
}

static void collectUnreachedReferences(const ReachInputs& inputs, FunctionID function, Vector<FunctionID>& found)
{
    auto add = [&](FunctionID reference) {
        if (!inputs.isReachable.get(reference.index()))
            found.append(reference);
    };

    // An operation that may enter a function has the effects of that body, also where it enters through an entrypoint: a function without a body counts as unknown code.
    for (FunctionID callee : inputs.callGraph.calleesOf(function))
        add(callee);
    forEachFunctionReference(inputs.program, inputs.program.at(function), [&](FunctionID target, FunctionReferenceKind kind) {
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
        for (const EntrypointLink& link : entrypointsOf(inputs, target))
            add(link.entrypoint);
    });

    // A call of a group takes its convention from the entrypoint it names. No closure need lead to that one: the program may create closures of the other members only.
    const Function& body = inputs.program.at(function);
    for (const Block& block : body.blocks) {
        for (ValueID id : block.values) {
            if (body.at(id).opcode == Opcode::CallGroup)
                add(body.at(id).function());
        }
    }
}

static BitVector findReachableFunctions(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector isReachable(count);
    Vector<FunctionID> frontier;
    auto markReachable = [&](FunctionID function) {
        if (function.index() < count && !isReachable.quickSet(function.index()) && program.at(function).hasBody())
            frontier.append(function);
    };

    Vector<EntrypointLink> entrypointsByTarget = collectEntrypointsByTarget(program, loops);
    ReachInputs inputs { program, callGraph, entrypointsByTarget.span(), isReachable };
    // The tables take the body of a module or of a function that is registered for the host as the sign that it can run.
    BitVector isCalledByHost = functionsCalledByHost(program);
    for (FunctionID root : callGraph.roots()) {
        if (isCalledByHost.get(root.index()) || !hasEntrypoint(inputs, root))
            markReachable(root);
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            markReachable(FunctionID(index));
    }

    while (!frontier.isEmpty()) {
        Vector<FunctionID> references = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            collectUnreachedReferences(inputs, frontier[position], found);
        });
        frontier.shrink(0);
        for (FunctionID reference : references)
            markReachable(reference);
    }
    return isReachable;
}

void removeUnreachableFunctions(ProgramEditor& editor, const CallGraph& callGraph, BuildContext& build)
{
    if (callGraph.roots().empty())
        return;

    const Program& program = editor.program();
    BitVector isReachable = findReachableFunctions(program, callGraph, build);
    uint64_t numberOfRemovedFunctions = 0;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (isReachable.get(index) || !program.functions[index].hasBody())
            continue;
        editor.removeBody(FunctionID(index));
        ++numberOfRemovedFunctions;
    }
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, numberOfRemovedFunctions);
}

// Every table names entrypoints, for closures and for the host alike.
static bool isEnteredOnlyByCalls(FunctionRole role)
{
    switch (role) {
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::GroupEntrypoint:
    case FunctionRole::CoroutineBody:
    case FunctionRole::StubBody:
    case FunctionRole::Model:
        return false;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Signature:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Continuation:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findBodiesWithoutCode(const Program& program, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector isCalledByHost = functionsCalledByHost(program);
    BitVector hasNoCode(count);
    for (unsigned index = 0; index < count; ++index) {
        const Function& function = program.functions[index];
        if (function.hasBody() && isEnteredOnlyByCalls(function.role) && !isCalledByHost.get(index) && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            hasNoCode.quickSet(index);
    }
    if (hasNoCode.isEmpty())
        return hasNoCode;

    Vector<FunctionID> frontier;
    for (unsigned index = 0; index < count; ++index) {
        if (program.functions[index].hasBody() && !hasNoCode.quickGet(index))
            frontier.append(FunctionID(index));
    }
    while (!frontier.isEmpty()) {
        Vector<FunctionID> called = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            forEachFunctionReference(program, program.at(frontier[position]), [&](FunctionID target, FunctionReferenceKind kind) {
                if (kind == FunctionReferenceKind::Call && hasNoCode.get(target.index()))
                    found.append(target);
            });
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
