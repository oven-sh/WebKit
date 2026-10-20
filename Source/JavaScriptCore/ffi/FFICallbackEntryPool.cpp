/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "FFICallbackEntryPool.h"

#if USE(BUN_JSC_ADDITIONS)

#include "FFIStubs.h"
#include <mutex>
#include <wtf/Locker.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace FFI {

CallbackEntryPool& CallbackEntryPool::singleton()
{
    static LazyNeverDestroyed<CallbackEntryPool> pool;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        pool.construct();
    });
    return pool;
}

std::optional<CallbackEntryPool::Failure> CallbackEntryPool::prepare()
{
    if (m_didPrepare)
        return m_preparationFailure;
    m_didPrepare = true;

    const Stubs* stubs = Stubs::current();
    if (!stubs)
        m_preparationFailure = Failure::NoStubs;
    else if (stubs->callbackEntries().empty() || !stubs->prepareCallbackContext())
        m_preparationFailure = Failure::CannotPrepareContext;
    if (m_preparationFailure)
        return m_preparationFailure;

    m_stubs = stubs;
    m_callbacks = FixedVector<std::atomic<JSFFICallback*>>(stubs->callbackEntries().size());
    m_entriesInUse.ensureSize(m_callbacks.size());
    return std::nullopt;
}

std::expected<unsigned, CallbackEntryPool::Failure> CallbackEntryPool::acquire()
{
    Locker locker { m_lock };
    if (auto failure = prepare())
        return makeUnexpected(*failure);

    // A BitVector has as many bits as its words hold, so a clear bit may lie past the last entry.
    size_t entry = m_entriesInUse.findBit(0, false);
    if (entry >= m_callbacks.size())
        return makeUnexpected(Failure::NoFreeEntry);

    m_entriesInUse.quickSet(entry);
    return static_cast<unsigned>(entry);
}

void CallbackEntryPool::bind(unsigned entry, JSFFICallback& callback)
{
    ASSERT(!callbackAt(entry));
    m_callbacks[entry].store(&callback, std::memory_order_release);
}

void CallbackEntryPool::release(unsigned entry)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(m_entriesInUse.quickGet(entry));
    m_callbacks[entry].store(nullptr, std::memory_order_release);
    m_entriesInUse.quickClear(entry);
}

void* CallbackEntryPool::entrypoint(unsigned entry) const
{
    return m_stubs->callbackEntries()[entry];
}

unsigned CallbackEntryPool::numberOfEntries() const
{
    return static_cast<unsigned>(m_callbacks.size());
}

unsigned CallbackEntryPool::numberOfEntriesInUse()
{
    Locker locker { m_lock };
    return static_cast<unsigned>(m_entriesInUse.bitCount());
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
