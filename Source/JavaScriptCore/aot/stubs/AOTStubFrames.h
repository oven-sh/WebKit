/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubRegisters.h"
#include <array>

namespace JSC { namespace AOT {

enum class StubFrameKind : uint8_t {
    Plain,
    Adapter,
    ConstructAdapter,
    SavedFrameAdapter,
    // As Adapter, in the frame of the adapter of SavedFrames: it has called the handler of a job, which lies in the callee slot.
    HandlerOfJob,
    EngineCallee,
    HostCall,
    GeneratorBody,
    AsyncGeneratorBody,
    ArgumentList,
    // The caller's site has two positions, and the stub is at the first: the first of two reads, or the making of an Error that it then throws.
    FirstOfTwoReads,
    // The stub reads the method that the caller's site calls.
    MethodRead,
    // As Plain, in a frame whose stub has spilled the argument registers.
    BelowSpilledArguments,
    // As BelowSpilledArguments, in a PlainFunctionConstructFrame whose callee has been entered.
    PlainFunctionConstruct,
    // As BelowSpilledArguments, in a PlainFunctionConstructFrame whose callee has not been entered: the stub makes `this`. Bytecode does that in the frame of the callee, so a stack trace lists the callee.
    PlainFunctionConstructMakesThis,
    // As ConstructAdapter, before the callee is entered: the adapter makes `this`.
    ConstructAdapterMakesThis,
    // In a stub that has no frame record and has pushed nothing: its return address is on top of the stack.
    AtStubEntry,
    // The frame is one that emitHostCallFrame() has made, and the stub does in it what the native callee would do. A run of compiled frames ends at it, as at a frame of the engine.
    InHostCallFrame,
    // Like Plain, in a frame with a record: after the operation it called returns, the stub only returns to its caller.
    ReturnsAfterOperation,
};
inline constexpr unsigned numberOfStubFrameKinds = 19;
static_assert(static_cast<unsigned>(StubFrameKind::ReturnsAfterOperation) + 1 == numberOfStubFrameKinds);

constexpr bool hasSingleReturnSite(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::EngineCallee:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::ReturnsAfterOperation:
    case StubFrameKind::HandlerOfJob:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::MethodRead:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::PlainFunctionConstruct:
    case StubFrameKind::PlainFunctionConstructMakesThis:
    case StubFrameKind::ConstructAdapterMakesThis:
    case StubFrameKind::AtStubEntry:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// What lies between the return address of a frame and the stack pointer its caller had at the call: emitSpillArgumentRegisters() puts the registers in front of the caller's stack words.
constexpr unsigned bytesAboveReturnAddressOf(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::EngineCallee:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::PlainFunctionConstruct:
    case StubFrameKind::PlainFunctionConstructMakesThis:
        return sizeOfSpilledArgumentRegisters;
    case StubFrameKind::Plain:
    case StubFrameKind::ReturnsAfterOperation:
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::ConstructAdapterMakesThis:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::HandlerOfJob:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::MethodRead:
    case StubFrameKind::AtStubEntry:
    case StubFrameKind::InHostCallFrame:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct PlainFunctionConstructFrame {
    static constexpr ptrdiff_t offsetOfThis = -8;
    static constexpr ptrdiff_t offsetOfCallee = -16;
    static constexpr ptrdiff_t offsetOfWord = -24;
    static constexpr ptrdiff_t offsetOfCount = -32;
};

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
