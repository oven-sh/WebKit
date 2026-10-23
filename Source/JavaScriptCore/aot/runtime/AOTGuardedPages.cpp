/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardedPages.h"

#if ENABLE(AOT)

#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
#include <sys/personality.h>
#endif

namespace JSC { namespace AOT {

#if ENABLE(AOT_GUARDED_PAGES)
static constexpr unsigned guardPagesOnEachEnd = 1;
#else
static constexpr unsigned guardPagesOnEachEnd = 0;
#endif

#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
// What the system places by itself shares one random base: an address of one mapping gives away the others, the C library's among them.
static void* tryReserveGuardedPagesAtRandomAddress(size_t bytes, OSAllocator::Usage usage)
{
    static constexpr uintptr_t lowestRandomAddress = 64 * GB;
    static constexpr uintptr_t highestRandomAddress = 124 * 1024 * GB;
    static constexpr unsigned maxDraws = 16;
    if (bytes >= highestRandomAddress - lowestRandomAddress)
        return nullptr;
    int persona = personality(0xffffffff);
    if (persona != -1 && persona & ADDR_NO_RANDOMIZE)
        return nullptr;
    uintptr_t numberOfSlots = (highestRandomAddress - lowestRandomAddress - bytes) / pageSize();
    for (unsigned draw = 0; draw < maxDraws; ++draw) {
        void* requestedAddress = std::bit_cast<void*>(lowestRandomAddress + cryptographicallyRandomNumber<uint64_t>() % numberOfSlots * pageSize());
        void* pages = OSAllocator::tryReserveAndCommit(bytes, usage, requestedAddress, true, false, false, guardPagesOnEachEnd);
        if (!pages)
            return nullptr;
        if (pages == requestedAddress)
            return pages;
        OSAllocator::releaseDecommitted(pages, bytes, guardPagesOnEachEnd);
    }
    return nullptr;
}
#endif

void* reserveGuardedPages(size_t bytes, OSAllocator::Usage usage)
{
#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
    if (void* pages = tryReserveGuardedPagesAtRandomAddress(bytes, usage))
        return pages;
#endif
    return OSAllocator::reserveAndCommit(bytes, usage, nullptr, true, false, false, guardPagesOnEachEnd);
}

void releaseGuardedPages(void* pages, size_t bytes)
{
    OSAllocator::releaseDecommitted(pages, bytes, guardPagesOnEachEnd);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
