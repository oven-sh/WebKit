/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <cstdint>
#include <span>

namespace JSC { namespace AOT {

class ParallelLoops;

class CodedTexts {
public:
    static constexpr unsigned textsInGroup = 8;

    CodedTexts() = default;
    JS_EXPORT_PRIVATE CodedTexts(std::span<const uint8_t> bytes, uint32_t count);

    uint32_t count() const { return m_count; }
    JS_EXPORT_PRIVATE Vector<uint8_t, 64> at(uint32_t number) const;

    static bool canHold(std::span<const uint8_t> text);
    JS_EXPORT_PRIVATE static Vector<uint8_t> encode(std::span<const std::span<const uint8_t>> ascendingTexts, const ParallelLoops&);

private:
    std::span<const uint8_t> m_bytes;
    uint32_t m_count { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
