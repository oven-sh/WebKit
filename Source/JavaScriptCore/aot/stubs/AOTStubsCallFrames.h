/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubFrames.h"
#include "CCallHelpers.h"
#include "StackAlignment.h"
#include <array>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

struct EngineCalleeFrame {
#if CPU(X86_64)
    static constexpr std::array<AdapterFrame::SavedRegister, 2> savedRegisters { {
        { GPRInfo::numberTagRegister, -8 },
        { GPRInfo::notCellMaskRegister, -16 },
    } };
#elif CPU(ARM64)
    static constexpr std::array<AdapterFrame::SavedRegister, 0> savedRegisters { };
#endif
    static constexpr unsigned saveAreaSize = savedRegisters.size() * sizeof(CPURegister);
    static_assert(!(saveAreaSize % stackAlignmentBytes()));
};

void emitSpillArgumentRegisters(CCallHelpers&);
void emitDropSpilledArgumentRegisters(CCallHelpers&);
void emitReturnDroppingSpilledArgumentRegisters(CCallHelpers&);

void emitDropArgumentListFrames(CCallHelpers&, GPRReg scratch);
void emitMoveStackWordsToArgumentListFrame(CCallHelpers&, unsigned numberOfWords, unsigned bytesAboveFrameRecord);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
