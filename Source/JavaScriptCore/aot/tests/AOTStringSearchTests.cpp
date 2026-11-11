/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTRuntimeFunctions.h"
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
                for (uint32_t position : { 0u, place ? place - 1 : 0u, place, std::min(place + 1, length), length })
                    AOT_TEST_CHECK(reverseFind(text, needle, position) == naiveReverseFind(text, needle, position));
            }
        }
    }
    for (uint32_t lengthOfNeedle : { 1u, 2u, 31u, 32u, 33u, 34u, 70u }) {
        Text<CharacterType> needle = repeated<CharacterType>('a', lengthOfNeedle);
        needle.last() = 'b';
        for (uint32_t length : { 33u, 48u, 64u, 65u, 100u, 200u }) {
            for (uint32_t place = 0; place <= length; place += 7) {
                for (CharacterType filler : { '.', 'a' }) {
                    Text<CharacterType> text = repeated<CharacterType>(filler, length);
                    if (place + needle.size() <= length) {
                        for (uint32_t i = 0; i < needle.size(); ++i)
                            text[place + i] = needle[i];
                    }
                    for (uint32_t position : { 0u, place ? place - 1 : 0u, place, length / 2, length }) {
                        AOT_TEST_CHECK(reverseFind(text, needle, position) == naiveReverseFind(text, needle, position));
                        AOT_TEST_CHECK(find(text, needle, position) == naiveFind(text, needle, position));
                    }
                }
            }
        }
    }
    for (ASCIILiteral unit : { "ab"_s, "a"_s, "aab"_s, "abaab"_s, "abcabcab"_s, "aabaabaaab"_s }) {
        Text<CharacterType> letters = textOf<CharacterType>(unit);
        for (uint32_t lengthOfNeedle : { 33u, 40u, 64u, 97u }) {
            for (uint32_t turned : { 0u, 1u, 15u, 16u, 17u, lengthOfNeedle / 2, lengthOfNeedle - 3, lengthOfNeedle - 2, lengthOfNeedle }) {
                Text<CharacterType> needle;
                for (uint32_t i = 0; i < lengthOfNeedle; ++i)
                    needle.append(letters[i % letters.size()]);
                if (turned + 1 < lengthOfNeedle)
                    std::swap(needle[turned], needle[turned + 1]);
                for (uint32_t gap = lengthOfNeedle - 3; gap <= lengthOfNeedle + 3; ++gap) {
                    Text<CharacterType> text;
                    for (uint32_t i = 0; i < 1000; ++i)
                        text.append(i % gap ? letters[i % letters.size()] : static_cast<CharacterType>('x'));
                    for (uint32_t position : { 0u, 1u, 500u, 1000u }) {
                        AOT_TEST_CHECK(reverseFind(text, needle, position) == naiveReverseFind(text, needle, position));
                        AOT_TEST_CHECK(find(text, needle, position) == naiveFind(text, needle, position));
                    }
                }
                for (uint32_t length : { 400u, 1000u }) {
                    for (uint32_t place : { 0u, 1u, length / 3, length / 2 + 1, length - lengthOfNeedle - 1, length - lengthOfNeedle, length }) {
                        Text<CharacterType> text;
                        for (uint32_t i = 0; i < length; ++i)
                            text.append(letters[i % letters.size()]);
                        if (place + lengthOfNeedle <= length) {
                            for (uint32_t i = 0; i < lengthOfNeedle; ++i)
                                text[place + i] = needle[i];
                        }
                        for (uint32_t position : { 0u, 1u, place ? place - 1 : 0u, place, place + 1, length / 2, length }) {
                            AOT_TEST_CHECK(reverseFind(text, needle, position) == naiveReverseFind(text, needle, position));
                            AOT_TEST_CHECK(find(text, needle, position) == naiveFind(text, needle, position));
                        }
                    }
                }
            }
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

static String in8Bits(ASCIILiteral literal)
{
    if (literal.isEmpty())
        return emptyString();
    String text = String(textOf<Latin1Character>(literal).span());
    AOT_TEST_CHECK(text.is8Bit());
    return text;
}

static constexpr ASCIILiteral programWithTexts =
    "layout L0 name=\"Texts\" fields=["
    "{key=\"caf\\u{e9}\" declared={type=number}}, "
    "{key=\"\\u{65e5}\\u{672c}\" declared={type=number}}]\n"
    "variable g0 name=\"kept\" module=m0 declared={type=anything} initialization=Undefined\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0 exports=[{name=\"kept\" variable=g0}]\n"
    "entry module=m0 hostReads=[{exportName=\"kept\"}]\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]\n"
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

static void testElementsSpreadFromPartOfTextThatBecameNarrow(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Text<Latin1Character> narrow { 0, 0xd8, 0, 0xdc, 0, 0xd8, 0, 0xdc };
    Text<char16_t> wide { 0, 0xd8, 0, 0xdc, 0, 0xd8, 0, 0xdc };
    AtomString name { String(narrow.span()) };
    Strong<JSString> whole(vm, jsString(vm, String(wide.span())));
    Strong<JSString> part(vm, jsSubstring(realm, whole.get(), 0, 4));
    AOT_TEST_CHECK(part->isSubstring() && !part->is8Bit());
    AOT_TEST_CHECK(numberOfElementsSpreadFrom(realm, part.get()) == 4);

    whole->toIdentifier(realm);
    AOT_TEST_CHECK(whole->tryGetValueImpl() == name.impl() && whole->is8Bit());
    AOT_TEST_CHECK(part->isSubstring() && !part->is8Bit());
    AOT_TEST_CHECK(numberOfElementsSpreadFrom(realm, part.get()) == 4);

    Text<char16_t> pairs { 0xd83d, 0xde00, 'a', 0xd83d, 0xd83d, 0xde00, 0xde00 };
    AOT_TEST_CHECK(numberOfElementsSpreadFrom(realm, jsString(vm, String(pairs.span()))) == 5);
}

template<LoadedProgramTest body>
static constexpr void (*withTexts)() = runNativeTestWithLoadedProgram<stringSearchTests, programWithTexts, body>;

std::span<const NativeTest> stringSearchTests()
{
    static constexpr NativeTest tests[] = {
        { "string-search/find"_s, testFind },
        { "string-search/reverse-find"_s, testReverseFind },
        { "string-search/utf8-length"_s, testUTF8Length },
        { "string-search/impl-equals-text-of-the-image"_s, withTexts<testImplEqualsTextOfTheImage> },
        { "string-search/elements-spread-from-part-of-text-that-became-narrow"_s, withTexts<testElementsSpreadFromPartOfTextThatBecameNarrow> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
