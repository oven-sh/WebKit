/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyHash.h"

#include "PyCommandLine.h"
#include <bit>
#include <mutex>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

static_assert(std::endian::native == std::endian::little);

namespace {

// What SipHash starts from: the two halves of the key, each combined with two constants.
struct InitialState {
    uint64_t v0 { 0x736f6d6570736575ULL };
    uint64_t v1 { 0x646f72616e646f6dULL };
    uint64_t v2 { 0x6c7967656e657261ULL };
    uint64_t v3 { 0x7465646279746573ULL };
};

InitialState initialState;

// https://www.aumasson.jp/siphash/siphash.pdf, with one round for each block and three at the end.
class SipHash13 {
public:
    ALWAYS_INLINE SipHash13()
        : m_v0(initialState.v0)
        , m_v1(initialState.v1)
        , m_v2(initialState.v2)
        , m_v3(initialState.v3)
    {
    }

    // Eight bytes, the first the least significant.
    ALWAYS_INLINE void add(uint64_t block)
    {
        m_v3 ^= block;
        round();
        m_v0 ^= block;
    }

    // `rest`: the bytes that are left, fewer than eight.
    ALWAYS_INLINE int64_t finish(uint64_t rest, size_t lengthInBytes)
    {
        add(rest | (static_cast<uint64_t>(lengthInBytes) << 56));
        m_v2 ^= 0xff;
        round();
        round();
        round();
        auto result = static_cast<int64_t>((m_v0 ^ m_v1) ^ (m_v2 ^ m_v3));
        // -1 says that hashing failed.
        return result == -1 ? -2 : result;
    }

private:
    ALWAYS_INLINE void round()
    {
        m_v0 += m_v1;
        m_v2 += m_v3;
        m_v1 = std::rotl(m_v1, 13) ^ m_v0;
        m_v3 = std::rotl(m_v3, 16) ^ m_v2;
        m_v0 = std::rotl(m_v0, 32);
        m_v2 += m_v1;
        m_v0 += m_v3;
        m_v1 = std::rotl(m_v1, 17) ^ m_v2;
        m_v3 = std::rotl(m_v3, 21) ^ m_v0;
        m_v2 = std::rotl(m_v2, 32);
    }

    uint64_t m_v0;
    uint64_t m_v1;
    uint64_t m_v2;
    uint64_t m_v3;
};

} // namespace

void initializeHashSecret()
{
    static std::once_flag once;
    std::call_once(once, [] {
        uint8_t bytes[16] = { };
        if (auto seed = configuration().hashSeed) {
            // 0: no key. Otherwise the bytes come out of a linear congruential generator.
            if (uint32_t x = *seed) {
                for (auto& byte : bytes) {
                    x = x * 214013 + 2531011;
                    byte = static_cast<uint8_t>(x >> 16);
                }
            }
        } else
            cryptographicallyRandomValues(std::span<uint8_t> { bytes });
        uint64_t key[2];
        memcpy(key, bytes, sizeof(key));
        initialState.v0 ^= key[0];
        initialState.v1 ^= key[1];
        initialState.v2 ^= key[0];
        initialState.v3 ^= key[1];
    });
}

int64_t hashOfBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return 0;
    SipHash13 state;
    const uint8_t* position = bytes.data();
    size_t left = bytes.size();
    for (; left >= 8; position += 8, left -= 8) {
        uint64_t block;
        memcpy(&block, position, 8);
        state.add(block);
    }
    uint64_t rest = 0;
    if (left) {
        if (bytes.size() >= 8) {
            // The last eight bytes, of which some have been taken.
            memcpy(&rest, position + left - 8, 8);
            rest >>= 8 * (8 - left);
        } else {
            unsigned shift = 0;
            if (left & 4) {
                uint32_t four;
                memcpy(&four, position, 4);
                rest = four;
                position += 4;
                shift = 32;
            }
            if (left & 2) {
                uint16_t two;
                memcpy(&two, position, 2);
                rest |= static_cast<uint64_t>(two) << shift;
                position += 2;
                shift += 16;
            }
            if (left & 1)
                rest |= static_cast<uint64_t>(*position) << shift;
        }
    }
    return state.finish(rest, bytes.size());
}

// Text in 16 bits of which every unit would fit 8.
static int64_t hashOfNarrowUnits(std::span<const char16_t> units)
{
    if (units.empty())
        return 0;
    SipHash13 state;
    size_t i = 0;
    for (; i + 8 <= units.size(); i += 8) {
        uint64_t block = 0;
        for (unsigned j = 0; j < 8; ++j)
            block |= static_cast<uint64_t>(units[i + j]) << (8 * j);
        state.add(block);
    }
    uint64_t rest = 0;
    for (unsigned j = 0; i + j < units.size(); ++j)
        rest |= static_cast<uint64_t>(units[i + j]) << (8 * j);
    return state.finish(rest, units.size());
}

// Text with code points beyond 16 bits.
static int64_t hashOfCodePoints(std::span<const char16_t> units)
{
    SipHash13 state;
    size_t count = 0;
    uint64_t pending = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t codePoint = units[i];
        if (U16_IS_LEAD(codePoint) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            codePoint = U16_GET_SUPPLEMENTARY(codePoint, units[++i]);
        if (count++ & 1)
            state.add(pending | (static_cast<uint64_t>(codePoint) << 32));
        else
            pending = codePoint;
    }
    return state.finish(count & 1 ? pending : 0, count * 4);
}

static bool hasSurrogatePair(std::span<const char16_t> units)
{
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1]))
            return true;
    }
    return false;
}

int64_t hashOfString(StringImpl* impl)
{
    if (impl->is8Bit())
        return hashOfBytes(byteCast<uint8_t>(impl->span8()));
    auto units = impl->span16();
    if (WTF::charactersAreAllLatin1(units))
        return hashOfNarrowUnits(units);
    if (hasSurrogatePair(units))
        return hashOfCodePoints(units);
    return hashOfBytes({ reinterpret_cast<const uint8_t*>(units.data()), units.size() * sizeof(char16_t) });
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
