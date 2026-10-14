/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
#include <wtf/HashSecrets.h>

#include <bit>
#include <mutex>
#include <wtf/OSRandomSource.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

// make_secret and what it calls, from wyhash by Wang Yi (https://github.com/wangyi-fudan/wyhash, public domain), as V8 has them
// in third_party/rapidhash-v8/secret.h. Every byte of a secret has four bits set, any two secrets differ in 32 bits, and
// every secret is prime.

static uint64_t wyrand(uint64_t& seed)
{
    seed += 0x2d358dccaa6c78a5ull;
    return HashSecrets::mix(seed, seed ^ 0x8bb84b93962eacc9ull);
}

static uint64_t mulMod(uint64_t a, uint64_t b, uint64_t m)
{
    uint64_t r = 0;
    while (b) {
        if (b & 1) {
            uint64_t r2 = r + a;
            if (r2 < r)
                r2 -= m;
            r = r2 % m;
        }
        b >>= 1;
        if (b) {
            uint64_t a2 = a + a;
            if (a2 < a)
                a2 -= m;
            a = a2 % m;
        }
    }
    return r;
}

static uint64_t powMod(uint64_t a, uint64_t b, uint64_t m)
{
    uint64_t r = 1;
    while (b) {
        if (b & 1)
            r = mulMod(r, a, m);
        b >>= 1;
        if (b)
            a = mulMod(a, a, m);
    }
    return r;
}

static bool isStrongProbablePrime(uint64_t n, uint64_t a)
{
    uint64_t d = n - 1;
    uint8_t s = 0;
    while (!(d & 0xff)) {
        d >>= 8;
        s += 8;
    }
    if (!(d & 0xf)) {
        d >>= 4;
        s += 4;
    }
    if (!(d & 0x3)) {
        d >>= 2;
        s += 2;
    }
    if (!(d & 0x1)) {
        d >>= 1;
        s += 1;
    }
    uint64_t b = powMod(a, d, n);
    if (b == 1 || b == n - 1)
        return true;
    for (uint8_t r = 1; r < s; r++) {
        b = mulMod(b, b, n);
        if (b <= 1)
            return false;
        if (b == n - 1)
            return true;
    }
    return false;
}

static bool isPrime(uint64_t n)
{
    if (n < 2 || !(n & 1))
        return false;
    if (n < 4)
        return true;
    if (!isStrongProbablePrime(n, 2))
        return false;
    if (n < 2047)
        return true;
    for (uint64_t a : { 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 }) {
        if (!isStrongProbablePrime(n, a))
            return false;
    }
    return true;
}

static HashSecrets makeHashSecrets(const uint64_t seed)
{
    static constexpr std::array<uint8_t, 70> c { 15, 23, 27, 29, 30, 39, 43, 45, 46, 51, 53, 54, 57, 58, 60, 71, 75, 77, 78, 83, 85, 86, 89, 90, 92, 99, 101, 102, 105, 106, 108, 113, 114, 116, 120, 135, 139, 141, 142, 147, 149, 150, 153, 154, 156, 163, 165, 166, 169, 170, 172, 177, 178, 180, 184, 195, 197, 198, 201, 202, 204, 209, 210, 212, 216, 225, 226, 228, 232, 240 };

    uint64_t state = seed;
    std::array<uint64_t, 3> secret;
    for (size_t i = 0; i < secret.size(); i++) {
        bool ok;
        do {
            ok = true;
            secret[i] = 0;
            for (size_t j = 0; j < 64; j += 8)
                secret[i] |= static_cast<uint64_t>(c[wyrand(state) % c.size()]) << j;
            if (!(secret[i] % 2)) {
                ok = false;
                continue;
            }
            for (size_t j = 0; j < i; j++) {
                if (std::popcount(secret[j] ^ secret[i]) != 32) {
                    ok = false;
                    break;
                }
            }
            if (ok && !isPrime(secret[i]))
                ok = false;
        } while (!ok);
    }
    return { seed, secret[0], secret[1], secret[2] };
}

void initializeHashSecrets(HashSecretsChoice choice)
{
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        if (choice == HashSecretsChoice::PerProcess) {
            uint64_t seed;
            cryptographicallyRandomValuesFromOS(asMutableByteSpan(seed));
            g_wtfHashSecrets = makeHashSecrets(seed);
        } else
            g_wtfHashSecrets = stableHashSecrets;
        // The one static atom that is a property key without going through an atom table, which is what hashes the others.
        StringImpl::empty()->hash();
    });
}

} // namespace WTF
