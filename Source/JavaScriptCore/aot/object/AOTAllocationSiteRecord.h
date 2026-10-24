/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace JSC { namespace AOT {

// The allocator is never null. Where there is none yet it is one without room, so that the test for room, which every allocation makes, is the only one.
struct AllocationSiteRecord {
    uint64_t header;
    uint64_t orderAndShapeWords;
    void* allocator;
    uint64_t unused;

    static constexpr ptrdiff_t offsetOfHeader() { return offsetof(AllocationSiteRecord, header); }
    static constexpr ptrdiff_t offsetOfOrderAndShapeWords() { return offsetof(AllocationSiteRecord, orderAndShapeWords); }
    static constexpr ptrdiff_t offsetOfAllocator() { return offsetof(AllocationSiteRecord, allocator); }
    static constexpr unsigned sizeShift = 5;
};
static_assert(sizeof(AllocationSiteRecord) == 1u << AllocationSiteRecord::sizeShift);
static_assert(std::is_trivially_default_constructible_v<AllocationSiteRecord> && std::is_standard_layout_v<AllocationSiteRecord>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
