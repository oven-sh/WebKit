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

// Shared by AOTBodyIndex.cpp and AOTBodyIndexFlow.cpp. Nobody else includes this.

struct BodyFlowEdge {
    BlockID from;
    BlockID to;
};

// An edge hands `argument` to the parameter of `to` with that index.
struct BodyArgumentLink {
    ValueID argument;
    BlockID from;
    BlockID to;
    uint32_t indexOfParameter { 0 };
};

// What one thread of the scan owns: the memory of the indexes it built, which lives as long as the table, and scratch that the next function uses again.
struct BodyIndexTable::Worker {
    static constexpr size_t bytesPerChunk = 1 * MB;

    // Aligned to eight bytes and zeroed. A request larger than a chunk gets a chunk of its own.
    std::span<uint8_t> allocate(size_t numberOfBytes);

    Vector<MallocSpan<uint8_t>> chunks;
    size_t usedBytesOfLastChunk { 0 };
    size_t allocatedBytes { 0 };
    Vector<ConstructionStartInModule> constructionStarts;
    // BodyIndexTable::valuesGoneOver().
    uint64_t valuesGoneOver { 0 };

    // In stream order: the targets of a block's terminator, then its handler.
    Vector<BodyFlowEdge> edges;
    // edges whose target does not stand behind their source.
    Vector<BodyFlowEdge> retreatingEdges;
    // In stream order. Of the body of a coroutine alone.
    Vector<BodyArgumentLink> argumentLinks;
    Vector<uint32_t> firstPredecessor;
    Vector<BlockID> predecessors;
    Vector<uint32_t> sizesOfSubtrees;
    Vector<uint32_t> nextNumbersBelow;
    Vector<BlockID> worklist;
    Vector<BodyLoop> loops;
    Vector<ValueID> releaseHints;
    // In stream order.
    Vector<BodyMentionOfIteration> mentionsOfIterations;
};

class BodyIndexBuilder {
    WTF_MAKE_NONCOPYABLE(BodyIndexBuilder);
public:
    BodyIndexBuilder(const Function&, BodyIndexTable::Worker&);

    // indexWords, computeDominators, numberDominatorTree, findLoops, and the loops copied behind the rest.
    const BodyIndex& run();

private:
    using BlockPlace = BodyIndex::BlockPlace;

    // AOTBodyIndex.cpp
    void allocate();
    // One pass over the words: the place of each value and block, uses, edges, incoming edges, the set of opcodes, release hints, construction starts.
    void indexWords();
    void noteUse(ValueID used, BodyUseKind, uint32_t user);
    // For BodyIndex::isReadByReleaseHintsAlone(). Behind indexWords(): it goes over the counts of uses, the release hints and the links of arguments, and only in the body of a coroutine that holds a ReleaseHint.
    void findParametersReadByReleaseHintsAlone();
    void noteEdge(BlockID from, BodyTarget, uint32_t targetIndex, bool isHandlerEdge);
    void noteProofs(const BodyBlock& from, const BodyTerminator&);
    void adoptLoops();
    void adoptMentionsOfIterations();

    // AOTBodyIndexFlow.cpp
    // One pass in stream order over Worker::edges. A second way only where a retreating edge's target does not dominate its source.
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
