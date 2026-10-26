/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <atomic>
#include <memory>
#include <wtf/Noncopyable.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

// A thread gets back the storage it had before, so a lease writes no memory that another thread reads.
template<typename Storage, uint32_t capacity>
class StoragePool {
    WTF_MAKE_NONCOPYABLE(StoragePool);
public:
    StoragePool()
        : m_slots(capacity)
    {
        for (auto& slot : m_slots)
            slot.store(nullptr, std::memory_order_relaxed);
    }

    ~StoragePool()
    {
        for (auto& slot : m_slots)
            delete slot.load(std::memory_order_relaxed);
    }

    Storage& lease()
    {
        uint32_t home = static_cast<uint64_t>(Thread::currentSingleton().uid()) * slotsPerThread % capacity;
        for (uint32_t distance = 0; distance < capacity; ++distance) {
            uint32_t number = (home + distance) % capacity;
            auto& slot = m_slots[number];
            Storage* storage = slot.load(std::memory_order_acquire);
            if (!storage) {
                auto created = makeUnique<Storage>(number);
                if (slot.compare_exchange_strong(storage, created.get(), std::memory_order_release, std::memory_order_acquire))
                    return *created.release();
            }
            if (!storage->isLeased.load(std::memory_order_relaxed) && !storage->isLeased.exchange(true, std::memory_order_acquire))
                return *storage;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static void giveBack(Storage& storage)
    {
        storage.isLeased.store(false, std::memory_order_release);
    }

    Storage* at(uint32_t number) const { return m_slots[number].load(std::memory_order_acquire); }

    bool hasLeases() const
    {
        for (auto& slot : m_slots) {
            Storage* storage = slot.load(std::memory_order_acquire);
            if (storage && storage->isLeased.load(std::memory_order_acquire))
                return true;
        }
        return false;
    }

private:
    // Room for the leases that one thread holds at a time.
    static constexpr uint32_t slotsPerThread = 8;
    static_assert(capacity % 2, "Threads whose numbers differ by less than the capacity start at different slots.");

    Vector<std::atomic<Storage*>> m_slots;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
