/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"

namespace JSC { namespace AOT { namespace Machine {

// A frame is named by the place of its frame record, the word below its return address, whether it has a record or not. Offsets from the frame pointer count from there.
// Beside the form of AOTMachineFrame.h x86-64 has two, from high addresses to low:
//
//     return address                               return address
//     frame pointer of the caller                  callee saves                    the last of the run at the place of the record
//     what SlotAssignment lays out                 what SlotAssignment lays out
//     one unused word, where needed                one unused word, where needed
//     callee saves            <- the stack pointer outgoing words                  <- the stack pointer
//
// In the first the setup pushes the saves last and the teardown pops them first: one or two bytes for the four of a load.
// In the second the frame pointer register keeps what the caller left in it, and every word is reached from the stack pointer.
struct FormOfFrame {
    bool hasFrameRecord { true };
    bool savesLieAtBottom { false };
    // One behind the highest byte of what SlotAssignment lays out.
    int32_t topOfSlots { 0 };
    // Of the first register of the run, which lies lowest.
    int32_t offsetOfFirstSave { 0 };
    uint32_t bytesBelowFrameRecord { 0 };
};

// For planFrame(), once the coverage and the saves are settled.
//  - No record where ProcedureTraits::mayOmitFrameRecord, the frame is not set up at entry by its traits, no Patch counts on the record, nothing names the FramePointer or a StackSlot,
//    and the setup allocates the frame in front of its stack check. An address off the stack pointer with an offset is a byte longer than one off the frame pointer; the record is two instructions at every entry.
//  - Else the saves at the bottom where there are saves and slots and no outgoing words, in a function that does not save its frame, and one byte of offset reaches every slot whichever form the frame has.
FormOfFrame chooseFormOfFrame(const Procedure&, FrameCoverage, RegisterMask saves);

// What the step writes takes the frame pointer register for the frame, or may: a Patch that says so, a generator that is told of a place in the frame, a use of the FramePointer or of a SlotBase.
// checkFrame() asks it of every step of a frame without a record.
bool stepCountsOnFrameRecord(const Procedure&, const Step&);

inline int32_t offsetOfFirstSaveOf(const Frame& frame)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    if (frame.savesLieAtBottom())
        return -static_cast<int32_t>(frame.bytesBelowFrameRecord());
    return (frame.hasFrameRecord() ? 0 : wordSize) - wordSize * static_cast<int32_t>(frame.numberOfCalleeSaves());
}

// The words that a setup allocates beside the record and the callee saves.
inline unsigned numberOfWordsBesideSavesOf(const Frame& frame)
{
    constexpr unsigned wordSize = sizeof(CPURegister);
    return frame.bytesBelowFrameRecord() / wordSize + !frame.hasFrameRecord() - frame.numberOfCalleeSaves();
}

// Of those: the ones above what SlotAssignment lays out.
inline unsigned numberOfWordsAboveSlotsOf(const Frame& frame)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    int32_t top = frame.hasFrameRecord() ? 0 : wordSize;
    if (!frame.savesLieAtBottom())
        top -= wordSize * static_cast<int32_t>(frame.numberOfCalleeSaves());
    return static_cast<unsigned>((top - frame.topOfSlots()) / wordSize);
}

// emitFrameSetup(), and the first words of what SlotAssignment lays out are pushed from these registers, the highest word first. Each holds what the code behind the setup would have stored there.
// With registers only on x86-64, in a frame that is allocated in front of its stack check, clears no word at its setups and saves general registers alone.
void emitFrameSetup(CCallHelpers&, const Procedure&, std::span<const GPRReg> registersPushedIntoFirstSlots);

#if CPU(X86_64)
// emitFrameTeardown() of a frame in one of the two forms above.
void emitTeardownOfFrameInOtherForm(CCallHelpers&, const Frame&);
#endif

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
