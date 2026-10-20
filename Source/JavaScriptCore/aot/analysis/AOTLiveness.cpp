/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLiveness.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include <algorithm>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

struct NarrowedOperandUse {
    ValueID subject;
    BlockID block;

    friend auto operator<=>(const NarrowedOperandUse&, const NarrowedOperandUse&) = default;
};

static void forEachNarrowedOperand(const Function& function, BlockID block, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
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
    , m_liveAtHead(cfg.numNodes())
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

    auto interiorPointers = collectInteriorPointers();
    m_hasTrackedBases = !interiorPointers.isEmpty();
    computeLiveAtHeads(interiorPointers.span());
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

void Liveness::computeLiveAtHeads(std::span<const InteriorPointer> interiorPointers)
{
    unsigned numberOfBlocks = m_cfg.numNodes();
    unsigned numberOfValues = m_useLists.numberOfValues();
    Vector<Vector<BlockID, 1>> additionalPredecessors;
    if (!m_additionalEdges.isEmpty()) {
        additionalPredecessors.grow(numberOfBlocks);
        for (auto [from, to] : m_additionalEdges)
            additionalPredecessors[to.index()].append(from);
    }

    Vector<NarrowedOperandUse> narrowedOperandUses = collectNarrowedOperandUses(m_function, m_cfg);
    Vector<ValueID> lastValueLiveInto(numberOfBlocks);
    Vector<BlockID, 16> worklist;
    auto appendUseBlocks = [&](ValueID used) {
        for (auto& use : m_useLists.usesOf(used)) {
            if (worklist.isEmpty() || worklist.last() != use.block)
                worklist.append(use.block);
        }
        for (auto& use : std::ranges::equal_range(narrowedOperandUses, used, { }, &NarrowedOperandUse::subject))
            worklist.append(use.block);
    };
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
        appendUseBlocks(value);
        for (auto& interiorPointer : pointers)
            appendUseBlocks(interiorPointer.pointer);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == location.block || lastValueLiveInto[block.index()] == value)
                continue;
            lastValueLiveInto[block.index()] = value;
            if (isTrackedBlock(block))
                m_liveAtHead[block.index()].append(value);
            worklist.append(m_cfg.predecessors(block));
            if (!additionalPredecessors.isEmpty())
                worklist.appendVector(additionalPredecessors[block.index()]);
        }
    }
}

std::span<const ValueID> Liveness::liveAtHead(BlockID block) const
{
    RELEASE_ASSERT(block.index() < m_cfg.numNodes());
    return m_liveAtHead[block.index()].span();
}

bool Liveness::isLiveAtHead(ValueID id, BlockID block) const
{
    RELEASE_ASSERT(id.index() < m_useLists.numberOfValues());
    RELEASE_ASSERT(isTrackedValue(id) && isTrackedBlock(block));
    return std::ranges::binary_search(liveAtHead(block), id);
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
