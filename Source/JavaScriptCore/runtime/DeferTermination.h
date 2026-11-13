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
        m_vm.traps().deferTermination(deferAction);
    }
    
    ~DeferTermination()
    {
        m_vm.traps().undoDeferTermination(deferAction);
    }

private:
    VM& m_vm;
};

using DeferTerminationForAWhile = DeferTermination<VMTraps::DeferAction::DeferForAWhile>;

// A DeferTerminationForAWhile that its owner can end before the end of the scope. A function that defers
// termination while it changes the state of an object, and then calls user code (a setter), ends the deferral
// first: held across the call, it keeps that code from being terminated for as long as it runs.
class EndableDeferTerminationForAWhile {
    WTF_MAKE_NONCOPYABLE(EndableDeferTerminationForAWhile);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE EndableDeferTerminationForAWhile(VM& vm)
        : m_vm(vm)
    {
        m_vm.traps().deferTermination(VMTraps::DeferAction::DeferForAWhile);
    }

    ALWAYS_INLINE ~EndableDeferTerminationForAWhile()
    {
        if (!m_ended)
            m_vm.traps().undoDeferTermination(VMTraps::DeferAction::DeferForAWhile);
    }

    // A TerminationException that was pending when the deferral began is pending again after this.
    ALWAYS_INLINE void end()
    {
        ASSERT(!m_ended);
        m_ended = true;
        m_vm.traps().undoDeferTermination(VMTraps::DeferAction::DeferForAWhile);
    }

private:
    VM& m_vm;
    bool m_ended { false };
};

} // namespace JSC
