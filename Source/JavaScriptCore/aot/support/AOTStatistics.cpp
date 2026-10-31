/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStatistics.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <wtf/PrintStream.h>
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

void Statistics::addPassTime(PassID pass, const PassTime& time)
{
    unsigned index = static_cast<unsigned>(pass);
    RELEASE_ASSERT(index < numberOfPasses);
    if (m_passTimes.isEmpty())
        m_passTimes.grow(numberOfPasses);
    PassTime& passTime = m_passTimes[index];
    passTime.wall += time.wall;
    passTime.cpu += time.cpu;
    passTime.runs += time.runs;
}

void Statistics::dumpPassTimes(PrintStream& out) const
{
    if (m_passTimes.isEmpty())
        return;
    for (PassID pass : allMembersOf<PassID>) {
        const PassTime& passTime = m_passTimes[static_cast<unsigned>(pass)];
        if (passTime.runs)
            out.println("  ", nameOf(pass), ": wall ", passTime.wall.milliseconds(), " ms, cpu ", passTime.cpu.milliseconds(), " ms, ", passTime.runs, " runs");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
