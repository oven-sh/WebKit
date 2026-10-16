/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/SHA1.h>

#include <cstddef>
#include <wtf/HexNumber.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

#if USE(CF)
#include <wtf/cf/VectorCF.h>
#endif

#if !PLATFORM(COCOA) && CPU(X86_64) && OS(LINUX) && COMPILER(CLANG)
#define WTF_SHA1_USES_X86_EXTENSIONS 1
#include <cpuid.h>
#include <immintrin.h>
#elif !PLATFORM(COCOA) && CPU(ARM64) && COMPILER(CLANG) && defined(__ARM_FEATURE_SHA2)
#define WTF_SHA1_USES_ARM64_INSTRUCTIONS 1
#include <arm_neon.h>
#endif

namespace WTF {

#if PLATFORM(COCOA)

SHA1::SHA1()
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CC_SHA1_Init(&m_context);
ALLOW_DEPRECATED_DECLARATIONS_END
}

void SHA1::addBytes(std::span<const std::byte> input)
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CC_SHA1_Update(&m_context, input.data(), input.size());
ALLOW_DEPRECATED_DECLARATIONS_END
}

void SHA1::computeHash(Digest& hash)
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CC_SHA1_Final(hash.data(), &m_context);
ALLOW_DEPRECATED_DECLARATIONS_END
}

#else

// A straightforward SHA-1 implementation based on RFC 3174.
// http://www.ietf.org/rfc/rfc3174.txt
// The names of functions and variables (such as "a", "b", and "f") follow notations in RFC 3174.

static inline uint32_t f(int t, uint32_t b, uint32_t c, uint32_t d)
{
    ASSERT(t >= 0 && t < 80);
    if (t < 20)
        return (b & c) | ((~b) & d);
    if (t < 40)
        return b ^ c ^ d;
    if (t < 60)
        return (b & c) | (b & d) | (c & d);
    return b ^ c ^ d;
}

static inline uint32_t k(int t)
{
    ASSERT(t >= 0 && t < 80);
    if (t < 20)
        return 0x5a827999;
    if (t < 40)
        return 0x6ed9eba1;
    if (t < 60)
        return 0x8f1bbcdc;
    return 0xca62c1d6;
}

static inline uint32_t rotateLeft(int n, uint32_t x)
{
    ASSERT(n >= 0 && n < 32);
    return (x << n) | (x >> (32 - n));
}

static void processBlockGeneric(std::array<uint32_t, 5>& hash, const std::array<uint8_t, 64>& buffer)
{
    std::array <uint32_t, 80> w { };
    for (int t = 0; t < 16; ++t)
        w[t] = (buffer[t * 4] << 24) | (buffer[t * 4 + 1] << 16) | (buffer[t * 4 + 2] << 8) | buffer[t * 4 + 3];
    for (int t = 16; t < 80; ++t)
        w[t] = rotateLeft(1, w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16]);

    uint32_t a = hash[0];
    uint32_t b = hash[1];
    uint32_t c = hash[2];
    uint32_t d = hash[3];
    uint32_t e = hash[4];

    for (int t = 0; t < 80; ++t) {
        uint32_t temp = rotateLeft(5, a) + f(t, b, c, d) + e + w[t] + k(t);
        e = d;
        d = c;
        c = rotateLeft(30, b);
        b = a;
        a = temp;
    }

    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
}

#if defined(WTF_SHA1_USES_X86_EXTENSIONS)

#define WTF_SHA1_TARGET __attribute__((target("sha,sse4.1,ssse3")))

template<int function>
WTF_SHA1_TARGET static ALWAYS_INLINE void processTwentyRounds(unsigned firstGroup, std::array<__m128i, 4>& w, __m128i& abcd, __m128i& previous)
{
    for (unsigned group = firstGroup; group < firstGroup + 5; ++group) {
        __m128i& words = w[group & 3];
        if (group >= 4)
            words = _mm_sha1msg2_epu32(_mm_xor_si128(_mm_sha1msg1_epu32(words, w[(group + 1) & 3]), w[(group + 2) & 3]), w[(group + 3) & 3]);
        __m128i e = _mm_sha1nexte_epu32(previous, words);
        previous = abcd;
        abcd = _mm_sha1rnds4_epu32(abcd, e, function);
    }
}

WTF_SHA1_TARGET static void processBlockAccelerated(std::array<uint32_t, 5>& hash, const std::array<uint8_t, 64>& buffer)
{
    const __m128i byteSwapMask = _mm_set_epi64x(0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
    __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hash.data())), 0x1b);
    __m128i initialABCD = abcd;
    __m128i previous = _mm_set_epi32(static_cast<int>(rotateLeft(2, hash[4])), 0, 0, 0);
    std::array<__m128i, 4> w;
    for (unsigned i = 0; i < 4; ++i)
        w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&buffer[i * 16])), byteSwapMask);
    processTwentyRounds<0>(0, w, abcd, previous);
    processTwentyRounds<1>(5, w, abcd, previous);
    processTwentyRounds<2>(10, w, abcd, previous);
    processTwentyRounds<3>(15, w, abcd, previous);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(hash.data()), _mm_shuffle_epi32(_mm_add_epi32(abcd, initialABCD), 0x1b));
    hash[4] += rotateLeft(30, static_cast<uint32_t>(_mm_extract_epi32(previous, 3)));
}

static bool cpuSupportsSHA1()
{
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & 1u << 9) || !(ecx & 1u << 19))
        return false;
    return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & 1u << 29);
}

#elif defined(WTF_SHA1_USES_ARM64_INSTRUCTIONS)

static void processBlockAccelerated(std::array<uint32_t, 5>& hash, const std::array<uint8_t, 64>& buffer)
{
    uint32x4_t abcd = vld1q_u32(hash.data());
    uint32_t e = hash[4];
    std::array<uint32x4_t, 4> w;
    for (unsigned i = 0; i < 4; ++i)
        w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(&buffer[i * 16])));
    for (unsigned group = 0; group < 20; ++group) {
        uint32x4_t& words = w[group & 3];
        if (group >= 4)
            words = vsha1su1q_u32(vsha1su0q_u32(words, w[(group + 1) & 3], w[(group + 2) & 3]), w[(group + 3) & 3]);
        uint32x4_t wordsPlusK = vaddq_u32(words, vdupq_n_u32(k(group * 4)));
        uint32_t nextE = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        if (group < 5)
            abcd = vsha1cq_u32(abcd, e, wordsPlusK);
        else if (group >= 10 && group < 15)
            abcd = vsha1mq_u32(abcd, e, wordsPlusK);
        else
            abcd = vsha1pq_u32(abcd, e, wordsPlusK);
        e = nextE;
    }
    vst1q_u32(hash.data(), vaddq_u32(vld1q_u32(hash.data()), abcd));
    hash[4] += e;
}

static bool cpuSupportsSHA1() { return true; }

#endif

#if defined(WTF_SHA1_USES_X86_EXTENSIONS) || defined(WTF_SHA1_USES_ARM64_INSTRUCTIONS)
static bool canUseAcceleratedSHA1()
{
    if (!cpuSupportsSHA1())
        return false;
    std::array<uint32_t, 5> one { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    std::array<uint32_t, 5> other = one;
    std::array<uint8_t, 64> block;
    for (unsigned i = 0; i < block.size(); ++i)
        block[i] = static_cast<uint8_t>(i * 37 + 11);
    for (unsigned round = 0; round < 2; ++round) {
        processBlockGeneric(one, block);
        processBlockAccelerated(other, block);
    }
    return one == other;
}
#endif

SHA1::SHA1()
{
    reset();
}

void SHA1::addBytes(std::span<const std::byte> input)
{
    m_totalBytes += input.size();
    while (!input.empty()) {
        ASSERT(m_cursor < 64);
        size_t count = std::min(input.size(), m_buffer.size() - m_cursor);
        memcpySpan(std::as_writable_bytes(std::span { m_buffer }).subspan(m_cursor, count), input.first(count));
        input = input.subspan(count);
        m_cursor += count;
        if (m_cursor == 64)
            processBlock();
    }
}

void SHA1::computeHash(Digest& digest)
{
    finalize();

    for (size_t i = 0; i < 5; ++i) {
        // Treat hashValue as a big-endian value.
        uint32_t hashValue = m_hash[i];
        for (int j = 0; j < 4; ++j) {
            digest[4 * i + (3 - j)] = hashValue & 0xFF;
            hashValue >>= 8;
        }
    }

    reset();
}

void SHA1::finalize()
{
    ASSERT(m_cursor < 64);
    m_buffer[m_cursor++] = 0x80;
    if (m_cursor > 56) {
        // Pad out to next block.
        while (m_cursor < 64)
            m_buffer[m_cursor++] = 0x00;
        processBlock();
    }

    for (size_t i = m_cursor; i < 56; ++i)
        m_buffer[i] = 0x00;

    // Write the length as a big-endian 64-bit value.
    uint64_t bits = m_totalBytes * 8;
    for (int i = 0; i < 8; ++i) {
        m_buffer[56 + (7 - i)] = bits & 0xFF;
        bits >>= 8;
    }
    m_cursor = 64;
    processBlock();
}

void SHA1::processBlock()
{
    ASSERT(m_cursor == 64);
    m_cursor = 0;
#if defined(WTF_SHA1_USES_X86_EXTENSIONS) || defined(WTF_SHA1_USES_ARM64_INSTRUCTIONS)
    static const bool useAccelerated = canUseAcceleratedSHA1();
    if (useAccelerated) {
        processBlockAccelerated(m_hash, m_buffer);
        return;
    }
#endif
    processBlockGeneric(m_hash, m_buffer);
}

void SHA1::reset()
{
    m_cursor = 0;
    m_totalBytes = 0;
    m_hash[0] = 0x67452301;
    m_hash[1] = 0xefcdab89;
    m_hash[2] = 0x98badcfe;
    m_hash[3] = 0x10325476;
    m_hash[4] = 0xc3d2e1f0;

    // Clear the buffer after use in case it's sensitive.
    m_buffer.fill(0);
}

#endif

void SHA1::addUTF8Bytes(StringView string)
{
    if (string.containsOnlyASCII()) {
        if (string.is8Bit())
            addBytes(asBytes(string.span8()));
        else
            addBytes(asBytes(String::make8Bit(string.span16()).span8()));
    } else
        addBytes(std::as_bytes(string.utf8().span()));
}

#if USE(CF)
void SHA1::addUTF8Bytes(CFStringRef string)
{
    if (auto characters = CFStringGetASCIICStringSpan(string); characters.data()) {
        addBytes(std::as_bytes(characters));
        return;
    }

    constexpr size_t bufferSize = 1024;
    if (size_t length = CFStringGetLength(string); length <= bufferSize) {
        std::array<UInt8, bufferSize> buffer;
        CFIndex usedBufferLength = 0;
        CFStringGetBytes(string, CFRangeMake(0, length), kCFStringEncodingASCII, 0, false, buffer.data(), buffer.size(), &usedBufferLength);
        if (length == static_cast<size_t>(usedBufferLength)) {
            addBytes(std::span { buffer }.first(length));
            return;
        }
    }

    addUTF8Bytes(String(string));
}
#endif // USE(CF)

ASCIICString SHA1::hexDigest(const Digest& digest)
{
    return toHexCString(digest);
}

ASCIICString SHA1::computeHexDigest()
{
    Digest digest;
    computeHash(digest);
    return hexDigest(digest);
}

} // namespace WTF
