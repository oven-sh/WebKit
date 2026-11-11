/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "FFICallbackEntryPool.h"

#if ENABLE(FFI)

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

struct CallbackEntryPool::Block {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Block);

    explicit Block(unsigned numberOfEntries)
        : callbacks(numberOfEntries)
    {
    }

    Stubs::BlockOfCallbackEntries entries;
    FixedVector<std::atomic<JSFFICallback*>> callbacks;
};

std::optional<CallbackEntryPool::Failure> CallbackEntryPool::addBlock()
{
    bool isFirst = m_blocks.isEmpty();
    if (isFirst) {
        if (m_failureOfFirstBlock)
            return m_failureOfFirstBlock;
        m_stubs = Stubs::current();
        if (!m_stubs)
            return m_failureOfFirstBlock = Failure::NoStubs;
    }

    auto block = makeUnique<Block>(m_stubs->numberOfCallbackEntriesInBlock());
    std::optional<Stubs::BlockOfCallbackEntries> entries = m_stubs->tryMakeBlockOfCallbackEntries(block->callbacks.mutableSpan().data(), isFirst);
    if (!entries) {
        if (isFirst)
            return m_failureOfFirstBlock = Failure::CannotPrepareContext;
        return Failure::CannotMapMoreEntries;
    }
    block->entries = *entries;
    m_blocks.append(block.release());
    m_entriesInUse.ensureSize(m_blocks.size() * m_stubs->numberOfCallbackEntriesInBlock());
    return std::nullopt;
}

std::atomic<JSFFICallback*>& CallbackEntryPool::wordOf(unsigned entry) const
{
    unsigned entriesInBlock = m_stubs->numberOfCallbackEntriesInBlock();
    return m_blocks[entry / entriesInBlock]->callbacks[entry % entriesInBlock];
}

std::expected<unsigned, CallbackEntryPool::Failure> CallbackEntryPool::acquire()
{
    Locker locker { m_lock };
    size_t numberOfEntries = m_blocks.isEmpty() ? 0 : m_blocks.size() * m_stubs->numberOfCallbackEntriesInBlock();
    // A BitVector has as many bits as its words hold, so a clear bit may lie past the last entry.
    size_t entry = std::min(m_entriesInUse.findBit(m_firstEntryThatMayBeFree, false), numberOfEntries);
    if (entry == numberOfEntries) {
        if (auto failure = addBlock())
            return makeUnexpected(*failure);
    }

    m_entriesInUse.quickSet(entry);
    m_firstEntryThatMayBeFree = entry + 1;
    return static_cast<unsigned>(entry);
}

void CallbackEntryPool::bind(unsigned entry, JSFFICallback& callback)
{
    Locker locker { m_lock };
    ASSERT(!wordOf(entry).load(std::memory_order_relaxed));
    wordOf(entry).store(&callback, std::memory_order_release);
}

void CallbackEntryPool::release(unsigned entry)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(m_entriesInUse.quickGet(entry));
    wordOf(entry).store(nullptr, std::memory_order_release);
    m_entriesInUse.quickClear(entry);
    m_firstEntryThatMayBeFree = std::min<size_t>(m_firstEntryThatMayBeFree, entry);
}

void* CallbackEntryPool::entrypoint(unsigned entry) const
{
    Locker locker { m_lock };
    unsigned entriesInBlock = m_stubs->numberOfCallbackEntriesInBlock();
    return m_stubs->callbackEntry(m_blocks[entry / entriesInBlock]->entries, entry % entriesInBlock);
}

JSFFICallback* CallbackEntryPool::callbackAt(unsigned entry) const
{
    Locker locker { m_lock };
    return wordOf(entry).load(std::memory_order_acquire);
}

unsigned CallbackEntryPool::numberOfEntries() const
{
    Locker locker { m_lock };
    return m_blocks.isEmpty() ? 0 : static_cast<unsigned>(m_blocks.size() * m_stubs->numberOfCallbackEntriesInBlock());
}

} } // namespace JSC::FFI

#endif // ENABLE(FFI)
