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

#pragma once

#include <array>
#include <bit>
#include <bmalloc/GigacageConfig.h>
#include <cstdint>
#include <wtf/Assertions.h>
#include <wtf/ExportMacros.h>
#include <wtf/Int128.h>

namespace WTF {

// The keys of StringHasher and intHash.
struct HashSecrets {
    static constexpr uint64_t mix(uint64_t a, uint64_t b)
    {
        UInt128 product = static_cast<UInt128>(a) * b;
        return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
    }

    constexpr HashSecrets(uint64_t secret0, uint64_t secret1, uint64_t secret2)
        : secret { secret0, secret1, secret2 }
        , seed(mix(secret0, secret1))
    {
    }

    std::array<uint64_t, 3> secret;
    uint64_t seed; // What rapidhash starts from, before the length.
};

// rapidhash's published secrets. A hash made with these is the same in every process, so it can be computed at compile
// time or stored in a file.
inline constexpr HashSecrets stableHashSecrets { 0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull };

// WTFConfig.h includes the headers that hash, so they cannot reach a field of WTF::Config.
// Should be kept in sync with the layout of WTFConfig.h:ReservedGlobalConfigSlots
constexpr size_t startSlotOfHashSecrets = Gigacage::startSlotOfGigacageConfig + Gigacage::reservedSlotsForGigacageConfig;
constexpr size_t reservedSlotsForHashSecrets = sizeof(HashSecrets) / sizeof(WebConfig::Slot);

#define g_wtfHashSecrets (*std::bit_cast<WTF::HashSecrets*>(&WebConfig::g_config[WTF::startSlotOfHashSecrets]))

enum class HashSecretsChoice : uint8_t {
    Stable, // stableHashSecrets
    PerProcess, // Random, so that which keys share a bucket cannot be known ahead of time.
};

// The first call picks this process's secrets. A hash made before it would not match one made after, so it has to come first.
WTF_EXPORT_PRIVATE void initializeHashSecrets(HashSecretsChoice = HashSecretsChoice::Stable);

ALWAYS_INLINE const HashSecrets& hashSecrets()
{
    ASSERT(g_wtfHashSecrets.secret[0]);
    return g_wtfHashSecrets;
}

} // namespace WTF
