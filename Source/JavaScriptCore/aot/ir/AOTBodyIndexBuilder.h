/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRFunction.h"
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT { namespace Typed {

struct BodyFlowEdge {
    BlockID from;
    BlockID to;
};

struct BodyArgumentLink {
    ValueID argument;
    BlockID from;
    BlockID to;
    uint32_t indexOfParameter { 0 };
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
    Vector<BodyArgumentLink> argumentLinks;
    Vector<uint32_t> firstPredecessor;
    Vector<BlockID> predecessors;
    Vector<uint32_t> sizesOfSubtrees;
    Vector<uint32_t> nextNumbersBelow;
    Vector<BlockID> worklist;
    Vector<BodyLoop> loops;
    Vector<ValueID> releaseHints;
    Vector<BodyMentionOfIteration> mentionsOfIterations;
};

class BodyIndexBuilder {
    WTF_MAKE_NONCOPYABLE(BodyIndexBuilder);
public:
    BodyIndexBuilder(const Function&, BodyIndexTable::Worker&);

    const BodyIndex& run();

private:
    using BlockPlace = BodyIndex::BlockPlace;

    void allocate();
    void indexWords();
    void noteUse(ValueID used, BodyUseKind, uint32_t user);
    void findParametersReadByReleaseHintsAlone();
    void noteEdge(BlockID from, BodyTarget, uint32_t targetIndex, bool isHandlerEdge);
    void noteProofs(const BodyBlock& from, const BodyTerminator&);
    void adoptLoops();
    void adoptMentionsOfIterations();

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
    std::span<uint8_t> m_usesOfValues;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
