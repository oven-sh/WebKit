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

#include <array>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace Python {

// A str is a JavaScript string, which is made of code units of 16 bits. Python counts in characters, and a character that does not fit in 16 bits is two code units, a surrogate pair. So which code unit the
// character at an index begins at is not the index, if there is a pair before it, and len() is not how many code units there are. Both are to take no longer for a long string than for a short one.
//
//   - Most strings have their characters in 8 bits, and have no pairs.
//   - Most of the rest have none either. That is found by going through the string, and it cannot change, so it is kept in the StringImpl, in a bit beside the one that says how wide the characters are.
//   - For a string that has pairs, where they are is written down: SurrogatePairs. There is nowhere in a string to keep that, so it is kept for as long as the strings lately asked about, in a cache of the VM's,
//     as what String.prototype.split() came to is.

// Where the surrogate pairs are in some code units, from the first of them as far as has been looked.
//
// A string that is added to over and over begins each time with what it was before, so what has been found out about the shorter one holds for the longer, and only what has been added is looked through. So several
// strings have one of these between them, each of which begins as the others do as far as it goes, and there can be more in it than is in the string that is being asked about. `limit` is how many of the pairs are.
// It is only ever added to. If two strings that begin alike go on differently, the second to be looked through has a copy of what they have in common: tryCopy().
class SurrogatePairs final : public RefCounted<SurrogatePairs> {
    WTF_MAKE_TZONE_ALLOCATED(SurrogatePairs);
public:
    static Ref<SurrogatePairs> create() { return adoptRef(*new SurrogatePairs); }
    ~SurrogatePairs();

    // Looks through what has not been looked through. False if there is no room to write down what is there, and then it is as it was.
    bool tryLookThrough(std::span<const char16_t>);
    // What is known of the first `length` code units, which have been looked through. Null if there is no room.
    RefPtr<SurrogatePairs> tryCopy(size_t length) const;
    size_t lengthLookedThrough() const { return m_lengthLookedThrough; }

    unsigned count() const { return m_count; }
    // How many of the first `limit` are before the character at an index.
    unsigned countBeforeCharacter(unsigned index, unsigned limit) const;
    // How many begin before a code unit.
    unsigned countBeforeCodeUnit(unsigned offset) const;

private:
    SurrogatePairs() = default;

    std::span<const unsigned> characters() const { return unsafeMakeSpan(m_characters, m_count); }

    // Of each, in order, which character it is. The code unit that it begins at is that and one more for each of those before it. So there is as much of this as there are pairs, and nothing to choose in how it
    // is laid out.
    unsigned* m_characters { nullptr };
    size_t m_count { 0 };
    size_t m_capacity { 0 };
    size_t m_lengthLookedThrough { 0 };
};

class SurrogatePairCache {
    WTF_MAKE_TZONE_ALLOCATED(SurrogatePairCache);
    WTF_MAKE_NONCOPYABLE(SurrogatePairCache);
public:
    SurrogatePairCache() = default;

    enum class Found : uint8_t {
        None, // It has no pairs.
        Some,
        TooMany, // To write down: there is not the memory.
    };
    // Of a string of 16 bit characters that is not known to have no pairs. If it turns out to have none, it is known from then on.
    Found find(StringImpl&, RefPtr<SurrogatePairs>&);
    // `longer` is a new string that begins with all of `shorter`. What is known of the one is a start on the other.
    void didAppend(StringImpl& shorter, StringImpl& longer);

    void clear() { m_entries.fill(Entry { }); }

private:
    struct Entry {
        RefPtr<StringImpl> string; // Or the buffer that it is the beginning of: see ExtensibleStringImpl.h.
        RefPtr<SurrogatePairs> pairs;
        // How far the string is known to be the same as what `pairs` is about. Past that, `pairs` may have been added to from another string that began the same.
        size_t lengthInCommon { 0 };
    };

    static StringImpl* keyFor(StringImpl&);
    Entry& entryFor(StringImpl* key) { return m_entries[PtrHash<StringImpl*>::hash(key) & (size - 1)]; }

    // As many as StringSplitCache has of strings. It is a guess that as many strings are gone through by index at a time as are split: what matters is that it is more than the two or three that a loop has in hand.
    static constexpr unsigned size = 64;
    static_assert(!(size & (size - 1)));
    std::array<Entry, size> m_entries { };
};

// The characters of one string.
class Characters {
public:
    Characters(VM&, const String&);

    // len()
    unsigned count() const;
    // Whether a character is anything other than a code unit.
    bool hasPairs() const { return m_pairs || m_isGoneThrough; }
    // The code unit that the character at an index begins at. The index can be count(), and then it is the length.
    unsigned codeUnitOf(unsigned index) const;
    // Which character it is that begins at a code unit.
    unsigned characterAt(unsigned offset) const;

private:
    RefPtr<SurrogatePairs> m_pairs;
    unsigned m_pairCount { 0 }; // Of those, how many are in this string.
    std::span<const char16_t> m_codeUnits; // Only if it is gone through.
    unsigned m_length { 0 };
    bool m_isGoneThrough { false }; // There are too many pairs to write down, so they are counted each time.
};

// Where one string is in another, in code units, or notFound. It is characters that are looked for. Half a pair by itself is a character, and is not part of the pair that has the same for one of its halves, so what
// begins with a second half or ends with a first half is not found where that would be in the middle of a pair. `haystack` begins and ends where a character does.
size_t findCharacters(StringView haystack, StringView needle, size_t start = 0);
size_t reverseFindCharacters(StringView haystack, StringView needle);
bool startsWithCharacters(StringView, StringView prefix);
bool endsWithCharacters(StringView, StringView suffix);

} } // namespace JSC::Python
