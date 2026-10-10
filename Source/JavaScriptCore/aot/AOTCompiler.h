/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTStubs.h"

namespace JSC {

class CCallHelpers;
class CodeBlock;
class JSScope;

namespace B3 { namespace Air {
class Code;
} }

namespace AOT {

struct CompiledCode;

void estimateFrequencies(B3::Procedure&);
CCallHelpers::JumpList emitStackCheck(CCallHelpers&, unsigned frameSize);
void emitStackOverflow(CCallHelpers&, StubCalls&, const CCallHelpers::JumpList&);
void dropPaddingAtEnd(CompiledFunctionInfo&, const void* start, B3::Air::Code&);
void usePinnedRegistersDirectly(B3::Air::Code&);

struct Node;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
