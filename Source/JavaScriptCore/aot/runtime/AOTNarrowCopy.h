/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/Latin1Character.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr uint32_t maxLengthCopiedAsLatin1 = 64;

ALWAYS_INLINE bool tryCopyAsLatin1(std::span<const char16_t> units, Latin1Character* destination)
{
    constexpr size_t unitsPerWord = sizeof(uint64_t) / sizeof(char16_t);
    auto tryCopyWord = [&](size_t at) ALWAYS_INLINE_LAMBDA {
        constexpr uint64_t upperBytes = 0xff00ff00ff00ff00ull;
        uint64_t word = WTF::unalignedLoad<uint64_t>(units.data() + at);
        if (word & upperBytes)
            return false;
        word = (word | word >> 8) & 0x0000ffff0000ffffull;
        WTF::unalignedStore<uint32_t>(destination + at, static_cast<uint32_t>(word | word >> 16));
        return true;
    };
    if (units.size() < unitsPerWord) {
        for (size_t at = 0; at < units.size(); ++at) {
            if (!isLatin1(units[at]))
                return false;
            destination[at] = static_cast<Latin1Character>(units[at]);
        }
        return true;
    }
    for (size_t at = 0; at + unitsPerWord < units.size(); at += unitsPerWord) {
        if (!tryCopyWord(at))
            return false;
    }
    return tryCopyWord(units.size() - unitsPerWord);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
