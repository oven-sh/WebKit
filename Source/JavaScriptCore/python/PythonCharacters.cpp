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

namespace JSC { namespace Python {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SurrogatePairs);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SurrogatePairCache);

static bool isPairAt(std::span<const char16_t> codeUnits, size_t i)
{
    // One half by itself is a character, as it is to JavaScript.
    return U16_IS_LEAD(codeUnits[i]) && i + 1 < codeUnits.size() && U16_IS_TRAIL(codeUnits[i + 1]);
}

RefPtr<SurrogatePairs> SurrogatePairs::tryCreate(std::span<const char16_t> codeUnits, size_t count)
{
    auto characters = MallocSpan<unsigned>::tryMalloc(count * sizeof(unsigned));
    if (!characters)
        return nullptr;
    auto rest = characters.mutableSpan();
    unsigned character = 0;
    for (size_t i = 0; i < codeUnits.size(); ++i, ++character) {
        if (!isPairAt(codeUnits, i))
            continue;
        rest[0] = character;
        rest = rest.subspan(1);
        ++i;
    }
    ASSERT(rest.empty());
    return adoptRef(*new SurrogatePairs(WTF::move(characters)));
}

unsigned SurrogatePairs::countBeforeCharacter(unsigned index) const
{
    auto characters = m_characters.span();
    return std::ranges::lower_bound(characters, index) - characters.begin();
}

unsigned SurrogatePairs::countBeforeCodeUnit(unsigned offset) const
{
    // The one that has `before` before it begins at the code unit m_characters[before] + before, which goes up with `before`.
    auto characters = m_characters.span();
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

auto SurrogatePairCache::find(StringImpl& string, RefPtr<SurrogatePairs>& pairs) -> Found
{
    ASSERT(!string.is8Bit());
    Entry& entry = m_entries[PtrHash<StringImpl*>::hash(&string) & (size - 1)];
    if (entry.string == &string) {
        pairs = entry.pairs;
        return Found::Some;
    }

    auto codeUnits = string.span16();
    size_t count = 0;
    for (size_t i = 0; i < codeUnits.size(); ++i) {
        if (isPairAt(codeUnits, i)) {
            ++count;
            ++i;
        }
    }
    if (!count) {
        string.setHasNoSurrogatePairs();
        return Found::None;
    }

    pairs = SurrogatePairs::tryCreate(codeUnits, count);
    if (!pairs)
        return Found::TooMany;
    entry = { &string, pairs };
    return Found::Some;
}

Characters::Characters(VM& vm, const String& string)
    : m_length(string.length())
{
    StringImpl* impl = string.impl();
    if (!impl || impl->is8Bit() || impl->hasNoSurrogatePairs())
        return;
    if (vm.ensurePythonSurrogatePairCache().find(*impl, m_pairs) == SurrogatePairCache::Found::TooMany) {
        m_isGoneThrough = true;
        m_codeUnits = impl->span16();
    }
}

unsigned Characters::count() const
{
    if (m_pairs)
        return m_length - m_pairs->count();
    return m_isGoneThrough ? characterAt(m_length) : m_length;
}

unsigned Characters::codeUnitOf(unsigned index) const
{
    if (m_pairs)
        return index + m_pairs->countBeforeCharacter(index);
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
        return offset - m_pairs->countBeforeCodeUnit(offset);
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
