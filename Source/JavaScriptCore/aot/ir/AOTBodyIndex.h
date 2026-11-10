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

inline constexpr unsigned maxComputedElementsOfArrayLiteral = 32;

enum class BodyUseKind : uint8_t {
    Operand,
    TerminatorOperand,
    EdgeArgument,
    EdgeProof,
    HandlerArgument,
    BlockSubject,
};

struct BodyUser {
    BodyUseKind kind { BodyUseKind::Operand };
    ValueID value;
    BlockID block;
};

struct BodyMentionOfIteration {
    ValueID open;
    ValueID user;
    BlockID block;
};

struct BodyLoop {
    BlockID head;
    BlockID last;
    LoopID parent;
    uint32_t depth { 0 };
    uint32_t firstMember { 0 };
    uint32_t numberOfMembers { 0 };
    uint32_t placeAmongLoops { 0 };
    uint32_t numberOfLoopsWithin { 1 };
    LoopID outermost;
};

struct BodyEdgeResult {
    Proof proof { Proof::None };
    ValueID test;
    ValueID subject;
    uint32_t operandIndex { 0 };
    bool isOnTakenEdge { false };
    std::optional<uint32_t> caseIndex;
    ValueID parameter;
};

struct BodyIncomingEdge {
    BlockID from;
    uint32_t targetIndex { 0 };
    BodyTarget target;
};

struct BodyLivenessData {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(BodyLivenessData);

    static constexpr uint32_t numberOfBranches = 16;
    using Branches = std::array<uint32_t, numberOfBranches>;
    static constexpr uint32_t shiftOf(uint32_t level) { return 6 + 4 * level; }

    bool holds(uint32_t set, ValueID value) const
    {
        for (uint32_t level = levels; level; --level)
            set = branches[set][(value.index() >> shiftOf(level - 1)) % numberOfBranches];
        return words[set] >> (value.index() % 64) & 1;
    }

    template<typename Functor>
    void forEachIn(uint32_t set, uint64_t first, uint64_t end, const Functor& functor) const
    {
        if (first < end)
            forEachInNode(levels, set, 0, first, end, functor);
    }

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

    Vector<uint32_t> lastUses;
    Vector<uint32_t> liveAtHeads;
    Vector<Branches> branches;
    Vector<uint64_t> words;
    uint32_t levels { 0 };
};

struct BodyLivenessScratch {
    struct Union {
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t result { 0 };
    };

    Vector<ValueID> reads;
    Vector<uint32_t> collected;
    Vector<Union> unions;
    struct Raise {
        uint32_t position { 0 };
        uint32_t head { 0 };
        friend auto operator<=>(const Raise&, const Raise&) = default;
    };
    Vector<Raise> raises;
};

struct KeptWithBodyIndex {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptWithBodyIndex);

    virtual ~KeptWithBodyIndex() = default;
};

class BodyIndex {
    WTF_MAKE_NONCOPYABLE(BodyIndex);
public:
    static constexpr uint32_t maxCountedUses = 31;
    static constexpr uint32_t maxCountedIncomingEdges = UINT16_MAX;

    const Body& body() const { return m_body; }
    const KeptWithBodyIndex* keptByWalk() const { return m_keptByWalk.load(); }
    const KeptWithBodyIndex& keepForWalk(std::unique_ptr<KeptWithBodyIndex>&& made) const
    {
        const KeptWithBodyIndex* kept = nullptr;
        if (m_keptByWalk.compare_exchange_strong(kept, made.get()))
            return *made.release();
        return *kept;
    }
    std::unique_ptr<const KeptWithBodyIndex> takeKeptByWalk() const { return std::unique_ptr<const KeptWithBodyIndex>(m_keptByWalk.exchange(nullptr)); }
    std::unique_ptr<const BodyLivenessData> takeLiveness() const { return std::unique_ptr<const BodyLivenessData>(m_liveness.exchange(nullptr)); }
    uint32_t numberOfBlocks() const { return m_body.numberOfBlocks(); }
    uint32_t numberOfValues() const { return m_body.numberOfValues(); }

    BodyValue value(ValueID id) const
    {
        uint32_t place = m_placesOfValues[id.index()];
        return place & isParameterBit ? m_body.parameterAt(place & ~isParameterBit, id) : m_body.operationAt(place, id);
    }
    bool isParameter(ValueID id) const { return m_placesOfValues[id.index()] & isParameterBit; }
    Opcode opcodeOf(ValueID id) const { return value(id).opcode(); }
    const OpcodeSet& opcodes() const { return m_opcodes; }
    bool holdsAnyOf(const OpcodeSet& wanted) const { return m_opcodes.intersects(wanted); }
    BlockID blockOf(ValueID id) const
    {
        if (m_blocks.size() < 2 || id.index() < m_blocks[1].firstParameter)
            return BlockID(0u);
        auto next = std::ranges::upper_bound(m_blocks, id.index(), { }, &BlockPlace::firstParameter);
        return BlockID(static_cast<uint32_t>(next - m_blocks.begin()) - 1);
    }

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
    ValueID onlyUsingValue(ValueID id) const
    {
        std::optional<BodyUser> user = onlyUser(id);
        return user ? user->value : ValueID();
    }
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
    std::span<const BodyMentionOfIteration> mentionsOfIteration(ValueID open) const
    {
        auto found = std::ranges::equal_range(m_mentionsOfIterations, open.index(), { }, [](const BodyMentionOfIteration& mention) {
            return mention.open.index();
        });
        return m_mentionsOfIterations.subspan(found.begin() - m_mentionsOfIterations.begin(), found.size());
    }
    ValueID stripCasts(ValueID id) const
    {
        if (isParameter(id) || !opcodesThatStateTypes.contains(opcodeOf(id)))
            return id;
        return stripCastsSlow(id);
    }

    BodyBlock block(BlockID id) const
    {
        const BlockPlace& place = m_blocks[id.index()];
        return m_body.blockAt(place.wordOffset, place.wordOffsetOfTerminator, id, ValueID(place.firstParameter));
    }
    ALWAYS_INLINE BodyTerminator terminator(BlockID id) const { return m_body.terminatorAt(m_blocks[id.index()].wordOffsetOfTerminator, id); }

    bool isReachable(BlockID id) const { return m_blocks[id.index()].flags & IsReachable; }
    uint32_t numberOfIncomingEdges(BlockID id) const { return m_blocks[id.index()].numberOfIncomingEdges; }
    bool isEnteredByHandlerEdge(BlockID id) const { return m_blocks[id.index()].flags & IsEnteredByHandlerEdge; }
    std::optional<BodyIncomingEdge> onlyIncomingEdge(BlockID id) const
    {
        const BlockPlace& place = m_blocks[id.index()];
        if (place.numberOfIncomingEdges != 1 || place.flags & IsEnteredByHandlerEdge || !place.immediateDominator)
            return std::nullopt;
        RELEASE_ASSERT(place.wordOffsetOfFirstIncomingTarget);
        return BodyIncomingEdge { place.immediateDominator, place.indexOfFirstIncomingTarget, BodyTarget(&m_body.words()[place.wordOffsetOfFirstIncomingTarget]) };
    }

    BlockID immediateDominator(BlockID id) const { return m_blocks[id.index()].immediateDominator; }
    std::span<const BlockID> blocksInDominatorOrder() const { return m_blocksInDominatorOrder; }
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
    const BodyLoop& loop(LoopID id) const { return m_loops[id.index()]; }
    std::span<const BlockID> membersOf(LoopID id) const { return m_membersOfLoops.subspan(m_loops[id.index()].firstMember, m_loops[id.index()].numberOfMembers); }
    LoopID loopOf(BlockID id) const { return m_blocks[id.index()].loop; }
    bool isLoopHead(BlockID id) const { return m_blocks[id.index()].flags & IsLoopHead; }
    bool isInLoop(BlockID block, LoopID loop) const
    {
        LoopID holder = loopOf(block);
        return holder && m_loops[holder.index()].placeAmongLoops - m_loops[loop.index()].placeAmongLoops < m_loops[loop.index()].numberOfLoopsWithin;
    }
    uint32_t loopDepthOf(BlockID id) const { return loopOf(id) ? m_loops[loopOf(id).index()].depth : 0; }
    bool hasIrreducibleFlow() const { return m_hasIrreducibleFlow; }

    JS_EXPORT_PRIVATE std::optional<BodyEdgeResult> edgeResult(BlockID from, uint32_t targetIndex, uint32_t proofIndex) const;
    ALWAYS_INLINE std::optional<BodyEdgeResult> edgeResult(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex) const
    {
        if (!terminator.isConditionalBranch()) [[unlikely]]
            return edgeResultOfAnyTerminator(terminator, targetIndex, target, proofIndex);
        constexpr TerminatorKind kind = TerminatorKind::Branch;
        static_assert(numberOfFixedTargetsOf(kind));
        if (targetIndex >= terminator.numberOfTargets() || targetIndex >= *numberOfFixedTargetsOf(kind) || !(targetsTakingProofsOf(kind) >> targetIndex & 1))
            return std::nullopt;
        std::span<const uint32_t> proofs = target.proofs();
        if (proofIndex >= proofs.size())
            return std::nullopt;
        BodyValue test = value(terminator.as<TerminatorKind::Branch>().conditionOperand());
        Proof proof = provesOf(test.opcode());
        if (proof == Proof::None)
            return std::nullopt;
        uint32_t operandIndex = proofs[proofIndex];
        ValueID subject = test ? test.operand(operandIndex) : terminator.operand(operandIndex);
        if (!subject)
            return std::nullopt;
        BodyEdgeResult result;
        result.proof = proof;
        result.subject = subject;
        result.operandIndex = operandIndex;
        if (test) {
            result.test = test.id();
            result.isOnTakenEdge = !targetIndex;
        } else
            result.caseIndex = targetIndex;
        result.parameter = ValueID(m_blocks[target.block().index()].firstParameter + static_cast<uint32_t>(target.arguments().size()) + proofIndex);
        return result;
    }
    JS_EXPORT_PRIVATE std::optional<BodyEdgeResult> edgeResultOfAnyTerminator(const BodyTerminator&, uint32_t targetIndex, BodyTarget, uint32_t proofIndex) const;
    std::optional<BodyEdgeResult> edgeResult(BlockID from, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex) const { return edgeResult(terminator(from), targetIndex, target, proofIndex); }
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
        uint32_t indexOfFirstIncomingTarget { 0 };
        uint32_t wordOffsetOfFirstIncomingTarget { 0 };
        uint16_t numberOfIncomingEdges { 0 };
        uint16_t flags { 0 };
    };
    static_assert(sizeof(BlockPlace) == 40);

    BodyIndex() = default;

    JS_EXPORT_PRIVATE ValueID stripCastsSlow(ValueID cast) const;

    Body m_body;
    std::span<const BlockPlace> m_blocks;
    std::span<const BlockID> m_blocksInDominatorOrder;
    std::span<const BodyLoop> m_loops;
    std::span<const BlockID> m_membersOfLoops;
    std::span<const uint32_t> m_placesOfValues;
    std::span<const uint32_t> m_onlyUsersOfValues;
    std::span<const uint8_t> m_usesOfValues;
    std::span<const BodyMentionOfIteration> m_mentionsOfIterations;
    OpcodeSet m_opcodes;
    bool m_hasIrreducibleFlow { false };
    mutable std::atomic<const BodyLivenessData*> m_liveness { nullptr };
    mutable std::atomic<const KeptWithBodyIndex*> m_keptByWalk { nullptr };
};

struct ConstructionStartInModule {
    ModuleID module;
    ConstructionStart start;
};

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

    JS_EXPORT_PRIVATE const BodyIndex& build(FunctionID, const Function&, unsigned worker);
    void adopt(FunctionID, const BodyIndex&);
    void addFunctionsWithoutBodies(uint32_t numberOfFunctions)
    {
        while (m_indexes.size() < numberOfFunctions)
            m_indexes.append(nullptr);
    }

    JS_EXPORT_PRIVATE Vector<ConstructionStartInModule> takeConstructionStarts();
    JS_EXPORT_PRIVATE uint64_t valuesGoneOver() const;

    JS_EXPORT_PRIVATE size_t sizeInBytes() const;

private:
    friend class BodyIndexBuilder;

    struct Worker;

    Vector<const BodyIndex*> m_indexes;
    Vector<std::unique_ptr<Worker>> m_workers;
};

class BodyLiveness {
    WTF_MAKE_NONCOPYABLE(BodyLiveness);
public:
    BodyLiveness() = default;

    JS_EXPORT_PRIVATE void compute(const BodyIndex&);
    bool isComputedFor(const BodyIndex& index) const { return m_index == &index; }
    void forget() { m_index = nullptr; }

    bool isLiveAtHead(ValueID value, BlockID block) const { return m_data->holds(m_data->liveAtHeads[block.index()], value); }
    template<typename Functor>
    void forEachLiveAtHead(BlockID block, ValueID first, ValueID end, const Functor& functor) const
    {
        m_data->forEachIn(m_data->liveAtHeads[block.index()], first.index(), std::min<uint32_t>(end.index(), m_index->numberOfValues()), functor);
    }
    template<typename Functor>
    void forEachLiveAtHead(BlockID block, const Functor& functor) const { forEachLiveAtHead(block, ValueID(0u), m_index->block(block).firstParameter(), functor); }
    bool answersFromSameDataAs(const BodyLiveness& other) const { return m_data == other.m_data; }
    JS_EXPORT_PRIVATE void countHeadsWhereLive(std::span<const BlockID>, std::span<uint32_t> numbersByValue) const;

    ValueID lastUse(ValueID value) const { return ValueID(m_data->lastUses[value.index()]); }

    bool isLiveBefore(ValueID value, ValueID point, BlockID block) const
    {
        if (value.index() >= point.index() || m_data->lastUses[value.index()] < point.index())
            return false;
        return m_index->block(block).holds(value) || isLiveAtHead(value, block);
    }

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
    const BodyLivenessData* m_data { nullptr };
    BodyLivenessData m_ownData;
    BodyLivenessScratch m_scratch;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
