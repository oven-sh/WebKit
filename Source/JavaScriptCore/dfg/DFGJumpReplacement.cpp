/*
 * Copyright (C) 2013-2017 Apple Inc. All rights reserved.
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

#include "config.h"
#include "DFGJumpReplacement.h"

#if ENABLE(DFG_JIT)

#include "JSThreadsSafepoint.h"
#include "MacroAssembler.h"
#include "Options.h"

namespace JSC { namespace DFG {

void JumpReplacement::fire()
{
    // SPEC-jit I2: code patching only world-stopped when useJSThreads is on.
    JSThreadsSafepoint::assertPatchingIsSafe();
    dataLogLnIf(Options::dumpDisassembly(),
        "Firing jump replacement watchpoint from ", RawPointer(m_source.dataLocation()),
        " to ", RawPointer(m_destination.dataLocation()));
    MacroAssembler::replaceWithJump(m_source, m_destination);
}

void JumpReplacement::installVMTrapBreakpoint()
{
    // SPEC-jit I2 / M2b (review round 4, R4-3): this rewrites a REACHABLE
    // invalidation point with a VM-halt instruction from the VMTraps
    // signal-sender thread — asynchronous cross-thread code patching while
    // mutators may be executing the patched code, which I2 forbids outside a
    // stop-the-world window. The design defense is M2b forcing
    // Options::usePollingTraps() under useJSThreads (deferred to the Task-14
    // handoff; docs/threads/INTEGRATE-jit.md M2b). Until M2b lands, fail fast
    // here instead of silently violating I2. Same guard at the caller,
    // DFG::CommonData::installVMTrapBreakpoints.
    RELEASE_ASSERT(!Options::useJSThreads() || Options::usePollingTraps());
    dataLogLnIf(Options::dumpDisassembly(),
        "Inserting VMTrap breakpoint at ", RawPointer(m_source.dataLocation()));
#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    MacroAssembler::replaceWithVMHalt(m_source);
#else
    UNREACHABLE_FOR_PLATFORM();
#endif
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)

