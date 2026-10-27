/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <algorithm>
#include <type_traits>
#include <wtf/text/CString.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> stringSearchTests();

template<typename CharacterType> using Text = Vector<CharacterType>;

static constexpr uint32_t subjectLengths[] = { 0, 1, 15, 16, 17, 64, 1000 };

template<typename CharacterType>
static Text<CharacterType> textOf(ASCIILiteral literal)
{
    Text<CharacterType> text;
    for (char character : literal.span())
        text.append(static_cast<CharacterType>(character));
    return text;
}

template<typename CharacterType>
static Text<CharacterType> repeated(CharacterType unit, uint32_t length)
{
    return Text<CharacterType>(FillWith { }, length, unit);
}

template<typename CharacterType>
static size_t findCharacter(const Text<CharacterType>& text, uint32_t unit, uint32_t start)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTStringFindCharacter8(text.span().data(), text.size(), unit, start);
    else
        return operationAOTStringFindCharacter16(text.span().data(), text.size(), unit, start);
}

template<typename CharacterType>
static size_t reverseFindCharacter(const Text<CharacterType>& text, uint32_t unit, int32_t position)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTStringReverseFindCharacter8(text.span().data(), text.size(), unit, position);
    else
        return operationAOTStringReverseFindCharacter16(text.span().data(), text.size(), unit, position);
}

template<typename CharacterType>
static size_t find(const Text<CharacterType>& text, const Text<CharacterType>& other, uint32_t start)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTStringFind8(text.span().data(), text.size(), other.span().data(), other.size(), start);
    else
        return operationAOTStringFind16(text.span().data(), text.size(), other.span().data(), other.size(), start);
}

template<typename CharacterType>
static size_t reverseFind(const Text<CharacterType>& text, const Text<CharacterType>& other, int32_t position)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTStringReverseFind8(text.span().data(), text.size(), other.span().data(), other.size(), position);
    else
        return operationAOTStringReverseFind16(text.span().data(), text.size(), other.span().data(), other.size(), position);
}

template<typename CharacterType>
static size_t countSeparators(const Text<CharacterType>& text, uint32_t separator)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTCountSeparators8(text.span().data(), text.size(), separator);
    else
        return operationAOTCountSeparators16(text.span().data(), text.size(), separator);
}

template<typename CharacterType>
static size_t findSeparators(const Text<CharacterType>& text, uint32_t separator, uint32_t* positions, uint32_t wanted)
{
    if constexpr (sizeof(CharacterType) == 1)
        return operationAOTFindSeparators8(text.span().data(), text.size(), separator, positions, wanted);
    else
        return operationAOTFindSeparators16(text.span().data(), text.size(), separator, positions, wanted);
}

template<typename CharacterType>
static size_t naiveFindCharacter(const Text<CharacterType>& text, uint32_t unit, uint32_t start)
{
    for (size_t i = start; i < text.size(); ++i) {
        if (text[i] == unit)
            return i;
    }
    return notFound;
}

template<typename CharacterType>
static size_t naiveReverseFindCharacter(const Text<CharacterType>& text, uint32_t unit, int32_t position)
{
    if (position < 0)
        return notFound;
    for (size_t i = std::min<size_t>(static_cast<size_t>(position) + 1, text.size()); i--;) {
        if (text[i] == unit)
            return i;
    }
    return notFound;
}

template<typename CharacterType>
static bool hasTextAt(const Text<CharacterType>& text, size_t index, const Text<CharacterType>& other)
{
    for (size_t k = 0; k < other.size(); ++k) {
        if (text[index + k] != other[k])
            return false;
    }
    return true;
}

template<typename CharacterType>
static size_t naiveFind(const Text<CharacterType>& text, const Text<CharacterType>& other, uint32_t start)
{
    for (size_t i = start; i + other.size() <= text.size(); ++i) {
        if (hasTextAt(text, i, other))
            return i;
    }
    return notFound;
}

template<typename CharacterType>
static size_t naiveReverseFind(const Text<CharacterType>& text, const Text<CharacterType>& other, int32_t position)
{
    if (other.size() > text.size())
        return notFound;
    for (size_t i = std::min<size_t>(std::max(position, 0), text.size() - other.size()) + 1; i--;) {
        if (hasTextAt(text, i, other))
            return i;
    }
    return notFound;
}

static Vector<int32_t> positionsFor(uint32_t length)
{
    return { 0, 1, static_cast<int32_t>(length / 2), static_cast<int32_t>(length) - 1, static_cast<int32_t>(length), static_cast<int32_t>(length) + 1, static_cast<int32_t>(length) + 1000, INT32_MAX };
}

template<typename CharacterType>
static Vector<Text<CharacterType>> subjectsWithUnit(uint32_t length, CharacterType unit, CharacterType filler)
{
    Vector<Text<CharacterType>> subjects;
    subjects.append(repeated(filler, length));
    subjects.append(repeated(unit, length));
    if (length) {
        Text<CharacterType> atFirst = repeated(filler, length);
        atFirst[0] = unit;
        subjects.append(WTF::move(atFirst));
        Text<CharacterType> atLast = repeated(filler, length);
        atLast[length - 1] = unit;
        subjects.append(WTF::move(atLast));
        Text<CharacterType> inTheMiddle = repeated(filler, length);
        inTheMiddle[length / 2] = unit;
        subjects.append(WTF::move(inTheMiddle));
    }
    return subjects;
}

template<typename CharacterType>
static void checkFindCharacter(CharacterType unit)
{
    for (uint32_t length : subjectLengths) {
        for (const Text<CharacterType>& subject : subjectsWithUnit<CharacterType>(length, unit, '.')) {
            for (int32_t start : positionsFor(length)) {
                if (start >= 0)
                    AOT_TEST_CHECK(findCharacter(subject, unit, start) == naiveFindCharacter(subject, unit, start));
            }
        }
    }
}

static void testFindCharacter()
{
    checkFindCharacter<Latin1Character>('x');
    checkFindCharacter<Latin1Character>(0xe9);
    checkFindCharacter<char16_t>('x');
    checkFindCharacter<char16_t>(0x65e5);

    Text<Latin1Character> narrow = textOf<Latin1Character>("abcAbc"_s);
    AOT_TEST_CHECK(findCharacter(narrow, 'c', 0) == 2 && findCharacter(narrow, 'c', 3) == 5 && findCharacter(narrow, 'c', 6) == notFound);
    AOT_TEST_CHECK(findCharacter(narrow, 'A', 0) == 3);
    AOT_TEST_CHECK(findCharacter(narrow, 0x100 + 'A', 0) == notFound);
    AOT_TEST_CHECK(findCharacter(narrow, 0x10000 + 'A', 0) == notFound);

    Text<char16_t> wide = textOf<char16_t>("abcAbc"_s);
    wide.append(0x100 + 'A');
    AOT_TEST_CHECK(findCharacter(wide, 0x100 + 'A', 0) == 6 && findCharacter(wide, 'A', 0) == 3);
    AOT_TEST_CHECK(findCharacter(wide, 0x10000 + 'A', 0) == notFound);
    AOT_TEST_CHECK(static_cast<int32_t>(findCharacter(wide, 'z', 0)) == -1);
}

template<typename CharacterType>
static void checkReverseFindCharacter(CharacterType unit)
{
    for (uint32_t length : subjectLengths) {
        for (const Text<CharacterType>& subject : subjectsWithUnit<CharacterType>(length, unit, '.')) {
            for (int32_t position : positionsFor(length))
                AOT_TEST_CHECK(reverseFindCharacter(subject, unit, position) == naiveReverseFindCharacter(subject, unit, position));
            AOT_TEST_CHECK(reverseFindCharacter(subject, unit, -1) == notFound);
            AOT_TEST_CHECK(reverseFindCharacter(subject, unit, INT32_MIN) == notFound);
        }
    }
}

static void testReverseFindCharacter()
{
    checkReverseFindCharacter<Latin1Character>('x');
    checkReverseFindCharacter<Latin1Character>(0xe9);
    checkReverseFindCharacter<char16_t>('x');
    checkReverseFindCharacter<char16_t>(0x65e5);

    Text<Latin1Character> narrow = textOf<Latin1Character>("abcAbc"_s);
    AOT_TEST_CHECK(reverseFindCharacter(narrow, 'c', 5) == 5 && reverseFindCharacter(narrow, 'c', 4) == 2 && reverseFindCharacter(narrow, 'c', 1) == notFound);
    AOT_TEST_CHECK(reverseFindCharacter(narrow, 'a', 0) == 0 && reverseFindCharacter(narrow, 'a', -1) == notFound);
    AOT_TEST_CHECK(reverseFindCharacter(narrow, 0x100 + 'A', 5) == notFound);

    Text<char16_t> wide = textOf<char16_t>("abcAbc"_s);
    AOT_TEST_CHECK(reverseFindCharacter(wide, 'a', 0) == 0 && reverseFindCharacter(wide, 'a', -1) == notFound);
    AOT_TEST_CHECK(reverseFindCharacter(wide, 0x10000 + 'A', 5) == notFound);
}

template<typename CharacterType>
static Vector<Text<CharacterType>> needles()
{
    return {
        textOf<CharacterType>(""_s),
        textOf<CharacterType>("a"_s),
        textOf<CharacterType>("ab"_s),
        textOf<CharacterType>("aaab"_s),
        textOf<CharacterType>("0123456789abcdef"_s),
        textOf<CharacterType>("0123456789abcdefg"_s),
    };
}

template<typename CharacterType>
static Vector<Text<CharacterType>> subjectsFor(const Text<CharacterType>& needle)
{
    Vector<Text<CharacterType>> subjects;
    subjects.append(textOf<CharacterType>(""_s));
    subjects.append(textOf<CharacterType>("aaaaab"_s));
    subjects.append(textOf<CharacterType>("a"_s));
    subjects.append(needle);
    for (uint32_t padding : { 1u, 15u, 40u }) {
        Text<CharacterType> atStart = needle;
        atStart.appendVector(repeated<CharacterType>('.', padding));
        subjects.append(atStart);

        Text<CharacterType> atEnd = repeated<CharacterType>('.', padding);
        atEnd.appendVector(needle);
        subjects.append(atEnd);

        Text<CharacterType> atBothEnds = atStart;
        atBothEnds.appendVector(needle);
        subjects.append(WTF::move(atBothEnds));

        subjects.append(repeated<CharacterType>('.', padding));
    }
    if (!needle.isEmpty()) {
        Text<CharacterType> shorter = needle;
        shorter.removeLast();
        subjects.append(WTF::move(shorter));
    }
    return subjects;
}

template<typename CharacterType>
static void checkFind()
{
    for (const Text<CharacterType>& needle : needles<CharacterType>()) {
        for (const Text<CharacterType>& subject : subjectsFor(needle)) {
            for (int32_t start : positionsFor(subject.size())) {
                if (start >= 0)
                    AOT_TEST_CHECK(find(subject, needle, start) == naiveFind(subject, needle, start));
            }
        }
    }
    // Many places have the first and the last character of the text and not what lies between. The text itself lies at every distance from both ends.
    for (ASCIILiteral wanted : { "a"_s, "ab"_s, "axb"_s, "a0123456789012345b"_s }) {
        Text<CharacterType> needle = textOf<CharacterType>(wanted);
        for (uint32_t length = 0; length <= 70; ++length) {
            for (uint32_t place = 0; place <= length; ++place) {
                Text<CharacterType> text;
                for (uint32_t i = 0; i < length; ++i)
                    text.append(static_cast<CharacterType>("ayb"[i % 3]));
                if (place + needle.size() <= length) {
                    for (uint32_t i = 0; i < needle.size(); ++i)
                        text[place + i] = needle[i];
                }
                for (uint32_t start : { 0u, place ? place - 1 : 0u, place, std::min(place + 1, length), length })
                    AOT_TEST_CHECK(find(text, needle, start) == naiveFind(text, needle, start));
            }
        }
    }
    Text<CharacterType> subject = textOf<CharacterType>("aaaaab"_s);
    AOT_TEST_CHECK(find(subject, textOf<CharacterType>("aaab"_s), 0) == 2);
    AOT_TEST_CHECK(find(subject, textOf<CharacterType>("aaab"_s), 3) == notFound);
    AOT_TEST_CHECK(find(subject, textOf<CharacterType>("aaaaabb"_s), 0) == notFound);
    AOT_TEST_CHECK(find(subject, textOf<CharacterType>(""_s), 4) == 4);
    AOT_TEST_CHECK(find(subject, textOf<CharacterType>(""_s), 6) == 6);
}

static void testFind()
{
    checkFind<Latin1Character>();
    checkFind<char16_t>();
}

template<typename CharacterType>
static void checkReverseFind()
{
    for (const Text<CharacterType>& needle : needles<CharacterType>()) {
        for (const Text<CharacterType>& subject : subjectsFor(needle)) {
            for (int32_t position : positionsFor(subject.size()))
                AOT_TEST_CHECK(reverseFind(subject, needle, position) == naiveReverseFind(subject, needle, position));
            AOT_TEST_CHECK(reverseFind(subject, needle, -1) == reverseFind(subject, needle, 0));
            AOT_TEST_CHECK(reverseFind(subject, needle, INT32_MIN) == naiveReverseFind(subject, needle, 0));
        }
    }
    Text<CharacterType> subject = textOf<CharacterType>("aaaaab"_s);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("aaab"_s), 6) == 2);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("aaab"_s), 1) == notFound);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("aa"_s), 6) == 3);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("aa"_s), -5) == 0);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("ab"_s), -5) == notFound);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>("aaaaabb"_s), 6) == notFound);
    AOT_TEST_CHECK(reverseFind(subject, textOf<CharacterType>(""_s), 100) == 6);
}

static void testReverseFind()
{
    checkReverseFind<Latin1Character>();
    checkReverseFind<char16_t>();
}

static size_t naiveUTF8Length(const Text<char16_t>& text)
{
    size_t result = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char16_t unit = text[i];
        if (unit < 0x80)
            result += 1;
        else if (unit < 0x800)
            result += 2;
        else if (U16_IS_LEAD(unit) && i + 1 < text.size() && U16_IS_TRAIL(text[i + 1])) {
            result += 4;
            ++i;
        } else
            result += 3;
    }
    return result;
}

static void checkUTF8Length(const Text<Latin1Character>& text, size_t expected)
{
    AOT_TEST_CHECK(operationAOTStringUTF8Length8(text.span().data(), text.size()) == expected);
    AOT_TEST_CHECK(String(text.span()).utf8().length() == expected);
}

static void checkUTF8Length(const Text<char16_t>& text, size_t expected)
{
    AOT_TEST_CHECK(naiveUTF8Length(text) == expected);
    AOT_TEST_CHECK(operationAOTStringUTF8Length16(text.span().data(), text.size()) == expected);
    AOT_TEST_CHECK(String(text.span()).utf8().length() == expected);
}

static void testUTF8Length()
{
    for (uint32_t length : { 0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 64u, 1000u }) {
        checkUTF8Length(repeated<Latin1Character>('a', length), length);
        checkUTF8Length(repeated<Latin1Character>(0xe9, length), 2 * length);
        checkUTF8Length(repeated<Latin1Character>(0x7f, length), length);
        checkUTF8Length(repeated<Latin1Character>(0x80, length), 2 * length);
        for (uint32_t index = 0; index < length; index += std::max(1u, length / 16)) {
            Text<Latin1Character> text = repeated<Latin1Character>('a', length);
            text[index] = 0xff;
            checkUTF8Length(text, length + 1);
        }

        checkUTF8Length(repeated<char16_t>('a', length), length);
        checkUTF8Length(repeated<char16_t>(0x7ff, length), 2 * length);
        checkUTF8Length(repeated<char16_t>(0x800, length), 3 * length);
        checkUTF8Length(repeated<char16_t>(0xffff, length), 3 * length);
        checkUTF8Length(repeated<char16_t>(0xd83d, length), 3 * length);
        checkUTF8Length(repeated<char16_t>(0xde00, length), 3 * length);
    }

    checkUTF8Length(Text<char16_t> { 0xd83d, 0xde00 }, 4);
    checkUTF8Length(Text<char16_t> { 'a', 0xd83d, 0xde00, 'b' }, 6);
    checkUTF8Length(Text<char16_t> { 0xd83d }, 3);
    checkUTF8Length(Text<char16_t> { 0xd83d, 'a' }, 4);
    checkUTF8Length(Text<char16_t> { 0xde00 }, 3);
    checkUTF8Length(Text<char16_t> { 'a', 0xde00 }, 4);
    checkUTF8Length(Text<char16_t> { 0xde00, 0xd83d }, 6);
    checkUTF8Length(Text<char16_t> { 0xd83d, 0xd83d, 0xde00 }, 7);
    checkUTF8Length(Text<char16_t> { 0xd83d, 0xde00, 0xde00 }, 7);
}

template<typename CharacterType>
static void checkSeparators(ASCIILiteral literal, std::initializer_list<uint32_t> expectedPositions)
{
    constexpr uint32_t untouched = 0xdddddddd;
    Text<CharacterType> text = textOf<CharacterType>(literal);
    Vector<uint32_t> expected(expectedPositions);
    AOT_TEST_CHECK(countSeparators(text, ',') == expected.size());
    AOT_TEST_CHECK(!countSeparators(text, ';'));

    for (uint32_t wanted = 0; wanted <= expected.size() + 2; ++wanted) {
        Vector<uint32_t> positions(FillWith { }, expected.size() + 3, untouched);
        size_t found = findSeparators(text, ',', positions.mutableSpan().data(), wanted);
        AOT_TEST_CHECK(found == std::min<size_t>(wanted, expected.size()));
        for (size_t i = 0; i < positions.size(); ++i)
            AOT_TEST_CHECK(positions[i] == (i < found ? expected[i] : untouched));
    }

    uint32_t position = untouched;
    AOT_TEST_CHECK(!findSeparators(text, ';', &position, 1) && position == untouched);
}

template<typename CharacterType>
static void checkAllSeparators()
{
    checkSeparators<CharacterType>(""_s, { });
    checkSeparators<CharacterType>("abc"_s, { });
    checkSeparators<CharacterType>(","_s, { 0 });
    checkSeparators<CharacterType>("a,b"_s, { 1 });
    checkSeparators<CharacterType>("a,,b"_s, { 1, 2 });
    checkSeparators<CharacterType>(",a,b,"_s, { 0, 2, 4 });
    checkSeparators<CharacterType>(",,,,"_s, { 0, 1, 2, 3 });
    checkSeparators<CharacterType>("0123456789abcdef,0123456789abcdef,0123456789abcdefg,"_s, { 16, 33, 51 });
}

static void testSeparators()
{
    checkAllSeparators<Latin1Character>();
    checkAllSeparators<char16_t>();

    Text<Latin1Character> narrow = textOf<Latin1Character>("a,b"_s);
    uint32_t position = 7;
    AOT_TEST_CHECK(!countSeparators(narrow, 0x100 + ','));
    AOT_TEST_CHECK(!findSeparators(narrow, 0x100 + ',', &position, 1) && position == 7);

    Text<char16_t> wide { 'a', 0x3001, 'b', ',' };
    AOT_TEST_CHECK(countSeparators(wide, 0x3001) == 1 && countSeparators(wide, ',') == 1 && !countSeparators(wide, 0x01));
    AOT_TEST_CHECK(findSeparators(wide, 0x3001, &position, 1) == 1 && position == 1);
}

static String in8Bits(ASCIILiteral literal)
{
    if (literal.isEmpty())
        return emptyString();
    String text = String(textOf<Latin1Character>(literal).span());
    AOT_TEST_CHECK(text.is8Bit());
    return text;
}

static String in16Bits(ASCIILiteral literal)
{
    if (literal.isEmpty())
        return emptyString();
    String text = String(textOf<char16_t>(literal).span());
    AOT_TEST_CHECK(!text.is8Bit());
    return text;
}

static int64_t signOf(std::strong_ordering order)
{
    return order < 0 ? -1 : order > 0;
}

static void testImpls()
{
    struct Pair {
        ASCIILiteral first;
        ASCIILiteral second;
        bool isEqual;
        int64_t order;
    };
    static constexpr Pair pairs[] = {
        { "same text"_s, "same text"_s, true, 0 },
        { "same texs"_s, "same text"_s, false, -1 },
        { "same text"_s, "same texs"_s, false, 1 },
        { "same"_s, "same text"_s, false, -1 },
        { "same text"_s, "same"_s, false, 1 },
        { ""_s, ""_s, true, 0 },
        { ""_s, "a"_s, false, -1 },
        { "0123456789abcdef0"_s, "0123456789abcdef1"_s, false, -1 },
    };
    for (const Pair& pair : pairs) {
        for (const String& first : { in8Bits(pair.first), in16Bits(pair.first) }) {
            for (const String& second : { in8Bits(pair.second), in16Bits(pair.second) }) {
                AOT_TEST_CHECK(!!operationAOTStringImplsAreEqual(first.impl(), second.impl()) == pair.isEqual);
                AOT_TEST_CHECK(WTF::equal(first.impl(), second.impl()) == pair.isEqual);
                AOT_TEST_CHECK(operationAOTCompareStringImpls(first.impl(), second.impl()) == pair.order);
                AOT_TEST_CHECK(signOf(codePointCompare(StringView(first), StringView(second))) == pair.order);
            }
        }
    }

    String accent = String(Text<Latin1Character> { 0xe9 }.span());
    String wideAccent = String(Text<char16_t> { 0xe9 }.span());
    String kanji = String(Text<char16_t> { 0x65e5 }.span());
    AOT_TEST_CHECK(operationAOTStringImplsAreEqual(accent.impl(), wideAccent.impl()));
    AOT_TEST_CHECK(!operationAOTCompareStringImpls(accent.impl(), wideAccent.impl()));
    AOT_TEST_CHECK(!operationAOTStringImplsAreEqual(accent.impl(), kanji.impl()));
    AOT_TEST_CHECK(operationAOTCompareStringImpls(accent.impl(), kanji.impl()) == -1);
    AOT_TEST_CHECK(operationAOTCompareStringImpls(kanji.impl(), accent.impl()) == 1);

    for (const String& text : { in8Bits("same text"_s), in16Bits("same text"_s), in8Bits(""_s), accent, wideAccent, kanji }) {
        String copy = text.isolatedCopy();
        AOT_TEST_CHECK(!copy.impl()->hasHash() || copy.isEmpty());
        AOT_TEST_CHECK(operationAOTHashStringImpl(copy.impl()) == text.impl()->hash() && copy.impl()->hasHash() && copy.impl()->existingHash() == text.impl()->hash());
    }
}

static constexpr ASCIILiteral programWithSeparators =
    "layout L0 name=\"Separators\" fields=["
    "{key=\", \" declared={type=number}}, "
    "{key=\"aa\" declared={type=number}}, "
    "{key=\"caf\\u{e9}\" declared={type=number}}, "
    "{key=\"\\u{65e5}\\u{672c}\" declared={type=number}}]\n"
    "variable g0 name=\"kept\" module=m0 declared={type=anything} initialization=Undefined\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0 exports=[{name=\"kept\" variable=g0}]\n"
    "entry module=m0 hostReads=[{exportName=\"kept\"}]\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0, v0, v0 layout=L0 fields=[0, 1, 2, 3]\n"
    "    PutModuleVariable v1 variable=g0\n"
    "    Return\n"
    "}\n"_s;

static uint32_t findText(StringView wanted)
{
    const ImageText& text = Image::current()->text();
    for (uint32_t index = 0; index < text.numberOfStrings(); ++index) {
        if (text.hasText(StringID(index)) && text.equals(StringID(index), wanted))
            return index;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the text");
}

template<typename CharacterType>
static void checkLongSeparators(Instance* instance, uint32_t separator, ASCIILiteral literal, std::initializer_list<uint32_t> expectedPositions)
{
    constexpr uint32_t untouched = 0xdddddddd;
    Text<CharacterType> text = textOf<CharacterType>(literal);
    Vector<uint32_t> expected(expectedPositions);
    auto count = [&] {
        if constexpr (sizeof(CharacterType) == 1)
            return operationAOTCountLongSeparators8(instance, text.span().data(), text.size(), separator);
        else
            return operationAOTCountLongSeparators16(instance, text.span().data(), text.size(), separator);
    };
    auto findUpTo = [&](uint32_t* positions, uint32_t wanted) {
        if constexpr (sizeof(CharacterType) == 1)
            return operationAOTFindLongSeparators8(instance, text.span().data(), text.size(), separator, positions, wanted);
        else
            return operationAOTFindLongSeparators16(instance, text.span().data(), text.size(), separator, positions, wanted);
    };

    AOT_TEST_CHECK(count() == expected.size());
    for (uint32_t wanted = 0; wanted <= expected.size() + 2; ++wanted) {
        Vector<uint32_t> positions(FillWith { }, expected.size() + 3, untouched);
        size_t found = findUpTo(positions.mutableSpan().data(), wanted);
        AOT_TEST_CHECK(found == std::min<size_t>(wanted, expected.size()));
        for (size_t i = 0; i < positions.size(); ++i)
            AOT_TEST_CHECK(positions[i] == (i < found ? expected[i] : untouched));
    }
}

template<typename CharacterType>
static void checkAllLongSeparators(Instance* instance)
{
    uint32_t commaAndSpace = findText(", "_s);
    checkLongSeparators<CharacterType>(instance, commaAndSpace, ""_s, { });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, ","_s, { });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, "a,b ,c"_s, { });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, ", "_s, { 0 });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, "a, b"_s, { 1 });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, "a, , b"_s, { 1, 3 });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, ", a, b, "_s, { 0, 3, 6 });
    checkLongSeparators<CharacterType>(instance, commaAndSpace, "a, b,"_s, { 1 });

    uint32_t twoLetters = findText("aa"_s);
    checkLongSeparators<CharacterType>(instance, twoLetters, "a"_s, { });
    checkLongSeparators<CharacterType>(instance, twoLetters, "aaa"_s, { 0 });
    checkLongSeparators<CharacterType>(instance, twoLetters, "aaaa"_s, { 0, 2 });
    checkLongSeparators<CharacterType>(instance, twoLetters, "aaaaa"_s, { 0, 2 });
    checkLongSeparators<CharacterType>(instance, twoLetters, "baaab"_s, { 1 });
}

static void testLongSeparators8(LoadedProgram& program)
{
    checkAllLongSeparators<Latin1Character>(&program.instance());
}

static void testLongSeparators16(LoadedProgram& program)
{
    checkAllLongSeparators<char16_t>(&program.instance());
}

static void testImplEqualsTextOfTheImage(LoadedProgram&)
{
    String accent = String(Text<Latin1Character> { 'c', 'a', 'f', 0xe9 }.span());
    String wideAccent = String(Text<char16_t> { 'c', 'a', 'f', 0xe9 }.span());
    String kanji = String(Text<char16_t> { 0x65e5, 0x672c }.span());
    uint32_t accentInImage = findText(accent);
    uint32_t kanjiInImage = findText(kanji);

    AOT_TEST_CHECK(operationAOTStringImplEqualsImageString(accent.impl(), accentInImage));
    AOT_TEST_CHECK(operationAOTStringImplEqualsImageString(wideAccent.impl(), accentInImage));
    AOT_TEST_CHECK(operationAOTStringImplEqualsImageString(kanji.impl(), kanjiInImage));
    AOT_TEST_CHECK(!operationAOTStringImplEqualsImageString(kanji.impl(), accentInImage));
    AOT_TEST_CHECK(!operationAOTStringImplEqualsImageString(accent.impl(), kanjiInImage));
    AOT_TEST_CHECK(!operationAOTStringImplEqualsImageString(in8Bits("cafe"_s).impl(), accentInImage));
    AOT_TEST_CHECK(!operationAOTStringImplEqualsImageString(in8Bits("caf"_s).impl(), accentInImage));
    AOT_TEST_CHECK(!operationAOTStringImplEqualsImageString(in8Bits(""_s).impl(), accentInImage));
}

template<LoadedProgramTest body>
static constexpr void (*withSeparators)() = runNativeTestWithLoadedProgram<stringSearchTests, programWithSeparators, body>;

std::span<const NativeTest> stringSearchTests()
{
    static constexpr NativeTest tests[] = {
        { "string-search/find-character"_s, testFindCharacter },
        { "string-search/reverse-find-character"_s, testReverseFindCharacter },
        { "string-search/find"_s, testFind },
        { "string-search/reverse-find"_s, testReverseFind },
        { "string-search/utf8-length"_s, testUTF8Length },
        { "string-search/separators"_s, testSeparators },
        { "string-search/impls"_s, testImpls },
        { "string-search/long-separators-8"_s, withSeparators<testLongSeparators8> },
        { "string-search/long-separators-16"_s, withSeparators<testLongSeparators16> },
        { "string-search/impl-equals-text-of-the-image"_s, withSeparators<testImplEqualsTextOfTheImage> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
