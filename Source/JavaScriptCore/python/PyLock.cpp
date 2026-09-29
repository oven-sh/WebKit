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


#include "config.h"
#include "PyLock.h"

#include "JSCInlines.h"
#include "WaiterListManager.h"

namespace JSC {

const ClassInfo PyLock::s_info = { "lock"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyLock) };

Structure* PyLock::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

PyLock* PyLock::create(VM& vm, Structure* structure)
{
    auto* lock = new (NotNull, allocateCell<PyLock>(vm)) PyLock(vm, structure);
    lock->finishCreation(vm);
    return lock;
}

void PyLock::destroy(JSCell* cell)
{
    auto* lock = static_cast<PyLock*>(cell);
    if (lock->m_hasBeenWaitedFor)
        WaiterListManager::singleton().unregister(reinterpret_cast<uint8_t*>(&lock->m_word), sizeof(m_word));
    lock->~PyLock();
}

PyLock::Result PyLock::lock(VM& vm, Seconds timeout)
{
    int32_t was = WTF::atomicCompareExchangeStrong(&m_word, Unlocked, Locked);
    if (was == Unlocked) [[likely]]
        return Result::Acquired;
    if (!timeout)
        return Result::TimedOut;
    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
    m_hasBeenWaitedFor = true;
    if (was != LockedAndWaitedFor)
        was = WTF::atomicExchange(&m_word, LockedAndWaitedFor);
    while (was != Unlocked) {
        switch (WaiterListManager::singleton().waitSync(vm, &m_word, LockedAndWaitedFor, deadline - MonotonicTime::now())) {
        case WaiterListManager::WaitSyncResult::Terminated:
            return Result::Terminated;
        case WaiterListManager::WaitSyncResult::TimedOut:
            return Result::TimedOut;
        case WaiterListManager::WaitSyncResult::OK:
        case WaiterListManager::WaitSyncResult::NotEqual:
            break;
        }
        was = WTF::atomicExchange(&m_word, LockedAndWaitedFor);
    }
    return Result::Acquired;
}

bool PyLock::unlock()
{
    int32_t was = WTF::atomicExchange(&m_word, Unlocked);
    if (was == LockedAndWaitedFor)
        WaiterListManager::singleton().notifyWaiter(&m_word, 1);
    return was != Unlocked;
}

PyLock::Result PyLock::waitUntilUnlocked(VM& vm, Seconds timeout)
{
    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
    while (true) {
        int32_t was = WTF::atomicCompareExchangeStrong(&m_word, Locked, LockedAndWaitedFor);
        if (was == Unlocked)
            return Result::Acquired;
        if (!timeout)
            return Result::TimedOut;
        m_hasBeenWaitedFor = true;
        switch (WaiterListManager::singleton().waitSync(vm, &m_word, LockedAndWaitedFor, deadline - MonotonicTime::now())) {
        case WaiterListManager::WaitSyncResult::Terminated:
            return Result::Terminated;
        case WaiterListManager::WaitSyncResult::TimedOut:
            return Result::TimedOut;
        case WaiterListManager::WaitSyncResult::OK:
            // There may be others waiting for the same, and only one is woken.
            WaiterListManager::singleton().notifyWaiter(&m_word, 1);
            break;
        case WaiterListManager::WaitSyncResult::NotEqual:
            break;
        }
    }
}

void PyLock::reset()
{
    WTF::atomicStore(&m_word, Unlocked);
    m_owner = 0;
    m_level = 0;
}

} // namespace JSC
