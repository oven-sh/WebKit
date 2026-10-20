/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubRegisters.h"
#include <array>

namespace JSC { namespace AOT {

enum class StubFrameKind : uint8_t {
    Plain,
    Adapter,
    ConstructAdapter,
    SavedFrameAdapter,
    EngineCallee,
    HostCall,
    GeneratorBody,
    ArgumentList,
    FirstOfTwoReads,
};
inline constexpr unsigned numberOfStubFrameKinds = 9;
static_assert(static_cast<unsigned>(StubFrameKind::FirstOfTwoReads) + 1 == numberOfStubFrameKinds);

constexpr bool hasSingleReturnSite(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::ArgumentList:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::FirstOfTwoReads:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct AdapterFrame {
    struct SavedRegister {
        GPRReg reg;
        ptrdiff_t offset;
    };

#if CPU(X86_64)
    static constexpr std::array<SavedRegister, 5> savedRegisters { {
        { GPRInfo::notCellMaskRegister, -8 },
        { GPRInfo::numberTagRegister, -16 },
        { instanceGPR, -24 },
        { X86Registers::r12, -32 },
        { X86Registers::ebx, -40 },
    } };
    static constexpr ptrdiff_t offsetOfInstance = -48;
    static constexpr unsigned saveAreaSize = 64;
#elif CPU(ARM64)
    static constexpr std::array<SavedRegister, 4> savedRegisters { {
        { GPRInfo::notCellMaskRegister, -8 },
        { GPRInfo::numberTagRegister, -16 },
        { instanceGPR, -24 },
        { cellCageBaseGPR, -32 },
    } };
    static constexpr ptrdiff_t offsetOfInstance = -40;
    static constexpr unsigned saveAreaSize = 48;
    static_assert(savedRegisters[0].offset == savedRegisters[1].offset + 8);
    static_assert(savedRegisters[3].offset == offsetOfInstance + 8);
#endif
    static constexpr ptrdiff_t offsetOfNewTarget = offsetOfInstance - 8;
    static_assert(!(saveAreaSize % 16) && -offsetOfNewTarget <= static_cast<ptrdiff_t>(saveAreaSize));
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
