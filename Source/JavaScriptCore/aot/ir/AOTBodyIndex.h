/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct Function;

using LoopID = ID<struct LoopIDTag>;

// One NewArray of Code lists no more computed elements than this: its emitter keeps every one of them live at once.
inline constexpr unsigned maxComputedElementsOfArrayLiteral = 32;

enum class BodyUseKind : uint8_t {
    Operand,
    TerminatorOperand,
    EdgeArgument,
    // The value arrives narrowed at a parameter of the target: BodyTarget::proofs().
    EdgeProof,
    HandlerArgument,
    BlockSubject,
};

struct BodyUser {
    BodyUseKind kind { BodyUseKind::Operand };
    // Of BodyUseKind::Operand.
    ValueID value;
    // Of every other kind: the block whose terminator, edge, handler edge or head names the value.
    BlockID block;
};

// An operand of `user`, which stands in `block`, is the IteratorOpen.
struct BodyMentionOfIteration {
    ValueID open;
    ValueID user;
    BlockID block;
};

struct BodyLoop {
    BlockID head;
    // The member that stands last in the stream. Blocks between head and last need not be members.
    BlockID last;
    LoopID parent;
    // One for a loop in no other.
    uint32_t depth { 0 };
    // BodyIndex::membersOf().
    uint32_t firstMember { 0 };
    uint32_t numberOfMembers { 0 };
    // The loop and the loops inside it have the places from its own on, `numberOfLoopsWithin` of them, itself among them.
    uint32_t placeAmongLoops { 0 };
    uint32_t numberOfLoopsWithin { 1 };
};

struct BodyEdgeResult {
    Proof proof { Proof::None };
    // The value that the Branch tests. Null on an edge of a switch, where the terminator is the test.
    ValueID test;
    // The operand of the test that arrives narrowed, and its index by the schema.
    ValueID subject;
    uint32_t operandIndex { 0 };
    bool isOnTakenEdge { false };
    // On an edge of a switch: the index of the target. The last is `otherwise`.
    std::optional<uint32_t> caseIndex;
    // Of the target block.
    ValueID parameter;
};

struct BodyIncomingEdge {
    BlockID from;
    // Of `target` among the targets of the terminator of `from`.
    uint32_t targetIndex { 0 };
    BodyTarget target;
};

// What BodyLiveness::compute() finds of a body. It never changes once it is made.
struct BodyLivenessData {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(BodyLivenessData);

    // A set of values is a tree: words of 64 values, and above them `levels` levels of nodes with sixteen branches. It is named by the number of its root, a word where there is no level.
    // Two sets share each part that is the same in both, so a set costs what tells it from the sets it was made of. Number 0 is the empty set at every level.
    static constexpr uint32_t numberOfBranches = 16;
    using Branches = std::array<uint32_t, numberOfBranches>;
    // A node of the level spans two to this power values.
    static constexpr uint32_t shiftOf(uint32_t level) { return 6 + 4 * level; }

    bool holds(uint32_t set, ValueID value) const
    {
        for (uint32_t level = levels; level; --level)
            set = branches[set][(value.index() >> shiftOf(level - 1)) % numberOfBranches];
        return words[set] >> (value.index() % 64) & 1;
    }

    // functor(ValueID), ascending: the values of the set from `first` up to `end`, which is no more than the body has values. It goes into no part of the set that lies outside.
    template<typename Functor>
    void forEachIn(uint32_t set, uint64_t first, uint64_t end, const Functor& functor) const
    {
        if (first < end)
            forEachInNode(levels, set, 0, first, end, functor);
    }

    // The node reaches into the stretch from `first` to `end`.
    template<typename Functor>
    void forEachInNode(uint32_t level, uint32_t node, uint64_t base, uint64_t first, uint64_t end, const Functor& functor) const
    {
        if (!node)
            return;
        if (!level) {
            uint64_t word = words[node];
            if (first > base)
                word &= ~uint64_t(0) << (first - base);
            if (end < base + 64)
                word &= (uint64_t(1) << (end - base)) - 1;
            for (; word; word &= word - 1)
                functor(ValueID(static_cast<uint32_t>(base + std::countr_zero(word))));
            return;
        }
        uint32_t shift = shiftOf(level - 1);
        uint64_t branch = first > base ? (first - base) >> shift : 0;
        uint64_t endOfBranches = std::min<uint64_t>(numberOfBranches, (end - base + (uint64_t(1) << shift) - 1) >> shift);
        for (; branch < endOfBranches; ++branch)
            forEachInNode(level - 1, branches[node][branch], base + (branch << shift), first, end, functor);
    }

    // By value.
    Vector<uint32_t> lastUses;
    // By block: the set.
    Vector<uint32_t> liveAtHeads;
    Vector<Branches> branches;
    Vector<uint64_t> words;
    uint32_t levels { 0 };
};

// What working out one body leaves for the next to use again.
struct BodyLivenessScratch {
    struct Union {
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t result { 0 };
    };

    // Of the block at work: what it reads and does not define.
    Vector<ValueID> reads;
    // By block: the set without what the edges that lead back add.
    Vector<uint32_t> collected;
    // Some of the unions of two nodes that have been worked out: each takes the place that its two numbers hash to.
    Vector<Union> unions;
    // What is live at the head is read no earlier than at the position.
    struct Raise {
        uint32_t position { 0 };
        uint32_t head { 0 };
        friend auto operator<=>(const Raise&, const Raise&) = default;
    };
    Vector<Raise> raises;
};

// What the walk works out of a body as it is written. It is made when the walk first asks for the body, once for the build, and kept with the index: whoever asks later, on any thread, for a unit or for a copy in another unit, reads that.
struct KeptWithBodyIndex {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptWithBodyIndex);

    virtual ~KeptWithBodyIndex() = default;
};

// What is asked of a body again and again, computed once: DESIGN.md section 6. Written by BodyIndexTable::build(), never changed but for m_liveness, kept for the build.
// Edges to handlers count as edges of the flow graph everywhere in it.
class BodyIndex {
    WTF_MAKE_NONCOPYABLE(BodyIndex);
public:
    static constexpr uint32_t maxCountedUses = 31;
    static constexpr uint32_t maxCountedIncomingEdges = UINT16_MAX;

    const Body& body() const { return m_body; }
    const KeptWithBodyIndex* keptByWalk() const { return m_keptByWalk.load(std::memory_order_acquire); }
    // Two threads find the same. The first to arrive keeps its own.
    const KeptWithBodyIndex& keepForWalk(std::unique_ptr<KeptWithBodyIndex>&& made) const
    {
        const KeptWithBodyIndex* kept = nullptr;
        if (m_keptByWalk.compare_exchange_strong(kept, made.get(), std::memory_order_acq_rel, std::memory_order_acquire))
            return *made.release();
        return *kept;
    }
    uint32_t numberOfBlocks() const { return m_body.numberOfBlocks(); }
    uint32_t numberOfValues() const { return m_body.numberOfValues(); }

    BodyValue value(ValueID id) const
    {
        uint32_t place = m_placesOfValues[id.index()];
        return place & isParameterBit ? m_body.parameterAt(place & ~isParameterBit, id) : m_body.operationAt(place, id);
    }
    bool isParameter(ValueID id) const { return m_placesOfValues[id.index()] & isParameterBit; }
    Opcode opcodeOf(ValueID id) const { return value(id).opcode(); }
    // Of the operations of the body, in blocks that nothing reaches too. Whoever looks for a few opcodes passes by the bodies that hold none of them.
    const OpcodeSet& opcodes() const { return m_opcodes; }
    bool holdsAnyOf(const OpcodeSet& wanted) const { return m_opcodes.intersects(wanted); }
    // `x = null` reads x, which makes a parameter at each join in front of it. Of a parameter that edges hand values to, in the body of a coroutine:
    // nothing mentions it but ReleaseHints and edges that hand it to other such parameters, round a loop too. A value with maxCountedUses mentions counts as read.
    // Only in the body of a coroutine so far.
    bool isReadByReleaseHintsAlone(ValueID parameter) const
    {
        size_t word = parameter.index() / bitsPerWord;
        return word < m_parametersReadByReleaseHintsAlone.size() && (m_parametersReadByReleaseHintsAlone[word] >> (parameter.index() % bitsPerWord)) & 1;
    }
    bool holdsParametersReadByReleaseHintsAlone() const { return !m_parametersReadByReleaseHintsAlone.empty(); }
    // By bisection over the blocks.
    BlockID blockOf(ValueID id) const
    {
        auto next = std::ranges::upper_bound(m_blocks, id.index(), { }, &BlockPlace::firstParameter);
        return BlockID(static_cast<uint32_t>(next - m_blocks.begin()) - 1);
    }

    // Every mention counts: f(x, x) uses x twice. It stops counting at maxCountedUses.
    uint32_t useCount(ValueID id) const { return m_usesOfValues[id.index()] & maxCountedUses; }
    bool isUnused(ValueID id) const { return !useCount(id); }
    bool hasOneUse(ValueID id) const { return useCount(id) == 1; }
    std::optional<BodyUser> onlyUser(ValueID id) const
    {
        if (!hasOneUse(id))
            return std::nullopt;
        auto kind = static_cast<BodyUseKind>(m_usesOfValues[id.index()] >> bitsOfUseCount);
        uint32_t user = m_onlyUsersOfValues[id.index()];
        if (kind == BodyUseKind::Operand)
            return BodyUser { kind, ValueID(user), { } };
        return BodyUser { kind, { }, BlockID(user) };
    }
    // The value whose operand is the only mention of `id`. Null otherwise.
    ValueID onlyUsingValue(ValueID id) const
    {
        std::optional<BodyUser> user = onlyUser(id);
        return user ? user->value : ValueID();
    }
    // The first mention of the value in the stream, however many follow. Nothing where nothing mentions it.
    std::optional<BodyUser> firstUser(ValueID id) const
    {
        if (isUnused(id))
            return std::nullopt;
        auto kind = static_cast<BodyUseKind>(m_usesOfValues[id.index()] >> bitsOfUseCount);
        uint32_t user = m_onlyUsersOfValues[id.index()];
        if (kind == BodyUseKind::Operand)
            return BodyUser { kind, ValueID(user), { } };
        return BodyUser { kind, { }, BlockID(user) };
    }
    // Who names the IteratorOpen, in the order of the stream: once for each operand of an operation whose row takes the state of an iteration or keeps values over a suspension, and of a ReleaseHint.
    std::span<const BodyMentionOfIteration> mentionsOfIteration(ValueID open) const
    {
        auto found = std::ranges::equal_range(m_mentionsOfIterations, open.index(), { }, [](const BodyMentionOfIteration& mention) {
            return mention.open.index();
        });
        return m_mentionsOfIterations.subspan(found.begin() - m_mentionsOfIterations.begin(), found.size());
    }
    // The value that stands behind the operations that state types (opcodesThatStateTypes), each of which hands its operand on.
    JS_EXPORT_PRIVATE ValueID valueBehindCasts(ValueID) const;

    BodyBlock block(BlockID id) const
    {
        const BlockPlace& place = m_blocks[id.index()];
        return m_body.blockAt(place.wordOffset, place.wordOffsetOfTerminator, id, ValueID(place.firstParameter));
    }
    BodyTerminator terminator(BlockID id) const { return block(id).terminator(); }

    bool isReachable(BlockID id) const { return m_blocks[id.index()].flags & IsReachable; }
    // It stops counting at maxCountedIncomingEdges. Two edges from one block count as two.
    uint32_t numberOfIncomingEdges(BlockID id) const { return m_blocks[id.index()].numberOfIncomingEdges; }
    bool isEnteredByHandlerEdge(BlockID id) const { return m_blocks[id.index()].flags & IsEnteredByHandlerEdge; }
    using OnlyIncomingEdge = BodyIncomingEdge;
    // Nothing for the entry, for a block that nothing reaches, for a block with several incoming edges and for one entered by a handler edge.
    std::optional<BodyIncomingEdge> onlyIncomingEdge(BlockID id) const
    {
        const BlockPlace& place = m_blocks[id.index()];
        if (place.numberOfIncomingEdges != 1 || place.flags & IsEnteredByHandlerEdge || !place.immediateDominator)
            return std::nullopt;
        RELEASE_ASSERT(place.wordOffsetOfFirstIncomingTarget);
        return BodyIncomingEdge { place.immediateDominator, place.indexOfFirstIncomingTarget, BodyTarget(&m_body.words()[place.wordOffsetOfFirstIncomingTarget]) };
    }

    // Null for the entry and for a block that nothing reaches.
    BlockID immediateDominator(BlockID id) const { return m_blocks[id.index()].immediateDominator; }
    // The reachable blocks in preorder of the dominator tree, siblings in stream order: the order in which the walk visits them.
    std::span<const BlockID> blocksInDominatorOrder() const { return m_blocksInDominatorOrder; }
    // A block's position in that order. What it dominates has the numbers from its own to lastDominatorNumberBelow().
    uint32_t dominatorNumber(BlockID id) const { return m_blocks[id.index()].dominatorNumber; }
    uint32_t lastDominatorNumberBelow(BlockID id) const { return m_blocks[id.index()].lastDominatorNumberBelow; }
    bool dominates(BlockID from, BlockID to) const
    {
        if (!isReachable(from) || !isReachable(to))
            return false;
        uint32_t number = dominatorNumber(to);
        return dominatorNumber(from) <= number && number <= lastDominatorNumberBelow(from);
    }
    bool strictlyDominates(BlockID from, BlockID to) const { return from != to && dominates(from, to); }
    uint32_t numberOfLoops() const { return static_cast<uint32_t>(m_loops.size()); }
    bool hasLoops() const { return !m_loops.empty(); }
    // In the order of their heads in the stream, so a loop's parent has a smaller id.
    const BodyLoop& loop(LoopID id) const { return m_loops[id.index()]; }
    // In the order of the stream, the head first, those of the loops inside it among them.
    // The stretch of the stream from the head to the last member may be as long as the body: a row of loops whose bodies the stream has behind all that follows them.
    std::span<const BlockID> membersOf(LoopID id) const { return m_membersOfLoops.subspan(m_loops[id.index()].firstMember, m_loops[id.index()].numberOfMembers); }
    // The innermost loop that holds the block. Null outside loops.
    LoopID loopOf(BlockID id) const { return m_blocks[id.index()].loop; }
    bool isLoopHead(BlockID id) const { return m_blocks[id.index()].flags & IsLoopHead; }
    bool isInLoop(BlockID block, LoopID loop) const
    {
        LoopID holder = loopOf(block);
        return holder && m_loops[holder.index()].placeAmongLoops - m_loops[loop.index()].placeAmongLoops < m_loops[loop.index()].numberOfLoopsWithin;
    }
    uint32_t loopDepthOf(BlockID id) const { return loopOf(id) ? m_loops[loopOf(id).index()].depth : 0; }
    // Some cycle has two ways in. Then there is one loop, its head is the entry, and every block is a member: DESIGN.md section 10, Loops.
    bool hasIrreducibleFlow() const { return m_hasIrreducibleFlow; }

    // What proof `proofIndex` of target `targetIndex` of the terminator of `from` hands to the target. Nothing where the words name no operand of a test that proves something.
    JS_EXPORT_PRIVATE std::optional<BodyEdgeResult> edgeResult(BlockID from, uint32_t targetIndex, uint32_t proofIndex) const;
    // The same, for a caller that already holds the terminator and the target. The overload above steps over every target in front of `targetIndex`, so asking it once per case of a switch is quadratic.
    JS_EXPORT_PRIVATE std::optional<BodyEdgeResult> edgeResult(const BodyTerminator&, uint32_t targetIndex, BodyTarget, uint32_t proofIndex) const;
    std::optional<BodyEdgeResult> edgeResult(BlockID from, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex) const { return edgeResult(terminator(from), targetIndex, target, proofIndex); }
    // functor(const BodyEdgeResult&), for each proof on each edge out of the terminator of `from`.
    template<typename Functor>
    void forEachEdgeResult(BlockID from, const Functor& functor) const
    {
        BodyTerminator terminator = this->terminator(from);
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
            for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
                if (std::optional<BodyEdgeResult> result = edgeResult(terminator, targetIndex, target, proofIndex))
                    functor(*result);
            }
            ++targetIndex;
        }
    }
    // Of a parameter that a test fills (BodyTarget::proofs()), in a block that one edge enters: the value that was tested, which the parameter is. Null for every other value.
    ValueID valueNarrowedInto(ValueID parameter) const
    {
        if (!isParameter(parameter))
            return { };
        std::optional<BodyIncomingEdge> edge = onlyIncomingEdge(blockOf(parameter));
        if (!edge)
            return { };
        uint32_t numberOfProofs = static_cast<uint32_t>(edge->target.proofs().size());
        for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
            std::optional<BodyEdgeResult> result = edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
            if (result && result->parameter == parameter)
                return result->subject;
        }
        return { };
    }

private:
    friend class BodyIndexBuilder;
    friend class BodyIndexTable;
    friend class BodyLiveness;

    static constexpr uint32_t isParameterBit = 1u << 31;
    static constexpr uint32_t bitsOfUseCount = 5;
    static_assert(maxCountedUses == (1u << bitsOfUseCount) - 1);
    static constexpr uint32_t bitsPerWord = 32;

    enum BlockFlag : uint16_t {
        IsReachable = 1 << 0,
        IsLoopHead = 1 << 1,
        IsEnteredByHandlerEdge = 1 << 2,
    };

    struct BlockPlace {
        uint32_t wordOffset { 0 };
        uint32_t wordOffsetOfTerminator { 0 };
        uint32_t firstParameter { 0 };
        BlockID immediateDominator;
        LoopID loop;
        uint32_t dominatorNumber { 0 };
        uint32_t lastDominatorNumberBelow { 0 };
        // Of the first edge in the stream that names the block.
        uint32_t indexOfFirstIncomingTarget { 0 };
        uint32_t wordOffsetOfFirstIncomingTarget { 0 };
        uint16_t numberOfIncomingEdges { 0 };
        uint16_t flags { 0 };
    };
    static_assert(sizeof(BlockPlace) == 40);

    BodyIndex() = default;

    Body m_body;
    std::span<const BlockPlace> m_blocks;
    std::span<const BlockID> m_blocksInDominatorOrder;
    std::span<const BodyLoop> m_loops;
    std::span<const BlockID> m_membersOfLoops;
    // Bit 31: a block parameter. The rest: the offset of its Wire::ValueHead or Wire::ParameterHead in the body's words.
    std::span<const uint32_t> m_placesOfValues;
    std::span<const uint32_t> m_onlyUsersOfValues;
    // The lower five bits count. The upper three are the BodyUseKind of the first mention.
    std::span<const uint8_t> m_usesOfValues;
    // One bit for each value. Empty where none is set.
    std::span<const uint32_t> m_parametersReadByReleaseHintsAlone;
    // By open, and those of one open in the order of the stream.
    std::span<const BodyMentionOfIteration> m_mentionsOfIterations;
    OpcodeSet m_opcodes;
    bool m_hasIrreducibleFlow { false };
    // Null until somebody asks: BodyLiveness::compute(). BodyIndexTable frees it.
    mutable std::atomic<const BodyLivenessData*> m_liveness { nullptr };
    // Null until the walk asks. BodyIndexTable frees it.
    mutable std::atomic<const KeptWithBodyIndex*> m_keptByWalk { nullptr };
};

struct ConstructionStartInModule {
    ModuleID module;
    ConstructionStart start;
};

// The indexes of a program's bodies, by function.
class BodyIndexTable {
    WTF_MAKE_NONCOPYABLE(BodyIndexTable);
public:
    JS_EXPORT_PRIVATE BodyIndexTable(uint32_t numberOfFunctions, unsigned numberOfWorkers);
    JS_EXPORT_PRIVATE ~BodyIndexTable();

    bool has(FunctionID id) const { return m_indexes[id.index()]; }
    const BodyIndex& at(FunctionID id) const
    {
        ASSERT(has(id));
        return *m_indexes[id.index()];
    }
    const BodyIndex* find(FunctionID id) const { return m_indexes[id.index()]; }

    // Once for each function that has a body, by whichever thread scans it: the first step of the Facts scan, while the words are in the cache.
    // `worker` is the number a parallel loop gives the thread. No two threads at work have the same.
    // It stops the build where the blocks do not end at the body's last word, the values are not as many as the record says, or a block is reached only over edges that do not lead forward in the stream.
    JS_EXPORT_PRIVATE const BodyIndex& build(FunctionID, const Function&, unsigned worker);
    // On one thread, for functions without a body that the program has got behind the last build().
    void addFunctionsWithoutBodies(uint32_t numberOfFunctions)
    {
        while (m_indexes.size() < numberOfFunctions)
            m_indexes.append(nullptr);
    }

    // Once every body is indexed, for Program::setConstructionStarts(). In no order.
    JS_EXPORT_PRIVATE Vector<ConstructionStartInModule> takeConstructionStarts();
    // One for each value each time a loop of build() over the words of a body has come to it. While no thread builds.
    JS_EXPORT_PRIVATE uint64_t valuesGoneOver() const;

    JS_EXPORT_PRIVATE size_t sizeInBytes() const;

private:
    friend class BodyIndexBuilder;

    struct Worker;

    Vector<const BodyIndex*> m_indexes;
    Vector<std::unique_ptr<Worker>> m_workers;
};

// Which values of a body a later operation still reads. It speaks of one body at a time.
// What compute() finds is worked out when the walk or the scan first asks for the body, once for the build, and kept with the index of the body: whoever asks later, on any thread, reads that.
//
// A position is a value id: `before p` is the point in front of the operation p. BodyBlock::endOfValues() is the position of the terminator, the edges out of the block and the edge to its handler.
// The arguments of the handler edge count as read at every position of the block.
class BodyLiveness {
    WTF_MAKE_NONCOPYABLE(BodyLiveness);
public:
    BodyLiveness() = default;

    // A ReleaseHint reads nothing, and an edge reads nothing that it brings to a parameter of which BodyIndex::isReadByReleaseHintsAlone() holds.
    JS_EXPORT_PRIVATE void compute(const BodyIndex&);
    bool isComputedFor(const BodyIndex& index) const { return m_index == &index; }
    void forget() { m_index = nullptr; }

    // Exact, without the block's own parameters.
    bool isLiveAtHead(ValueID value, BlockID block) const { return m_data->holds(m_data->liveAtHeads[block.index()], value); }
    // functor(ValueID), ascending: what is live at the head among the values from `first` up to `end`. In a large body as many values may be live at a head as the body has blocks,
    // so whoever asks at many heads names the values he asks about: the work goes by them and not by all that is live.
    template<typename Functor>
    void forEachLiveAtHead(BlockID block, ValueID first, ValueID end, const Functor& functor) const
    {
        m_data->forEachIn(m_data->liveAtHeads[block.index()], first.index(), std::min<uint32_t>(end.index(), m_index->numberOfValues()), functor);
    }
    template<typename Functor>
    void forEachLiveAtHead(BlockID block, const Functor& functor) const { forEachLiveAtHead(block, ValueID(0u), m_index->block(block).firstParameter(), functor); }
    // Both read what the index of one body keeps.
    bool answersFromSameDataAs(const BodyLiveness& other) const { return m_data == other.m_data; }

    // The last position in the stream at which the value is read, raised to the end of the last member of every loop at whose head the value is live, wherever its readers stand.
    // In irreducible flow: raised to the end of every block with an edge that leads back to a block at whose head the value is live.
    // The value's own id where nothing reads it.
    ValueID lastUse(ValueID value) const { return ValueID(m_data->lastUses[value.index()]); }

    // `point` lies in `block`. Never no for a value that is read later. It may say yes for a value whose remaining readers all stand in blocks that `block` does not reach,
    // and such a value is still available at the point.
    bool isLiveBefore(ValueID value, ValueID point, BlockID block) const
    {
        if (value.index() >= point.index() || m_data->lastUses[value.index()] < point.index())
            return false;
        return m_index->block(block).holds(value) || isLiveAtHead(value, block);
    }

    // functor(ValueID), ascending: the values for which isLiveBefore() says yes. It looks at what is live at the head and at every value of the block in front of the point.
    template<typename Functor>
    void forEachLiveBefore(ValueID point, BlockID block, const Functor& functor) const
    {
        const Vector<uint32_t>& lastUses = m_data->lastUses;
        forEachLiveAtHead(block, [&](ValueID value) {
            if (lastUses[value.index()] >= point.index())
                functor(value);
        });
        for (uint32_t value = m_index->block(block).firstParameter().index(); value < point.index(); ++value) {
            if (lastUses[value] >= point.index())
                functor(ValueID(value));
        }
    }

    // The same for the values that are defined in front of the operation `point` and read behind it: what a suspension has to keep.
    template<typename Functor>
    void forEachLiveAcross(ValueID point, BlockID block, const Functor& functor) const
    {
        forEachLiveBefore(point, block, [&](ValueID value) {
            if (m_data->lastUses[value.index()] > point.index())
                functor(value);
        });
    }

private:
    const BodyIndex* m_index { nullptr };
    // What the index of the body keeps, or m_ownData.
    const BodyLivenessData* m_data { nullptr };
    // Where a body is worked out, and what computeAsSuspensionsHold() found.
    BodyLivenessData m_ownData;
    BodyLivenessScratch m_scratch;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
