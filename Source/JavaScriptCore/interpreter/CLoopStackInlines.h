/*
 * Copyright (C) 2012-2025 Apple Inc. All rights reserved.
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

#if ENABLE(C_LOOP)

#include "CLoopStack.h"
#include "CallFrame.h"
#include "CodeBlock.h"
#include "JSCConfig.h"
#include "StackManagerInlines.h"
#include "VM.h"
#include "VMLite.h"
#include "VMThreadContext.h"

namespace JSC {

ALWAYS_INLINE VM& CLoopStack::vm() const
{
    return stackManager().vm();
}

// Saves the interpreter SP into the calling thread's own segment (the
// offlineasm cloop emits this before every native/slow-path call; the
// segment's currentStackPointer/lastStackPointer are owner-exclusive while
// the owner lives — sanitizeStack() only touches the calling thread's own
// segment), and
// republishes this thread's stack limit if another thread published its own
// in the meantime (GIL handoff), so the asm fast-path stack checks compare
// against the running thread's segment. See the class comment in CLoopStack.h.
ALWAYS_INLINE void CLoopStack::setCurrentStackPointer(void* sp)
{
    ThreadState& state = threadState();
    state.currentStackPointer = sp;
    publishStackLimit(state);
}

inline bool CLoopStack::ensureCapacityFor(Register* newTopOfStack)
{
    ThreadState& state = threadState();
    bool success = true;
    if (newTopOfStack < state.end) [[unlikely]]
        success = grow(state, newTopOfStack);
    // This is the C++ slow path of the asm stack checks: republishing here
    // guarantees a stale published limit self-corrects on the first failing
    // check after a GIL handoff.
    publishStackLimit(state);
    return success;
}

ALWAYS_INLINE StackManager& CLoopStack::stackManager() const
{
    return *std::bit_cast<StackManager*>(std::bit_cast<uintptr_t>(this) - StackManager::offsetOfCLoopStack());
}

// See the declaration comment in CLoopStack.h: this must mirror the asm
// gilOffGroup3Check discriminator byte-for-byte (process byte -> lite
// presence -> lite->gilOff) so the slot C++ publishes is exactly the slot
// the rerouted asm stack checks read — the per-lite read leg is the
// VMLiteCLoopStackLimitOffset chained const used by the two `if C_LOOP`
// stack checks in LowLevelInterpreter64.asm (doVMEntry stack-height check
// and the arity check). GIL-off, the per-lite slot is strictly
// thread-local: its sole writer is setCLoopStackLimit() reached through
// this routing, and its sole fast-path reader is the owning thread's
// interpreter, so cross-segment limit comparisons cannot occur.
// The lite's StackManager is reached
// through threadContext directly (NOT perThreadTrapsIfExists, which returns
// null in the pre-registration window while lite->vm is still unset and
// would either deref null or fall back to the carrier slot the asm no
// longer reads); the asm storage selection has no lite->vm dependence
// either. The carrier's CLoopStack stays the single segment registry for
// GC scanning — only the limit publish is per-lite.
ALWAYS_INLINE StackManager& CLoopStack::publishTargetStackManager() const
{
    if (g_jscConfig.gilOffProcess) [[unlikely]] {
        if (VMLite* lite = VMLite::currentIfExists(); lite && lite->gilOff)
            return lite->threadContext.traps().cloopStack().stackManager();
    }
    return stackManager();
}

ALWAYS_INLINE void CLoopStack::publishStackLimit(ThreadState& state) const
{
    StackManager& manager = publishTargetStackManager();
    if (manager.cloopStackLimit() != static_cast<void*>(state.end)) [[unlikely]]
        manager.setCLoopStackLimit(state.end);
}

} // namespace JSC

#endif // ENABLE(C_LOOP)
