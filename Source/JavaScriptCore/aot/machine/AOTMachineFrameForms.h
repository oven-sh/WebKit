/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"

namespace JSC { namespace AOT { namespace Machine {

struct FormOfFrame {
    bool hasFrameRecord { true };
    bool savesLieAtBottom { false };
    int32_t topOfSlots { 0 };
    int32_t offsetOfFirstSave { 0 };
    uint32_t bytesBelowFrameRecord { 0 };
};

FormOfFrame chooseFormOfFrame(const Procedure&, FrameCoverage, RegisterMask saves);

bool stepCountsOnFrameRecord(const Procedure&, const Step&);

inline int32_t offsetOfFirstSaveOf(const Frame& frame)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    if (frame.savesLieAtBottom())
        return -static_cast<int32_t>(frame.bytesBelowFrameRecord());
    return (frame.hasFrameRecord() ? 0 : wordSize) - wordSize * static_cast<int32_t>(frame.numberOfCalleeSaves());
}

inline unsigned numberOfWordsBesideSavesOf(const Frame& frame)
{
    constexpr unsigned wordSize = sizeof(CPURegister);
    return frame.bytesBelowFrameRecord() / wordSize + !frame.hasFrameRecord() - frame.numberOfCalleeSaves();
}

inline unsigned numberOfWordsAboveSlotsOf(const Frame& frame)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    int32_t top = frame.hasFrameRecord() ? 0 : wordSize;
    if (!frame.savesLieAtBottom())
        top -= wordSize * static_cast<int32_t>(frame.numberOfCalleeSaves());
    return static_cast<unsigned>((top - frame.topOfSlots()) / wordSize);
}

void emitFrameSetup(CCallHelpers&, const Procedure&, std::span<const GPRReg> registersPushedIntoFirstSlots);
void emitTeardownOfFrameThatPopsWords(CCallHelpers&, const Frame&, std::span<const GPRReg> registersPoppedOutOfWords);

#if CPU(X86_64)
void emitTeardownOfFrameInOtherForm(CCallHelpers&, const Frame&);
#endif

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
