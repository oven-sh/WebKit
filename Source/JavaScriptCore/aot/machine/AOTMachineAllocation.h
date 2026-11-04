/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineLiveness.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

// Where an operand is read or a result is written.
class Location {
public:
    enum Kind : uint8_t {
        None,
        Register,
        // A place in the frame that assignSlots() chooses.
        Spill,
        // The bytes of a StackSlot, as the base of an address.
        SlotBytes,
        // A word of the outgoing area, by its offset from the stack pointer.
        Outgoing,
        // A word of the instance, by its offset from the instance register, that holds the constant.
        InstanceWord,
        // The constant stands in the instruction, or the reader of a cold operand takes it from the operation.
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

// resolveParallelMoves() parks values in scratchGPR and scratchFPR: either may be the destination of a Move, a Reload or a Materialize and the source of a Move or a Save.
// An Execute or a Swap leaves nothing in them, but for the Execute of an Extract or of an ArgumentReg, which writes neither. A Materialize or a KeepPointer leaves nothing in scratchGPR but its own destination.
// A Move, a Reload, a Save or a ClearSpill writes neither, but for its destination.
// No step names secondScratchGPR: ARM64 reaches a far slot through it, so every step but a Move may write it, and so may the store of a spilled value.
// None of the three is a leaf, a result or a scratch register of an Execute, unless a Patch constrains an operand to it by name.
enum class StepKind : uint8_t {
    // The operation, with its leaves in sources, in the order of forEachLeaf(), and its result in destination. A Patch or Check has the rest in a PatchAssignment.
    // An ArgumentReg is moved from its register to destination. An Extract has its register as destination and as sources[0], or its spill where it has no register.
    // Of a Branch with ValueFlag::GoesByOverflow `other` is the operand whose register its sum has taken, null where it has taken none: the failing successor is entered with the operand in that register again.
    // Between the Execute of such a sum and that of its Branch stand Moves, Reloads, Saves and Executes of Truncs alone.
    Execute,
    // All below: `operation` is the value that moves, destination and sources[0] say where to and from where.
    // Register to register, within a bank.
    Move,
    // Spill or outgoing word to register.
    Reload,
    // Register to spill or outgoing word.
    Save,
    // An operation without position into a register.
    Materialize,
    // Two registers of one bank change contents: destination and sources[0]. `operation` holds the value that ends in destination, `other` the one that ends in sources[0].
    Swap,
    // Zero into the spill in destination: the program has let go of the value.
    // It may stand one position behind SpillFacts::end, right behind the operation that read the value last, unless a spill has been written since that Execute.
    ClearSpill,
    // sources[0] holds a narrow reference. The pointer it stands for goes to the spill in destination. Writes the scratch register.
    KeepPointer,
};

struct Step {
    StepKind kind;
    // Of what moves.
    Width width { Width64 };
    // In Allocation::patchAssignment(): for an Execute of a Patch or Check, and for an Execute of any other operation for which numberOfScratchGPRsOf() is not zero.
    // The latter has its registers in PatchAssignment::gpScratch, lowest Reg::index() first, no locations, and usedRegisters zero.
    uint32_t patchAssignment { UINT32_MAX };
    Location destination;
    Location sources[maxLeaves];
    Operation* operation { nullptr };
    Operation* other { nullptr };
};

struct PatchAssignment {
    // The results, then the operands: what PatchParameters lists. A Check's predicate is not among them: its leaves are the sources of the step.
    std::span<const Location> locations;
    // The registers of the results, and every register from which a value is read behind the operation, a constant that stays bound among them.
    // A generator may take any register outside PatchParameters::unavailableRegisters() for its own. So behind the Execute of a Patch a register that is not listed holds nothing.
    // That does not hold of a Check, whose generator writes the late path alone, nor of a Patch that only keeps its children alive.
    RegisterMask usedRegisters { 0 };
    RegisterMask gpScratch { 0 };
    RegisterMask fpScratch { 0 };
};

struct SpillFacts {
    // What lies there. For the pointer beside a narrow reference: the reference.
    Operation* value { nullptr };
    // The spill holds its value from begin to end. No step reads or writes it outside: on either side assignSlots() may give its place to another spill.
    // Of a Phi without a register at its definition: from the first to the last place where the moves of an incoming edge write it, at least.
    // Two kinds of write may stand outside. No later step of the same block then reads a spill that held its value at the write and whose range lies apart from this one's:
    //  - a Save into the spill of a Phi, in a block whose one successor is the block of the Phi;
    //  - a Save or a KeepPointer among the steps in front of the operation at whose late position the range begins, Extracts not counted.
    Position begin { 0 };
    Position end { 0 };
    // A landing pad stands just ahead of what it rejoins, which may be far behind the blocks that lead to it. Where landing pads alone name the spill from some place on, it holds nothing between
    // the last block that names it or leads to one of them and the first of those pads: from beginOfGap to endOfGap no step names it, and assignSlots() may give its place to another spill.
    // Both are set or neither. The gap begins and ends with whole blocks, and what lies in the spill is lost on an edge to a head in the gap.
    Position beginOfGap { noPosition };
    Position endOfGap { noPosition };
    Width width { Width64 };
    // Some suspension holds it: Allocation::spillsHeldAt().
    bool livesAcrossSuspension { false };
    bool isPointerBesideNarrowReference { false };
    // Made in a rare region for a value from outside, and read there alone. Nothing stores the value behind its definition.
    // Saves write it: the first where the range begins, and another wherever the value loses its register in a later block of the region, which a path may reach without the first.
    bool isSaveInRareRegion { false };
    // Of a value that is defined ahead of the framed part and loses its register in it. Nothing stores the value behind its definition:
    // a Save among the steps of Allocation::HeadOfFramedPart writes the spill at each head below which a step reads it, and no step outside the framed part names it.
    bool isStoredAtHeadOfFramedPart { false };

    bool hasGap() const { return beginOfGap != noPosition; }
    bool isInGap(Position position) const { return hasGap() && beginOfGap <= position && position <= endOfGap; }
    // Some position from `first` to `last` lies in the range and outside the gap.
    bool holdsValueBetween(Position first, Position last) const
    {
        if (first > last || begin > last || first > std::max(begin, end))
            return false;
        return !hasGap() || first < beginOfGap || last > endOfGap;
    }
    // No position lies in the ranges of both and in the gap of neither: the two may share a place.
    bool liesApartFrom(const SpillFacts& other) const
    {
        if (!hasGap())
            return !other.holdsValueBetween(begin, std::max(begin, end));
        return !other.holdsValueBetween(begin, beginOfGap - 1) && !other.holdsValueBetween(endOfGap + 1, end);
    }
};

// Allocator::allocateBlock() sets begin ahead of enterBlock(), so what that appends for the block lies in the range. Whoever appends the Execute of the last operation sets end and terminator.
// appendEdgeMoves() alone sets the range of the edge moves.
struct BlockSteps {
    // Everything but the last operation. Where the block has several successors and one of them has Phis that live in their spills: the steps that write those spills too, at the end.
    uint32_t begin { 0 };
    uint32_t end { 0 };
    // The moves for the edge to its one successor. They may have been appended long after the rest.
    // They run ahead of the operation that ends the block. So they write memory and the scratch registers alone, unless Operation::isUnconditionalJump() holds of it.
    uint32_t beginOfEdgeMoves { 0 };
    uint32_t endOfEdgeMoves { 0 };
    // The Execute of the operation that ends the block. No step of the block stands behind it.
    uint32_t terminator { UINT32_MAX };
};

struct RegisterBinding {
    Reg reg;
    Operation* value;
};

// Step 4: what the allocator decided. The encoder and the checker read nothing else of it.
class Allocation {
    WTF_MAKE_NONCOPYABLE(Allocation);
public:
    std::span<const Step> steps() const { return m_steps.span(); }
    const Step& step(uint32_t index) const { return m_steps[index]; }
    const BlockSteps& stepsOf(const Block* block) const { return m_blocks[block->index()]; }
    // By Block::index(): what stepsOf() answers.
    const BlockSteps* stepsOfBlocks() const { return m_blocks.begin(); }
    // The last Execute of the operation among the steps of its block ahead of the one that ends it. Null if there is none.
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
    // How often the allocator has given an operation its places: Allocator::appendStep() counts each Execute as it appends it.
    // It is not read off steps(), which would hide an Execute that was appended and cut back.
    uint32_t numberOfOperationsAllocated() const { return m_numberOfOperationsAllocated; }

    // For the tests that damage a sound allocation and expect the checker to refuse it.
    std::span<Step> stepsForTesting() { return m_steps.span(); }
    BlockSteps& stepsForTesting(const Block* block) { return m_blocks[block->index()]; }
    PatchAssignment& patchAssignmentForTesting(uint32_t index) { return m_patchAssignments[index]; }

    // The registers that hold values where the block is entered, the Phis of the block among them. A spilled value that is no Phi of the block is not listed, whether a register holds it or not,
    // but at a head of the framed part: another head may have spilled what this one still has to save.
    // A value that is live there and not listed lies in its spill or is made again.
    // Every head has its list where ProcedureTraits::checksAllocation is set. Otherwise only the heads that the allocator itself asks for have one: those of rare regions, and the heads of the framed part.
    std::span<const RegisterBinding> registersAtHeadOf(const Block* block) const { return m_registersAtHead[block->index()]; }

    // A spilled value is stored once, from the register it is defined in, right behind its definition:
    // behind the Execute; at the head of its block for a Phi; at the head of the first successor for a result of a Patch that ends its block.
    // There is no step for that store: the decision falls after the definition has been passed. A Phi without such a register is written to its spill by the moves of the incoming edges.
    // The one result of a Patch that lies in an outgoing word has no such register and no such store: a Reload and a Save copy it, behind the Execute, or at the head of the first successor where the Patch ends its block.
    // A member of a tuple that lies in an outgoing word is reloaded there into the register that registerAtDefinitionOf() names for its Extract, and is stored behind that Extract like any other value.
    // Where the members outnumber the registers, registerAtDefinitionOf() names none for such a member: a Reload and a Save copy it there, through the register of a member that is reloaded behind them.
    bool isSpilled(const Operation* value) const { return spillOf(value) != noSpill; }
    SpillIndex spillOf(const Operation* value) const { return m_spillOfValue[value->index()]; }
    // By Operation::index(): what spillOf() answers.
    const SpillIndex* spillsOfValues() const { return m_spillOfValue.begin(); }
    Reg registerAtDefinitionOf(const Operation* value) const { return m_registerAtDefinition[value->index()]; }
    bool isStoredBehindDefinition(const Operation* value) const
    {
        SpillIndex spill = spillOf(value);
        return spill != noSpill && registerAtDefinitionOf(value).isSet() && !m_spills[spill].isStoredAtHeadOfFramedPart;
    }

    std::span<const SpillFacts> spills() const { return m_spills.span(); }
    // The Patches with isSuspension() that are reached, in the order of allocation.
    std::span<PatchOperation* const> suspensions() const { return m_suspensions.span(); }
    // By the place of the suspension in suspensions(). Bit s: the suspension reads spill s, or a path leads on from it to a step that reads spill s ahead of every step that writes it,
    // or the collector needs the value behind it: Liveness::valuesKeptForCollector(), and the pointer beside a narrow reference that the suspension names.
    // A range goes by the order, so it also reaches over a suspension from which nothing leads to a read: a landing pad stands behind every block that leads to it. Such a spill is not held there.
    const BitSpan& spillsHeldAt(uint32_t suspension) const { return m_spillsHeldAtSuspension[suspension]; }

    // Those the function writes, so it saves them.
    RegisterMask usedCalleeSaves() const { return m_usedCalleeSaves; }
    // Something outside the rare regions needs a frame: a Patch that does not run without one, a spill, a use of a SlotBase or of the FramePointer, an outgoing word, a callee-saved register.
    bool needsFrameOutsideRareRegions() const { return m_needsFrameOutsideRareRegions; }
    bool needsFrameIn(uint32_t rareRegion) const { return m_regionNeedsFrame.get(rareRegion); }
    // Allocator::allocatePatch() notes both as it goes, so nobody reads the steps again to find out.
    bool holdsPatchThatFollowsFrameSetup() const { return m_holdsPatchThatFollowsFrameSetup; }
    // Some Patch does not run without a frame, follows a frame setup or leaves the function other than by returning: what it calls or jumps to counts on the stack check of a setup.
    bool holdsPatchThatCountsOnStackCheck() const { return m_holdsPatchThatCountsOnStackCheck; }
    // Some Patch takes the frame pointer register for the frame, or has an operand that lies in the frame: its generator is told an offset from the frame pointer, and may read the word off that register.
    bool holdsPatchThatCountsOnFrameRecord() const { return m_holdsPatchThatCountsOnFrameRecord; }

    // The framed part: its heads, blocks outside rare regions and loops that are no entrypoints, and every block that one of them dominates. No head where Allocator::findFramedPart() has found none.
    // No head dominates another. A way that leaves what one head dominates may come to another, which sets the frame up anew.
    // On no way to its first head the function writes memory, and some way outside rare regions leaves the function without passing one.
    struct HeadOfFramedPart {
        const Block* block { nullptr };
        // Saves alone. They run where the head is entered, ahead of the stores of what is defined there and of BlockSteps::begin, from the registers of registersAtHeadOf().
        uint32_t beginOfSteps { 0 };
        uint32_t endOfSteps { 0 };
    };
    // In the order of the blocks.
    std::span<const HeadOfFramedPart> headsOfFramedPart() const { return m_headsOfFramedPart.span(); }
    bool hasFramedPart() const { return !m_headsOfFramedPart.isEmpty(); }
    bool isInFramedPart(const Block* block) const { return m_isInFramedPart.get(block->index()); }
    // Of a block in the framed part: the head that dominates it.
    const HeadOfFramedPart& headOfFramedPartAbove(const Block* block) const { return m_headsOfFramedPart[m_headAboveBlock[block->index()]]; }
    bool isHeadOfFramedPart(const Block* block) const { return isInFramedPart(block) && headOfFramedPartAbove(block).block == block; }
    const Block* onlyHeadOfFramedPart() const { return m_headsOfFramedPart.size() == 1 ? m_headsOfFramedPart[0].block : nullptr; }
    // Something outside both the framed part and the rare regions needs a frame after all, or reads what the framed part alone stores. Then every spilled value is stored behind its definition.
    bool needsFrameAheadOfFramedPart() const { return m_needsFrameAheadOfFramedPart; }
    // On a way to a first head, counting on Procedure::largestFrameSetUpWithoutStackCheck(). planFrame() sets a larger frame up at the entry.
    bool writesMemoryAheadOfFramedPart() const { return m_writesMemoryAheadOfFramedPart; }

private:
    friend class Allocator;
    friend class Arena;

    Allocation() = default;

    ArenaVector<Step> m_steps;
    ArenaVector<BlockSteps> m_blocks;
    ArenaVector<PatchAssignment> m_patchAssignments;
    ArenaVector<std::span<const RegisterBinding>> m_registersAtHead;
    // By Operation::index().
    ArenaVector<SpillIndex> m_spillOfValue;
    ArenaVector<Reg> m_registerAtDefinition;
    ArenaVector<SpillFacts> m_spills;
    ArenaVector<PatchOperation*> m_suspensions;
    ArenaVector<BitSpan> m_spillsHeldAtSuspension;
    RegisterMask m_usedCalleeSaves { 0 };
    uint32_t m_numberOfOperationsAllocated { 0 };
    bool m_needsFrameOutsideRareRegions { false };
    bool m_needsFrameAheadOfFramedPart { false };
    bool m_writesMemoryAheadOfFramedPart { false };
    bool m_holdsPatchThatFollowsFrameSetup { false };
    bool m_holdsPatchThatCountsOnStackCheck { false };
    bool m_holdsPatchThatCountsOnFrameRecord { false };
    BitSpan m_regionNeedsFrame;
    ArenaVector<HeadOfFramedPart> m_headsOfFramedPart;
    // By Block::index().
    BitSpan m_isInFramedPart;
    // By Block::index(), of the blocks in the framed part: the place in m_headsOfFramedPart.
    ArenaVector<uint32_t> m_headAboveBlock;
};

Allocation* allocateRegisters(Procedure&);

// One transfer of a set that happens at once: no source is read after a destination that overlaps it is written.
struct PendingMove {
    Operation* value;
    Location destination;
    // A register, a spill, an outgoing word, or Immediate for a value that is made again.
    Location source;
    Width width;
};

// Appends Move, Reload, Save, Materialize and Swap steps that have the effect of the set. No two moves have one destination.
// A cycle of registers is broken by Swap. From memory to memory goes through the scratch register of the bank.
// On x86-64, where r11 is the only scratch GPR and may be needed to reach an address, a word goes from memory to memory through the scratch FPR.
void resolveParallelMoves(Arena&, std::span<PendingMove>, ArenaVector<Step>& output);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
