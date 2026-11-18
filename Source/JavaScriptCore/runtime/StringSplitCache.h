/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 * Copyright (C) 2012 the V8 project authors. All rights reserved.
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

#include <JavaScriptCore/MatchResult.h>
#include <array>
#include <atomic>
#include <bit>
#include <wtf/DebugHeap.h>
#include <wtf/HashFunctions.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomStringImpl.h>

namespace JSC {

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER_AND_EXPORT(StringSplitCache, WTF_INTERNAL);

class JSCellButterfly;
class RegExp;

class StringSplitCache {
    WTF_MAKE_TZONE_ALLOCATED(StringSplitCache);
    WTF_MAKE_NONCOPYABLE(StringSplitCache);
public:
    StringSplitCache() = default;

    struct StringEntry {
        RefPtr<StringImpl> m_subject { nullptr };
        RefPtr<StringImpl> m_separator { nullptr };
        JSCellButterfly* m_butterfly { nullptr };
    };

    struct RegExpEntry {
        RefPtr<StringImpl> m_subject { nullptr };
        RegExp* m_regExp { nullptr };
        JSCellButterfly* m_butterfly { nullptr };
        MatchResult m_lastMatch { };
    };

    // A key is the same StringImpl each time the program comes by: an atom, as every literal of a parsed program is, or a string in the image of a compiled one.
    static bool canBeKey(const StringImpl* string) { return string && string->isAtomOrInImage(); }

    // Asked when the cache has no answer. Most literals are split once, so a string of an image enters the cache at its second split, and a program that splits each of them once makes no cache.
    static bool isWorthCaching(const StringImpl& subject)
    {
        ASSERT(canBeKey(&subject));
        return subject.isAtom() || wasSplitBefore(subject);
    }

    // If a program splits more strings of its image than the cache holds, every result would evict another before it is used again. While few lookups succeed, most strings are not cached.
    static bool hasRoomForImageString()
    {
        unsigned misses = s_imageStringMisses.load(std::memory_order_relaxed);
        s_imageStringMisses.store(misses + 1, std::memory_order_relaxed);
        return misses < stringCacheSize || !(misses % imageStringMissesPerEntry);
    }

    static void noteHitOfImageString()
    {
        unsigned misses = s_imageStringMisses.load(std::memory_order_relaxed);
        s_imageStringMisses.store(misses - std::min(misses, imageStringMissesPerHit), std::memory_order_relaxed);
    }

    JSCellButterfly* getForString(const String& subject, const String& separator);
    JSCellButterfly* getForKeys(StringImpl* subject, StringImpl* separator);
    void setForString(const String& subject, const String& separator, JSCellButterfly*);

    JSCellButterfly* getForRegExp(const String& subject, RegExp*, MatchResult& lastMatch);
    void setForRegExp(const String& subject, RegExp*, JSCellButterfly*, MatchResult lastMatch);

    void clear()
    {
        m_stringEntries.fill(StringEntry { });
        m_regExpEntries.fill(RegExpEntry { });
    }

private:
    static constexpr unsigned stringCacheSize = 64;
    static constexpr unsigned regExpCacheSize = 256;
    static constexpr unsigned imageStringMissesPerEntry = 16;
    static constexpr unsigned imageStringMissesPerHit = 4;
    static_assert(!(stringCacheSize & (stringCacheSize - 1)));
    static_assert(!(regExpCacheSize & (regExpCacheSize - 1)));

    size_t regExpEntryIndex(StringImpl* subject, RegExp* regExp) const
    {
        return pairIntHash(subject->hash(), PtrHash<RegExp*>::hash(regExp)) & (m_regExpEntries.size() - 1);
    }

    static bool wasSplitBefore(const StringImpl& subject)
    {
        uintptr_t place = std::bit_cast<uintptr_t>(&subject) / alignof(StringImpl);
        uint64_t bit = 1ull << (place % 64);
        return s_splitOnce[place / 64 % s_splitOnce.size()].fetch_or(bit, std::memory_order_relaxed) & bit;
    }

    // One bit for each string that was split. The strings are static, so all threads share the bits. If two strings share a bit, the second one is cached at its first split.
    inline static constinit std::array<std::atomic<uint64_t>, 128> s_splitOnce { };

    inline static constinit std::atomic<unsigned> s_imageStringMisses { 0 };

    std::array<StringEntry, stringCacheSize> m_stringEntries { };
    std::array<RegExpEntry, regExpCacheSize> m_regExpEntries { };
};

} // namespace JSC
