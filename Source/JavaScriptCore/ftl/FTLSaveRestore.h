/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(FTL_JIT)

#include "FPRInfo.h"
#include "GPRInfo.h"
#include "Reg.h"
#include <wtf/ScopedLambda.h>

namespace JSC {

class AssemblyHelpers;

namespace FTL {

size_t NODELETE requiredScratchMemorySizeInBytes();

size_t NODELETE offsetOfReg(Reg);
size_t NODELETE offsetOfGPR(GPRReg);
size_t NODELETE offsetOfFPR(FPRReg);

// Assumes that top-of-stack can be used as a pointer-sized scratchpad. Saves all of
// the registers into the scratch buffer such that RegisterID * sizeof(int64_t) is the
// offset of every register.
void saveAllRegisters(AssemblyHelpers& jit, char* scratchMemory);

void restoreAllRegisters(AssemblyHelpers& jit, char* scratchMemory);

// ---- UNGIL §A.1.6 (ANNEX A16, BINDING) — U-T4b FTL/OSR emission helpers.
// GIL-off, a scratch-buffer ADDRESS baked into shared code would be used by N
// threads at once. Every baked site instead bakes a process-wide
// ScratchBufferRegistry INDEX and emits the frozen addressing sequence
//     loadVMLite -> segment -> [index] (-> + offsetof(ScratchBuffer, m_buffer))
// against the CURRENT thread's VMLite. Rematerialization is the correctness
// carrier (§A.1.2): callers may (and do) re-emit this sequence per use. The
// install/backfill contract (VM::allocateBakedScratchBufferIndex /
// VMLite::backfillBakedScratchBuffers) guarantees a buffer exists at
// (lite, index) before any code emitted against the index runs, so the
// emitted loads are not null-checked. The two loads are address-dependent
// (segment pointer feeds the entry load), which orders them against the
// release-publishing install on all supported targets.
// Each helper clobbers ONLY `dest`.

// dest <- ScratchBuffer* at (current lite, bakedIndex).
void materializeBakedScratchBufferPointer(AssemblyHelpers&, unsigned bakedIndex, GPRReg dest);
// dest <- that buffer's dataBuffer().
void materializeBakedScratchBufferDataPointer(AssemblyHelpers&, unsigned bakedIndex, GPRReg dest);

// gilOff variants of save/restoreAllRegisters: instead of baking the scratch
// base as an immediate, `materializeBase` emits code producing it in the GPR
// it is handed (it must clobber only that GPR). Emitted code is otherwise
// identical to the char* forms.
void saveAllRegisters(AssemblyHelpers&, const WTF::ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase);
void restoreAllRegisters(AssemblyHelpers&, const WTF::ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase);

// gilOff replacement for
// AssemblyHelpers::restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(vm.topEntryFrame):
// identical restore sequence, but topEntryFrame is the CURRENT lite's
// per-thread slot (UNGIL §A.1.3 Group-3 rerouting) instead of the baked
// &vm.topEntryFrame.
void restoreCalleeSavesFromCurrentVMLiteEntryFrameCalleeSavesBuffer(AssemblyHelpers&);

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)
