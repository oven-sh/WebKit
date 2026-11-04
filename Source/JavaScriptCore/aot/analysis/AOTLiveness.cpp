/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLiveness.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTHeldOperands.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <bit>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesWithBase = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (baseOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

static void forEachNarrowedOperand(const Function& function, BlockID block, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    bool hasProofs = function.at(block).terminator.targets.containsIf([](const Target& target) {
        return !target.proofs.isEmpty();
    });
    if (!hasProofs)
        return;
    forEachEdgeResult(function, block, [&](const EdgeResult& result, ValueID) {
        if (result.kind == EdgeResultKind::NarrowedOperand)
            functor(result.subject);
    });
}

static Vector<std::pair<ValueID, BlockID>> collectNarrowedOperandUses(const Function& function, const CFG& cfg)
{
    Vector<std::pair<ValueID, BlockID>> uses;
    for (BlockID block : cfg.reversePostOrder()) {
        forEachNarrowedOperand(function, block, [&](ValueID subject) {
            uses.append({ subject, block });
        });
    }
    AOT::sortInLine(uses);
    removeRepeatedElements(uses);
    return uses;
}

Liveness::Liveness(const Function& function, const CFG& cfg, const UseLists& useLists, const LivenessOptions& options)
    : m_function(function)
    , m_cfg(cfg)
    , m_useLists(useLists)
{
    AOT_THRESHOLD(size_t, maxWordsForPerBlockSolver, 1 << 18);
    AOT_THRESHOLD(size_t, maxVisitsOfPerValueSolver, 1 << 22);
    AOT_THRESHOLD(size_t, maxVisitsOfEachBlockByPerValueSolver, 8);
    RELEASE_ASSERT(m_function.blocks.size() == m_cfg.numNodes());
    RELEASE_ASSERT(m_function.values.size() == m_useLists.numberOfValues());
    if (options.trackedValues)
        m_trackedValues = *options.trackedValues;
    if (options.trackedBlocks)
        m_trackedBlocks = *options.trackedBlocks;
    for (auto& edge : options.additionalEdges) {
        if (m_cfg.isReachable(edge.first) && m_cfg.isReachable(edge.second))
            m_additionalEdges.append(edge);
    }
    AOT::sort(m_additionalEdges);

    Vector<InteriorPointer> interiorPointers;
    if (!options.opcodesOfValues || options.opcodesOfValues->intersects(opcodesWithBase)) {
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID pointer : m_function.at(block).values) {
                forEachBase(pointer, [&](ValueID base) {
                    if (isTrackedValue(base))
                        interiorPointers.append(InteriorPointer { base, pointer });
                    return IterationStatus::Continue;
                });
            }
        }
        AOT::sortInLine(interiorPointers);
    }
    m_hasTrackedBases = !interiorPointers.isEmpty();

    unsigned numberOfBlocks = m_cfg.numNodes();
    m_firstLiveValue.fill(0, numberOfBlocks + 1);
    CrossBlockValues crossBlockValues = collectCrossBlockValues(interiorPointers.span());
    if (crossBlockValues.values.isEmpty())
        return;

    AdditionalPredecessors additionalPredecessors;
    if (!m_additionalEdges.isEmpty()) {
        additionalPredecessors.grow(numberOfBlocks);
        for (auto [from, to] : m_additionalEdges)
            additionalPredecessors[to.index()].append(from);
    }

    size_t numberOfValues = crossBlockValues.values.size();
    size_t numberOfReachableBlocks = m_cfg.reversePostOrder().size();
    bool isDenseEnough = numberOfValues * 64 >= numberOfReachableBlocks;
    bool isSmallEnough = numberOfReachableBlocks * ((numberOfValues + 63) / 64) <= maxWordsForPerBlockSolver;
    size_t maxVisits = maxVisitsOfPerValueSolver;
    if (options.solver == LivenessOptions::Solver::ChosenBySize)
        maxVisits = std::min(maxVisits, maxVisitsOfEachBlockByPerValueSolver * numberOfReachableBlocks);
    if (options.solver == LivenessOptions::Solver::ChosenBySize && isDenseEnough && isSmallEnough)
        solvePerBlock(crossBlockValues, additionalPredecessors);
    else if (options.solver == LivenessOptions::Solver::SharedSets || !solvePerValue(crossBlockValues, additionalPredecessors, maxVisits))
        solveWithSharedSets(crossBlockValues, additionalPredecessors);
}

ValueID Liveness::baseOf(ValueID id) const
{
    const Value* producer = &m_function.at(id);
    if (producer->opcode == Opcode::ResultAt)
        producer = &m_function.at(producer->operand(OperandIndex::ResultAt::results));
    std::optional<uint8_t> operandIndex = Typed::baseOf(producer->opcode);
    if (!operandIndex)
        return { };
    return producer->operand(*operandIndex);
}

template<typename Functor>
void Liveness::forEachBase(ValueID id, NOESCAPE const Functor& functor) const
{
    unsigned remainingSteps = m_useLists.numberOfValues();
    for (ValueID base = baseOf(id); base && remainingSteps; base = baseOf(base), --remainingSteps) {
        if (functor(base) == IterationStatus::Done)
            return;
    }
}

Liveness::CrossBlockValues Liveness::collectCrossBlockValues(std::span<const InteriorPointer> interiorPointers) const
{
    CrossBlockValues result;
    Vector<std::pair<ValueID, BlockID>> narrowedOperandUses = collectNarrowedOperandUses(m_function, m_cfg);
    Vector<BlockID, 16> useBlocks;
    auto appendUseBlocks = [&](ValueID used) {
        for (auto& use : m_useLists.usesOf(used)) {
            if (useBlocks.isEmpty() || useBlocks.last() != use.block)
                useBlocks.append(use.block);
        }
        if (narrowedOperandUses.isEmpty())
            return;
        for (auto& use : std::ranges::equal_range(narrowedOperandUses, used, { }, &std::pair<ValueID, BlockID>::first))
            useBlocks.append(use.second);
    };
    unsigned numberOfValues = m_useLists.numberOfValues();
    for (unsigned index = 0; index < numberOfValues; ++index) {
        ValueID value(index);
        size_t numberOfPointers = 0;
        while (numberOfPointers < interiorPointers.size() && interiorPointers[numberOfPointers].base == value)
            ++numberOfPointers;
        auto pointers = interiorPointers.first(numberOfPointers);
        interiorPointers = interiorPointers.subspan(numberOfPointers);

        ValueLocation location = m_useLists.locationOf(value);
        if (!isTrackedValue(value) || location.placement == ValuePlacement::Detached)
            continue;
        useBlocks.shrink(0);
        appendUseBlocks(value);
        for (auto& interiorPointer : pointers)
            appendUseBlocks(interiorPointer.pointer);

        uint32_t firstUseBlockIndex = result.useBlocks.size();
        for (BlockID block : useBlocks) {
            if (block != location.block)
                result.useBlocks.append(block);
        }
        if (result.useBlocks.size() != firstUseBlockIndex)
            result.values.append({ value, location.block, firstUseBlockIndex });
    }
    return result;
}

bool Liveness::solvePerValue(const CrossBlockValues& crossBlockValues, const AdditionalPredecessors& additionalPredecessors, size_t remainingVisits)
{
    struct LiveValue {
        BlockID block;
        ValueID value;
    };

    unsigned numberOfBlocks = m_cfg.numNodes();
    Vector<LiveValue> liveValues;
    Vector<ValueID> lastValueLiveInto(numberOfBlocks);
    Vector<BlockID, 16> worklist;
    for (size_t index = 0; index < crossBlockValues.values.size(); ++index) {
        ValueID value = crossBlockValues.values[index].value;
        BlockID definingBlock = crossBlockValues.values[index].definingBlock;
        worklist.append(crossBlockValues.useBlocksOf(index));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == definingBlock || lastValueLiveInto[block.index()] == value)
                continue;
            lastValueLiveInto[block.index()] = value;
            if (!remainingVisits--) {
                m_firstLiveValue.fill(0, numberOfBlocks + 1);
                return false;
            }
            if (isTrackedBlock(block)) {
                liveValues.append({ block, value });
                ++m_firstLiveValue[block.index() + 1];
            }
            worklist.append(m_cfg.predecessors(block));
            if (!additionalPredecessors.isEmpty())
                worklist.appendVector(additionalPredecessors[block.index()]);
        }
    }

    for (unsigned index = 0; index < numberOfBlocks; ++index)
        m_firstLiveValue[index + 1] += m_firstLiveValue[index];
    m_liveValues.grow(liveValues.size());
    Vector<uint32_t> next(m_firstLiveValue.span());
    for (const LiveValue& live : liveValues)
        m_liveValues[next[live.block.index()]++] = live.value;
    return true;
}

void Liveness::solvePerBlock(const CrossBlockValues& crossBlockValues, const AdditionalPredecessors& additionalPredecessors)
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    std::span<const CrossBlockValue> values = crossBlockValues.values.span();
    size_t wordsPerBlock = (values.size() + 63) / 64;

    Vector<uint64_t, 128> liveAtHeadStorage(FillWith { }, order.size() * wordsPerBlock, 0ull);
    std::span<uint64_t> liveAtHeads = liveAtHeadStorage.mutableSpan();
    auto liveAtHeadOf = [&](BlockID block) {
        return liveAtHeads.subspan(m_cfg.reversePostOrderIndex(block) * wordsPerBlock, wordsPerBlock);
    };

    Vector<uint32_t, 64> firstDefinitionStorage(FillWith { }, order.size() + 1, 0u);
    std::span<uint32_t> firstDefinition = firstDefinitionStorage.mutableSpan();
    for (size_t index = 0; index < values.size(); ++index) {
        ++firstDefinition[m_cfg.reversePostOrderIndex(values[index].definingBlock) + 1];
        for (BlockID block : crossBlockValues.useBlocksOf(index))
            liveAtHeadOf(block)[index / 64] |= 1ull << (index % 64);
    }
    for (size_t orderIndex = 0; orderIndex < order.size(); ++orderIndex)
        firstDefinition[orderIndex + 1] += firstDefinition[orderIndex];
    Vector<uint32_t, 32> definitionStorage(values.size());
    std::span<uint32_t> definitions = definitionStorage.mutableSpan();
    Vector<uint32_t, 64> nextDefinition(firstDefinitionStorage.span());
    for (uint32_t index = 0; index < values.size(); ++index)
        definitions[nextDefinition[m_cfg.reversePostOrderIndex(values[index].definingBlock)]++] = index;

    Vector<uint8_t, 64> isStaleStorage(FillWith { }, order.size(), static_cast<uint8_t>(true));
    std::span<uint8_t> isStale = isStaleStorage.mutableSpan();
    Vector<uint64_t, 16> liveThroughStorage(wordsPerBlock);
    std::span<uint64_t> liveThrough = liveThroughStorage.mutableSpan();
    for (bool needsAnotherSweep = true; std::exchange(needsAnotherSweep, false);) {
        for (size_t orderIndex = order.size(); orderIndex--;) {
            if (!isStale[orderIndex])
                continue;
            isStale[orderIndex] = false;

            BlockID block = order[orderIndex];
            std::ranges::fill(liveThrough, 0);
            auto addSuccessor = [&](BlockID successor) {
                std::span<const uint64_t> liveAtSuccessor = liveAtHeadOf(successor);
                for (size_t word = 0; word < wordsPerBlock; ++word)
                    liveThrough[word] |= liveAtSuccessor[word];
            };
            for (BlockID successor : m_cfg.successors(block))
                addSuccessor(successor);
            if (!m_additionalEdges.isEmpty()) {
                for (auto& edge : std::ranges::equal_range(m_additionalEdges, block, { }, &std::pair<BlockID, BlockID>::first))
                    addSuccessor(edge.second);
            }
            for (uint32_t definition : definitions.subspan(firstDefinition[orderIndex], firstDefinition[orderIndex + 1] - firstDefinition[orderIndex]))
                liveThrough[definition / 64] &= ~(1ull << (definition % 64));

            std::span<uint64_t> live = liveAtHeads.subspan(orderIndex * wordsPerBlock, wordsPerBlock);
            bool didChange = false;
            for (size_t word = 0; word < wordsPerBlock; ++word) {
                uint64_t merged = live[word] | liveThrough[word];
                didChange |= merged != live[word];
                live[word] = merged;
            }
            if (!didChange)
                continue;

            auto markStale = [&](BlockID predecessor) {
                size_t predecessorIndex = m_cfg.reversePostOrderIndex(predecessor);
                isStale[predecessorIndex] = true;
                needsAnotherSweep |= predecessorIndex >= orderIndex;
            };
            for (BlockID predecessor : m_cfg.predecessors(block))
                markStale(predecessor);
            if (!additionalPredecessors.isEmpty()) {
                for (BlockID predecessor : additionalPredecessors[block.index()])
                    markStale(predecessor);
            }
        }
    }

    unsigned numberOfBlocks = m_cfg.numNodes();
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        uint32_t numberOfLiveValues = 0;
        if (isTrackedBlock(BlockID(blockIndex))) {
            for (uint64_t word : liveAtHeadOf(BlockID(blockIndex)))
                numberOfLiveValues += std::popcount(word);
        }
        m_firstLiveValue[blockIndex + 1] = m_firstLiveValue[blockIndex] + numberOfLiveValues;
    }
    m_liveValues.grow(m_firstLiveValue[numberOfBlocks]);
    std::span<ValueID> liveValues = m_liveValues.mutableSpan();
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        size_t next = m_firstLiveValue[blockIndex];
        if (next == m_firstLiveValue[blockIndex + 1])
            continue;
        std::span<const uint64_t> live = liveAtHeadOf(BlockID(blockIndex));
        for (size_t word = 0; word < wordsPerBlock; ++word) {
            for (uint64_t bits = live[word]; bits; bits &= bits - 1)
                liveValues[next++] = values[word * 64 + std::countr_zero(bits)].value;
        }
    }
}

void Liveness::SharedSets::setNumberOfKeys(size_t numberOfKeys)
{
    m_nodes.shrink(0);
    m_nodes.append(Node { });
    m_numberOfInnerLevels = 0;
    while (numberOfKeys > 1ull << (bitsPerLeafKey + bitsPerSlotIndex * m_numberOfInnerLevels))
        ++m_numberOfInnerLevels;
}

auto Liveness::SharedSets::append(const Node& node) -> Set
{
    m_nodes.append(node);
    return m_nodes.size() - 1;
}

auto Liveness::SharedSets::add(Set set, uint32_t key, unsigned level) -> Set
{
    Node node = m_nodes[set];
    uint32_t& slot = node[level ? slotOf(key, level) : wordOf(key)];
    uint32_t changed = level ? add(slot, key, level - 1) : slot | maskOf(key);
    if (changed == slot)
        return set;
    slot = changed;
    return append(node);
}

auto Liveness::SharedSets::remove(Set set, uint32_t key, unsigned level) -> Set
{
    if (!set)
        return emptySet;
    Node node = m_nodes[set];
    uint32_t& slot = node[level ? slotOf(key, level) : wordOf(key)];
    uint32_t changed = level ? remove(slot, key, level - 1) : slot & ~maskOf(key);
    if (changed == slot)
        return set;
    slot = changed;
    return node == Node { } ? emptySet : append(node);
}

auto Liveness::SharedSets::unite(Set first, Set second, unsigned level) -> Set
{
    if (first == second || !second)
        return first;
    if (!first)
        return second;

    Node firstNode = m_nodes[first];
    Node secondNode = m_nodes[second];
    Node united;
    for (unsigned slot = 0; slot < slotsPerNode; ++slot)
        united[slot] = level ? unite(firstNode[slot], secondNode[slot], level - 1) : firstNode[slot] | secondNode[slot];
    if (united == firstNode)
        return first;
    if (united == secondNode)
        return second;
    return append(united);
}

bool Liveness::SharedSets::contains(Set set, uint32_t key) const
{
    for (unsigned level = m_numberOfInnerLevels; level && set; --level)
        set = m_nodes[set][slotOf(key, level)];
    return m_nodes[set][wordOf(key)] & maskOf(key);
}

void Liveness::solveWithSharedSets(const CrossBlockValues& crossBlockValues, const AdditionalPredecessors& additionalPredecessors)
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    std::span<const CrossBlockValue> values = crossBlockValues.values.span();
    unsigned numberOfBlocks = m_cfg.numNodes();

    m_sharedSets.setNumberOfKeys(values.size());
    m_sharedSetAtHead.fill(SharedSets::emptySet, numberOfBlocks);
    m_valuesInSharedSets.reserveInitialCapacity(values.size());
    Vector<uint32_t> firstDefinition(FillWith { }, numberOfBlocks + 1, 0u);
    for (uint32_t index = 0; index < values.size(); ++index) {
        m_valuesInSharedSets.append(values[index].value);
        ++firstDefinition[values[index].definingBlock.index() + 1];
        for (BlockID block : crossBlockValues.useBlocksOf(index)) {
            if (m_cfg.isReachable(block))
                m_sharedSetAtHead[block.index()] = m_sharedSets.add(m_sharedSetAtHead[block.index()], index);
        }
    }
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex)
        firstDefinition[blockIndex + 1] += firstDefinition[blockIndex];
    Vector<uint32_t> definitions(values.size());
    Vector<uint32_t> nextDefinition(firstDefinition.span());
    for (uint32_t index = 0; index < values.size(); ++index)
        definitions[nextDefinition[values[index].definingBlock.index()]++] = index;

    Vector<uint8_t> isStale(FillWith { }, order.size(), static_cast<uint8_t>(true));
    for (bool needsAnotherSweep = true; std::exchange(needsAnotherSweep, false);) {
        for (size_t orderIndex = order.size(); orderIndex--;) {
            if (!std::exchange(isStale[orderIndex], false))
                continue;

            BlockID block = order[orderIndex];
            SharedSets::Set liveThrough = SharedSets::emptySet;
            for (BlockID successor : m_cfg.successors(block))
                liveThrough = m_sharedSets.unite(liveThrough, m_sharedSetAtHead[successor.index()]);
            if (!m_additionalEdges.isEmpty()) {
                for (auto& edge : std::ranges::equal_range(m_additionalEdges, block, { }, &std::pair<BlockID, BlockID>::first))
                    liveThrough = m_sharedSets.unite(liveThrough, m_sharedSetAtHead[edge.second.index()]);
            }
            for (uint32_t definition : definitions.subspan(firstDefinition[block.index()], firstDefinition[block.index() + 1] - firstDefinition[block.index()]))
                liveThrough = m_sharedSets.remove(liveThrough, definition);

            SharedSets::Set& live = m_sharedSetAtHead[block.index()];
            SharedSets::Set united = m_sharedSets.unite(live, liveThrough);
            if (united == live)
                continue;
            live = united;

            auto markStale = [&](BlockID predecessor) {
                size_t predecessorIndex = m_cfg.reversePostOrderIndex(predecessor);
                isStale[predecessorIndex] = true;
                needsAnotherSweep |= predecessorIndex >= orderIndex;
            };
            for (BlockID predecessor : m_cfg.predecessors(block))
                markStale(predecessor);
            if (!additionalPredecessors.isEmpty()) {
                for (BlockID predecessor : additionalPredecessors[block.index()])
                    markStale(predecessor);
            }
        }
    }

    if (!m_trackedBlocks)
        return;
    for (BlockID block : order) {
        if (!isTrackedBlock(block))
            m_sharedSetAtHead[block.index()] = SharedSets::emptySet;
    }
}

void Liveness::forEachLiveAtHead(BlockID block, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    if (!m_sharedSetAtHead.isEmpty()) [[unlikely]] {
        m_sharedSets.forEach(m_sharedSetAtHead[block.index()], [&](uint32_t index) {
            functor(m_valuesInSharedSets[index]);
        });
        return;
    }
    uint32_t first = m_firstLiveValue[block.index()];
    for (ValueID value : m_liveValues.subspan(first, m_firstLiveValue[block.index() + 1] - first))
        functor(value);
}

Liveness::LiveValues Liveness::liveAtHead(BlockID block) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    if (!m_sharedSetAtHead.isEmpty()) [[unlikely]] {
        Vector<ValueID> values;
        forEachLiveAtHead(block, [&](ValueID value) {
            values.append(value);
        });
        return LiveValues(WTF::move(values));
    }
    uint32_t first = m_firstLiveValue[block.index()];
    return LiveValues(m_liveValues.subspan(first, m_firstLiveValue[block.index() + 1] - first));
}

void Liveness::forEachLiveAtHeadOfAny(std::span<const BlockID> blocks, NOESCAPE const ScopedLambda<void(ValueID)>& functor) const
{
    if (m_sharedSetAtHead.isEmpty()) [[likely]] {
        for (BlockID block : blocks)
            forEachLiveAtHead(block, functor);
        return;
    }
    Vector<uint8_t> isVisited(FillWith { }, m_sharedSets.numberOfSets(), static_cast<uint8_t>(0));
    for (BlockID block : blocks) {
        RELEASE_ASSERT(block.index() < m_cfg.numNodes());
        m_sharedSets.forEachInPartsNotVisited(m_sharedSetAtHead[block.index()], isVisited, [&](uint32_t index) {
            functor(m_valuesInSharedSets[index]);
        });
    }
}

bool Liveness::isAnyLiveAtHead(BlockID block) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    if (!m_sharedSetAtHead.isEmpty()) [[unlikely]]
        return m_sharedSetAtHead[block.index()] != SharedSets::emptySet;
    return m_firstLiveValue[block.index()] != m_firstLiveValue[block.index() + 1];
}

bool Liveness::isAnyLiveAtHead(BlockID block, const BitVector& values, AnswersAboutValues& answers) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    if (!m_sharedSetAtHead.isEmpty()) [[unlikely]] {
        if (std::exchange(answers.m_liveness, this) != this)
            answers.m_answersOfSharedSets.fill(0, m_sharedSets.numberOfSets());
        return m_sharedSets.containsAny(m_sharedSetAtHead[block.index()], answers.m_answersOfSharedSets, [&](uint32_t index) {
            return values.get(m_valuesInSharedSets[index].index());
        });
    }
    uint32_t first = m_firstLiveValue[block.index()];
    return std::ranges::any_of(m_liveValues.subspan(first, m_firstLiveValue[block.index() + 1] - first), [&](ValueID value) {
        return values.get(value.index());
    });
}

bool Liveness::isLiveAtHead(ValueID id, BlockID block) const
{
    RELEASE_ASSERT(id.index() < m_useLists.numberOfValues());
    RELEASE_ASSERT(isTrackedValue(id) && isTrackedBlock(block));
    if (!m_sharedSetAtHead.isEmpty()) [[unlikely]] {
        size_t index = std::ranges::lower_bound(m_valuesInSharedSets, id) - m_valuesInSharedSets.begin();
        return index < m_valuesInSharedSets.size() && m_valuesInSharedSets[index] == id && m_sharedSets.contains(m_sharedSetAtHead[block.index()], index);
    }
    uint32_t first = m_firstLiveValue[block.index()];
    return std::ranges::binary_search(m_liveValues.subspan(first, m_firstLiveValue[block.index() + 1] - first), id);
}

void Liveness::forEachLastCrossing(NOESCAPE const ScopedLambda<bool(ValueID)>& isPoint, NOESCAPE const ScopedLambda<LastCrossing>& functor) const
{
    std::optional<ProgramPoint> onlyPoint;
    unsigned numberOfPoints = 0;
    for (BlockID block : m_cfg.reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size() && isTrackedBlock(block); ++index) {
            if (isPoint(values[index]) && !numberOfPoints++)
                onlyPoint = ProgramPoint { block, index };
        }
    }
    if (!numberOfPoints)
        return;
    if (numberOfPoints == 1) {
        LocalCalc localCalc(*this, onlyPoint->block);
        for (uint32_t index = m_function.at(onlyPoint->block).values.size(); index > onlyPoint->index + 1;)
            localCalc.execute(--index);
        ValueID point = m_function.at(onlyPoint->block).values[onlyPoint->index];
        Vector<ValueID> live;
        localCalc.forEachLive([&](ValueID id) {
            if (id != point)
                live.append(id);
        });
        functor(point, live.span(), [&](ValueID id) {
            return localCalc.isLive(id);
        });
        return;
    }

    // `ends` holds what is live where the walk stands and is live after no point from there on.
    Vector<Vector<ValueID>> endsAtHead(m_cfg.numNodes());
    BitVector ends(m_useLists.numberOfValues());
    BitVector isUsedLater(m_useLists.numberOfValues());
    Vector<ValueID> mayEnd;
    Vector<ValueID> usedLater;
    Vector<BlockID, 4> successors;

    // The targets of the edges that leave the natural loop of the back edge, as far as the walk has been there.
    Vector<BlockID, 4> exits;
    BitVector isInLoop(m_cfg.numNodes());
    Vector<BlockID> loop;
    auto findExitsBehind = [&](BlockID latch, BlockID header) {
        exits.shrink(0);
        loop.shrink(0);
        auto reach = [&](BlockID block) {
            if (!isInLoop.set(block.index()))
                loop.append(block);
        };
        reach(header);
        reach(latch);
        bool isNatural = true;
        for (size_t index = 1; index < loop.size(); ++index) {
            for (BlockID predecessor : m_cfg.predecessors(loop[index])) {
                isNatural &= m_cfg.reversePostOrderIndex(predecessor) >= m_cfg.reversePostOrderIndex(header);
                reach(predecessor);
            }
        }
        for (BlockID block : loop) {
            for (BlockID target : m_cfg.successors(block)) {
                if (isNatural && !isInLoop.get(target.index()) && m_cfg.reversePostOrderIndex(target) > m_cfg.reversePostOrderIndex(latch) && !exits.contains(target))
                    exits.append(target);
            }
        }
        for (BlockID block : loop)
            isInLoop.clear(block.index());
    };

    auto takeEnds = [&] {
        Vector<ValueID> result;
        for (ValueID id : mayEnd) {
            if (ends.clear(id.index()))
                result.append(id);
        }
        mayEnd.shrink(0);
        AOT::sort(result);
        return result;
    };

    for (BlockID block : m_cfg.reversePostOrder() | std::views::reverse) {
        if (!isTrackedBlock(block))
            continue;
        successors.shrink(0);
        successors.append(m_cfg.successors(block));
        for (auto& edge : std::ranges::equal_range(m_additionalEdges, block, { }, &std::pair<BlockID, BlockID>::first))
            successors.append(edge.second);
        auto isAhead = [&](BlockID successor) {
            return m_cfg.reversePostOrderIndex(successor) > m_cfg.reversePostOrderIndex(block);
        };
        auto isLiveAtTail = [&](ValueID id) {
            return std::ranges::any_of(successors, [&](BlockID successor) {
                return isLiveAtHead(id, successor);
            });
        };
        auto add = [&](ValueID id) {
            if (!ends.set(id.index()))
                mayEnd.append(id);
        };
        auto isLiveAfterLaterPoint = [&](ValueID id, BlockID from) {
            return isLiveAtHead(id, from) && !std::ranges::binary_search(endsAtHead[from.index()], id);
        };
        for (BlockID successor : successors) {
            auto addUnlessLiveAfterLaterPoint = [&](ValueID id) {
                for (BlockID other : successors) {
                    if (other != successor && isAhead(other) && isLiveAfterLaterPoint(id, other))
                        return;
                }
                add(id);
            };
            if (isAhead(successor)) {
                for (ValueID id : endsAtHead[successor.index()])
                    addUnlessLiveAfterLaterPoint(id);
                continue;
            }
            // What is live at the head of a loop is defined outside the loop, so it is live all the way to each exit at whose target it is live.
            findExitsBehind(block, successor);
            forEachLiveAtHead(successor, [&](ValueID id) {
                for (BlockID exit : exits) {
                    if (isLiveAfterLaterPoint(id, exit))
                        return;
                }
                addUnlessLiveAfterLaterPoint(id);
            });
        }

        auto useOne = [&](ValueID id) {
            if (isUsedLater.set(id.index()))
                return;
            usedLater.append(id);
            if (!ends.get(id.index()) && !isLiveAtTail(id))
                add(id);
        };
        auto use = [&](ValueID id) {
            if (!id)
                return;
            if (isTrackedValue(id))
                useOne(id);
            if (!m_hasTrackedBases)
                return;
            forEachBase(id, [&](ValueID base) {
                if (isTrackedValue(base))
                    useOne(base);
                return IterationStatus::Continue;
            });
        };
        auto isLiveAfterPoint = [&](ValueID id) {
            return isUsedLater.get(id.index()) || isLiveAtTail(id);
        };

        const Block& body = m_function.at(block);
        body.forEachChildOutsideValues(use);
        forEachNarrowedOperand(m_function, block, use);
        for (uint32_t index = body.values.size(); index--;) {
            ValueID id = body.values[index];
            ends.clear(id.index());
            isUsedLater.clear(id.index());
            bool holds = isPoint(id);
            if (holds)
                functor(id, takeEnds().span(), isLiveAfterPoint);
            const Value& value = m_function.at(id);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                // What the point holds it holds while the function is suspended there, so that does not end before the point.
                if (holds && isHeldOperand(value, operandIndex)) {
                    if (!isUsedLater.set(operand.index()))
                        usedLater.append(operand);
                    return;
                }
                use(operand);
            });
        }
        for (ValueID parameter : body.parameters)
            ends.clear(parameter.index());
        endsAtHead[block.index()] = takeEnds();
        for (ValueID id : usedLater)
            isUsedLater.clear(id.index());
        usedLater.shrink(0);
    }
}

LivenessOfAskedValues::LivenessOfAskedValues(const Function& function, const CFG& cfg, const UseLists& useLists)
    : m_function(function)
    , m_cfg(cfg)
    , m_useLists(useLists)
{
    RELEASE_ASSERT(m_function.blocks.size() == m_cfg.numNodes());
    RELEASE_ASSERT(m_function.values.size() == m_useLists.numberOfValues());
    m_setOfValue.fill(0, m_useLists.numberOfValues());
}

bool LivenessOfAskedValues::answersFor(const OpcodeSet& opcodesOfValues)
{
    return !opcodesOfValues.intersects(opcodesWithBase);
}

uint32_t LivenessOfAskedValues::findBlocksEnteredWith(ValueID id)
{
    if (!m_narrowedOperandUses)
        m_narrowedOperandUses = collectNarrowedOperandUses(m_function, m_cfg);

    BitVector entered(m_cfg.numNodes());
    ValueLocation location = m_useLists.locationOf(id);
    if (location.placement != ValuePlacement::Detached) {
        Vector<BlockID, 16> worklist;
        for (const Use& use : m_useLists.usesOf(id))
            worklist.append(use.block);
        for (auto& use : std::ranges::equal_range(*m_narrowedOperandUses, id, { }, &std::pair<ValueID, BlockID>::first))
            worklist.append(use.second);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == location.block || entered.quickSet(block.index()))
                continue;
            worklist.append(m_cfg.predecessors(block));
        }
    }
    m_blocksEnteredWithValue.append(WTF::move(entered));
    return m_blocksEnteredWithValue.size();
}

bool LivenessOfAskedValues::isLiveAtHead(ValueID id, BlockID block)
{
    RELEASE_ASSERT(m_function.values.size() == m_setOfValue.size() && m_function.blocks.size() == m_cfg.numNodes());
    RELEASE_ASSERT(id.index() < m_setOfValue.size() && m_cfg.isReachable(block));
    return blocksEnteredWith(id).quickGet(block.index());
}

const BitVector& LivenessOfAskedValues::blocksEnteredWith(ValueID id)
{
    uint32_t& set = m_setOfValue[id.index()];
    if (!set)
        set = findBlocksEnteredWith(id);
    return m_blocksEnteredWithValue[set - 1];
}

Liveness::LocalCalc::LocalCalc(const Liveness& liveness, BlockID block)
    : m_liveness(liveness)
    , m_block(block)
{
    RELEASE_ASSERT(liveness.m_cfg.isReachable(block));
    {
        Locker locker { liveness.m_spareWordsLock };
        if (!liveness.m_spareWords.isEmpty())
            m_words = liveness.m_spareWords.takeLast();
    }
    if (m_words.isEmpty())
        m_words.fill(0, (liveness.m_useLists.numberOfValues() + 63) / 64);
    m_index = liveness.m_function.at(block).values.size();

    liveness.m_function.at(block).forEachChildOutsideValues([&](ValueID child) {
        use(child);
    });
    forEachNarrowedOperand(liveness.m_function, block, [&](ValueID subject) {
        use(subject);
    });

    auto addLiveAtHead = [&](BlockID successor) {
        RELEASE_ASSERT(liveness.isTrackedBlock(successor));
        liveness.forEachLiveAtHead(successor, [&](ValueID id) {
            add(id);
        });
    };
    for (BlockID successor : liveness.m_cfg.successors(block))
        addLiveAtHead(successor);
    for (auto& edge : std::ranges::equal_range(liveness.m_additionalEdges, block, { }, &std::pair<BlockID, BlockID>::first))
        addLiveAtHead(edge.second);
}

Liveness::LocalCalc::~LocalCalc()
{
    for (uint32_t word : m_wordsInUse)
        m_words[word] = 0;
    Locker locker { m_liveness.m_spareWordsLock };
    m_liveness.m_spareWords.append(WTF::move(m_words));
}

bool Liveness::LocalCalc::add(ValueID id)
{
    uint64_t& word = m_words[id.index() / 64];
    uint64_t bit = 1ull << (id.index() % 64);
    if (word & bit)
        return true;
    // The word may be listed: its last bit may have been cleared.
    if (!word)
        m_wordsInUse.append(id.index() / 64);
    word |= bit;
    return false;
}

std::span<const uint32_t> Liveness::LocalCalc::wordsInUse() const
{
    if (m_numberOfSortedWords != m_wordsInUse.size()) {
        AOT::sort(m_wordsInUse);
        removeRepeatedElements(m_wordsInUse);
        m_numberOfSortedWords = m_wordsInUse.size();
    }
    return m_wordsInUse.span();
}

void Liveness::LocalCalc::execute(uint32_t index)
{
    RELEASE_ASSERT(m_index && index == m_index - 1);
    m_index = index;
    ValueID id = m_liveness.m_function.at(m_block).values[index];
    m_words[id.index() / 64] &= ~(1ull << (id.index() % 64));
    m_liveness.m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
        use(operand);
    });
}

void Liveness::LocalCalc::use(ValueID id)
{
    if (!id)
        return;
    if (m_liveness.isTrackedValue(id))
        add(id);
    if (!m_liveness.m_hasTrackedBases)
        return;
    m_liveness.forEachBase(id, [&](ValueID base) {
        if (!m_liveness.isTrackedValue(base))
            return IterationStatus::Continue;
        return add(base) ? IterationStatus::Done : IterationStatus::Continue;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
