/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT { namespace Typed {

static bool mayBeEnteredByUnknownCode(const FlowResults& flow, FunctionID id, const Function& function, const BodySummary& body)
{
    bool mayBeResumed = body.onReturn.maySuspend;
    if (mayBeResumed || function.mayBeCalledByHost())
        return true;
    return !function.hasOnlyDirectCallers() && callerStateOf(flow, id) != CallerState::AllKnown;
}

SummaryNodeNumbering::SummaryNodeNumbering(const Program& program, const ParallelLoops& loops)
{
    unsigned numberOfFunctions = program.functions.size();
    m_firstNode.grow(numberOfFunctions + 1);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        m_firstNode[index] = 2 + program.functions[index].numberOfEntryParameters();
    });
    uint32_t firstNode = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstNode += std::exchange(m_firstNode[index], firstNode);
    m_firstNode[numberOfFunctions] = firstNode;
    m_numberOfNodes = firstNode + 1;
}

unsigned SummaryNodeNumbering::indexOf(const SummaryNode& node) const
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        RELEASE_ASSERT(node.function.index() < m_firstNode.size() - 1);
        return m_firstNode[node.function.index()];
    case SummaryPart::OnThrow:
        RELEASE_ASSERT(node.function.index() < m_firstNode.size() - 1);
        return m_firstNode[node.function.index()] + 1;
    case SummaryPart::Parameter:
        RELEASE_ASSERT(node.parameter < numberOfParametersOf(node.function));
        return m_firstNode[node.function.index()] + 2 + node.parameter;
    case SummaryPart::UnknownCode:
        return m_numberOfNodes - 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned SummaryNodeNumbering::numberOfParametersOf(FunctionID function) const
{
    RELEASE_ASSERT(function.index() < m_firstNode.size() - 1);
    return m_firstNode[function.index() + 1] - m_firstNode[function.index()] - 2;
}

void BodySummaries::didChange(std::span<const FunctionID> functions)
{
    for (FunctionID id : functions) {
        if (id.index() < m_isChanged.size())
            m_isChanged[id.index()] = 1;
    }
}

EffectSummarizer::EffectSummarizer(const Program& program, const FlowResults& flow, BuildContext& build, BodySummaries* kept)
    : m_program(program)
    , m_flow(flow)
    , m_build(build)
    , m_kept(kept)
    , m_numbering(program, build.parallelLoops())
{
}

EffectSummaries EffectSummarizer::run()
{
    PhaseTimes times(m_build, PassID::ComputeEffectSummaries);
    EffectSummaries seed = createSummariesFromFlow(m_program, m_flow, m_build);
    summarizeBodies(seed);
    m_build.destroyLater(WTF::move(seed));
    times.didFinish("summarizeBodies"_s);
    createNodes();
    times.didFinish("createNodes"_s);
    // The two ask nothing of each other.
    runTasksInParallel(m_build.parallelLoops(), {
        [&] {
            propagateEffects(times);
        },
        [&] {
            propagateWitnesses();
        },
    });
    times.didFinish("propagateWitnesses"_s);
    EffectSummaries summaries = assemble();
    if (m_kept) {
        m_kept->m_bodies = WTF::move(m_bodies);
        m_kept->m_isChanged.fill(0, m_program.functions.size());
    } else
        m_build.destroyLater(WTF::move(m_bodies));
    times.didFinish("assemble"_s);
    return summaries;
}

Vector<uint8_t> EffectSummarizer::findBodiesThatStillHold(const ParallelArray<BodySummary>& bodies) const
{
    const Vector<uint8_t>& isChanged = m_kept->m_isChanged;
    RELEASE_ASSERT(isChanged.size() == bodies.size() && bodies.size() <= m_program.functions.size());
    Vector<uint8_t> stillHolds(FillWith { }, m_program.functions.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(m_build.parallelLoops(), bodies.size(), [&](unsigned index) {
        stillHolds[index] = !isChanged[index] && std::ranges::none_of(bodies[index].dependencies, [&](const SummaryDependency& dependency) {
            return dependency.source.function && isChanged[dependency.source.function.index()];
        });
    });
    return stillHolds;
}

void EffectSummarizer::summarizeBodies(const EffectSummaries& seed)
{
    auto summarize = [&](unsigned index) {
        FunctionID id(index);
        EffectContext context { m_program, seed };
        std::unique_ptr<FunctionAnalyses> analyses = createAnalysesForSummary(m_build, context, m_flow, id, m_program.functions[index]);
        return summarizeBodyLocally(context, id, *analyses);
    };

    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    Vector<uint32_t> largestFirst = largeFunctions(m_program, loops);
    if (!m_kept) {
        m_bodies = ParallelArray<BodySummary>(loops, numberOfFunctions, largestFirst.span(), summarize);
        return;
    }

    ParallelArray<BodySummary> previous = WTF::move(m_kept->m_bodies);
    Vector<uint8_t> stillHolds = findBodiesThatStillHold(previous);
    const bool checksKeptBodies = m_build.options().verifiesAfterEveryPass;
    m_bodies = ParallelArray<BodySummary>(loops, numberOfFunctions, largestFirst.span(), [&](unsigned index) {
        if (!stillHolds[index])
            return summarize(index);
        RELEASE_ASSERT(!checksKeptBodies || previous[index] == summarize(index));
        return WTF::move(previous[index]);
    });
    m_build.destroyLater(WTF::move(previous));

    uint64_t numberKept = std::ranges::count(stillHolds, 1);
    m_build.statistics().count(StatisticID::BodySummariesKept, numberKept);
    m_build.statistics().count(StatisticID::BodySummariesComputed, numberOfFunctions - numberKept);
}

Effects EffectSummarizer::effectsOfUnknownCode() const
{
    Effects unknown = Effects::unknownCode();
    unknown.writes.remove(compiledCodeHeaps);
    for (unsigned index = 0; index < m_program.variables.size(); ++index) {
        if (m_program.variables[index].flow.writers != WriterState::AllKnown)
            unknown.writes.add(m_program, MemoryLocation::moduleVariable(VariableID(index)));
    }
    for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        if (!layout.isEnvironment())
            continue;
        LayoutID id(index);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (flowOf(m_program, FieldReference { id, field }).location.writers != WriterState::AllKnown)
                unknown.writes.add(m_program, MemoryLocation::environmentField(id, field));
        }
    }
    return unknown;
}

// The sources of a node are the nodes whose effects it takes in. A body names sources for the nodes of its own function only, so each function
// fills its own part of the lists. Unknown code takes in every node of the functions that it may enter. A node that takes in unknown code has those
// from there, so it does not list them: most of the dependencies of a program go this way, and what reaches what stays the same.
void EffectSummarizer::createNodes()
{
    static_assert(std::is_trivially_copyable_v<Effects> && std::is_trivially_destructible_v<Effects>);

    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    unsigned numberOfNodes = m_numbering.numberOfNodes();
    unsigned unknownCodeNode = m_numbering.indexOf(SummaryNode::unknownCode());

    BitVector isCalledIndirectly = functionsCalledIndirectly(m_flow, numberOfFunctions);
    Vector<uint8_t> isEnteredByUnknownCode(numberOfFunctions);
    Vector<uint32_t> firstSourceIn(numberOfFunctions);
    Vector<uint32_t> firstNodeEntered(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        const Function& function = m_program.functions[index];
        const BodySummary& body = m_bodies[index];
        isEnteredByUnknownCode[index] = function.hasBody() && (isCalledIndirectly.get(index) || mayBeEnteredByUnknownCode(m_flow, FunctionID(index), function, body));
        firstSourceIn[index] = body.dependencies.size();
        firstNodeEntered[index] = isEnteredByUnknownCode[index] ? 2 + m_numbering.numberOfParametersOf(FunctionID(index)) : 0;
    });
    uint64_t numberOfDependencies = 0;
    uint64_t numberOfNodesEntered = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        numberOfDependencies += std::exchange(firstSourceIn[index], static_cast<uint32_t>(numberOfDependencies));
        numberOfNodesEntered += std::exchange(firstNodeEntered[index], static_cast<uint32_t>(numberOfNodesEntered));
    }
    RELEASE_ASSERT(numberOfDependencies + numberOfNodesEntered <= UINT32_MAX);

    m_effects = MallocSpan<Effects>::malloc(numberOfNodes * sizeof(Effects));
    m_witnesses.grow(numberOfNodes);
    m_firstSource.grow(numberOfNodes);
    m_endOfSources.grow(numberOfNodes);
    m_sources.grow(numberOfDependencies + numberOfNodesEntered);

    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        const BodySummary& body = m_bodies[index];
        unsigned onReturn = m_numbering.indexOf(SummaryNode::onReturn(id));
        unsigned onThrow = m_numbering.indexOf(SummaryNode::onThrow(id));
        unsigned numberOfOwnNodes = 2 + m_numbering.numberOfParametersOf(id);
        RELEASE_ASSERT(body.parameters.size() + 2 == numberOfOwnNodes && onThrow == onReturn + 1);

        m_effects[onReturn] = body.onReturn.withoutSuspension();
        m_effects[onThrow] = body.onThrow.withoutSuspension();
        if (body.onReturn.mayCallUnknownCode)
            m_witnesses[onReturn] = body.unknownCodeWitness;
        if (body.onThrow.mayCallUnknownCode)
            m_witnesses[onThrow] = body.unknownCodeWitness;
        for (uint32_t parameter = 0; parameter < body.parameters.size(); ++parameter) {
            Effects memory = Effects::none();
            memory.reads = body.parameters[parameter].memory.reads;
            memory.writes = body.parameters[parameter].memory.writes;
            m_effects[m_numbering.indexOf(SummaryNode::parameterOf(id, parameter))] = memory;
        }

        Vector<uint8_t, 16> takesInUnknownCode;
        takesInUnknownCode.fill(0, numberOfOwnNodes);
        for (const SummaryDependency& dependency : body.dependencies) {
            RELEASE_ASSERT(dependency.target.function == id && dependency.target.part != SummaryPart::UnknownCode);
            if (dependency.source.part == SummaryPart::UnknownCode)
                takesInUnknownCode[m_numbering.indexOf(dependency.target) - onReturn] = 1;
        }
        auto comesThroughUnknownCode = [&](const SummaryDependency& dependency, unsigned ownNode) {
            return takesInUnknownCode[ownNode] && dependency.source.part != SummaryPart::UnknownCode && isEnteredByUnknownCode[dependency.source.function.index()];
        };

        Vector<uint32_t, 16> nextSourceOf;
        nextSourceOf.fill(0, numberOfOwnNodes);
        for (const SummaryDependency& dependency : body.dependencies) {
            unsigned ownNode = m_numbering.indexOf(dependency.target) - onReturn;
            if (!comesThroughUnknownCode(dependency, ownNode))
                ++nextSourceOf[ownNode];
        }
        uint32_t first = firstSourceIn[index];
        for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode) {
            uint32_t count = nextSourceOf[ownNode];
            m_firstSource[onReturn + ownNode] = first;
            nextSourceOf[ownNode] = first;
            first += count;
        }
        for (const SummaryDependency& dependency : body.dependencies) {
            unsigned target = m_numbering.indexOf(dependency.target);
            if (dependency.source.part == SummaryPart::UnknownCode)
                m_witnesses[target] = body.unknownCodeWitness;
            if (!comesThroughUnknownCode(dependency, target - onReturn))
                m_sources[nextSourceOf[target - onReturn]++] = m_numbering.indexOf(dependency.source);
        }
        for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode)
            m_endOfSources[onReturn + ownNode] = nextSourceOf[ownNode];

        if (isEnteredByUnknownCode[index]) {
            uint32_t firstEntered = static_cast<uint32_t>(numberOfDependencies) + firstNodeEntered[index];
            for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode)
                m_sources[firstEntered + ownNode] = onReturn + ownNode;
        }
    });

    m_effects[unknownCodeNode] = effectsOfUnknownCode();
    m_firstSource[unknownCodeNode] = static_cast<uint32_t>(numberOfDependencies);
    m_endOfSources[unknownCodeNode] = static_cast<uint32_t>(numberOfDependencies + numberOfNodesEntered);
}

std::span<const uint32_t> EffectSummarizer::sourcesOf(unsigned node) const
{
    return m_sources.subspan(m_firstSource[node], m_endOfSources[node] - m_firstSource[node]);
}

void EffectSummarizer::takeInSources(uint32_t node)
{
    uint32_t unknownCodeNode = m_numbering.numberOfNodes() - 1;
    Effects effects = m_effects[node];
    uint8_t takesInUnknownCode = m_takesInUnknownCode[node];
    for (uint32_t source : sourcesOf(node)) {
        if (source == unknownCodeNode) {
            takesInUnknownCode = 1;
            continue;
        }
        if (!m_isResolved[source])
            continue;
        effects.merge(m_effects[source]);
        takesInUnknownCode |= m_takesInUnknownCode[source];
    }
    m_effects[node] = effects;
    m_takesInUnknownCode[node] = takesInUnknownCode;
}

// In each round the nodes whose sources were all resolved in earlier rounds. What is left at the end lies in a cycle or takes one in,
// but for a few nodes that are not worth another round.
Vector<uint32_t> EffectSummarizer::resolveNodesBelowCycles()
{
    constexpr size_t fewestNodesWorthARound = 2048;
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t unknownCodeNode = m_numbering.numberOfNodes() - 1;

    Vector<uint32_t> unresolved(unknownCodeNode, [](size_t node) {
        return static_cast<uint32_t>(node);
    });
    Vector<uint32_t> resolvedInThisRound;
    for (;;) {
        resolvedInThisRound = collectInParallel<uint32_t>(loops, unresolved.size(), [&](unsigned position, Vector<uint32_t>& resolved) {
            uint32_t node = unresolved[position];
            bool isReady = std::ranges::all_of(sourcesOf(node), [&](uint32_t source) {
                return source == unknownCodeNode || m_isResolved[source];
            });
            if (!isReady)
                return;
            takeInSources(node);
            resolved.append(node);
        });
        forEachIndexInParallel(loops, resolvedInThisRound.size(), [&](unsigned position) {
            m_isResolved[resolvedInThisRound[position]] = 1;
        });
        unresolved = collectInParallel<uint32_t>(loops, unresolved.size(), [&](unsigned position, Vector<uint32_t>& rest) {
            if (!m_isResolved[unresolved[position]])
                rest.append(unresolved[position]);
        });
        if (resolvedInThisRound.size() < fewestNodesWorthARound)
            return unresolved;
    }
}

// A cycle of calls may overflow the stack.
void EffectSummarizer::resolveNodesInAndAboveCycles(std::span<const uint32_t> nodes)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t unknownCodeNode = m_numbering.numberOfNodes() - 1;
    unsigned count = static_cast<unsigned>(nodes.size());
    if (!count)
        return;

    Vector<uint32_t> positionOf(m_numbering.numberOfNodes());
    Vector<uint32_t> firstSource(count + 1);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        uint32_t node = nodes[position];
        positionOf[node] = position;
        takeInSources(node);
        firstSource[position] = std::ranges::count_if(sourcesOf(node), [&](uint32_t source) {
            return source != unknownCodeNode && !m_isResolved[source];
        });
    });
    uint32_t numberOfSources = 0;
    for (unsigned position = 0; position < count; ++position)
        numberOfSources += std::exchange(firstSource[position], numberOfSources);
    firstSource[count] = numberOfSources;
    Vector<uint32_t> sources(numberOfSources);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        uint32_t next = firstSource[position];
        for (uint32_t source : sourcesOf(nodes[position])) {
            if (source != unknownCodeNode && !m_isResolved[source])
                sources[next++] = positionOf[source];
        }
    });
    auto sourcesAt = [&](unsigned position) {
        return sources.subspan(firstSource[position], firstSource[position + 1] - firstSource[position]);
    };

    // The search for components follows the sources, so it numbers the component of a source before the components that take it in.
    StronglyConnectedComponents components = computeStronglyConnectedComponents(count, sourcesAt);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        Effects united = Effects::none();
        uint8_t takesInUnknownCode = 0;
        std::span<const uint32_t> members = components.membersOf(component);
        for (uint32_t member : members) {
            united.merge(m_effects[nodes[member]]);
            takesInUnknownCode |= m_takesInUnknownCode[nodes[member]];
            for (uint32_t source : sourcesAt(member)) {
                if (components.componentOf[source] == component)
                    continue;
                united.merge(m_effects[nodes[source]]);
                takesInUnknownCode |= m_takesInUnknownCode[nodes[source]];
            }
        }
        if (components.isInCycle.quickGet(members[0]))
            united.mayThrow = true;
        for (uint32_t member : members) {
            m_effects[nodes[member]] = united;
            m_takesInUnknownCode[nodes[member]] = takesInUnknownCode;
        }
    }
}

void EffectSummarizer::addEffectsOfUnknownCode()
{
    constexpr size_t nodesInChunk = 4096;
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t unknownCodeNode = m_numbering.numberOfNodes() - 1;

    std::span<const uint32_t> sources = sourcesOf(unknownCodeNode);
    unsigned numberOfChunks = static_cast<unsigned>((sources.size() + nodesInChunk - 1) / nodesInChunk);
    Vector<Effects> unitedInChunk(FillWith { }, numberOfChunks, Effects::none());
    Vector<uint8_t> takesInUnknownCodeInChunk(FillWith { }, numberOfChunks, static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
        size_t first = chunk * nodesInChunk;
        for (uint32_t source : sources.subspan(first, std::min(nodesInChunk, sources.size() - first))) {
            unitedInChunk[chunk].merge(m_effects[source]);
            takesInUnknownCodeInChunk[chunk] |= m_takesInUnknownCode[source];
        }
    });
    Effects unknownCode = m_effects[unknownCodeNode];
    for (const Effects& part : unitedInChunk)
        unknownCode.merge(part);
    if (takesInUnknownCodeInChunk.contains(1))
        unknownCode.mayThrow = true;
    m_effects[unknownCodeNode] = unknownCode;

    forEachIndexInParallel(loops, (unknownCodeNode + nodesInChunk - 1) / nodesInChunk, [&](unsigned chunk) {
        uint32_t end = std::min<uint32_t>(unknownCodeNode, (chunk + 1) * nodesInChunk);
        for (uint32_t node = chunk * nodesInChunk; node < end; ++node) {
            if (m_takesInUnknownCode[node])
                m_effects[node].merge(unknownCode);
        }
    });
}

// Unknown code takes in most of a program and most of a program takes it in: with it nearly every node lies in one cycle, which only one thread can find.
// Without it the calls that name their callee are left: few cycles, and most nodes below all of them. So a node first only remembers that it
// takes in unknown code. What unknown code does is then what its sources do, and goes to the nodes that remember.
void EffectSummarizer::propagateEffects(PhaseTimes& times)
{
    unsigned numberOfNodes = m_numbering.numberOfNodes();
    Vector<Effects> effectsOfNodesAlone;
    if (m_build.options().verifiesAfterEveryPass)
        effectsOfNodesAlone = Vector<Effects>(m_effects.span());

    m_takesInUnknownCode.fill(0, numberOfNodes);
    m_isResolved.fill(0, numberOfNodes);
    Vector<uint32_t> nodesInAndAboveCycles = resolveNodesBelowCycles();
    times.didFinish("resolveNodesBelowCycles"_s);
    m_build.statistics().count(StatisticID::EffectSummaryNodes, numberOfNodes);
    m_build.statistics().count(StatisticID::EffectSummaryNodesInAndAboveCycles, nodesInAndAboveCycles.size());
    resolveNodesInAndAboveCycles(nodesInAndAboveCycles.span());
    times.didFinish("resolveNodesInAndAboveCycles"_s);
    addEffectsOfUnknownCode();
    times.didFinish("addEffectsOfUnknownCode"_s);

    if (m_build.options().verifiesAfterEveryPass)
        checkEffectsAgainstPlainSolve(effectsOfNodesAlone.span());
}

void EffectSummarizer::checkEffectsAgainstPlainSolve(std::span<const Effects> effectsOfNodesAlone) const
{
    Vector<Effects> effects(effectsOfNodesAlone);
    StronglyConnectedComponents components = computeStronglyConnectedComponents(m_numbering.numberOfNodes(), [&](unsigned node) {
        return sourcesOf(node);
    });
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        Effects united = Effects::none();
        std::span<const uint32_t> members = components.membersOf(component);
        for (uint32_t member : members) {
            united.merge(effects[member]);
            for (uint32_t source : sourcesOf(member)) {
                if (components.componentOf[source] != component)
                    united.merge(effects[source]);
            }
        }
        if (components.isInCycle.quickGet(members[0]))
            united.mayThrow = true;
        for (uint32_t member : members) {
            effects[member] = united;
            RELEASE_ASSERT(m_effects[member] == united);
        }
    }
}

// A node without a witness takes the witness of the source that a breadth-first walk from the nodes with one comes to first, where the walk starts
// at the nodes in ascending order and goes from a node to those that take it in in ascending order. Every source with a witness of a node that
// has none was reached in the latest round, so the ranks in that round order them.
void EffectSummarizer::propagateWitnesses()
{
    constexpr uint32_t notReached = UINT32_MAX;
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfNodes = m_numbering.numberOfNodes();

    Vector<uint32_t> rankOf(FillWith { }, numberOfNodes, notReached);
    Vector<uint32_t> reached = findIndicesInParallel(loops, numberOfNodes, [&](unsigned node) {
        return !!m_witnesses[node];
    });
    Vector<uint32_t> waiting = findIndicesInParallel(loops, numberOfNodes, [&](unsigned node) {
        return !m_witnesses[node];
    });

    while (!reached.isEmpty() && !waiting.isEmpty()) {
        forEachIndexInParallel(loops, reached.size(), [&](unsigned rank) {
            rankOf[reached[rank]] = rank;
        });
        Vector<uint64_t> found = collectInParallel<uint64_t>(loops, waiting.size(), [&](unsigned position, Vector<uint64_t>& foundInChunk) {
            uint32_t node = waiting[position];
            uint32_t first = notReached;
            for (uint32_t source : sourcesOf(node))
                first = std::min(first, rankOf[source]);
            if (first != notReached)
                foundInChunk.append(static_cast<uint64_t>(first) << 32 | node);
        });
        parallelSort(loops, found.mutableSpan(), std::less<uint64_t> { });

        Vector<uint32_t> next(found.size());
        forEachIndexInParallel(loops, found.size(), [&](unsigned rank) {
            uint32_t node = static_cast<uint32_t>(found[rank]);
            m_witnesses[node] = m_witnesses[reached[found[rank] >> 32]];
            next[rank] = node;
        });
        reached = WTF::move(next);
        waiting = collectInParallel<uint32_t>(loops, waiting.size(), [&](unsigned position, Vector<uint32_t>& stillWaiting) {
            if (!m_witnesses[waiting[position]])
                stillWaiting.append(waiting[position]);
        });
    }
}

EffectSummaries EffectSummarizer::assemble()
{
    ParallelArray<FunctionSummary> summaries(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        Vector<ParameterSummary> parameters = m_bodies[index].parameters;
        for (uint32_t parameter = 0; parameter < parameters.size(); ++parameter) {
            const Effects& memory = m_effects[m_numbering.indexOf(SummaryNode::parameterOf(id, parameter))];
            parameters[parameter].memory = OperandMemory { memory.reads.restrictedTo(perObjectHeaps), memory.writes.restrictedTo(perObjectHeaps) };
        }
        unsigned onReturn = m_numbering.indexOf(SummaryNode::onReturn(id));
        unsigned onThrow = m_numbering.indexOf(SummaryNode::onThrow(id));
        UnknownCodeWitness witness = m_witnesses[onReturn] ? m_witnesses[onReturn] : m_witnesses[onThrow];
        return createFunctionSummary(m_effects[onReturn], m_effects[onThrow], WTF::move(parameters), witness);
    });
    return EffectSummaries(WTF::move(summaries), m_effects[m_numbering.indexOf(SummaryNode::unknownCode())]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
