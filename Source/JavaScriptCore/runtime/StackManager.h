/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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

#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/SentinelLinkedList.h>

#if ENABLE(C_LOOP)
#include "CLoopStack.h"
#endif

namespace JSC {

class VM;

class StackManager {
public:
    class Mirror : public BasicRawSentinelNode<Mirror> {
    public:
        void* softStackLimit() const { return m_softStackLimit.loadRelaxed(); }
        void* trapAwareSoftStackLimit() const LIFETIME_BOUND { return m_trapAwareSoftStackLimit.loadRelaxed(); }

        static constexpr ptrdiff_t offsetOfSoftStackLimit()
        {
            return OBJECT_OFFSETOF(Mirror, m_softStackLimit);
        }

    private:
        Atomic<void*> m_trapAwareSoftStackLimit { nullptr };
        // V7: written by the owning StackManager under m_mirrorLock; read
        // lock-free by C++ (e.g. JSWebAssemblyInstance::softStackLimit) and
        // by generated code via offsetOfSoftStackLimit() — relaxed atomic
        // keeps the word raw-readable at the same offset (layout
        // static_assert below).
        Atomic<void*> m_softStackLimit { nullptr };

        friend class LLIntOffsetsExtractor;
        friend class StackManager;
    };

    void registerMirror(Mirror&);
    void unregisterMirror(Mirror&);

    bool hasStopRequest() { return trapAwareSoftStackLimit() == stopRequestMarker(); }
    CONCURRENT_SAFE void requestStop();
    CONCURRENT_SAFE void cancelStop();

    void* softStackLimit() const { return m_softStackLimit.loadRelaxed(); }
    void* trapAwareSoftStackLimit() const LIFETIME_BOUND { return m_trapAwareSoftStackLimit.loadRelaxed(); }

    void setStackSoftLimit(void* newLimit);

    static constexpr ptrdiff_t offsetOfSoftStackLimit()
    {
        return OBJECT_OFFSETOF(StackManager, m_softStackLimit);
    }

    // Generated code reads the word through this raw pointer (and through
    // offsetOfSoftStackLimit() above); the relaxed-atomic conversion keeps
    // size/offset identical (static_asserts below), so the baked pointer
    // load is unchanged and hardware-atomic.
    void** addressOfSoftStackLimit() { return reinterpret_cast<void**>(&m_softStackLimit); }

#if ENABLE(C_LOOP)
    void* cloopStackLimit() { return m_cloopStackLimit; }
    void setCLoopStackLimit(void* newStackLimit);
    ALWAYS_INLINE void* currentCLoopStackPointer() const { return m_cloopStack.currentStackPointer(); }

    CLoopStack& cloopStack() LIFETIME_BOUND { return m_cloopStack; }
    const CLoopStack& cloopStack() const LIFETIME_BOUND { return m_cloopStack; }

    static constexpr ptrdiff_t offsetOfCLoopStack()
    {
        return OBJECT_OFFSETOF(StackManager, m_cloopStack);
    }
#endif

    VM& vm();

    static constexpr uintptr_t StopRequestMarkerValue = std::numeric_limits<uintptr_t>::max();

private:
    static ALWAYS_INLINE void* stopRequestMarker() { return reinterpret_cast<void*>(StopRequestMarkerValue); }

    Atomic<void*> m_trapAwareSoftStackLimit { nullptr };
    // V7: the VM-level word is written by CARRIER entries
    // (VM::updateStackLimits) and read (a) lock-free by no-lite C++ fallback
    // readers and the per-lite null-fallback chain
    // (softStackLimitForCurrentThread / softStackLimitForCurrentThreadGilOffSlow,
    // VM::updateStackLimits' pre-read), (b) by generated code via the raw
    // pointer/offset accessors above, and (c) under m_mirrorLock by
    // cancelStop / registerMirror / the mirror fanouts. Relaxed atomic closes
    // the unlocked readers; the store now also happens under m_mirrorLock
    // (setStackSoftLimit) so the lock pairs with the locked readers and the
    // m_trapAwareSoftStackLimit restore in cancelStop observes a consistent
    // snapshot.
    Atomic<void*> m_softStackLimit { nullptr };

    Lock m_mirrorLock;
    SentinelLinkedList<Mirror, BasicRawSentinelNode<Mirror>> m_mirrors WTF_GUARDED_BY_LOCK(m_mirrorLock);

#if ENABLE(C_LOOP)
    // Read by the LLInt cloop stack checks. Flag-off (and GIL-on) the carrier
    // VM's instance is read via the VMCLoopStackLimitOffset chained const;
    // GIL-off the per-lite instance (VMTraps inside VMLite::threadContext) is
    // read via VMLiteCLoopStackLimitOffset (both consts are built from the
    // raw member through the LLIntOffsetsExtractor friendship below — no
    // offsetOf accessor needed). Per-lite, the slot is single-writer/
    // single-reader (the owning thread, via
    // CLoopStack::publishTargetStackManager routing); cross-thread stop
    // delivery uses m_trapAwareSoftStackLimit under m_mirrorLock, never this
    // slot.
    void* m_cloopStackLimit { nullptr };

    // m_cloopStack must be declared after the m_mirrors list because CLoopStack
    // initialization requires calling setCLoopStackLimit() which relies on
    // m_mirrors (above) being already initialized.
    CLoopStack m_cloopStack;
#endif
    friend class LLIntOffsetsExtractor;
};

static_assert(sizeof(Atomic<void*>) == sizeof(void*), "m_trapAwareSoftStackLimit relies on this invariant");
static_assert(sizeof(Atomic<void*>) == sizeof(void*), "baked JIT/LLInt loads through offsetOfSoftStackLimit()/addressOfSoftStackLimit() rely on m_softStackLimit staying one raw pointer word");
static_assert(std::atomic<void*>::is_always_lock_free);

} // namespace JSC
