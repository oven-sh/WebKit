/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTStubs.h"
#include <wtf/ScopedLambda.h>

namespace JSC {

class CCallHelpers;
class CodeBlock;
class JSScope;

namespace B3 {
class Procedure;
namespace Air {
class Code;
}
}

namespace AOT {

struct CompiledCode;

void estimateFrequencies(B3::Procedure&);
CCallHelpers::JumpList emitStackCheck(CCallHelpers&, unsigned frameSize);
void emitStackOverflow(CCallHelpers&, StubCalls&, const CCallHelpers::JumpList&, unsigned frameSize);
#if CPU(ARM64)
void pushCalleeSaves(CCallHelpers&, const RegisterAtOffsetList&);
void popCalleeSaves(CCallHelpers&, const RegisterAtOffsetList&);
#endif
void emitCollapsibleFrameReturn(CCallHelpers&);
void dropCollapsibleCallerFrames(CCallHelpers&, GPRReg scratch);
CCallHelpers::Jump branchIfReturnAddressIsNotInCollapsibleFrame(CCallHelpers&, GPRReg returnAddress);
#if CPU(ARM64)
static constexpr uint64_t droppableFrameSignature = 0xaa1e03f1aa1e03feull;
static constexpr unsigned droppableFrameSignatureOffset = 4;
static constexpr unsigned droppableFrameExitOffset = 8;
#else
static constexpr uint64_t droppableFrameSignature = 0x504f524424841f0full;
static constexpr unsigned droppableFrameSignatureOffset = 2;
static constexpr unsigned droppableFrameExitOffset = 10;
#endif
void emitDroppableFrameExit(CCallHelpers&, const ScopedLambda<void()>& emitFrameTeardown);
void dropDroppableCallerFrame(CCallHelpers&);
void dropPaddingAtEnd(CompiledFunctionInfo&, const void* start, B3::Air::Code&);

struct Node;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
