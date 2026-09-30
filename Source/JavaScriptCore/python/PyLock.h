/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "JSObject.h"
#include "PyInstance.h"

namespace JSC {

// _thread.lock and _thread.RLock: PyMutex and _PyRecursiveMutex of CPython's Include/cpython/lock.h.
//
// To wait for one is to wait as Atomics.wait() does, for a word that is in the cell, and by the same means: WaiterListManager. So a thread that is waiting is known to the engine to be, and is woken if it is to be
// terminated.
class PyLock final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyLockSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    PYTHON_OVERLOADS_OPERATORS
    // RLock can be derived from. See PyType::createInstanceStructure().
    static CallData getCallData(JSCell*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static PyLock* create(VM&, Structure*);

    enum class Result : uint8_t {
        Acquired,
        TimedOut,
        Terminated, // The thread is to do no more. That has been thrown.
    };

    bool isLocked() const { return WTF::atomicLoad(const_cast<int32_t*>(&m_word), std::memory_order_relaxed) != Unlocked; }
    bool tryLock() { return WTF::atomicCompareExchangeStrong(&m_word, Unlocked, Locked) == Unlocked; }
    Result lock(VM&, Seconds timeout);
    // False if it was not locked.
    bool unlock();
    // Waits for it not to be locked, and leaves it so: PyEvent_WaitTimed(), for what is locked until something has happened.
    Result waitUntilUnlocked(VM&, Seconds timeout);
    // As it was when it was made, whatever may have been waiting for it.
    void reset();

    // For an RLock: which thread has it, and how many times over besides the first.
    uint64_t owner() const { return m_owner; }
    void setOwner(uint64_t owner) { m_owner = owner; }
    size_t level() const { return m_level; }
    void setLevel(size_t level) { m_level = level; }

private:
    PyLock(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    // "Futexes Are Tricky", Ulrich Drepper. To let go of one that nothing is waiting for is then to write a word.
    static constexpr int32_t Unlocked = 0;
    static constexpr int32_t Locked = 1;
    static constexpr int32_t LockedAndWaitedFor = 2;

    int32_t m_word { Unlocked };
    // What is known of who is waiting for a word is kept for as long as there is the word.
    bool m_hasBeenWaitedFor { false };
    uint64_t m_owner { 0 };
    size_t m_level { 0 };
};

} // namespace JSC
