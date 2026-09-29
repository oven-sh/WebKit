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
#include "PythonCharacters.h"

#include "VM.h"
#include <unicode/utf16.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/ExtensibleStringImpl.h>

namespace JSC { namespace Python {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SurrogatePairs);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SurrogatePairCache);

static bool isPairAt(std::span<const char16_t> codeUnits, size_t i)
{
    // One half by itself is a character, as it is to JavaScript.
    return U16_IS_LEAD(codeUnits[i]) && i + 1 < codeUnits.size() && U16_IS_TRAIL(codeUnits[i + 1]);
}

SurrogatePairs::~SurrogatePairs()
{
    fastFree(m_characters);
}

bool SurrogatePairs::tryLookThrough(std::span<const char16_t> codeUnits)
{
    if (codeUnits.size() <= m_lengthLookedThrough)
        return true;
    // What was last, if it is the first half of a pair, was taken to be by itself. Now there may be something after it.
    size_t start = m_lengthLookedThrough;
    if (start && U16_IS_LEAD(codeUnits[start - 1]))
        --start;

    size_t found = 0;
    for (size_t i = start; i < codeUnits.size(); ++i) {
        if (isPairAt(codeUnits, i)) {
            ++found;
            ++i;
        }
    }
    if (m_count + found > m_capacity) {
        // The first time it is as much as is needed, and after that twice as much, since what is added to once is likely to be added to again.
        size_t capacity = m_capacity ? std::max(m_count + found, 2 * m_capacity) : found;
        unsigned* characters;
        if (!WTF::tryFastRealloc(m_characters, capacity * sizeof(unsigned)).getValue(characters))
            return false;
        m_characters = characters;
        m_capacity = capacity;
    }
    // All of the pairs so far are before this.
    unsigned character = start - m_count;
    for (size_t i = start; i < codeUnits.size(); ++i, ++character) {
        if (isPairAt(codeUnits, i)) {
            m_characters[m_count++] = character;
            ++i;
        }
    }
    m_lengthLookedThrough = codeUnits.size();
    return true;
}

RefPtr<SurrogatePairs> SurrogatePairs::tryCopy(size_t length) const
{
    ASSERT(length <= m_lengthLookedThrough);
    Ref copy = create();
    // Those that are wholly in it. One that begins with the last code unit is found again when what comes after is looked through.
    size_t count = length ? countBeforeCodeUnit(length - 1) : 0;
    if (count) {
        if (!tryFastMalloc(count * sizeof(unsigned)).getValue(copy->m_characters))
            return nullptr;
        memcpy(copy->m_characters, m_characters, count * sizeof(unsigned));
    }
    copy->m_count = count;
    copy->m_capacity = count;
    copy->m_lengthLookedThrough = length;
    return copy;
}

unsigned SurrogatePairs::countBeforeCharacter(unsigned index, unsigned limit) const
{
    auto characters = this->characters().first(limit);
    return std::ranges::lower_bound(characters, index) - characters.begin();
}

unsigned SurrogatePairs::countBeforeCodeUnit(unsigned offset) const
{
    // The one that has `before` before it begins at the code unit characters[before] + before, which goes up with `before`.
    auto characters = this->characters();
    unsigned low = 0;
    unsigned high = characters.size();
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (static_cast<uint64_t>(characters[middle]) + middle < offset)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

StringImpl* SurrogatePairCache::keyFor(StringImpl& string)
{
    return string.isPrefixOfExtensibleBuffer() ? &ExtensibleStringImpl::bufferOf(string) : &string;
}

void SurrogatePairCache::didAppend(StringImpl& shorter, StringImpl& longer)
{
    StringImpl* key = keyFor(shorter);
    Entry& known = entryFor(key);
    if (known.string != key)
        return;
    // They may be in the same place.
    Entry entry { keyFor(longer), known.pairs, std::min<size_t>(known.lengthInCommon, shorter.length()) };
    entryFor(entry.string.get()) = WTF::move(entry);
}

auto SurrogatePairCache::find(StringImpl& string, RefPtr<SurrogatePairs>& pairs) -> Found
{
    ASSERT(!string.is8Bit());
    StringImpl* key = keyFor(string);
    Entry& entry = entryFor(key);
    if (entry.string != key)
        entry = { key, SurrogatePairs::create(), 0 };
    // What is wanted is what is known of the string so far, and to add to that from the string. Neither will do if it has been added to from another.
    if (std::min<size_t>(entry.pairs->lengthLookedThrough(), string.length()) > entry.lengthInCommon)
        entry.pairs = entry.pairs->tryCopy(entry.lengthInCommon);
    if (!entry.pairs || !entry.pairs->tryLookThrough(string.span16())) {
        entry = { };
        return Found::TooMany;
    }
    entry.lengthInCommon = std::max<size_t>(entry.lengthInCommon, string.length());
    pairs = entry.pairs;
    // Those that begin before its last code unit, so that both halves are in it.
    if (!pairs->countBeforeCodeUnit(string.length() - 1)) {
        string.setHasNoSurrogatePairs();
        // If it is by itself there is nothing more to be found out about it.
        if (key == &string)
            entry = { };
        return Found::None;
    }
    return Found::Some;
}

Characters::Characters(VM& vm, const String& string)
    : m_length(string.length())
{
    StringImpl* impl = string.impl();
    if (!impl || impl->is8Bit() || impl->hasNoSurrogatePairs())
        return;
    switch (vm.ensurePythonSurrogatePairCache().find(*impl, m_pairs)) {
    case SurrogatePairCache::Found::None:
        m_pairs = nullptr;
        break;
    case SurrogatePairCache::Found::Some:
        m_pairCount = m_pairs->countBeforeCodeUnit(m_length - 1);
        break;
    case SurrogatePairCache::Found::TooMany:
        m_pairs = nullptr;
        m_isGoneThrough = true;
        m_codeUnits = impl->span16();
        break;
    }
}

unsigned Characters::count() const
{
    if (m_pairs)
        return m_length - m_pairCount;
    return m_isGoneThrough ? characterAt(m_length) : m_length;
}

unsigned Characters::codeUnitOf(unsigned index) const
{
    if (m_pairs)
        return index + m_pairs->countBeforeCharacter(index, m_pairCount);
    if (!m_isGoneThrough)
        return index;
    size_t i = 0;
    for (; index && i < m_codeUnits.size(); ++i, --index)
        i += isPairAt(m_codeUnits, i);
    return i;
}

unsigned Characters::characterAt(unsigned offset) const
{
    if (m_pairs)
        return offset - std::min(m_pairs->countBeforeCodeUnit(offset), m_pairCount);
    if (!m_isGoneThrough)
        return offset;
    unsigned character = 0;
    for (size_t i = 0; i < offset; ++i, ++character)
        i += isPairAt(m_codeUnits, i);
    return character;
}

// Nearly always not.
static bool couldBePartOfPair(StringView haystack, StringView needle)
{
    return !haystack.is8Bit() && !needle.isEmpty() && (U16_IS_TRAIL(needle[0]) || U16_IS_LEAD(needle[needle.length() - 1]));
}

static bool isInMiddleOfPair(StringView haystack, size_t offset)
{
    return offset && offset < haystack.length() && U16_IS_LEAD(haystack[offset - 1]) && U16_IS_TRAIL(haystack[offset]);
}

size_t findCharacters(StringView haystack, StringView needle, size_t start)
{
    size_t found = haystack.find(needle, start);
    if (!couldBePartOfPair(haystack, needle)) [[likely]]
        return found;
    while (found != notFound && (isInMiddleOfPair(haystack, found) || isInMiddleOfPair(haystack, found + needle.length())))
        found = haystack.find(needle, found + 1);
    return found;
}

size_t reverseFindCharacters(StringView haystack, StringView needle)
{
    size_t found = haystack.reverseFind(needle);
    if (!couldBePartOfPair(haystack, needle)) [[likely]]
        return found;
    while (found != notFound && (isInMiddleOfPair(haystack, found) || isInMiddleOfPair(haystack, found + needle.length())))
        found = found ? haystack.reverseFind(needle, found - 1) : notFound;
    return found;
}

bool startsWithCharacters(StringView string, StringView prefix)
{
    return string.startsWith(prefix) && !isInMiddleOfPair(string, prefix.length());
}

bool endsWithCharacters(StringView string, StringView suffix)
{
    return string.endsWith(suffix) && !isInMiddleOfPair(string, string.length() - suffix.length());
}

} } // namespace JSC::Python
