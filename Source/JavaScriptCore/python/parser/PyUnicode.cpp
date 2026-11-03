/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyUnicode.h"

#include "PyUnicodeNameAliases.h"
#include "PyUnicodeNewNames.h"
#include "PyUnicodeTables.h"
#include <algorithm>
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <wtf/HexNumber.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

static ALWAYS_INLINE const UnicodeTables::Record& recordOf(char32_t c)
{
    using namespace UnicodeTables;
    if (c > 0x10ffff)
        return records[0];
    return records[stage2[(stage1[c >> shift] << shift) + (c & ((1 << shift) - 1))]];
}

static ALWAYS_INLINE bool has(char32_t c, UnicodeTables::Flag flag) { return recordOf(c).flags & flag; }

namespace Unicode {

bool isAlpha(char32_t c) { return has(c, UnicodeTables::IsAlpha); }
bool isLowercase(char32_t c) { return has(c, UnicodeTables::IsLowercase); }
bool isUppercase(char32_t c) { return has(c, UnicodeTables::IsUppercase); }
bool isTitlecase(char32_t c) { return has(c, UnicodeTables::IsTitlecase); }
bool isCased(char32_t c) { return has(c, UnicodeTables::IsCased); }
bool isCaseIgnorable(char32_t c) { return has(c, UnicodeTables::IsCaseIgnorable); }
bool isDecimal(char32_t c) { return has(c, UnicodeTables::IsDecimal); }
bool isDigit(char32_t c) { return has(c, UnicodeTables::IsDigit); }
bool isNumeric(char32_t c) { return has(c, UnicodeTables::IsNumeric); }
bool isLineBreak(char32_t c) { return has(c, UnicodeTables::IsLineBreak); }

int decimalValue(char32_t c)
{
    auto& record = recordOf(c);
    return record.flags & UnicodeTables::IsDecimal ? record.decimal : -1;
}

static ALWAYS_INLINE int32_t mappingOf(const UnicodeTables::Record& record, CaseMapping mapping)
{
    switch (mapping) {
    case CaseMapping::Lower:
        return record.lower;
    case CaseMapping::Upper:
        return record.upper;
    case CaseMapping::Title:
        return record.title;
    case CaseMapping::Fold:
        return record.fold;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned mapCase(char32_t c, CaseMapping mapping, char32_t (&result)[3])
{
    auto& record = recordOf(c);
    int32_t value = mappingOf(record, mapping);
    if (!(record.flags & UnicodeTables::HasSequences)) {
        result[0] = c + value;
        return 1;
    }
    unsigned length = value & 3;
    for (unsigned i = 0; i < length; ++i)
        result[i] = UnicodeTables::sequences[(value >> 2) + i];
    return length;
}

char32_t mapCaseToOne(char32_t c, CaseMapping mapping)
{
    auto& record = recordOf(c);
    int32_t value = mappingOf(record, mapping);
    return record.flags & UnicodeTables::HasSequences ? UnicodeTables::sequences[value >> 2] : c + value;
}

} // namespace Unicode

bool isXIDStart(char32_t c) { return has(c, UnicodeTables::IsXIDStart); }
bool isXIDContinue(char32_t c) { return has(c, UnicodeTables::IsXIDContinue); }
bool isPrintable(char32_t c) { return has(c, UnicodeTables::IsPrintable); }
bool isWhitespace(char32_t c) { return has(c, UnicodeTables::IsWhitespace); }

String normalizeNFKC(const String& string)
{
    if (string.containsOnlyASCII())
        return string;
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* normalizer = unorm2_getNFKCInstance(&status);
    if (U_FAILURE(status))
        return string;
    auto characters = StringView(string).upconvertedCharacters();
    auto source = characters.span();
    if (unorm2_isNormalized(normalizer, source.data(), source.size(), &status) && U_SUCCESS(status))
        return string;
    status = U_ZERO_ERROR;
    Vector<char16_t, 64> buffer(64);
    int32_t length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        buffer.grow(length);
        status = U_ZERO_ERROR;
        length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    }
    if (U_FAILURE(status))
        return string;
    return String(buffer.span().first(length));
}

String nameOfNewCodePoint(char32_t c)
{
    for (auto& range : unicodeNewNameRanges) {
        if (c >= range.first && c <= range.last)
            return makeString(range.prefix, hex(static_cast<unsigned>(c), 4));
    }
    auto* entry = std::ranges::lower_bound(unicodeNewNames, c, { }, &UnicodeNewName::character);
    if (entry != std::end(unicodeNewNames) && entry->character == c && entry->name.length())
        return entry->name;
    return { };
}

std::optional<char32_t> newCodePointFromName(std::span<const uint8_t> name)
{
    // Whatever the case of the letters, as ICU has it.
    Vector<char, 128> upperName;
    for (uint8_t byte : name)
        upperName.append(toASCIIUpper(static_cast<char>(byte)));
    std::span<const char> wanted = upperName.span();
    for (auto& range : unicodeNewNameRanges) {
        auto prefix = range.prefix.span();
        if (wanted.size() <= prefix.size() || wanted.size() > prefix.size() + 6 || !equalSpans(wanted.first(prefix.size()), prefix))
            continue;
        char32_t c = 0;
        for (char digit : wanted.subspan(prefix.size())) {
            if (!isASCIIHexDigit(digit))
                return std::nullopt;
            c = c * 16 + toASCIIHexValue(digit);
        }
        // Written as the name has it: no zeros in front.
        if (c >= range.first && c <= range.last && wanted.size() - prefix.size() == (c > 0xFFFFF ? 6u : c > 0xFFFF ? 5u : 4u))
            return c;
    }
    auto compare = [](std::span<const char> a, std::span<const char> b) { return std::ranges::lexicographical_compare(a, b); };
    auto* index = std::ranges::lower_bound(unicodeNewNamesByName, wanted, compare, [](uint16_t index) { return unicodeNewNames[index].name.span(); });
    if (index != std::end(unicodeNewNamesByName) && wanted.size() && equalSpans(unicodeNewNames[*index].name.span(), wanted))
        return unicodeNewNames[*index].character;
    return std::nullopt;
}

std::optional<char32_t> codePointFromName(std::span<const uint8_t> name)
{
    Vector<char, 128> buffer;
    buffer.append(byteCast<char>(name));
    buffer.append('\0');
    for (UCharNameChoice choice : { U_UNICODE_CHAR_NAME, U_CHAR_NAME_ALIAS }) {
        UErrorCode status = U_ZERO_ERROR;
        UChar32 result = u_charFromName(choice, buffer.span().data(), &status);
        if (U_SUCCESS(status))
            return static_cast<char32_t>(result);
    }
    if (auto added = newCodePointFromName(name))
        return added;

    // The other aliases: the names of control characters, abbreviations...
    for (char& character : buffer)
        character = toASCIIUpper(character);
    auto upperName = buffer.span().first(name.size());
    auto compare = [](std::span<const char> a, std::span<const char> b) { return std::ranges::lexicographical_compare(a, b); };
    auto* alias = std::ranges::lower_bound(unicodeNameAliases, upperName, compare, [](const UnicodeNameAlias& alias) { return alias.name.span(); });
    if (alias != std::end(unicodeNameAliases) && equalSpans(alias->name.span(), std::span<const char>(upperName)))
        return alias->character;
    return std::nullopt;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
