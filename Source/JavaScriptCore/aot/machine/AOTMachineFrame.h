/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "RegisterAtOffsetList.h"

namespace JSC { namespace AOT { namespace Machine {

enum class FrameCoverage : uint8_t {
    // No path needs one.
    None,
    // Set up at every entrypoint, torn down in front of every Return and by every Patch that leaves the function.
    WholeFunction,
    // Set up where a rare region that needs one is entered, torn down where it is left. The code outside runs in the frame of the caller.
    RareRegions,
    // Set up at each of Allocation::headsOfFramedPart(), torn down where a way leaves the framed part, in front of every Return in it and by every Patch that leaves the function from it.
    // A rare region outside it sets up its own, as with RareRegions. The ways that leave the function without passing a head run in the frame of the caller.
    FramedPart,
};

// The usual form, from high addresses to low. x86-64 has two more: AOTMachineFrameForms.h.
//
//     return address                       on ARM64 stored with the frame pointer
//     frame pointer of the caller          <- the frame pointer register
//     callee saves                         one run, ascending by Reg::index(), the first at the lowest address
//     one unused word                      where the number of callee saves is odd, so that what follows starts at a multiple of 16
//     what SlotAssignment lays out
//     outgoing words                       <- the stack pointer, a multiple of stackAlignmentBytes()
//
// The stack pointer does not move between setup and teardown, and nothing lies below it.
class Frame {
    WTF_MAKE_NONCOPYABLE(Frame);
public:
    FrameCoverage coverage() const { return m_coverage; }
    bool hasFrame() const { return m_coverage != FrameCoverage::None; }
    // AOTMachineFrameForms.h. Without a record offsets from the frame pointer count from the place that the record would have, the word below the return address, which is the first word of such a frame.
    bool hasFrameRecord() const { return m_hasFrameRecord; }
    bool savesLieAtBottom() const { return m_savesLieAtBottom; }
    // As an offset from the frame pointer: one behind the highest byte of what SlotAssignment lays out.
    int32_t topOfSlots() const { return m_topOfSlots; }
    bool nothingCountsOnStackCheck() const { return m_nothingCountsOnStackCheck; }
    void setHasFrameRecordForTesting(bool hasFrameRecord) { m_hasFrameRecord = hasFrameRecord; }

    // Below the frame record. With the record it is a multiple of stackAlignmentBytes().
    unsigned bytesBelowFrameRecord() const { return m_bytesBelowFrameRecord; }
    // What FrameDescription::frameSizeInBytes and ImageFrame count: the same.
    unsigned frameSize() const { return m_bytesBelowFrameRecord; }
    // At the bottom: the outgoing words and what no suspension keeps.
    unsigned bytesDeadAtSuspensions() const { return m_bytesDeadAtSuspensions; }
    // The words that every setup clears, as offsets from the frame pointer: [begin, end). They are those from SlotAssignment::firstWordClearedAtSetup() on: as a rule none.
    int32_t beginOfWordsClearedAtSetup() const { return m_beginOfWordsClearedAtSetup; }
    int32_t endOfWordsClearedAtSetup() const { return m_endOfWordsClearedAtSetup; }

    RegisterMask calleeSavesMask() const { return m_calleeSaves; }
    RegisterSet calleeSaveRegisters() const { return registerSetOf(m_calleeSaves); }
    // Offsets from the frame pointer, in the order of the run.
    const RegisterAtOffsetList& calleeSaveRegisterAtOffsetList() const LIFETIME_BOUND { return m_calleeSaveOffsets; }
    unsigned numberOfCalleeSaves() const { return static_cast<unsigned>(m_calleeSaveOffsets.registerCount()); }
    // Those of the saved registers that a suspension does not keep.
    RegisterSet calleeSavesClobberedAtSuspensions() const { return registerSetOf(m_calleeSavesClobberedAtSuspensions); }

    // The registers that the allocator hands out in this function, whether or not it has.
    RegisterSet mutableRegs() const { return registerSetOf(m_mutableRegisters); }
    JS_EXPORT_PRIVATE bool isPinned(Reg) const;

    int32_t offsetFromFPOf(SpillIndex spill) const { return m_offsetOfSpill[spill]; }
    // For the tests that damage a sound frame and expect the checker to refuse it.
    void setOffsetFromFPForTesting(SpillIndex spill, int32_t offset) { m_offsetOfSpill[spill] = offset; }

    // With FrameCoverage::FramedPart alone.
    bool holdsStepsOf(const Block* block) const { return m_blocksInFrame.get(block->index()); }
    bool isSetUpAtHeadOf(const Block* block) const { return m_blocksThatSetUpFrame.get(block->index()); }
    // Ahead of the jump that ends the block.
    bool isTornDownAtEndOf(const Block* block) const { return m_blocksThatTearDownFrame.get(block->index()); }
    // The block is left without the frame, and its jump leads to a block that holds a Return and nothing else and that ways in the frame lead to as well.
    // That block runs in the frame, and the jump is written as a return.
    bool returnsInPlaceOfJumpAtEndOf(const Block* block) const { return m_blocksThatReturnInPlaceOfJump.get(block->index()); }
    void setHoldsStepsForTesting(const Block* block, bool holds) { holds ? m_blocksInFrame.set(block->index()) : m_blocksInFrame.clear(block->index()); }

    // Where the frame is set up. On ARM64 from the stack pointer: its offsets are not negative, so one instruction reaches 32 KB.
    // On x86-64 from the frame pointer, which needs no SIB byte, but for a word that one byte reaches from the stack pointer alone.
    MacroAssembler::Address addressInFrame(intptr_t offsetFromFP) const
    {
        ASSERT(hasFrame());
        intptr_t offsetFromSP = offsetFromFP + static_cast<intptr_t>(m_bytesBelowFrameRecord);
#if CPU(X86_64)
        if (m_hasFrameRecord && (isRepresentableAs<int8_t>(static_cast<int64_t>(offsetFromFP)) || !isRepresentableAs<int8_t>(static_cast<int64_t>(offsetFromSP))))
            return MacroAssembler::Address(MacroAssembler::framePointerRegister, safeCast<int32_t>(offsetFromFP));
#endif
        return MacroAssembler::Address(MacroAssembler::stackPointerRegister, safeCast<int32_t>(offsetFromSP));
    }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class Arena;
    friend Frame* planFrame(Procedure&);

    Frame() = default;

    FrameCoverage m_coverage { FrameCoverage::None };
    // StackCheckSite::nothingCountsOnCheck of every setup.
    bool m_nothingCountsOnStackCheck { false };
    bool m_hasFrameRecord { true };
    bool m_savesLieAtBottom { false };
    int32_t m_topOfSlots { 0 };
    unsigned m_bytesBelowFrameRecord { 0 };
    unsigned m_bytesDeadAtSuspensions { 0 };
    RegisterMask m_calleeSaves { 0 };
    RegisterMask m_calleeSavesClobberedAtSuspensions { 0 };
    RegisterMask m_mutableRegisters { 0 };
    RegisterAtOffsetList m_calleeSaveOffsets;
    // By SpillIndex.
    std::span<int32_t> m_offsetOfSpill;
    int32_t m_beginOfWordsClearedAtSetup { 0 };
    int32_t m_endOfWordsClearedAtSetup { 0 };
    // By Block::index().
    BitSpan m_blocksInFrame;
    BitSpan m_blocksThatSetUpFrame;
    BitSpan m_blocksThatTearDownFrame;
    BitSpan m_blocksThatReturnInPlaceOfJump;

    friend void emitFrameSetup(CCallHelpers&, const Procedure&);
};

// Chooses the coverage, fixes the set of callee saves, gives every spill and every StackSlot its offset, and publishes the dead words of suspensions and the words to clear there.
//  - WholeFunction if ProcedureTraits::setsUpFrameAtEntry(), if a Patch followsFrameSetup(), or if Allocation::needsFrameOutsideRareRegions().
//  - In a function that saves its frame, the saves are what a saved frame can describe. ARM64: the shortest run of even length that ends in x24, the last of suspensionClobberedGPRs, and holds every register used: none, x23 and x24, x21 to x24, or x19 to x24. x86-64: rbx, r15 or both.
//  - With FrameCoverage::RareRegions there are no callee saves: the allocator takes none there.
//  - FramedPart where the allocation has a framed part and nothing ahead of it needs a frame. Every setup saves every callee save, that of a rare region too.
//    In a function with StackSlots a way leaves the framed part only into a block that returns in the frame: an address in the frame may live as long as the function runs.
Frame* planFrame(Procedure&);

// From allocateRegisters() on. The block holds a Return and nothing else that makes code: no step but the Execute of a Nop, a Phi or an Upsilon, no move of an edge, no store of a spilled value.
// Whatever the Return reads, the edges into the block have brought to its register. planFrame() and the encoder both go by this.
bool plannedBlockOnlyReturns(const Procedure&, const Block*);

// Each writes at the place of the assembler and may write the scratch registers of AOTMachineTarget.h. None of them writes a register that holds an argument, a result or a value of the allocator.

// Pushes the record, moves the stack pointer, calls the stack check generator of the procedure, stores the callee saves, clears the few words that a suspension may keep unwritten and cannot clear itself.
// A frame of more than maxBytesAllocatedBeforeStackCheck bytes is checked first and moves the stack pointer behind the check: StackCheckSite::isAllocated.
// ARM64 stores in pairs. On x86-64 the record is `push rbp; mov rbp, rsp`, and a frame that is allocated in front of its check pushes the callee saves behind the record, ahead of the check.
// So does ARM64 where general registers are all that lies below the record: the pushes allocate the frame, and no instruction moves the stack pointer by itself.
JS_EXPORT_PRIVATE void emitFrameSetup(CCallHelpers&, const Procedure&);
// Restores the callee saves and pops the record. Behind it the stack pointer is what it was at the entry, and on ARM64 the link register holds the return address.
// The stack pointer stands where the setup left it: a frame that holds saved general registers and nothing else is popped word by word.
JS_EXPORT_PRIVATE void emitFrameTeardown(CCallHelpers&, const Frame&);
// The first half of the above alone, for a Patch that still reads the frame before it leaves.
JS_EXPORT_PRIVATE void emitCalleeSaveRestore(CCallHelpers&, const Frame&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
