/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/text/AtomStringImpl.h>

namespace JSC {

class JSString;
class VM;

class JSONAtomStringCache {
public:
    static constexpr auto maxStringLengthForCache = 27;
    static constexpr auto capacity = 512;

    struct Slot {
        char16_t m_buffer[maxStringLengthForCache] { };
        char16_t m_length { 0 };
        RefPtr<AtomStringImpl> m_impl;
    };
    static_assert(sizeof(Slot) <= 64);

    using Cache = std::array<Slot, capacity>;
    using JSStringCache = std::array<JSString*, capacity>;

    template<typename CharacterType>
    ALWAYS_INLINE Ref<AtomStringImpl> makeIdentifier(VM&, std::span<const CharacterType> characters);

    template<typename CharacterType>
    ALWAYS_INLINE AtomStringImpl* existingIdentifier(VM&, std::span<const CharacterType> characters);

    template<typename CharacterType>
    ALWAYS_INLINE JSString* tryMakeJSString(VM&, std::span<const CharacterType> characters);

    // The cache on the VM has no lock, and with the GIL off every thread of the
    // VM parses at once: a slot written by two threads pairs one thread's
    // characters with another's atom (a wrong property name) and races the
    // RefPtr assignment. GIL-off, each thread uses a cache of its own, which
    // holds atoms only: a JSString it cached could not be cleared or visited by
    // the collector (Heap::finalize clears the VM's), so that half is off.
    // Flag off and GIL on: one predicted-false byte test in live().
    static ALWAYS_INLINE JSONAtomStringCache& live(VM&);
    JS_EXPORT_PRIVATE static JSONAtomStringCache& gilOffPerThreadCache();

    ALWAYS_INLINE void clear()
    {
        m_cache.fill({ });
        m_jsStrings.fill(nullptr);
    }

    ALWAYS_INLINE void clearJSStrings()
    {
        m_jsStrings.fill(nullptr);
    }

private:
    ALWAYS_INLINE unsigned cacheIndex(char16_t firstCharacter, char16_t lastCharacter, char16_t length)
    {
        unsigned hash = (firstCharacter << 6) ^ ((lastCharacter << 14) ^ firstCharacter);
        hash += (hash >> 14) + (length << 14);
        hash ^= hash << 14;
        return (hash + (hash >> 6)) % capacity;
    }

    ALWAYS_INLINE Slot& cacheSlot(char16_t firstCharacter, char16_t lastCharacter, char16_t length)
    {
        return m_cache[cacheIndex(firstCharacter, lastCharacter, length)];
    }

    Cache m_cache { };
    JSStringCache m_jsStrings { };
    bool m_jsStringCachingDisabled { false };
};

} // namespace JSC
