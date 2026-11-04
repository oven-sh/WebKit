/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStatistics.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

static_assert(std::atomic<uint64_t>::is_always_lock_free);

Statistics::Statistics()
    : m_counters(new std::atomic<uint64_t>[numberOfStripes * countersPerStripe]())
{
}

NEVER_INLINE void Statistics::count(StatisticID id, uint64_t amount)
{
    if (!amount)
        return;
    ASSERT(m_counters);
    if (id == StatisticID::None)
        return;
    unsigned index = static_cast<unsigned>(id);
    RELEASE_ASSERT(index < numberOfStatistics);
    unsigned stripe = Thread::currentSingleton().uid() % numberOfStripes;
    m_counters[stripe * countersPerStripe + index].fetch_add(amount, std::memory_order_relaxed);
}

Vector<uint64_t> Statistics::values() const
{
    ASSERT(m_counters);
    Vector<uint64_t> sums(FillWith { }, numberOfStatistics, 0);
    for (unsigned stripe = 0; stripe < numberOfStripes; ++stripe) {
        for (unsigned index = 0; index < numberOfStatistics; ++index)
            sums[index] += m_counters[stripe * countersPerStripe + index].load(std::memory_order_relaxed);
    }
    return sums;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
