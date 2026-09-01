/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include "VM.h"
#include "VMEntryScope.h"
#include "VMLite.h"

namespace JSC {

// UNGIL §A.1.5 re-key (review fix; closes the VMEntryScope.cpp
// "OPEN-BLOCKER — HARD ACTIVATION BLOCKER"): GIL-off, the per-entry record
// lives ONLY on the CURRENT lite (the VM-member shadow is never written —
// with N concurrently-entered threads it would be a last-writer-wins race),
// so the ctor/dtor fast-path gates must consult the lite record, not the raw
// vm.entryScope shadow. Without this, every gilOff entry reached setUpSlow
// (tripping its RELEASE_ASSERT on any nested entry — host callbacks,
// VM::drainMicrotasks' spawned arm, dispatchStopHandler), and the dtor gate
// was always false, so tearDownSlow never ran and lite.entryScope dangled
// (isAnyThreadEntered() stuck true; W3 watchdog misfires; UAF risk).
// GIL-on/flag-off: m_gilOff is false — the landed single-shadow gates,
// one predicted-false byte test added.

ALWAYS_INLINE VMEntryScope::VMEntryScope(VM& vm, JSGlobalObject* globalObject)
    : m_vm(vm)
    , m_globalObject(globalObject)
{
    if (vm.gilOff()) [[unlikely]] {
        // Relaxed: own-thread record (the only writer is this thread).
        if (!VMLite::current().entryScope.load(std::memory_order_relaxed))
            setUpSlow();
    } else if (!vm.entryScope)
        setUpSlow();
    vm.clearLastException();
}

ALWAYS_INLINE VMEntryScope::~VMEntryScope()
{
    if (m_vm.gilOff()) [[unlikely]] {
        if (VMLite::current().entryScope.load(std::memory_order_relaxed) != this)
            return;
        tearDownSlow();
        return;
    }
    if (m_vm.entryScope != this)
        return;
    tearDownSlow();
}

} // namespace JSC
