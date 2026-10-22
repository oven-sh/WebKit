/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
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

static bool allocates(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return block.values.containsIf([&](ValueID value) {
            return interpretedOf(function.at(value).opcode) == Interpretation::Allocation;
        });
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

// A variant names one of the allocations that build it.
static BitVector findReturnedObjectsWithFields(const Program& program, const Function& function, const FreshResultShape& shape)
{
    Vector<LayoutID, 2> layouts;
    for (const FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Fields || !variant.description.layout)
            continue;
        LayoutKind kind = program.at(variant.description.layout).kind;
        if (kind == LayoutKind::Record || kind == LayoutKind::Instance)
            layouts.appendIfNotContains(variant.description.layout);
    }

    BitVector objects;
    if (layouts.isEmpty())
        return objects;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::NewObject && layouts.contains(function.at(id).layout()))
                objects.set(id.index());
        }
    }
    return objects;
}

static bool storesIntoNamedField(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropSet:
    case Opcode::PropDefine:
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return true;
    default:
        return false;
    }
}

// DeferClosuresInFields leaves the closure of a field unmade until the field is read, but only where the allocation is given the NewClosure itself.
static bool givesFieldsClosuresMadeOnDemand(FunctionAnalyses& analyses, const FreshResultShape& shape)
{
    const AnalysisContext& context = analyses.context();
    if (!context.build.isEnabled(PassID::DeferClosuresInFields, context.id))
        return false;
    const Function& function = analyses.function();
    BitVector objects = findReturnedObjectsWithFields(context.program, function, shape);
    if (objects.isEmpty())
        return false;

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::NewObject) {
                bool listsClosure = objects.get(id.index()) && std::ranges::any_of(value.valuesOperands(), [&](ValueID listed) {
                    return listed && canLeaveClosureUnmade(analyses, block, id, listed);
                });
                if (listsClosure)
                    return true;
                continue;
            }
            if (!storesIntoNamedField(value.opcode))
                continue;
            ValueID receiver = sameObjectThroughCasts(function, value.operand(*receiverOf(value.opcode)));
            if (objects.get(receiver.index()) && canLeaveClosureUnmade(analyses, block, receiver, value.operand(*storedOf(value.opcode))))
                return true;
        }
    }
    return false;
}

static bool returnsFreshResult(const AnalysisContext& context, const Function& function)
{
    FunctionAnalyses analyses(context, function);
    FreshResultShape shape = computeFreshResults(analyses);
    if (shape.state == FreshResultsState::Unknown && shape.variants.size() > context.build.limit(LimitID::MaxVirtualObjectVariants))
        context.build.gaveUp(LimitID::MaxVirtualObjectVariants, analyses.subject());
    if (!shape.isFresh())
        return false;
    if (givesFieldsClosuresMadeOnDemand(analyses, shape)) {
        context.build.statistics().count(StatisticID::FreshResultsKeptWholeForClosures);
        return false;
    }
    return true;
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

// A loop lasts as long as its longest task from the moment that one begins.
static Vector<uint32_t> findLargestFirst(unsigned count, const Invocable<const Function&(unsigned)> auto& functionAt)
{
    constexpr size_t fewestValues = 4096;
    Vector<uint64_t> keys;
    for (unsigned position = 0; position < count; ++position) {
        size_t numberOfValues = functionAt(position).values.size();
        if (numberOfValues >= fewestValues)
            keys.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | position);
    }
    std::ranges::sort(keys);
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });
}

static Vector<FunctionID> findCallersOf(const CallGraph& callGraph, const BitVector& callees)
{
    Vector<FunctionID> callers;
    for (size_t callee : callees)
        callers.append(callGraph.callersOf(FunctionID(callee)));
    std::ranges::sort(callers);
    removeRepeatedElements(callers);
    return callers;
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
        prototypes.add(WTF::move(variant.prototype));
    }
    applyRemaps(remaps, prototypes);
    for (size_t index = 0; index < shape.variants.size(); ++index)
        shape.variants[index].prototype = WTF::move(prototypes.edit(ValueID(index)));
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

// Only a caller that names the function takes the wrapper in and does without the allocation. A function that is reached by value alone would pay for the wrapper, its call and the parts on every call.
static bool isNamedByCode(const CallGraph& callGraph, FunctionID id)
{
    return id.index() >= callGraph.numberOfFunctions() || callGraph.numberOfCallSitesInCode(id) || callGraph.isExpectedByCode(id);
}

bool FreshResultSplitter::isWrapperThatNoCodeNames(FunctionID id) const
{
    return isWrapper(id) && !isNamedByCode(m_inputs.callGraph, id);
}

bool FreshResultSplitter::shouldConsider(FunctionID id) const
{
    return !isWrapper(id)
        && isNamedByCode(m_inputs.callGraph, id)
        && canReturnParts(program().at(id))
        && build().isEnabled(PassID::SplitFreshResults, id)
        && !isCold(m_inputs.heat.of(program(), id));
}

void FreshResultSplitter::collectExistingWrappers()
{
    const Vector<Function>& functions = program().functions;
    Vector<uint32_t> wrappers = findIndicesInParallel(build().parallelLoops(), functions.size(), [&](unsigned index) {
        return functions[index].role == FunctionRole::ResultWrapper;
    });
    for (uint32_t index : wrappers)
        m_wrappers.set(index);
}

Vector<SplitCandidate> FreshResultSplitter::findCandidates()
{
    const ParallelLoops& loops = build().parallelLoops();
    unsigned count = program().functions.size();
    auto phases = makeUnique<InterningPhases>(m_editor, count);
    Vector<SplitCandidate> candidates;
    {
        ProgramCaches caches(program(), build());
        candidates = collectInParallel<SplitCandidate>(loops, count, largeFunctions(program(), loops).span(), [&](unsigned index, Vector<SplitCandidate>& found) {
            FunctionID id(index);
            if (!shouldConsider(id))
                return;
            const Function& function = program().at(id);
            switch (classifyTailCalls(function)) {
            case TailCalls::Required:
                return;
            case TailCalls::Allowed: {
                if (!returnsValue(function) || !allocates(function))
                    return;
                TaskArenas arenas(*phases, index);
                appendIfFresh(contextFor(id), Function(function), arenas.types, found);
                return;
            }
            case TailCalls::None:
                if (returnsFreshResult(contextFor(id), function))
                    found.append(SplitCandidate { id, std::nullopt });
                return;
            }
        });
    }
    remapExpandedBodies(loops, phases->finish(), candidates);
    build().destroyLater(WTF::move(phases));
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

// A function that has not been split has the body that the call graph was built from, so whoever forwards the result of a wrapper is among its callers.
Vector<SplitCandidate> FreshResultSplitter::findForwarders()
{
    Vector<FunctionID> callers = findCallersOf(m_inputs.callGraph, m_wrappersOfLastRound);
    if (callers.isEmpty())
        return { };

    const ParallelLoops& loops = build().parallelLoops();
    unsigned count = callers.size();
    Vector<uint32_t> largestFirst = findLargestFirst(count, [&](unsigned position) -> const Function& {
        return program().at(callers[position]);
    });
    InterningPhases phases(m_editor, count);
    Vector<SplitCandidate> forwarders;
    {
        ProgramCaches caches(program(), build());
        forwarders = collectInParallel<SplitCandidate>(loops, count, largestFirst.span(), [&](unsigned position, Vector<SplitCandidate>& found) {
            FunctionID id = callers[position];
            auto calls = findForwardedCalls(id);
            if (calls.isEmpty())
                return;

            TaskArenas arenas(phases, position);
            Function copy = program().at(id);
            {
                FunctionAnalyses analyses(contextFor(id), copy);
                FunctionPassContext context { program(), m_inputs, id, copy, analyses, arenas.types, arenas.strings, build() };
                if (!expandCalls(context, calls.span()))
                    return;
            }
            appendIfFresh(contextFor(id), WTF::move(copy), arenas.types, found);
        });
    }
    remapExpandedBodies(loops, phases.finish(), forwarders);
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
    auto bodyOf = [&](unsigned index) -> const Function& {
        const SplitCandidate& candidate = candidates[index];
        return candidate.expandedBody ? *candidate.expandedBody : program().at(candidate.id);
    };
    Vector<uint32_t> largestFirst = findLargestFirst(count, bodyOf);

    Vector<FreshResultShape> shapes(count);
    InterningPhases workerPhases(m_editor, count);
    {
        ProgramCaches caches(program(), build());
        forEachIndexInParallel(loops, count, largestFirst.span(), [&](unsigned index) {
            TaskArenas arenas(workerPhases, index);
            FunctionID id = candidates[index].id;
            ResultWorker made = createResultWorker(contextFor(id), { id, bodyOf(index) }, arenas.types);
            RELEASE_ASSERT(made.shape.isFresh());
            simplifyEditedFunction(m_runner, program(), m_inputs, id, made.function, arenas);
            m_editor.replaceFunction(workers[index], WTF::move(made.function));
            shapes[index] = WTF::move(made.shape);
        });
    }
    BodyRemaps workerRemaps = workerPhases.finish();
    if (!workerRemaps.isIdentity()) {
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            applyRemaps(workerRemaps, m_editor.at(workers[index]));
            remapShape(workerRemaps, shapes[index]);
        });
    }

    Vector<Function> wrappers(count);
    InterningPhases wrapperPhases(m_editor, count);
    {
        ProgramCaches caches(program(), build());
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            TaskArenas arenas(wrapperPhases, index);
            FunctionID id = candidates[index].id;
            wrappers[index] = createResultWrapper(contextFor(id), workers[index], program().at(id), shapes[index], arenas.types);
        });
    }
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
    build().destroyLater(WTF::move(candidates));
    build().destroyLater(WTF::move(shapes));
    return count;
}

void FreshResultSplitter::run()
{
    PhaseTimes times(build(), PassID::SplitFreshResults);
    collectExistingWrappers();
    Vector<SplitCandidate> candidates = findCandidates();
    times.didFinish("findCandidates"_s);
    m_numberOfSplits = split(WTF::move(candidates));
    times.didFinish("split"_s);
    for (uint64_t depth = 0;; ++depth) {
        if (m_wrappersOfLastRound.isEmpty())
            break;
        if (depth >= build().limit(LimitID::MaxForwardingDepth)) {
            build().gaveUp(LimitID::MaxForwardingDepth, Subject { });
            break;
        }
        m_numberOfSplits += split(findForwarders());
    }
    times.didFinish("splitForwarders"_s);
    build().statistics().count(StatisticID::FreshResultsSplit, m_numberOfSplits);
    expandCallSites();
    times.didFinish("expandCallSites"_s);
    cloneWorkersByReadResults();
    times.didFinish("cloneWorkersByReadResults"_s);
}

void splitFreshResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    FreshResultSplitter(editor, inputs, runner).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
