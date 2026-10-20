/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class TailCalls : uint8_t { Required, Allowed, None };

} // anonymous namespace

static TailCalls classifyTailCalls(const Function& function)
{
    TailCalls tailCalls = TailCalls::None;
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        switch (block.terminator.requirement()) {
        case TailCallRequirement::Required:
            return TailCalls::Required;
        case TailCallRequirement::Allowed:
            tailCalls = TailCalls::Allowed;
            break;
        }
    }
    return tailCalls;
}

static unsigned demoteTailCalls(const AnalysisContext& context, Function& function, TypeArena& types)
{
    unsigned numberOfDemotedCalls = 0;
    IRBuilder builder(context, function, types);
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (function.blocks[index].terminator.kind != TerminatorKind::ReturnCall)
            continue;
        builder.demoteReturnCall(BlockID(index));
        ++numberOfDemotedCalls;
    }
    return numberOfDemotedCalls;
}

static bool returnsValue(const Function& function)
{
    return std::ranges::any_of(function.blocks, [](const Block& block) {
        return block.terminator.kind == TerminatorKind::Return && block.terminator.valueOperand();
    });
}

static ValueID returnedValueOf(const Function& function, const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        if (ValueID value = terminator.valueOperand())
            return sameObjectThroughCasts(function, value);
        return { };
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool returnsFreshResult(const AnalysisContext& context, const Function& function)
{
    FunctionAnalyses analyses(context, function);
    FreshResultShape shape = computeFreshResults(analyses);
    if (shape.state == FreshResultsState::Unknown && shape.variants.size() > context.build.limit(LimitID::MaxVirtualObjectVariants))
        context.build.gaveUp(LimitID::MaxVirtualObjectVariants, analyses.subject());
    return shape.isFresh();
}

static void appendIfFresh(const AnalysisContext& context, Function&& body, TypeArena& types, Vector<SplitCandidate>& found)
{
    if (classifyTailCalls(body) == TailCalls::Required)
        return;
    unsigned numberOfDemotedCalls = demoteTailCalls(context, body, types);
    if (!returnsFreshResult(context, body))
        return;
    context.build.statistics().count(StatisticID::ReturnCallsDemoted, numberOfDemotedCalls);
    found.append(SplitCandidate { context.id, WTF::move(body) });
}

static void remapExpandedBodies(const ParallelLoops& loops, const BodyRemaps& remaps, Vector<SplitCandidate>& candidates)
{
    if (remaps.isIdentity())
        return;
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
        if (std::optional<Function>& body = candidates[index].expandedBody)
            applyRemaps(remaps, *body);
    });
}

static void remapShape(const BodyRemaps& remaps, FreshResultShape& shape)
{
    for (TypeID& type : shape.partTypes)
        remaps.types.apply(type);

    Function prototypes;
    for (FreshResultVariant& variant : shape.variants) {
        remaps.types.apply(variant.description.type);
        remaps.types.apply(variant.description.elementType);
        prototypes.values.append(WTF::move(variant.prototype));
    }
    applyRemaps(remaps, prototypes);
    for (size_t index = 0; index < shape.variants.size(); ++index)
        shape.variants[index].prototype = WTF::move(prototypes.values[index]);
}

FreshResultSplitter::FreshResultSplitter(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
    : m_editor(editor)
    , m_inputs(inputs)
    , m_runner(runner)
{
}

AnalysisContext FreshResultSplitter::contextFor(FunctionID id) const
{
    return { build(), program(), &m_inputs.flow, &m_inputs.effects, id };
}

bool FreshResultSplitter::shouldConsider(FunctionID id) const
{
    return !isWrapper(id)
        && canReturnParts(program().at(id))
        && build().isEnabled(PassID::SplitFreshResults, id)
        && !isCold(m_inputs.heat.of(program(), id));
}

void FreshResultSplitter::collectExistingWrappers()
{
    const Vector<Function>& functions = program().functions;
    for (size_t index = 0; index < functions.size(); ++index) {
        if (functions[index].role == FunctionRole::ResultWrapper)
            m_wrappers.set(index);
    }
}

Vector<SplitCandidate> FreshResultSplitter::findCandidates()
{
    unsigned count = program().functions.size();
    InterningPhases phases(m_editor, count);
    auto candidates = collectInParallel<SplitCandidate>(build().parallelLoops(), count, [&](unsigned index, Vector<SplitCandidate>& found) {
        FunctionID id(index);
        if (!shouldConsider(id))
            return;
        const Function& function = program().at(id);
        switch (classifyTailCalls(function)) {
        case TailCalls::Required:
            return;
        case TailCalls::Allowed: {
            if (!returnsValue(function))
                return;
            TaskArenas arenas(phases, index);
            appendIfFresh(contextFor(id), Function(function), arenas.types, found);
            return;
        }
        case TailCalls::None:
            if (returnsFreshResult(contextFor(id), function))
                found.append(SplitCandidate { id, std::nullopt });
            return;
        }
    });
    remapExpandedBodies(build().parallelLoops(), phases.finish(), candidates);
    return candidates;
}

Vector<WrapperCallSite, 2> FreshResultSplitter::findForwardedCalls(FunctionID id) const
{
    if (!shouldConsider(id) || m_inputs.callGraph.recursion(id) != Recursion::NotRecursive)
        return { };

    const Function& function = program().at(id);
    Vector<WrapperCallSite, 2> calls;
    BitVector collectedCalls;
    std::optional<CFG> cfg;
    bool forwardsLastRound = false;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        ValueID returned = returnedValueOf(function, function.blocks[index].terminator);
        if (!returned)
            continue;
        const Value& call = function.at(returned);
        if (call.opcode != Opcode::CallDirect || !isWrapper(call.function()))
            continue;
        if (!cfg)
            cfg.emplace(function);
        if (!cfg->isReachable(BlockID(index)) || collectedCalls.set(returned.index()))
            continue;
        calls.append(WrapperCallSite { returned, call.function() });
        forwardsLastRound |= isWrapperOfLastRound(call.function());
    }
    if (!forwardsLastRound)
        return { };
    return calls;
}

Vector<SplitCandidate> FreshResultSplitter::findForwarders()
{
    unsigned count = program().functions.size();
    InterningPhases phases(m_editor, count);
    auto forwarders = collectInParallel<SplitCandidate>(build().parallelLoops(), count, [&](unsigned index, Vector<SplitCandidate>& found) {
        FunctionID id(index);
        auto calls = findForwardedCalls(id);
        if (calls.isEmpty())
            return;

        TaskArenas arenas(phases, index);
        Function copy = program().at(id);
        {
            FunctionAnalyses analyses(contextFor(id), copy);
            FunctionPassContext context { program(), m_inputs, id, copy, analyses, arenas.types, arenas.strings, build() };
            if (!expandCalls(context, calls.span()))
                return;
        }
        appendIfFresh(contextFor(id), WTF::move(copy), arenas.types, found);
    });
    remapExpandedBodies(build().parallelLoops(), phases.finish(), forwarders);
    return forwarders;
}

// An inline frame holds a copy of the body, which the worker has. Stack traces hide the wrapper.
void FreshResultSplitter::moveInlineFramesToWorkers(std::span<const SplitCandidate> candidates, std::span<const FunctionID> workers)
{
    unsigned count = program().functions.size();
    Vector<FunctionID> workerOf(count);
    for (size_t index = 0; index < candidates.size(); ++index)
        workerOf[candidates[index].id.index()] = workers[index];
    auto isSplit = [&](const InlineFrame& frame) {
        return frame.callee && workerOf[frame.callee.index()];
    };
    forEachIndexInParallel(build().parallelLoops(), count, [&](unsigned index) {
        if (std::ranges::none_of(program().functions[index].inlineFrames, isSplit))
            return;
        for (InlineFrame& frame : m_editor.at(FunctionID(index)).inlineFrames) {
            if (isSplit(frame))
                frame.callee = workerOf[frame.callee.index()];
        }
    });
}

unsigned FreshResultSplitter::split(Vector<SplitCandidate>&& candidates)
{
    m_wrappersOfLastRound.clearAll();
    if (candidates.isEmpty())
        return 0;

    unsigned count = candidates.size();
    const ParallelLoops& loops = build().parallelLoops();
    FunctionID firstWorker = m_editor.reserveFunctions(count);
    Vector<FunctionID> workers(count, [&](size_t index) {
        return FunctionID(firstWorker.index() + index);
    });

    Vector<FreshResultShape> shapes(count);
    InterningPhases workerPhases(m_editor, count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        TaskArenas arenas(workerPhases, index);
        const SplitCandidate& candidate = candidates[index];
        const Function& body = candidate.expandedBody ? *candidate.expandedBody : program().at(candidate.id);
        ResultWorker made = createResultWorker(contextFor(candidate.id), { candidate.id, body }, arenas.types);
        RELEASE_ASSERT(made.shape.isFresh());
        simplifyEditedFunction(m_runner, program(), m_inputs, candidate.id, made.function, arenas);
        m_editor.replaceFunction(workers[index], WTF::move(made.function));
        shapes[index] = WTF::move(made.shape);
    });
    BodyRemaps workerRemaps = workerPhases.finish();
    if (!workerRemaps.isIdentity()) {
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            applyRemaps(workerRemaps, m_editor.at(workers[index]));
            remapShape(workerRemaps, shapes[index]);
        });
    }

    Vector<Function> wrappers(count);
    InterningPhases wrapperPhases(m_editor, count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        TaskArenas arenas(wrapperPhases, index);
        FunctionID id = candidates[index].id;
        wrappers[index] = createResultWrapper(contextFor(id), workers[index], program().at(id), shapes[index], arenas.types);
    });
    BodyRemaps wrapperRemaps = wrapperPhases.finish();
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        applyRemaps(wrapperRemaps, wrappers[index]);
        m_editor.replaceFunction(candidates[index].id, WTF::move(wrappers[index]));
    });
    moveInlineFramesToWorkers(candidates.span(), workers.span());

    for (const SplitCandidate& candidate : candidates) {
        m_wrappers.set(candidate.id.index());
        m_wrappersOfLastRound.set(candidate.id.index());
    }
    return count;
}

void FreshResultSplitter::run()
{
    collectExistingWrappers();
    m_numberOfSplits = split(findCandidates());
    for (uint64_t depth = 0;; ++depth) {
        if (m_wrappersOfLastRound.isEmpty())
            break;
        if (depth >= build().limit(LimitID::MaxForwardingDepth)) {
            build().gaveUp(LimitID::MaxForwardingDepth, Subject { });
            break;
        }
        m_numberOfSplits += split(findForwarders());
    }
    build().statistics().count(StatisticID::FreshResultsSplit, m_numberOfSplits);
    expandCallSites();
    cloneWorkersByReadResults();
}

void splitFreshResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    FreshResultSplitter(editor, inputs, runner).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
