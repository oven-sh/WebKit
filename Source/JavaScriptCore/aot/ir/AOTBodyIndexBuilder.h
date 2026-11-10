/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRFunction.h"
#include "AOTOpcodeSets.h"
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT { namespace Typed {

struct BodyFlowEdge {
    BlockID from;
    BlockID to;
};

struct BodyIndexTable::Worker {
    static constexpr size_t bytesPerChunk = 1 * MB;

    std::span<uint8_t> allocate(size_t numberOfBytes);

    Vector<MallocSpan<uint8_t>> chunks;
    size_t usedBytesOfLastChunk { 0 };
    size_t allocatedBytes { 0 };
    Vector<ConstructionStartInModule> constructionStarts;
    uint64_t valuesGoneOver { 0 };

    Vector<BodyFlowEdge> edges;
    Vector<BodyFlowEdge> retreatingEdges;
    Vector<uint32_t> firstPredecessor;
    Vector<BlockID> predecessors;
    Vector<uint32_t> sizesOfSubtrees;
    Vector<uint32_t> nextNumbersBelow;
    Vector<BlockID> worklist;
    Vector<BodyLoop> loops;
    Vector<BodyMentionOfIteration> mentionsOfIterations;
    Vector<BodyIndex::BlockPlace> blocks;
};

inline constexpr OpcodeSet opcodesThatMayNameIterationsInBodyIndex {
    Opcode::IteratorResume, Opcode::IteratorClose, Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::StepAsyncGenerator, Opcode::AwaitBySavingFrame, Opcode::AwaitCallBySavingFrame,
    Opcode::YieldBySavingFrame, Opcode::AsyncGeneratorYield
};

class BodyIndexBuilder {
    WTF_MAKE_NONCOPYABLE(BodyIndexBuilder);
public:
    BodyIndexBuilder(const Function&, BodyIndexTable::Worker&);
    BodyIndexBuilder(const Function&, BodyIndexTable&, unsigned worker);

    const BodyIndex& run();

    void begin();
    void fileBlock(BlockID, uint32_t wordOffset, uint32_t firstParameter, uint32_t numberOfParameters, uint32_t numberOfValues);
    void fileOperation(ValueID, uint32_t wordOffset, Opcode);
    void fileUse(ValueID used, BodyUseKind, uint32_t user);
    void fileWhatFewOperationsBring(const BodyValue&, BlockID);
    void fileTerminator(BlockID, uint32_t wordOffset);
    void fileEdge(BlockID from, BodyTarget, uint32_t targetIndex, bool isHandlerEdge);
    void fileProofs(BlockID from, const BodyTerminator&);
    const BodyIndex& finish();

private:
    using BlockPlace = BodyIndex::BlockPlace;

    void indexWords();
    void adoptLoops();
    void adoptMentionsOfIterations();
    void adoptBlocks();
    BodyIndex::RareParts& ensureRareParts();

    void computeDominators();
    BlockID nearestCommonDominator(BlockID, BlockID) const;
    void computeDominatorsOfIrreducibleFlow();
    void numberDominatorTree();
    void findLoops();
    void collectPredecessors();
    void treatBodyAsOneLoop();

    const Function& m_function;
    BodyIndexTable::Worker& m_worker;
    BodyIndex* m_index { nullptr };
    std::span<BlockPlace> m_blocks;
    std::span<BlockID> m_blocksInDominatorOrder;
    std::span<uint32_t> m_placesOfValues;
    std::span<uint32_t> m_onlyUsersOfValues;
    std::span<uint16_t> m_narrowOnlyUsersOfValues;
    std::span<uint8_t> m_usesOfValues;
    OpcodeSet m_opcodes;
    BodyIndex::RareParts* m_rareParts { nullptr };
    bool m_hasNarrowNumbers { false };
};

ALWAYS_INLINE void BodyIndexBuilder::fileBlock(BlockID block, uint32_t wordOffset, uint32_t firstParameter, uint32_t numberOfParameters, uint32_t numberOfValues)
{
    static_assert(sizeof(Wire::ParameterHead) == sizeof(uint32_t));

    m_worker.valuesGoneOver += numberOfParameters + numberOfValues;
    BlockPlace& place = m_blocks[block.index()];
    place.wordOffset = wordOffset;
    place.firstParameter = firstParameter;
    uint32_t placeOfParameter = BodyIndex::isParameterBit | (wordOffset + BodyBlock::numberOfHeadWords);
    for (uint32_t parameter = firstParameter; parameter < firstParameter + numberOfParameters; ++parameter)
        m_placesOfValues[parameter] = placeOfParameter++;
}

ALWAYS_INLINE void BodyIndexBuilder::fileOperation(ValueID value, uint32_t wordOffset, Opcode opcode)
{
    m_placesOfValues[value.index()] = wordOffset;
    m_opcodes.add(opcode);
}

ALWAYS_INLINE void BodyIndexBuilder::fileTerminator(BlockID block, uint32_t wordOffset)
{
    m_blocks[block.index()].wordOffsetOfTerminator = wordOffset;
}

ALWAYS_INLINE void BodyIndexBuilder::fileUse(ValueID used, BodyUseKind kind, uint32_t user)
{
    static_assert(static_cast<unsigned>(BodyUseKind::BlockSubject) < (1u << (8 - BodyIndex::bitsOfUseCount)));

    uint8_t& uses = m_usesOfValues[used.index()];
    uint32_t count = uses & BodyIndex::maxCountedUses;
    if (!count) {
        uses = static_cast<uint8_t>(static_cast<unsigned>(kind) << BodyIndex::bitsOfUseCount | 1);
        if (m_hasNarrowNumbers) [[likely]]
            m_narrowOnlyUsersOfValues[used.index()] = static_cast<uint16_t>(user);
        else
            m_onlyUsersOfValues[used.index()] = user;
        return;
    }
    if (count < BodyIndex::maxCountedUses)
        ++uses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
