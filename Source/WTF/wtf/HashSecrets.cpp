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

constinit HashSecrets g_wtfHashSecrets = stableHashSecrets;
#if ASSERT_ENABLED
constinit bool g_wtfHashSecretsAreInitialized = false;
#endif

// wyhash's make_secret without its primality test, which takes milliseconds: every byte has four bits set, every secret
// is odd, and any two secrets differ in 32 bits.
static HashSecrets makeHashSecrets(std::span<const uint8_t> random)
{
    static constexpr std::array<uint8_t, 70> balancedBytes { 15, 23, 27, 29, 30, 39, 43, 45, 46, 51, 53, 54, 57, 58, 60, 71, 75, 77, 78, 83, 85, 86, 89, 90, 92, 99, 101, 102, 105, 106, 108, 113, 114, 116, 120, 135, 139, 141, 142, 147, 149, 150, 153, 154, 156, 163, 165, 166, 169, 170, 172, 177, 178, 180, 184, 195, 197, 198, 201, 202, 204, 209, 210, 212, 216, 225, 226, 228, 232, 240 };

    std::array<uint64_t, 2> state;
    memcpySpan(asMutableByteSpan(state), random);
    auto next = [&] {
        state[0] += stableHashSecrets.secret[0];
        return HashSecrets::mix(state[0], state[0] ^ state[1]);
    };

    std::array<uint64_t, 3> secret;
    for (size_t i = 0; i < secret.size(); ++i) {
        bool ok;
        do {
            secret[i] = 0;
            for (unsigned shift = 0; shift < 64; shift += 8)
                secret[i] |= static_cast<uint64_t>(balancedBytes[next() % balancedBytes.size()]) << shift;
            ok = secret[i] & 1;
            for (size_t j = 0; ok && j < i; ++j)
                ok = std::popcount(secret[j] ^ secret[i]) == 32;
        } while (!ok);
    }
    return { secret[0], secret[1], secret[2] };
}

void initializeHashSecrets()
{
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        std::array<uint8_t, 16> random;
        cryptographicallyRandomValuesFromOS(random);
        g_wtfHashSecrets = makeHashSecrets(random);
#if ASSERT_ENABLED
        g_wtfHashSecretsAreInitialized = true;
#endif
        // The one static atom that is a property key without going through an atom table, which is what hashes the others.
        StringImpl::empty()->hash();
    });
}

} // namespace WTF
