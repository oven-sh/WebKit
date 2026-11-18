/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <bit>
#include <cstdint>

namespace JSC { namespace AOT {

struct PrefetchStep {
    enum class Kind : uint8_t {
        Row,
        Element,
        Fields,
        Field,
        FirstElement,
        SecondElement,
        Storage,
    };

    static constexpr unsigned maxSlots = 64;
    static constexpr uint8_t noSlot = UINT8_MAX;
    static constexpr unsigned maxLines = 12;
    static constexpr unsigned maxRows = 8;
    static constexpr unsigned maxDepth = maxRows - 2;

    static constexpr PrefetchStep decode(uint32_t word) { return std::bit_cast<PrefetchStep>(word); }
    constexpr uint32_t encoded() const { return std::bit_cast<uint32_t>(*this); }

    Kind kind;
    uint8_t parent;
    uint8_t slotOrCount;
    uint8_t linesOrDistance;
};
static_assert(sizeof(PrefetchStep) == 4);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
