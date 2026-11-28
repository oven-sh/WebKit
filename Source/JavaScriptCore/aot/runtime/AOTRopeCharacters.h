/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGrowingText.h"
#include "JSString.h"
#include <array>
#include <bit>
#include <cstring>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

ALWAYS_INLINE void copyShortString(std::span<const Latin1Character> characters, Latin1Character* to)
{
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    if (size >= 8) {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + size - 8, last);
        return;
    }
    if (size >= 4) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(from);
        uint32_t last = WTF::unalignedLoad<uint32_t>(from + size - 4);
        WTF::unalignedStore<uint32_t>(to, first);
        WTF::unalignedStore<uint32_t>(to + size - 4, last);
        return;
    }
    if (size) {
        Latin1Character first = from[0];
        Latin1Character middle = from[size / 2];
        Latin1Character last = from[size - 1];
        to[0] = first;
        to[size / 2] = middle;
        to[size - 1] = last;
    }
}

ALWAYS_INLINE std::array<const JSString*, JSRopeString::s_maxInternalRopeLength> fibersOf(const JSRopeString* rope)
{
    using CompactFibers = JSRopeString::CompactFibers;
    static_assert(CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16);
    auto* bytes = reinterpret_cast<const uint8_t*>(rope);
    uint64_t upperWord = WTF::unalignedLoad<uint64_t>(bytes + JSRopeString::offsetOfFiber2());
    uint64_t second = WTF::unalignedLoad<uint32_t>(bytes + JSRopeString::offsetOfFiber1Lower()) | (upperWord & 0xffff) << 32;
    return { std::bit_cast<const JSString*>(WTF::unalignedLoad<uint64_t>(bytes + JSRopeString::offsetOfFiber0()) & JSRopeString::stringMask), std::bit_cast<const JSString*>(second), std::bit_cast<const JSString*>(upperWord >> 16) };
}

ALWAYS_INLINE const Latin1Character* latin1CharactersInPlace(const JSString* string, unsigned& length)
{
    if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]] {
        length = impl->length();
        return impl->is8Bit() ? impl->span8().data() : nullptr;
    }
    auto* rope = uncheckedDowncast<JSRopeString>(string);
    if (!rope->isSubstring())
        return nullptr;
    length = rope->length();
    const StringImpl* base = rope->substringBase()->tryGetValueImpl();
    return base->is8Bit() ? base->span8().data() + rope->substringOffset() : nullptr;
}

ALWAYS_INLINE void copyIntoRoom(const Latin1Character* from, size_t length, Latin1Character* to, size_t room)
{
    constexpr size_t maxLengthOfShortString = 2 * sizeof(uint64_t);
#if !ASAN_ENABLED
    constexpr uintptr_t smallestPage = 4096;
    if (length <= maxLengthOfShortString && room >= maxLengthOfShortString && (std::bit_cast<uintptr_t>(from) & (smallestPage - 1)) <= smallestPage - maxLengthOfShortString) [[likely]] {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t second = WTF::unalignedLoad<uint64_t>(from + sizeof(uint64_t));
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + sizeof(uint64_t), second);
        return;
    }
#else
    UNUSED_PARAM(room);
#endif
    if (length > maxLengthOfShortString) {
        memcpy(to, from, length);
        return;
    }
    copyShortString({ from, length }, to);
}

const Latin1Character* tryWriteRope(const JSString* rope, Latin1Character* to, const Latin1Character* endOfRoom);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
