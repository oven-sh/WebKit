/*
 * Copyright (C) 2015-2025 Apple Inc. All rights reserved.
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

#include "Debugger.h"
#include "EntryFrame.h"
#include "FuzzerAgent.h"
#include "JSLock.h"
#include "ProfilerDatabase.h"
#include "SideDataRepository.h"
#include "VM.h"
#include "VMLite.h"
#include "Watchdog.h"

#if ENABLE(C_LOOP)
#include "CLoopStackInlines.h"
#endif

namespace JSC {

inline ActiveScratchBufferScope::ActiveScratchBufferScope(ScratchBuffer* buffer, size_t activeScratchBufferSizeInJSValues)
    : m_scratchBuffer(buffer)
{
    // Tell GC mark phase how much of the scratch buffer is active during the call operation this scope is used in.
    if (m_scratchBuffer)
        m_scratchBuffer->setActiveLength(activeScratchBufferSizeInJSValues * sizeof(EncodedJSValue));
}

inline ActiveScratchBufferScope::~ActiveScratchBufferScope()
{
    // Tell the GC that we're not using the scratch buffer anymore.
    if (m_scratchBuffer)
        m_scratchBuffer->setActiveLength(0);
}

// UNGIL §A.2.2 (AB-17 item 3, C++-reader leg only): GIL-off, the soft stack
// limit is per-thread state on the current lite; the VM-level word serves
// only no-lite threads. This helper reads the PLAIN per-lite soft limit
// (StackManager::m_softStackLimit, dual-published by VM::updateStackLimits
// from the entering thread's own StackBounds) — deliberately NOT the
// trap-aware word: it changes which thread's OVERFLOW limit a C++ reader
// compares against, never trap observability. Trap delivery is the per-lite
// trap-aware word's job (item-3c stop fan + item-3b servicing dispatch, both
// LANDED in the AB-17 change, as is the LLInt/JIT generated-code reroute —
// those sites now read the per-lite word GIL-off; see the ACTIVATION
// CHECKLIST — STATUS block in VMTraps.h).
ALWAYS_INLINE void* softStackLimitForCurrentThread(const VM& vm)
{
    if (vm.gilOff()) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->gilOff && lite->vm == &vm) {
            // Null until this thread's first VMEntryScope publish; fall back
            // to the VM word then (matches pre-reroute behavior — a null
            // per-lite limit must not disable overflow detection).
            if (void* liteLimit = lite->threadContext.traps().softStackLimit())
                return liteLimit;
        }
    }
    return vm.softStackLimit();
}

bool VM::ensureJSStackCapacityFor(Register* newTopOfStack)
{
#if !ENABLE(C_LOOP)
    return newTopOfStack >= softStackLimitForCurrentThread(*this);
#else
    return cloopStack().ensureCapacityFor(newTopOfStack);
#endif

}

bool VM::isSafeToRecurseSoft() const
{
    bool safe = isSafeToRecurse(softStackLimitForCurrentThread(*this));
#if ENABLE(C_LOOP)
    safe = safe && cloopStack().isSafeToRecurse();
#endif
    return safe;
}

template<typename Func>
void VM::logEvent(CodeBlock* codeBlock, const char* summary, const Func& func)
{
    if (!m_perBytecodeProfiler) [[likely]]
        return;
    
    m_perBytecodeProfiler->logEvent(codeBlock, summary, func());
}

inline CallFrame* VM::topJSCallFrame() const
{
    // UNGIL §A.1.3 mode split: GIL-off the live topCallFrame/topEntryFrame
    // are the CURRENT lite's Group-3 words; the raw VM-block members are
    // inert spare storage (stale or another thread's frame — walking them
    // crashes in isZombieFrame, observed via VM::throwException on the
    // smoke.js recursive-hold throw). GIL-on/flag-off group3Primitives()
    // aliases the VM block, byte-identical behavior.
    const VMLitePrimitives& primitives = group3Primitives();
    CallFrame* frame = primitives.topCallFrame;
    if (!frame) [[unlikely]]
        return frame;
    if (!frame->isNativeCalleeFrame() && !frame->isZombieFrame()) [[likely]]
        return frame;
    EntryFrame* entryFrame = primitives.topEntryFrame;
    do {
        frame = frame->callerFrame(entryFrame);
        ASSERT(!frame || !frame->isZombieFrame());
    } while (frame && frame->isNativeCalleeFrame());
    return frame;
}

inline void VM::setFuzzerAgent(std::unique_ptr<FuzzerAgent>&& fuzzerAgent)
{
    RELEASE_ASSERT_WITH_MESSAGE(!m_fuzzerAgent, "Only one FuzzerAgent can be specified at a time.");
    m_fuzzerAgent = WTF::move(fuzzerAgent);
}

template<typename Func>
inline void VM::forEachDebugger(const Func& callback)
{
    if (m_debuggers.isEmpty()) [[likely]]
        return;

    for (auto* debugger = m_debuggers.head(); debugger; debugger = debugger->next())
        callback(*debugger);
}

template<typename Type, typename Functor>
Type& VM::ensureSideData(void* key, const Functor& functor)
{
    m_hasSideData = true;
    return sideDataRepository().ensure<Type>(this, key, functor);
}

inline std::optional<RefPtr<Thread>> VM::ownerThread() const { return m_apiLock->ownerThread(); }
inline std::optional<uint64_t> VM::ownerThreadUID() const { return m_apiLock->ownerThreadUID(); }

inline JSPropertyNameEnumerator* VM::emptyPropertyNameEnumerator()
{
    if (m_emptyPropertyNameEnumerator) [[likely]]
        return m_emptyPropertyNameEnumerator.get();
    return emptyPropertyNameEnumeratorSlow();
}

#define JSC_DEFINE_VM_LAZY_EXECUTABLE(_name) \
    inline NativeExecutable* VM::_name##Executable() \
    { \
        if (m_##_name##Executable) [[likely]] \
            return m_##_name##Executable.get(); \
        return _name##ExecutableSlow(); \
    }
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseResolvingFunctionResolve)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseResolvingFunctionReject)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseFirstResolvingFunctionResolve)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseFirstResolvingFunctionReject)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseResolvingFunctionResolveWithInternalMicrotask)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseResolvingFunctionRejectWithInternalMicrotask)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseCapabilityExecutor)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllFulfillFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllSlowFulfillFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllSettledFulfillFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllSettledRejectFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllSettledSlowFulfillFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAllSettledSlowRejectFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAnyRejectFunction)
JSC_DEFINE_VM_LAZY_EXECUTABLE(promiseAnySlowRejectFunction)
#undef JSC_DEFINE_VM_LAZY_EXECUTABLE

} // namespace JSC
