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

#pragma once

#include <JavaScriptCore/ScriptFetchParameters.h>

#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/text/StringHash.h>

namespace JSC {

// The registry key is (resolved specifier, attribute type, host-defined attribute
// string). The string component is needed because every Bun-defined `with { type }`
// value ("text", "file", "toml", ...) maps onto the single Type::HostDefined enum
// value, so the enum alone cannot distinguish them. Per
// https://tc39.es/proposal-import-attributes/ the same specifier imported with
// different attribute lists designates different modules.
using ModuleMapKey = std::tuple<UniquedStringImpl*, ScriptFetchParameters::Type, String>;

// Always build keys through this helper so the string component is guaranteed to
// be null for every non-HostDefined type; otherwise two spellings of the same
// non-HostDefined key (e.g. with and without an attached parameters object) could
// land in different buckets.
inline ModuleMapKey makeModuleMapKey(UniquedStringImpl* specifier, ScriptFetchParameters::Type type, const ScriptFetchParameters* parameters)
{
#if USE(BUN_JSC_ADDITIONS)
    if (type == ScriptFetchParameters::Type::HostDefined && parameters)
        return { specifier, type, parameters->hostDefinedImportType() };
    ASSERT_WITH_MESSAGE(type != ScriptFetchParameters::Type::HostDefined, "a HostDefined module map key needs its ScriptFetchParameters; without the attribute string it would collide with every other host-defined entry for the specifier");
#else
    UNUSED_PARAM(parameters);
#endif
    return { specifier, type, String() };
}

struct ModuleMapHash {
    static unsigned hash(const ModuleMapKey& key)
    {
        auto* specifier = std::get<0>(key);
        unsigned identifierHash = specifier ? specifier->existingSymbolAwareHash() : 0;
        unsigned enumHash(std::get<1>(key));
        const String& hostDefinedImportType = std::get<2>(key);
        unsigned hostDefinedImportTypeHash = hostDefinedImportType.isNull() ? 0 : hostDefinedImportType.impl()->hash();
        return WTF::pairIntHash(WTF::pairIntHash(identifierHash, enumHash), hostDefinedImportTypeHash);
    }

    static bool equal(const ModuleMapKey& a, const ModuleMapKey& b)
    {
        return std::get<0>(a) == std::get<0>(b) && std::get<1>(a) == std::get<1>(b) && std::get<2>(a) == std::get<2>(b);
    }

    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

template <typename T>
using ModuleMap = UncheckedKeyHashMap<ModuleMapKey, T, ModuleMapHash>;

using ResolutionMapKey = std::pair<UniquedStringImpl*, UniquedStringImpl*>;

struct ResolutionMapHash {
    static unsigned hash(const ResolutionMapKey& key)
    {
        unsigned referrerHash = key.first ? key.first->existingSymbolAwareHash() : 0;
        unsigned specifierHash = key.second ? key.second->existingSymbolAwareHash() : 0;
        return WTF::pairIntHash(referrerHash, specifierHash);
    }

    static bool equal(const ResolutionMapKey& a, const ResolutionMapKey& b)
    {
        return a.first == b.first && a.second == b.second;
    }

    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

template <typename T>
using ResolutionMap = UncheckedKeyHashMap<ResolutionMapKey, T, ResolutionMapHash>;

} // namespace JSC
