/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Python {

// ---- hash() of str, bytes, and whatever else is hashed as the bytes it is made of ----
//
// SipHash-1-3 with a key that the process makes up when it starts, or makes from PYTHONHASHSEED: what CPython does, bit for bit, so
// that with the same seed a str has the same hash here and there, and a set of strs the same order. (Whoever can choose the keys
// of a dict, as the sender of a request can, must not be able to choose keys that collide: hence the key.)

// From configuration().hashSeed. The first call counts. Before any str or bytes is hashed.
void initializeHashSecret();

int64_t hashOfBytes(std::span<const uint8_t>);

// A str is hashed as CPython has it in memory: one, two or four bytes for each code point, whichever is the least that will do for
// all of them. How the text is kept here, in 8 or in 16 bits, makes no difference.
//
// It takes reading the whole text, every time: neither a JSString nor a StringImpl has room to remember the result in.
int64_t hashOfString(StringImpl*);

// What a dict makes of a hash.
ALWAYS_INLINE uint32_t foldHash(int64_t hash) { return static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(static_cast<uint64_t>(hash) >> 32); }

// ---- hash() of a tuple, given those of its items ----

class TupleHasher {
public:
    ALWAYS_INLINE void add(int64_t itemHash)
    {
        m_accumulator += static_cast<uint64_t>(itemHash) * prime2;
        m_accumulator = (m_accumulator << 31) | (m_accumulator >> 33);
        m_accumulator *= prime1;
        ++m_length;
    }
    ALWAYS_INLINE int64_t finish() const
    {
        uint64_t result = m_accumulator + (m_length ^ (prime5 ^ 3527539ULL));
        return result == static_cast<uint64_t>(-1) ? 1546275796 : static_cast<int64_t>(result);
    }
    // What a slice does.
    ALWAYS_INLINE int64_t finishWithoutLength() const { return m_accumulator == static_cast<uint64_t>(-1) ? 1546275796 : static_cast<int64_t>(m_accumulator); }

private:
    // xxHash, simplified.
    static constexpr uint64_t prime1 = 11400714785074694791ULL;
    static constexpr uint64_t prime2 = 14029467366897019727ULL;
    static constexpr uint64_t prime5 = 2870177450012600261ULL;
    uint64_t m_accumulator { prime5 };
    uint64_t m_length { 0 };
};

} // namespace Python
} // namespace JSC
