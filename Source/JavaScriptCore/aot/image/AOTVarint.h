/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <bit>
#include <cstring>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

class WriterOfReservedBytes {
public:
    explicit WriterOfReservedBytes(uint8_t* firstReservedByte)
        : m_end(firstReservedByte)
    {
    }

    uint8_t* end() const { return m_end; }

    void append(uint8_t byte) { *m_end++ = byte; }
    void append(std::span<const uint8_t> bytes)
    {
        if (bytes.empty())
            return;
        memcpy(m_end, bytes.data(), bytes.size());
        m_end += bytes.size();
    }

private:
    uint8_t* m_end;
};

inline void appendVarint(WriterOfReservedBytes& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

inline uint64_t readVarint(const uint8_t*& cursor)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        uint8_t byte = *cursor++;
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            break;
    }
    return value;
}

constexpr unsigned sizeOfVarint(uint64_t value)
{
    return static_cast<unsigned>((std::bit_width(value | 1) + 6) / 7);
}

static_assert(sizeOfVarint(0) == 1 && sizeOfVarint(0x7f) == 1 && sizeOfVarint(0x80) == 2 && sizeOfVarint(UINT32_MAX) == 5 && sizeOfVarint(UINT64_MAX) == 10);

constexpr uint64_t zigZagEncode(int64_t value) { return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63); }
constexpr int64_t zigZagDecode(uint64_t value) { return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1); }

static_assert(!zigZagEncode(0) && zigZagEncode(-1) == 1 && zigZagEncode(1) == 2 && zigZagEncode(INT64_MIN) == UINT64_MAX);
static_assert(!zigZagDecode(0) && zigZagDecode(1) == -1 && zigZagDecode(2) == 1 && zigZagDecode(UINT64_MAX) == INT64_MIN);
static_assert(zigZagDecode(zigZagEncode(INT64_MAX)) == INT64_MAX);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
