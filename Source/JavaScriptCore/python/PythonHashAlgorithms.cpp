/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonHashAlgorithms.h"

#include <bit>
#include <utility>

namespace JSC { namespace Python {

namespace {

ALWAYS_INLINE uint32_t load32LittleEndian(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
ALWAYS_INLINE uint32_t load32BigEndian(const uint8_t* p) { return static_cast<uint32_t>(p[3]) | static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[0]) << 24; }
ALWAYS_INLINE uint64_t load64LittleEndian(const uint8_t* p) { return static_cast<uint64_t>(load32LittleEndian(p)) | static_cast<uint64_t>(load32LittleEndian(p + 4)) << 32; }
ALWAYS_INLINE uint64_t load64BigEndian(const uint8_t* p) { return static_cast<uint64_t>(load32BigEndian(p + 4)) | static_cast<uint64_t>(load32BigEndian(p)) << 32; }

template<typename Word> ALWAYS_INLINE Word loadLittleEndian(const uint8_t* p)
{
    if constexpr (sizeof(Word) == 8)
        return load64LittleEndian(p);
    else
        return load32LittleEndian(p);
}

// What is given is written out once for each of so many, and is told which it is in a way that the compiler knows of. So which word a step takes, and how far it turns it, are part of the instruction and are not looked up, and
// what one step hands on to the next is handed on by being called something else.
template<size_t count, typename Function>
ALWAYS_INLINE void unrolled(const Function& function)
{
    [&]<size_t... indices>(std::index_sequence<indices...>) ALWAYS_INLINE_LAMBDA {
        (function(std::integral_constant<size_t, indices>()), ...);
    }(std::make_index_sequence<count>());
}

// The first so many bytes of some words.
template<typename Word>
void storeWords(std::span<uint8_t> out, std::span<const Word> words, bool isBigEndian)
{
    for (size_t i = 0; i < out.size(); ++i) {
        size_t byte = i % sizeof(Word);
        out[i] = static_cast<uint8_t>(words[i / sizeof(Word)] >> (8 * (isBigEndian ? sizeof(Word) - 1 - byte : byte)));
    }
}

} // anonymous namespace

// ---- MD5: RFC 1321

MD5Hash::MD5Hash()
{
    m_state = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
}

void MD5Hash::compress(const uint8_t* block)
{
    // floor(2^32 * abs(sin(i + 1)))
    static constexpr uint32_t table[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static constexpr uint8_t shifts[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };

    uint32_t words[16];
    for (unsigned i = 0; i < 16; ++i)
        words[i] = load32LittleEndian(block + 4 * i);
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    unrolled<64>([&] (auto index) ALWAYS_INLINE_LAMBDA {
        constexpr size_t i = index;
        uint32_t f;
        size_t g;
        if constexpr (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if constexpr (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if constexpr (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t next = b + std::rotl(a + f + table[i] + words[g], shifts[i / 16][i % 4]);
        a = d;
        d = c;
        c = b;
        b = next;
    });
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
}

void MD5Hash::digest(std::span<uint8_t, digestSize> out) const
{
    auto state = finished(8, false);
    storeWords<uint32_t>(out, state, false);
}

// ---- SHA-1: FIPS 180-4, 6.1

SHA1Hash::SHA1Hash()
{
    m_state = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
}

void SHA1Hash::compress(const uint8_t* block)
{
    uint32_t words[80];
    for (unsigned i = 0; i < 16; ++i)
        words[i] = load32BigEndian(block + 4 * i);
    for (unsigned i = 16; i < 80; ++i)
        words[i] = std::rotl(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
    unrolled<80>([&] (auto index) ALWAYS_INLINE_LAMBDA {
        constexpr size_t i = index;
        uint32_t f, k;
        if constexpr (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if constexpr (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if constexpr (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t next = std::rotl(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = next;
    });
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void SHA1Hash::digest(std::span<uint8_t, digestSize> out) const
{
    auto state = finished(8, true);
    storeWords<uint32_t>(out, state, true);
}

// ---- SHA-224 and SHA-256: FIPS 180-4, 6.2 and 6.3

SHA256Hash::SHA256Hash(size_t digestSize)
    : m_digestSize(digestSize)
{
    ASSERT(digestSize == 28 || digestSize == 32);
    if (digestSize == 28)
        m_state = { 0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4 };
    else
        m_state = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
}

void SHA256Hash::compress(const uint8_t* block)
{
    static constexpr uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    // Only the last sixteen are gone by, so that is how many are kept, each written over by the one that is sixteen on from it. There are registers enough for that many.
    uint32_t words[16];
    for (unsigned i = 0; i < 16; ++i)
        words[i] = load32BigEndian(block + 4 * i);
    auto v = m_state;
    unrolled<64>([&] (auto index) ALWAYS_INLINE_LAMBDA {
        constexpr size_t i = index;
        if constexpr (i >= 16) {
            uint32_t before15 = words[(i + 1) % 16];
            uint32_t before2 = words[(i + 14) % 16];
            words[i % 16] += (std::rotr(before15, 7) ^ std::rotr(before15, 18) ^ (before15 >> 3)) + words[(i + 9) % 16] + (std::rotr(before2, 17) ^ std::rotr(before2, 19) ^ (before2 >> 10));
        }
        uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
        uint32_t choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + choice + constants[i] + words[i % 16];
        uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
        uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + majority;
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    });
    for (unsigned i = 0; i < 8; ++i)
        m_state[i] += v[i];
}

void SHA256Hash::digest(std::span<uint8_t> out) const
{
    ASSERT(out.size() == m_digestSize);
    auto state = finished(8, true);
    storeWords<uint32_t>(out, state, true);
}

// ---- SHA-384 and SHA-512: FIPS 180-4, 6.4 and 6.5

SHA512Hash::SHA512Hash(size_t digestSize)
    : m_digestSize(digestSize)
{
    ASSERT(digestSize == 48 || digestSize == 64);
    if (digestSize == 48)
        m_state = { 0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };
    else
        m_state = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
}

void SHA512Hash::compress(const uint8_t* block)
{
    static constexpr uint64_t constants[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
        0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
        0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
        0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
        0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
        0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
        0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull, 0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
        0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
        0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
    };
    // Only the last sixteen are gone by, so that is how many are kept, each written over by the one that is sixteen on from it. There are registers enough for that many.
    uint64_t words[16];
    for (unsigned i = 0; i < 16; ++i)
        words[i] = load64BigEndian(block + 8 * i);
    auto v = m_state;
    unrolled<80>([&] (auto index) ALWAYS_INLINE_LAMBDA {
        constexpr size_t i = index;
        if constexpr (i >= 16) {
            uint64_t before15 = words[(i + 1) % 16];
            uint64_t before2 = words[(i + 14) % 16];
            words[i % 16] += (std::rotr(before15, 1) ^ std::rotr(before15, 8) ^ (before15 >> 7)) + words[(i + 9) % 16] + (std::rotr(before2, 19) ^ std::rotr(before2, 61) ^ (before2 >> 6));
        }
        uint64_t s1 = std::rotr(v[4], 14) ^ std::rotr(v[4], 18) ^ std::rotr(v[4], 41);
        uint64_t choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint64_t t1 = v[7] + s1 + choice + constants[i] + words[i % 16];
        uint64_t s0 = std::rotr(v[0], 28) ^ std::rotr(v[0], 34) ^ std::rotr(v[0], 39);
        uint64_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint64_t t2 = s0 + majority;
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    });
    for (unsigned i = 0; i < 8; ++i)
        m_state[i] += v[i];
}

void SHA512Hash::digest(std::span<uint8_t> out) const
{
    ASSERT(out.size() == m_digestSize);
    auto state = finished(16, true);
    storeWords<uint64_t>(out, state, true);
}

// ---- SHA-3 and SHAKE: FIPS 202

KeccakHash::KeccakHash(unsigned capacityBits, uint8_t suffix)
    : m_rate((1600 - capacityBits) / 8)
    , m_suffix(suffix)
{
}

// Keccak-f[1600]
void KeccakHash::permute()
{
    static constexpr uint64_t roundConstants[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull, 0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull, 0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull, 0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
    };
    static constexpr uint8_t rotations[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
    static constexpr uint8_t lanes[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };

    // Its own, so that the compiler can tell that nothing else has hold of it and need not keep it in memory.
    auto a = m_state;
    for (uint64_t roundConstant : roundConstants) {
        // theta
        uint64_t parity[5];
        unrolled<5>([&] (auto x) ALWAYS_INLINE_LAMBDA {
            parity[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        });
        unrolled<5>([&] (auto x) ALWAYS_INLINE_LAMBDA {
            uint64_t d = parity[(x + 4) % 5] ^ std::rotl(parity[(x + 1) % 5], 1);
            unrolled<5>([&] (auto y) ALWAYS_INLINE_LAMBDA {
                a[5 * y + x] ^= d;
            });
        });
        // rho and pi
        uint64_t carried = a[1];
        unrolled<24>([&] (auto i) ALWAYS_INLINE_LAMBDA {
            uint64_t next = a[lanes[i]];
            a[lanes[i]] = std::rotl(carried, rotations[i]);
            carried = next;
        });
        // chi
        unrolled<5>([&] (auto y) ALWAYS_INLINE_LAMBDA {
            uint64_t row[5];
            unrolled<5>([&] (auto x) ALWAYS_INLINE_LAMBDA {
                row[x] = a[5 * y + x];
            });
            unrolled<5>([&] (auto x) ALWAYS_INLINE_LAMBDA {
                a[5 * y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
            });
        });
        // iota
        a[0] ^= roundConstant;
    }
    m_state = a;
}

ALWAYS_INLINE void KeccakHash::advance(size_t count)
{
    m_position += count;
    if (m_position == m_rate) {
        permute();
        m_position = 0;
    }
}

void KeccakHash::update(std::span<const uint8_t> data)
{
    auto absorbByte = [&] {
        m_state[m_position / 8] ^= static_cast<uint64_t>(data.front()) << (8 * (m_position % 8));
        advance(1);
        data = data.subspan(1);
    };
    // A word at a time, once it is at the beginning of one. Every rate is a whole number of them.
    while (!data.empty() && m_position % 8)
        absorbByte();
    while (data.size() >= 8) {
        m_state[m_position / 8] ^= load64LittleEndian(data.data());
        advance(8);
        data = data.subspan(8);
    }
    while (!data.empty())
        absorbByte();
}

void KeccakHash::digest(std::span<uint8_t> out) const
{
    KeccakHash copy = *this;
    copy.m_state[copy.m_position / 8] ^= static_cast<uint64_t>(m_suffix) << (8 * (copy.m_position % 8));
    copy.m_state[(m_rate - 1) / 8] ^= 0x80ull << (8 * ((m_rate - 1) % 8));
    copy.permute();
    size_t position = 0;
    for (auto& byte : out) {
        if (position == m_rate) {
            copy.permute();
            position = 0;
        }
        byte = static_cast<uint8_t>(copy.m_state[position / 8] >> (8 * (position % 8)));
        ++position;
    }
}

// ---- BLAKE2: RFC 7693, and for the parameters "BLAKE2: simpler, smaller, fast as MD5", 2.8

namespace {

template<typename Word> struct Blake2Constants;

template<> struct Blake2Constants<uint64_t> {
    static constexpr uint64_t initial[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
    static constexpr unsigned rounds = 12;
    static constexpr int rotations[4] = { 32, 24, 16, 63 };
};

template<> struct Blake2Constants<uint32_t> {
    static constexpr uint32_t initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static constexpr unsigned rounds = 10;
    static constexpr int rotations[4] = { 16, 12, 8, 7 };
};

constexpr uint8_t blake2Sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

} // anonymous namespace

template<typename Word>
Blake2Hash<Word>::Blake2Hash(const Blake2Parameters& parameters)
    : m_digestSize(parameters.digestSize)
    , m_isLastNode(parameters.isLastNode)
{
    ASSERT(parameters.digestSize && parameters.digestSize <= maxDigestSize);
    ASSERT(parameters.key.size() <= maxKeySize && parameters.salt.size() <= saltSize && parameters.person.size() <= personSize);

    // The block of parameters, which is as long as the state.
    std::array<uint8_t, 8 * sizeof(Word)> block { };
    block[0] = parameters.digestSize;
    block[1] = static_cast<uint8_t>(parameters.key.size());
    block[2] = parameters.fanout;
    block[3] = parameters.depth;
    for (unsigned i = 0; i < 4; ++i)
        block[4 + i] = static_cast<uint8_t>(parameters.leafSize >> (8 * i));
    // Eight bytes of it in BLAKE2b and six in BLAKE2s.
    constexpr unsigned nodeOffsetBytes = isB ? 8 : 6;
    for (unsigned i = 0; i < nodeOffsetBytes; ++i)
        block[8 + i] = static_cast<uint8_t>(parameters.nodeOffset >> (8 * i));
    block[8 + nodeOffsetBytes] = parameters.nodeDepth;
    block[9 + nodeOffsetBytes] = parameters.innerSize;
    // And then, in BLAKE2b, fourteen that are kept for later.
    constexpr size_t saltOffset = isB ? 32 : 16;
    memcpySpan(std::span { block }.subspan(saltOffset, parameters.salt.size()), parameters.salt);
    memcpySpan(std::span { block }.subspan(saltOffset + saltSize, parameters.person.size()), parameters.person);

    for (unsigned i = 0; i < 8; ++i)
        m_state[i] = Blake2Constants<Word>::initial[i] ^ loadLittleEndian<Word>(block.data() + i * sizeof(Word));

    // A key is a first block, made up with zeros.
    if (!parameters.key.empty()) {
        std::array<uint8_t, blockSize> keyBlock { };
        memcpySpan(std::span { keyBlock }.first(parameters.key.size()), parameters.key);
        update(keyBlock);
        zeroSpan(std::span { keyBlock });
    }
}

template<typename Word>
void Blake2Hash<Word>::compress(const uint8_t* block, bool isLast)
{
    using Constants = Blake2Constants<Word>;
    Word m[16];
    for (unsigned i = 0; i < 16; ++i)
        m[i] = loadLittleEndian<Word>(block + i * sizeof(Word));
    Word v[16];
    for (unsigned i = 0; i < 8; ++i) {
        v[i] = m_state[i];
        v[i + 8] = Constants::initial[i];
    }
    v[12] ^= m_count[0];
    v[13] ^= m_count[1];
    if (isLast) {
        v[14] = ~v[14];
        if (m_isLastNode)
            v[15] = ~v[15];
    }
    auto mix = [&] (unsigned a, unsigned b, unsigned c, unsigned d, Word x, Word y) ALWAYS_INLINE_LAMBDA {
        v[a] = v[a] + v[b] + x;
        v[d] = std::rotr(static_cast<Word>(v[d] ^ v[a]), Constants::rotations[0]);
        v[c] = v[c] + v[d];
        v[b] = std::rotr(static_cast<Word>(v[b] ^ v[c]), Constants::rotations[1]);
        v[a] = v[a] + v[b] + y;
        v[d] = std::rotr(static_cast<Word>(v[d] ^ v[a]), Constants::rotations[2]);
        v[c] = v[c] + v[d];
        v[b] = std::rotr(static_cast<Word>(v[b] ^ v[c]), Constants::rotations[3]);
    };
    for (unsigned round = 0; round < Constants::rounds; ++round) {
        const uint8_t* s = blake2Sigma[round % 10];
        mix(0, 4, 8, 12, m[s[0]], m[s[1]]);
        mix(1, 5, 9, 13, m[s[2]], m[s[3]]);
        mix(2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(3, 7, 11, 15, m[s[6]], m[s[7]]);
        mix(0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (unsigned i = 0; i < 8; ++i)
        m_state[i] ^= v[i] ^ v[i + 8];
}

template<typename Word>
void Blake2Hash<Word>::update(std::span<const uint8_t> data)
{
    auto count = [&] (size_t bytes) {
        m_count[0] += static_cast<Word>(bytes);
        if (m_count[0] < static_cast<Word>(bytes))
            ++m_count[1];
    };
    // The last block is gone through differently, and it is not known to be the last until there is no more. So a full one is kept until more comes.
    while (!data.empty()) {
        if (m_buffered == blockSize) {
            count(blockSize);
            compress(m_buffer.data(), false);
            m_buffered = 0;
        }
        size_t taken = std::min(blockSize - m_buffered, data.size());
        memcpySpan(std::span { m_buffer }.subspan(m_buffered, taken), data.first(taken));
        m_buffered += taken;
        data = data.subspan(taken);
    }
}

template<typename Word>
void Blake2Hash<Word>::digest(std::span<uint8_t> out) const
{
    ASSERT(out.size() == m_digestSize);
    Blake2Hash copy = *this;
    copy.m_count[0] += static_cast<Word>(copy.m_buffered);
    if (copy.m_count[0] < static_cast<Word>(copy.m_buffered))
        ++copy.m_count[1];
    zeroSpan(std::span { copy.m_buffer }.subspan(copy.m_buffered));
    copy.compress(copy.m_buffer.data(), true);
    storeWords<Word>(out, copy.m_state, false);
}

template class Blake2Hash<uint64_t>;
template class Blake2Hash<uint32_t>;

} } // namespace JSC::Python
