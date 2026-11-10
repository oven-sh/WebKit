/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineLiveness.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

class Location {
public:
    enum Kind : uint8_t {
        None,
        Register,
        Spill,
        SlotBytes,
        Outgoing,
        InstanceWord,
        Immediate,
    };

    constexpr Location() = default;
    static constexpr Location reg(Reg reg) { return Location(Register, reg.index()); }
    static constexpr Location spill(SpillIndex index) { return Location(Spill, index); }
    static constexpr Location slotBytes(unsigned indexOfStackSlot) { return Location(SlotBytes, indexOfStackSlot); }
    static constexpr Location outgoing(uint32_t offsetFromSP) { return Location(Outgoing, offsetFromSP); }
    static constexpr Location instanceWord(int32_t offset) { return Location(InstanceWord, static_cast<uint32_t>(offset) & payloadMask); }
    static constexpr Location immediate() { return Location(Immediate, 0); }

    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> payloadBits); }
    constexpr explicit operator bool() const { return kind() != None; }
    constexpr bool isRegister() const { return kind() == Register; }
    constexpr bool isSpill() const { return kind() == Spill; }
    constexpr bool isImmediate() const { return kind() == Immediate; }
    constexpr bool isMemory() const { return kind() == Spill || kind() == Outgoing || kind() == InstanceWord; }

    Reg reg() const
    {
        ASSERT(isRegister());
        return Reg::fromIndex(payload());
    }
    GPRReg gpr() const { return reg().gpr(); }
    FPRReg fpr() const { return reg().fpr(); }
    SpillIndex spillIndex() const
    {
        ASSERT(isSpill());
        return payload();
    }
    unsigned indexOfStackSlot() const
    {
        ASSERT(kind() == SlotBytes);
        return payload();
    }
    uint32_t offsetFromSP() const
    {
        ASSERT(kind() == Outgoing);
        return payload();
    }
    int32_t offsetInInstance() const
    {
        ASSERT(kind() == InstanceWord);
        return static_cast<int32_t>(payload() << (32 - payloadBits)) >> (32 - payloadBits);
    }

    friend constexpr bool operator==(Location, Location) = default;
    void dump(PrintStream&) const;

private:
    static constexpr unsigned payloadBits = 28;
    static constexpr uint32_t payloadMask = (1u << payloadBits) - 1;

    constexpr Location(Kind kind, uint32_t payload)
        : m_bits(static_cast<uint32_t>(kind) << payloadBits | payload)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(payload <= payloadMask);
    }

    constexpr uint32_t payload() const { return m_bits & payloadMask; }

    uint32_t m_bits { 0 };
};

enum class StepKind : uint8_t {
    Execute,
    Move,
    Reload,
    Save,
    Materialize,
    Swap,
    KeepPointer,
};

struct Step {
    StepKind kind;
    Width width { Width64 };
    uint32_t patchAssignment { UINT32_MAX };
    Location destination;
    Location sources[maxLeaves];
    Operation* operation { nullptr };
    Operation* other { nullptr };
};

struct PatchAssignment {
    std::span<const Location> locations;
    RegisterMask usedRegisters { 0 };
    RegisterMask gpScratch { 0 };
    RegisterMask fpScratch { 0 };
};

struct SpillFacts {
    Operation* value { nullptr };
    Position begin { 0 };
    Position end { 0 };
    Position beginOfGap { noPosition };
    Position endOfGap { noPosition };
    Width width { Width64 };
    bool livesAcrossSuspension { false };
    bool isPointerBesideNarrowReference { false };
    bool isSaveInRareRegion { false };
    bool isStoredAtHeadOfFramedPart { false };
    bool isSharedByPhis { false };
    bool isRead { false };
    bool isDeadSlot { false };

    bool hasGap() const { return beginOfGap != noPosition; }
    bool isInGap(Position position) const { return hasGap() && beginOfGap <= position && position <= endOfGap; }
    bool holdsValueBetween(Position first, Position last) const
    {
        if (first > last || begin > last || first > std::max(begin, end))
            return false;
        return !hasGap() || first < beginOfGap || last > endOfGap;
    }
    bool liesApartFrom(const SpillFacts& other) const
    {
        if (!hasGap())
            return !other.holdsValueBetween(begin, std::max(begin, end));
        return !other.holdsValueBetween(begin, beginOfGap - 1) && !other.holdsValueBetween(endOfGap + 1, end);
    }
};

struct BlockSteps {
    uint32_t begin { 0 };
    uint32_t end { 0 };
    uint32_t beginOfEdgeMoves { 0 };
    uint32_t endOfEdgeMoves { 0 };
    uint32_t terminator { UINT32_MAX };
};

struct RegisterBinding {
    Reg reg;
    Operation* value;
};

class Allocation {
    WTF_MAKE_NONCOPYABLE(Allocation);
public:
    std::span<const Step> steps() const { return m_steps.span(); }
    const Step& step(uint32_t index) const { return m_steps[index]; }
    const BlockSteps& stepsOf(const Block* block) const { return m_blocks[block->index()]; }
    const BlockSteps* stepsOfBlocks() const { return m_blocks.begin(); }
    const Step* findExecuteOf(const Operation* operation) const
    {
        const BlockSteps& steps = stepsOf(operation->owner());
        for (uint32_t index = steps.end; index-- > steps.begin;) {
            if (m_steps[index].kind == StepKind::Execute && m_steps[index].operation == operation)
                return &m_steps[index];
        }
        return nullptr;
    }
    const PatchAssignment& patchAssignment(uint32_t index) const { return m_patchAssignments[index]; }
    uint32_t numberOfPatchAssignments() const { return m_patchAssignments.size(); }
    uint32_t numberOfOperationsAllocated() const { return m_numberOfOperationsAllocated; }
    uint64_t laterUsesScanned() const { return m_laterUsesScanned; }
    uint64_t livenessSearchBlocksVisited() const { return m_livenessSearchBlocksVisited; }

    std::span<Step> stepsForTesting() { return m_steps.span(); }
    BlockSteps& stepsForTesting(const Block* block) { return m_blocks[block->index()]; }
    PatchAssignment& patchAssignmentForTesting(uint32_t index) { return m_patchAssignments[index]; }

    std::span<const RegisterBinding> registersAtHeadOf(const Block* block) const { return m_registersAtHead[block->index()]; }

    bool isSpilled(const Operation* value) const { return spillOf(value) != noSpill; }
    SpillIndex spillOf(const Operation* value) const { return m_spillOfValue[value->index()]; }
    const SpillIndex* spillsOfValues() const { return m_spillOfValue.begin(); }
    Reg registerAtDefinitionOf(const Operation* value) const { return m_registerAtDefinition[value->index()]; }
    bool isStoredBehindDefinition(const Operation* value) const
    {
        SpillIndex spill = spillOf(value);
        return spill != noSpill && registerAtDefinitionOf(value).isSet() && !m_spills[spill].isStoredAtHeadOfFramedPart && !m_spills[spill].isDeadSlot && !isWrittenByEdges(value);
    }
    bool isWrittenByEdges(const Operation* phi) const { return m_phisWrittenByEdges.size() && m_phisWrittenByEdges.get(phi->index()); }

    std::span<const SpillFacts> spills() const { return m_spills.span(); }
    std::span<SpillFacts> spillsForTesting() { return m_spills.span(); }
    std::span<PatchOperation* const> suspensions() const { return m_suspensions.span(); }
    const BitSpan& liveSpillsAt(uint32_t suspension) const { return m_liveSpillsAtSuspension[suspension]; }

    RegisterMask usedCalleeSaves() const { return m_usedCalleeSaves; }
    bool needsFrameOutsideRareRegions() const { return m_needsFrameOutsideRareRegions; }
    bool needsFrameIn(uint32_t rareRegion) const { return m_regionNeedsFrame.get(rareRegion); }
    bool holdsPatchThatFollowsFrameSetup() const { return m_holdsPatchThatFollowsFrameSetup; }
    bool holdsPatchThatCountsOnStackCheck() const { return m_holdsPatchThatCountsOnStackCheck; }
    bool holdsPatchThatCountsOnFrameRecord() const { return m_holdsPatchThatCountsOnFrameRecord; }
    bool holdsPatchWithOperandInFrame() const { return m_holdsPatchWithOperandInFrame; }

    struct HeadOfFramedPart {
        const Block* block { nullptr };
        uint32_t beginOfSteps { 0 };
        uint32_t endOfSteps { 0 };
    };
    std::span<const HeadOfFramedPart> headsOfFramedPart() const { return m_headsOfFramedPart.span(); }
    std::span<HeadOfFramedPart> headsOfFramedPartForTesting() { return m_headsOfFramedPart.span(); }
    bool hasFramedPart() const { return !m_headsOfFramedPart.isEmpty(); }
    bool recordsWhatCheckersRead() const { return m_recordsWhatCheckersRead; }
    bool leavesOutSavesAtHeads() const { return m_leavesOutSavesAtHeads; }
    bool isInFramedPart(const Block* block) const { return m_isInFramedPart.get(block->index()); }
    const HeadOfFramedPart& headOfFramedPartAbove(const Block* block) const { return m_headsOfFramedPart[m_headAboveBlock[block->index()]]; }
    bool isHeadOfFramedPart(const Block* block) const { return isInFramedPart(block) && headOfFramedPartAbove(block).block == block; }
    struct FurtherHeadOfFramedPart {
        uint32_t head { 0 };
        uint32_t rest { UINT32_MAX };
    };
    uint32_t furtherHeadsOfFramedPartAbove(const Block* block) const { return m_setOfHeadsAboveBlock.isEmpty() ? UINT32_MAX : m_setOfHeadsAboveBlock[block->index()]; }
    std::span<const FurtherHeadOfFramedPart> furtherHeadsOfFramedPart() const { return m_setsOfHeads.span(); }
    template<typename Functor>
    bool everyHeadOfFramedPartAbove(const Block* block, const Functor& functor) const
    {
        if (!functor(headOfFramedPartAbove(block)))
            return false;
        for (uint32_t further = furtherHeadsOfFramedPartAbove(block); further != UINT32_MAX; further = m_setsOfHeads[further].rest) {
            if (!functor(m_headsOfFramedPart[m_setsOfHeads[further].head]))
                return false;
        }
        return true;
    }
    const Block* onlyHeadOfFramedPart() const { return m_headsOfFramedPart.size() == 1 ? m_headsOfFramedPart[0].block : nullptr; }
    bool needsFrameAheadOfFramedPart() const { return m_needsFrameAheadOfFramedPart; }
    bool writesMemoryAheadOfFramedPart() const { return m_writesMemoryAheadOfFramedPart; }

private:
    friend class Allocator;
    friend class Arena;

    Allocation() = default;

    ArenaVector<Step> m_steps;
    ArenaVector<BlockSteps> m_blocks;
    ArenaVector<PatchAssignment> m_patchAssignments;
    ArenaVector<std::span<const RegisterBinding>> m_registersAtHead;
    ArenaVector<SpillIndex> m_spillOfValue;
    ArenaVector<Reg> m_registerAtDefinition;
    ArenaVector<SpillFacts> m_spills;
    ArenaVector<PatchOperation*> m_suspensions;
    ArenaVector<BitSpan> m_liveSpillsAtSuspension;
    RegisterMask m_usedCalleeSaves { 0 };
    BitSpan m_phisWrittenByEdges;
    uint32_t m_numberOfOperationsAllocated { 0 };
    uint64_t m_laterUsesScanned { 0 };
    uint64_t m_livenessSearchBlocksVisited { 0 };
    bool m_needsFrameOutsideRareRegions { false };
    bool m_needsFrameAheadOfFramedPart { false };
    bool m_recordsWhatCheckersRead { false };
    bool m_leavesOutSavesAtHeads { false };
    bool m_writesMemoryAheadOfFramedPart { false };
    bool m_holdsPatchThatFollowsFrameSetup { false };
    bool m_holdsPatchThatCountsOnStackCheck { false };
    bool m_holdsPatchThatCountsOnFrameRecord { false };
    bool m_holdsPatchWithOperandInFrame { false };
    BitSpan m_regionNeedsFrame;
    ArenaVector<HeadOfFramedPart> m_headsOfFramedPart;
    BitSpan m_isInFramedPart;
    ArenaVector<uint32_t> m_headAboveBlock;
    ArenaVector<uint32_t> m_setOfHeadsAboveBlock;
    ArenaVector<FurtherHeadOfFramedPart> m_setsOfHeads;
};

Allocation* allocateRegisters(Procedure&);

struct PendingMove {
    Operation* value;
    Location destination;
    Location source;
    Width width;
};

void resolveParallelMoves(Arena&, std::span<PendingMove>, ArenaVector<Step>& output);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
