/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCellButterfly.h"
#include "MarkedBlockCage.h"
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct State;

struct SortedStorage {
    static constexpr uint32_t multiplier = 0x9e3779b1u;
    static constexpr unsigned shift = 24;
    static constexpr size_t numberOfEntries = size_t { 1 } << (32 - shift);

    static SortedStorage& ensure(State&);

    ALWAYS_INLINE JSCellButterfly* tryFind(const JSCellButterfly* shared) const
    {
        uintptr_t distance = std::bit_cast<uintptr_t>(shared) - cageBase;
        if (distance >= MarkedBlockCage::maxSize)
            return nullptr;
        uint32_t encoded = static_cast<uint32_t>(distance >> MarkedBlockCage::shift);
        uint64_t entry = entries[encoded * multiplier >> shift];
        if (entry >> 32 != encoded)
            return nullptr;
        return uncheckedDowncast<JSCellButterfly>(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)));
    }
    void remember(const JSCellButterfly* shared, const JSCellButterfly* sorted)
    {
        if (std::bit_cast<uintptr_t>(shared) - cageBase >= MarkedBlockCage::maxSize || std::bit_cast<uintptr_t>(sorted) - cageBase >= MarkedBlockCage::maxSize)
            return;
        uint32_t encoded = MarkedBlockCage::encode(cageBase, shared);
        entries[encoded * multiplier >> shift] = static_cast<uint64_t>(encoded) << 32 | MarkedBlockCage::encode(cageBase, sorted);
    }

    uint64_t entries[numberOfEntries];
    uintptr_t cageBase;
};
static_assert(std::is_trivially_default_constructible_v<SortedStorage>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
