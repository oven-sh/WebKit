/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
#include "VMTrapsInlines.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC {

template<VMTraps::DeferAction deferAction = VMTraps::DeferAction::DeferUntilEndOfScope>
class DeferTermination {
    WTF_MAKE_NONCOPYABLE(DeferTermination);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    DeferTermination(VM& vm)
        : m_vm(vm)
    {
        // FIX (stw-watchdog-timeout round, deferral family): deferral is a
        // property of the DEFERRING THREAD's stack — GIL-off the count lives
        // in the current thread's per-lite VMTraps instance
        // (trapsForCurrentThread(); same reroute as DeferTraps, see
        // VMTrapsInlines.h). The shared VM-level count raced N threads'
        // increments (the m_deferTerminationCount == 1 assert in
        // deferTerminationSlow) and one thread's deferral masked
        // NeedTermination for every sibling. The dtor resolves the SAME
        // instance on the same thread (RAII stack scope). GIL-on / flag-off:
        // trapsForCurrentThread() == vm.traps(), byte-identical.
        m_vm.trapsForCurrentThread().deferTermination(deferAction);
    }

    ~DeferTermination()
    {
        m_vm.trapsForCurrentThread().undoDeferTermination(deferAction);
    }

private:
    VM& m_vm;
};

using DeferTerminationForAWhile = DeferTermination<VMTraps::DeferAction::DeferForAWhile>;

} // namespace JSC
