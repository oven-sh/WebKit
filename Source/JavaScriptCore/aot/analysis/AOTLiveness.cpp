/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLiveness.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include <algorithm>
#include <bit>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

struct NarrowedOperandUse {
    ValueID subject;
    BlockID block;

    friend auto operator<=>(const NarrowedOperandUse&, const NarrowedOperandUse&) = default;
};

static constexpr OpcodeSet opcodesWithBase = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (baseOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

static bool hasProofs(const Terminator& terminator)
{
    for (const Target& target : terminator.targets) {
        if (!target.proofs.isEmpty())
            return true;
    }
    return false;
}

static void forEachNarrowedOperand(const Function& function, BlockID block, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    if (!hasProofs(function.at(block).terminator))
        return;
    forEachEdgeResult(function, block, [&](const EdgeResult& result, ValueID) {
        if (result.kind == EdgeResultKind::NarrowedOperand)
            functor(result.subject);
    });
}

static Vector<NarrowedOperandUse> collectNarrowedOperandUses(const Function& function, const CFG& cfg)
{
    Vector<NarrowedOperandUse> uses;
    for (BlockID block : cfg.reversePostOrder()) {
        forEachNarrowedOperand(function, block, [&](ValueID subject) {
            uses.append(NarrowedOperandUse { subject, block });
        });
    }
    std::ranges::sort(uses);
    removeRepeatedElements(uses);
    return uses;
}

Liveness::Liveness(const Function& function, const CFG& cfg, const UseLists& useLists, const LivenessOptions& options)
    : m_function(function)
    , m_cfg(cfg)
    , m_useLists(useLists)
{
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
    std::ranges::sort(m_additionalEdges);

    Vector<InteriorPointer> interiorPointers;
    if (!options.opcodesOfValues || options.opcodesOfValues->intersects(opcodesWithBase))
        interiorPointers = collectInteriorPointers();
    m_hasTrackedBases = !interiorPointers.isEmpty();
    computeLiveAtHeads(interiorPointers.span(), options.solver);
}

bool Liveness::isTrackedBlock(BlockID block) const
{
    return m_cfg.isReachable(block) && (!m_trackedBlocks || m_trackedBlocks->get(block.index()));
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

Vector<Liveness::InteriorPointer> Liveness::collectInteriorPointers() const
{
    Vector<InteriorPointer> interiorPointers;
    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID pointer : m_function.at(block).values) {
            forEachBase(pointer, [&](ValueID base) {
                if (isTrackedValue(base))
                    interiorPointers.append(InteriorPointer { base, pointer });
                return IterationStatus::Continue;
            });
        }
    }
    std::ranges::sort(interiorPointers);
    return interiorPointers;
}

template<typename Functor>
void Liveness::forEachValueWithUseBlocks(std::span<const InteriorPointer> interiorPointers, NOESCAPE const Functor& functor) const
{
    Vector<NarrowedOperandUse> narrowedOperandUses = collectNarrowedOperandUses(m_function, m_cfg);
    Vector<BlockID, 16> useBlocks;
    auto appendUseBlocks = [&](ValueID used) {
        for (auto& use : m_useLists.usesOf(used)) {
            if (useBlocks.isEmpty() || useBlocks.last() != use.block)
                useBlocks.append(use.block);
        }
        if (narrowedOperandUses.isEmpty())
            return;
        for (auto& use : std::ranges::equal_range(narrowedOperandUses, used, { }, &NarrowedOperandUse::subject))
            useBlocks.append(use.block);
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
        functor(value, location.block, useBlocks);
    }
}

Liveness::CrossBlockValues Liveness::collectCrossBlockValues(std::span<const InteriorPointer> interiorPointers) const
{
    CrossBlockValues result;
    forEachValueWithUseBlocks(interiorPointers, [&](ValueID value, BlockID definingBlock, const Vector<BlockID, 16>& useBlocks) {
        uint32_t firstUseBlockIndex = result.useBlocks.size();
        for (BlockID block : useBlocks) {
            if (block != definingBlock)
                result.useBlocks.append(block);
        }
        if (result.useBlocks.size() != firstUseBlockIndex)
            result.values.append({ value, definingBlock, firstUseBlockIndex });
    });
    return result;
}

void Liveness::computeLiveAtHeads(std::span<const InteriorPointer> interiorPointers, LivenessOptions::Solver solver)
{
    static constexpr size_t maxWordsForPerBlockSolver = 1 << 18;

    unsigned numberOfBlocks = m_cfg.numNodes();
    m_firstLiveValue.fill(0, numberOfBlocks + 1);
    CrossBlockValues crossBlockValues = collectCrossBlockValues(interiorPointers);
    if (crossBlockValues.values.isEmpty())
        return;

    AdditionalPredecessors additionalPredecessors;
    if (!m_additionalEdges.isEmpty()) {
        additionalPredecessors.grow(numberOfBlocks);
        for (auto [from, to] : m_additionalEdges)
            additionalPredecessors[to.index()].append(from);
    }

    // The per-block solver visits every block, which does not pay for a few tracked values in a large function.
    size_t numberOfValues = crossBlockValues.values.size();
    size_t numberOfReachableBlocks = m_cfg.reversePostOrder().size();
    bool isDenseEnough = numberOfValues * 64 >= numberOfReachableBlocks;
    bool isSmallEnough = numberOfReachableBlocks * ((numberOfValues + 63) / 64) <= maxWordsForPerBlockSolver;
    if (solver == LivenessOptions::Solver::ChosenBySize && isDenseEnough && isSmallEnough)
        solvePerBlock(crossBlockValues, additionalPredecessors);
    else
        solvePerValue(crossBlockValues, additionalPredecessors);
}

void Liveness::solvePerValue(const CrossBlockValues& crossBlockValues, const AdditionalPredecessors& additionalPredecessors)
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
}

void Liveness::solvePerBlock(const CrossBlockValues& crossBlockValues, const AdditionalPredecessors& additionalPredecessors)
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    std::span<const CrossBlockValue> values = crossBlockValues.values.span();
    size_t wordsPerBlock = (values.size() + 63) / 64;

    Vector<uint64_t, 128> liveAtHeadStorage;
    liveAtHeadStorage.fill(0, order.size() * wordsPerBlock);
    std::span<uint64_t> liveAtHeads = liveAtHeadStorage.mutableSpan();
    auto liveAtHeadOf = [&](BlockID block) {
        return liveAtHeads.subspan(m_cfg.reversePostOrderIndex(block) * wordsPerBlock, wordsPerBlock);
    };

    Vector<uint32_t, 64> firstDefinitionStorage;
    firstDefinitionStorage.fill(0, order.size() + 1);
    std::span<uint32_t> firstDefinition = firstDefinitionStorage.mutableSpan();
    for (size_t index = 0; index < values.size(); ++index) {
        ++firstDefinition[m_cfg.reversePostOrderIndex(values[index].definingBlock) + 1];
        for (BlockID block : crossBlockValues.useBlocksOf(index))
            liveAtHeadOf(block)[index / 64] |= 1ull << (index % 64);
    }
    for (size_t orderIndex = 0; orderIndex < order.size(); ++orderIndex)
        firstDefinition[orderIndex + 1] += firstDefinition[orderIndex];
    Vector<uint32_t, 32> definitionStorage;
    definitionStorage.grow(values.size());
    std::span<uint32_t> definitions = definitionStorage.mutableSpan();
    Vector<uint32_t, 64> nextDefinition(firstDefinitionStorage.span());
    for (uint32_t index = 0; index < values.size(); ++index)
        definitions[nextDefinition[m_cfg.reversePostOrderIndex(values[index].definingBlock)]++] = index;

    Vector<uint8_t, 64> isStaleStorage;
    isStaleStorage.fill(true, order.size());
    std::span<uint8_t> isStale = isStaleStorage.mutableSpan();
    Vector<uint64_t, 16> liveThroughStorage;
    liveThroughStorage.grow(wordsPerBlock);
    std::span<uint64_t> liveThrough = liveThroughStorage.mutableSpan();
    bool needsAnotherSweep = true;
    while (needsAnotherSweep) {
        needsAnotherSweep = false;
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

std::span<const ValueID> Liveness::liveAtHead(BlockID block) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    uint32_t first = m_firstLiveValue[block.index()];
    return m_liveValues.subspan(first, m_firstLiveValue[block.index() + 1] - first);
}

bool Liveness::isLiveAtHead(ValueID id, BlockID block) const
{
    RELEASE_ASSERT(id.index() < m_useLists.numberOfValues());
    RELEASE_ASSERT(isTrackedValue(id) && isTrackedBlock(block));
    return std::ranges::binary_search(liveAtHead(block), id);
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
    if (!m_narrowedOperandUses) {
        m_narrowedOperandUses.emplace();
        for (const NarrowedOperandUse& use : collectNarrowedOperandUses(m_function, m_cfg))
            m_narrowedOperandUses->append({ use.subject, use.block });
    }

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
    uint32_t& set = m_setOfValue[id.index()];
    if (!set)
        set = findBlocksEnteredWith(id);
    return m_blocksEnteredWithValue[set - 1].quickGet(block.index());
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
    , m_live(liveness.m_useLists.numberOfValues())
{
    RELEASE_ASSERT(liveness.m_cfg.isReachable(block));
    m_index = liveness.m_function.at(block).values.size();

    liveness.m_function.at(block).forEachChildOutsideValues([&](ValueID child) {
        use(child);
    });
    forEachNarrowedOperand(liveness.m_function, block, [&](ValueID subject) {
        use(subject);
    });

    auto addLiveAtHead = [&](BlockID successor) {
        RELEASE_ASSERT(liveness.isTrackedBlock(successor));
        for (ValueID id : liveness.liveAtHead(successor))
            m_live.quickSet(id.index());
    };
    for (BlockID successor : liveness.m_cfg.successors(block))
        addLiveAtHead(successor);
    for (auto& edge : std::ranges::equal_range(liveness.m_additionalEdges, block, { }, &std::pair<BlockID, BlockID>::first))
        addLiveAtHead(edge.second);
}

void Liveness::LocalCalc::execute(uint32_t index)
{
    RELEASE_ASSERT(m_index && index == m_index - 1);
    m_index = index;
    ValueID id = m_liveness.m_function.at(m_block).values[index];
    m_live.clear(id.index());
    m_liveness.m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
        use(operand);
    });
}

void Liveness::LocalCalc::use(ValueID id)
{
    if (!id)
        return;
    if (m_liveness.isTrackedValue(id))
        m_live.set(id.index());
    if (!m_liveness.m_hasTrackedBases)
        return;
    m_liveness.forEachBase(id, [&](ValueID base) {
        if (!m_liveness.isTrackedValue(base))
            return IterationStatus::Continue;
        return m_live.set(base.index()) ? IterationStatus::Done : IterationStatus::Continue;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
