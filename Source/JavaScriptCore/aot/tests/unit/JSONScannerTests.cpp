/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTJSONHooks.h"
#include "AOTJSONScanner.h"
#include "AOTJSONValueParser.h"
#include "AOTSchemaRuntimeOperations.h"
#include <initializer_list>
#include <string>
#include <wtf/FastFloat.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::JSONMemberSearch;

static_assert(maxJSONInteger == (int64_t(1) << 53) - 1);
static_assert(maxSkippedJSONDepth == 64);
static_assert(2 * SIMD::stride<uint8_t> <= jsonSourcePadding);

using ScanFunction = const uint8_t* (*)(const uint8_t*);

struct OffsetCase {
    ASCIILiteral text;
    ptrdiff_t offset;
};

struct SkippedStringCase {
    ASCIILiteral text;
    ptrdiff_t offset;
    bool hasEscapes;
};

struct IntegerCase {
    ASCIILiteral text;
    int64_t integer;
    ptrdiff_t offset;
};

static PaddedText paddedBytes(std::initializer_list<uint8_t> bytes)
{
    return PaddedText(std::span<const uint8_t> { bytes.begin(), bytes.size() });
}

static PaddedText paddedRun(size_t count, uint8_t repeated, std::initializer_list<uint8_t> suffix)
{
    Vector<uint8_t> bytes(count, [&](size_t) { return repeated; });
    for (uint8_t byte : suffix)
        bytes.append(byte);
    return PaddedText(bytes.span());
}

static PaddedText paddedString(const String& text)
{
    AOT_TEST_CHECK(text.is8Bit());
    return PaddedText(byteCast<uint8_t>(text.span8()));
}

static void checkOffsets(ScanFunction scan, std::span<const OffsetCase> cases)
{
    for (const OffsetCase& testCase : cases) {
        PaddedText text(testCase.text);
        AOT_TEST_CHECK(scan(text.start()) == text.start() + testCase.offset);
    }
}

static void checkRejected(ScanFunction scan, std::span<const ASCIILiteral> texts)
{
    for (ASCIILiteral literal : texts) {
        PaddedText text(literal);
        AOT_TEST_CHECK(!scan(text.start()));
    }
}

static String nestedArrays(unsigned depth, ASCIILiteral innermost = "1"_s)
{
    StringBuilder builder;
    for (unsigned i = 0; i < depth; ++i)
        builder.append('[');
    builder.append(innermost);
    for (unsigned i = 0; i < depth; ++i)
        builder.append(']');
    return builder.toString();
}

static String nestedMixed(unsigned depth)
{
    StringBuilder builder;
    for (unsigned i = 0; i < depth; ++i)
        builder.append(i % 2 ? "["_s : "{\"k\":"_s);
    builder.append('0');
    for (unsigned i = depth; i--;)
        builder.append(i % 2 ? ']' : '}');
    return builder.toString();
}

static String quotedRun(size_t length, std::u16string_view suffix)
{
    std::u16string text(1, u'"');
    text.append(length, u'a');
    text.append(suffix);
    text.push_back(u'"');
    return text16(text);
}

static String stringValue(JSValue value)
{
    AOT_TEST_CHECK(value.isString());
    return asString(value)->tryGetValue().data;
}

static String parseScalarString(DetachedInstance& fixture, const uint8_t* cursor)
{
    auto scalar = tryParseJSONScalarAt(fixture.instance(), fixture.vm(), cursor);
    AOT_TEST_CHECK(scalar && scalar->end == fixture.words().sourceEnd);
    return stringValue(scalar->value);
}

static const uint8_t* openBytes(DetachedInstance& fixture, std::span<const uint8_t> bytes)
{
    AOT_TEST_CHECK(fixture.runtime().open(fixture.bytes(bytes), stackPointerAtDepth(1)));
    return fixture.words().sourceStart;
}

static void testWhiteSpace()
{
    PaddedText empty(""_s);
    AOT_TEST_CHECK(skipJSONWhitespaceCharacters(empty.start()) == empty.start());
    PaddedText spaceThenValue(" \n\r\t x"_s);
    AOT_TEST_CHECK(skipJSONWhitespaceCharacters(spaceThenValue.start()) == spaceThenValue.start() + 5);
    PaddedText onlySpace("  \t\n"_s);
    AOT_TEST_CHECK(skipJSONWhitespaceCharacters(onlySpace.start()) == onlySpace.end());

    static constexpr uint8_t otherSpaces[] = { 0x0b, 0x0c, 0x1f, 0x85, 0xa0 };
    for (uint8_t byte : otherSpaces) {
        PaddedText text = paddedBytes({ byte });
        AOT_TEST_CHECK(skipJSONWhitespaceCharacters(text.start()) == text.start());
    }
}

static void testPlainCharacters()
{
    static constexpr uint8_t stops[] = { '"', '\\', 0x01, 0x0a, 0x1f };
    for (size_t length = 0; length <= 80; ++length) {
        for (uint8_t stop : stops) {
            PaddedText text = paddedRun(length, 'a', { stop, 'a', 'a', 'a' });
            AOT_TEST_CHECK(findEndOfPlainJSONCharacters(text.start()) == text.start() + length);
        }
        PaddedText withoutStop = paddedRun(length, 'a', { });
        AOT_TEST_CHECK(findEndOfPlainJSONCharacters(withoutStop.start()) == withoutStop.end());
    }

    PaddedText plainBytes = paddedBytes({ 0x20, 0x21, 0x7f, 0x80, 0xfe, 0xff, '"' });
    AOT_TEST_CHECK(findEndOfPlainJSONCharacters(plainBytes.start()) == plainBytes.start() + 6);
}

static void testEscapeLengths()
{
    static constexpr ASCIILiteral shortEscapes[] = { "\\\""_s, "\\\\"_s, "\\/"_s, "\\b"_s, "\\f"_s, "\\n"_s, "\\r"_s, "\\t"_s };
    for (ASCIILiteral literal : shortEscapes) {
        PaddedText text(literal);
        AOT_TEST_CHECK(jsonEscapeLength(text.start()) == 2);
    }

    static constexpr ASCIILiteral unicodeEscapes[] = { "\\u0041"_s, "\\uFFFF"_s, "\\uabcd"_s, "\\ud800"_s };
    for (ASCIILiteral literal : unicodeEscapes) {
        PaddedText text(literal);
        AOT_TEST_CHECK(jsonEscapeLength(text.start()) == 6);
    }

    static constexpr ASCIILiteral malformed[] = {
        "\\"_s, "\\u"_s, "\\u0"_s, "\\u00"_s, "\\u004"_s, "\\u004g"_s, "\\u 041"_s, "\\U0041"_s, "\\x41"_s, "\\a"_s, "\\v"_s, "\\0"_s, "\\'"_s,
    };
    for (ASCIILiteral literal : malformed) {
        PaddedText text(literal);
        AOT_TEST_CHECK(!jsonEscapeLength(text.start()));
    }
}

static void testSkipString()
{
    static constexpr SkippedStringCase cases[] = {
        { "\""_s, 1, false },
        { "abc\"x"_s, 4, false },
        { "a\\\"b\""_s, 5, true },
        { "\\ud800\""_s, 7, true },
        { "\\\\\""_s, 3, true },
    };
    for (const SkippedStringCase& testCase : cases) {
        PaddedText text(testCase.text);
        bool hasEscapes = false;
        AOT_TEST_CHECK(skipJSONString(text.start(), hasEscapes) == text.start() + testCase.offset);
        AOT_TEST_CHECK(hasEscapes == testCase.hasEscapes);
    }

    static constexpr size_t lengths[] = { 15, 16, 17, 63, 64, 65, 5000 };
    for (size_t length : lengths) {
        PaddedText closed = paddedRun(length, 'a', { '"' });
        bool hasEscapes = false;
        AOT_TEST_CHECK(skipJSONString(closed.start(), hasEscapes) == closed.end());
        AOT_TEST_CHECK(!hasEscapes);

        PaddedText unclosed = paddedRun(length, 'a', { });
        AOT_TEST_CHECK(!skipJSONString(unclosed.start(), hasEscapes));
    }

    static constexpr ASCIILiteral malformed[] = { ""_s, "abc\\"_s, "\\u12\""_s, "\\u12"_s, "\\x\""_s, "a\x1f\""_s, "a\n\""_s };
    checkRejected([](const uint8_t* cursor) {
        bool hasEscapes = false;
        return skipJSONString(cursor, hasEscapes);
    }, malformed);
}

static void testSkipNumber()
{
    static constexpr OffsetCase cases[] = {
        { "0"_s, 1 }, { "-0"_s, 2 }, { "10"_s, 2 }, { "01"_s, 1 }, { "-01"_s, 2 }, { "1.5"_s, 3 }, { "1.5."_s, 3 }, { "1e5x"_s, 3 },
        { "1E5"_s, 3 }, { "1e-5"_s, 4 }, { "1E+5"_s, 4 }, { "123abc"_s, 3 },
    };
    checkOffsets([](const uint8_t* cursor) { return skipJSONNumber(cursor); }, cases);

    static constexpr ASCIILiteral malformed[] = { ""_s, "-"_s, "-a"_s, "+1"_s, ".5"_s, "1."_s, "1.e1"_s, "1e"_s, "1e+"_s, "0e"_s };
    checkRejected([](const uint8_t* cursor) { return skipJSONNumber(cursor); }, malformed);
}

static constexpr OffsetCase skippedValues[] = {
    { "true,"_s, 4 }, { "truex"_s, 4 }, { "false]"_s, 5 }, { "null "_s, 4 }, { "\"a\" "_s, 3 }, { "\"\""_s, 2 }, { "\"a\\\"\\u0041\" "_s, 11 }, { "0"_s, 1 }, { "-1.5e3,"_s, 6 },
    { "{}x"_s, 2 }, { "[]x"_s, 2 }, { "{ }"_s, 3 }, { "[ \n]"_s, 4 }, { "[1,\"]\",[2]] "_s, 11 }, { "{\"a\":{\"b\":[]},\"}\":null},"_s, 23 },
    { "{\"a\":1,\"b\":[true,null,{\"c\":\"d\"}],\"e\":{}}"_s, 40 }, { "{ \"a\" : [ 1 , 2 ] , \"b\" : { } }"_s, 31 },
};

static constexpr ASCIILiteral malformedValues[] = {
    ""_s, " 1"_s, "t"_s, "tru"_s, "f"_s, "fals"_s, "n"_s, "nul"_s, "True"_s, "-"_s, "1."_s, "1e"_s, "\"a"_s, "\"a\\"_s, "\"\\u00"_s,
    "{"_s, "["_s, "[1"_s, "[1,"_s, "[[1]"_s, "{\"a"_s, "{\"a\""_s, "{\"a\":"_s, "{\"a\":1"_s, "{\"a\":1,"_s, "{\"a\":{}"_s,
    "{\"a\":1,}"_s, "{\"a\":}"_s, "{\"a\" 1}"_s, "{1:2}"_s, "{,}"_s, "{]"_s, "{\"a\":1]"_s, "[1,]"_s, "[,1]"_s, "[1 2]"_s, "[}"_s, "[1}"_s, "[\"a"_s,
};

static void testSkipValue()
{
    checkOffsets(skipJSONValue, skippedValues);
    checkRejected(skipJSONValue, malformedValues);
}

static void testDamagedTexts()
{
    static constexpr ASCIILiteral literal = "{\"a\":[1,-2.5e3,true,false,null,\"x\\n\\u0041\",{\"b\":{}},[]],\"c\":\"d\"}"_s;
    PaddedText whole(literal);
    AOT_TEST_CHECK(skipJSONValue(whole.start()) == whole.end());

    std::span<const uint8_t> bytes = byteCast<uint8_t>(literal.span());
    for (size_t length = 0; length < bytes.size(); ++length) {
        PaddedText prefix(bytes.first(length));
        AOT_TEST_CHECK(!skipJSONValue(prefix.start()));
    }

    static constexpr uint8_t replacements[] = { 0x00, '"', '\\', '{', '}', '[', ']', ',', ':', 'x', 0xff };
    for (size_t position = 0; position < bytes.size(); ++position) {
        for (uint8_t replacement : replacements) {
            Vector<uint8_t> damaged(bytes);
            damaged[position] = replacement;
            PaddedText text(damaged.span());
            const uint8_t* result = skipJSONValue(text.start());
            AOT_TEST_CHECK(!result || (text.start() < result && result <= text.end()));
        }
    }
}

static void testNestingLimit()
{
    PaddedText deepestArrays = paddedString(nestedArrays(64));
    AOT_TEST_CHECK(skipJSONValue(deepestArrays.start()) == deepestArrays.end());
    PaddedText tooManyArrays = paddedString(nestedArrays(65));
    AOT_TEST_CHECK(!skipJSONValue(tooManyArrays.start()));

    PaddedText emptyAtLimit = paddedString(nestedArrays(64, "[]"_s));
    AOT_TEST_CHECK(skipJSONValue(emptyAtLimit.start()) == emptyAtLimit.end());

    String mixed = nestedMixed(64);
    PaddedText deepestMixed = paddedString(mixed);
    AOT_TEST_CHECK(skipJSONValue(deepestMixed.start()) == deepestMixed.end());
    PaddedText tooManyMixed = paddedString(nestedMixed(65));
    AOT_TEST_CHECK(!skipJSONValue(tooManyMixed.start()));

    Vector<uint8_t> wrongOutermostCloser(byteCast<uint8_t>(mixed.span8()));
    AOT_TEST_CHECK(wrongOutermostCloser.last() == '}');
    wrongOutermostCloser.last() = ']';
    PaddedText damaged(wrongOutermostCloser.span());
    AOT_TEST_CHECK(!skipJSONValue(damaged.start()));
}

static void checkParsedNumber(const PaddedText& text, size_t consumed)
{
    ParsedJSONNumber parsed = parseJSONNumber(text.start(), text.end());
    size_t length = 0;
    auto expected = WTF::parseJSONDouble(byteCast<Latin1Character>(std::span { text.start(), consumed }), length);
    AOT_TEST_CHECK(expected && length == consumed);
    AOT_TEST_CHECK(parsed.end == text.start() + consumed);
    AOT_TEST_CHECK(std::bit_cast<uint64_t>(parsed.number) == std::bit_cast<uint64_t>(*expected));
}

static PaddedText decimalText(std::span<const uint8_t> digits, size_t point, bool isNegative)
{
    Vector<uint8_t> bytes;
    if (isNegative)
        bytes.append('-');
    if (!point) {
        bytes.append('0');
        bytes.append('.');
    }
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && i == point)
            bytes.append('.');
        bytes.append(digits[i]);
    }
    return PaddedText(bytes.span());
}

static void checkExactNumberPath()
{
    static constexpr ASCIILiteral digitStrings[] = { "123456789012345"_s, "999999999999999"_s, "100000000000001"_s };
    for (ASCIILiteral digitString : digitStrings) {
        for (size_t digitCount = 1; digitCount <= 15; ++digitCount) {
            auto digits = byteCast<uint8_t>(digitString.span()).first(digitCount);
            for (size_t point = 0; point <= digitCount; ++point) {
                for (bool isNegative : { false, true }) {
                    PaddedText text = decimalText(digits, point, isNegative);
                    checkParsedNumber(text, text.size());
                }
            }
        }
    }
}

static void testParseNumber()
{
    static constexpr OffsetCase cases[] = {
        { "0"_s, 1 }, { "-0"_s, 2 }, { "7"_s, 1 }, { "10"_s, 2 }, { "0.5"_s, 3 }, { "-0.5"_s, 4 }, { "12,"_s, 2 }, { "1.5]"_s, 3 }, { "1e3"_s, 3 },
        { "1E+3"_s, 4 }, { "1e-2"_s, 4 }, { "0.000001"_s, 8 }, { "123456789012345"_s, 15 }, { "1234567890123456"_s, 16 },
        { "12345678901234567"_s, 17 }, { "9007199254740991"_s, 16 }, { "9007199254740992"_s, 16 }, { "1.7976931348623157e308"_s, 22 }, { "5e-324"_s, 6 },
    };
    for (const OffsetCase& testCase : cases)
        checkParsedNumber(PaddedText(testCase.text), testCase.offset);

    static constexpr ASCIILiteral malformed[] = { ""_s, "-"_s, "-a"_s, "a"_s, "+1"_s, ".5"_s, "1."_s, "1.e1"_s, "01"_s, "-01"_s, "00"_s, "1e400"_s, "-1e400"_s };
    for (ASCIILiteral literal : malformed) {
        PaddedText text(literal);
        ParsedJSONNumber parsed = parseJSONNumber(text.start(), text.end());
        AOT_TEST_CHECK(!parsed.end && !parsed.number);
    }

    static constexpr ASCIILiteral truncatedExponents[] = { "1e"_s, "1e+"_s, "1.5E-"_s };
    for (ASCIILiteral literal : truncatedExponents) {
        PaddedText text(literal);
        AOT_TEST_CHECK(parseJSONNumber(text.start(), text.end()).end != text.end());
    }

    checkExactNumberPath();
}

static void checkIntegerRejected(const PaddedText& text)
{
    ParsedJSONInteger parsed = parseJSONInteger(text.start());
    AOT_TEST_CHECK(!parsed.end && !parsed.integer);
}

static void checkSmallIntegerRejected(const PaddedText& text)
{
    JSValue value = jsUndefined();
    AOT_TEST_CHECK(!tryParseSmallJSONInteger(text.start(), value));
    AOT_TEST_CHECK(value.isUndefined());
}

static void testParseInteger()
{
    static constexpr IntegerCase integers[] = {
        { "0"_s, 0, 1 }, { "0,"_s, 0, 1 }, { "7"_s, 7, 1 }, { "-7"_s, -7, 2 }, { "10"_s, 10, 2 }, { "123,"_s, 123, 3 },
        { "1000000000000000"_s, 1000000000000000, 16 }, { "9007199254740991"_s, maxJSONInteger, 16 }, { "-9007199254740991"_s, -maxJSONInteger, 17 },
    };
    for (const IntegerCase& testCase : integers) {
        PaddedText text(testCase.text);
        ParsedJSONInteger parsed = parseJSONInteger(text.start());
        AOT_TEST_CHECK(parsed.integer == testCase.integer);
        AOT_TEST_CHECK(parsed.end == text.start() + testCase.offset);
    }

    static constexpr ASCIILiteral notIntegers[] = {
        ""_s, "-"_s, "a"_s, "+1"_s, "-0"_s, "01"_s, "00"_s, "-01"_s, "1.0"_s, "1."_s, "0.0"_s, "1e3"_s, "1E3"_s, "0e0"_s,
        "9007199254740992"_s, "-9007199254740992"_s, "9999999999999999"_s, "12345678901234567"_s,
    };
    for (ASCIILiteral literal : notIntegers)
        checkIntegerRejected(PaddedText(literal));
    checkIntegerRejected(paddedRun(70, '1', { }));

    static constexpr IntegerCase smallIntegers[] = { { "0"_s, 0, 1 }, { "7"_s, 7, 1 }, { "12,"_s, 12, 2 }, { "999999999"_s, 999999999, 9 } };
    for (const IntegerCase& testCase : smallIntegers) {
        PaddedText text(testCase.text);
        JSValue value = jsUndefined();
        AOT_TEST_CHECK(tryParseSmallJSONInteger(text.start(), value) == text.start() + testCase.offset);
        AOT_TEST_CHECK(value.asInt32() == testCase.integer);
    }

    static constexpr ASCIILiteral notSmallIntegers[] = { ""_s, "-1"_s, "01"_s, "0.5"_s, "1.5"_s, "1e3"_s, "1E3"_s, "1000000000"_s };
    for (ASCIILiteral literal : notSmallIntegers)
        checkSmallIntegerRejected(PaddedText(literal));
    checkSmallIntegerRejected(paddedRun(70, '1', { }));
}

static PaddedText runWithByteAt(size_t position, uint8_t byte)
{
    Vector<uint8_t> bytes(40, [](size_t) -> uint8_t { return 'a'; });
    bytes[position] = byte;
    return PaddedText(bytes.span());
}

static void checkShortTextEncoding(JSONSourceEncoding encoding, uint8_t smallestChanged, uint8_t largestUnchanged)
{
    JSONWords words { };
    words.sourceEncoding = encoding;
    static constexpr unsigned lengths[] = { 0, 1, 15, 16, 17, 31, 32 };
    for (unsigned length : lengths) {
        for (size_t position = 0; position <= 35; ++position) {
            PaddedText changed = runWithByteAt(position, smallestChanged);
            AOT_TEST_CHECK(isShortTextLatin1AfterDecoding(words, changed.start(), length) == (position >= length));
            PaddedText unchanged = runWithByteAt(position, largestUnchanged);
            AOT_TEST_CHECK(isShortTextLatin1AfterDecoding(words, unchanged.start(), length));
        }
    }
}

static void testShortTextEncoding()
{
    checkShortTextEncoding(JSONSourceEncoding::UTF8, 0x80, 0x7f);
    checkShortTextEncoding(JSONSourceEncoding::UTF16, 0xff, 0xfe);
}

static void checkStringInEncoding(DetachedInstance& fixture, const String& source, TextEncoding encoding, const String& expected)
{
    Instance& instance = fixture.instance();
    VM& vm = fixture.vm();
    const JSONWords& words = fixture.words();
    const uint8_t* cursor = fixture.open(source, encoding);
    AOT_TEST_CHECK(*cursor == '"');

    ParsedJSONString parsed = makeUncachedJSONString(instance, vm, cursor + 1);
    AOT_TEST_CHECK(parsed.end == words.sourceEnd);
    AOT_TEST_CHECK(parsed.string->tryGetValue().data == expected);

    StringBuilder builder;
    AOT_TEST_CHECK(decodeJSONString(words, cursor + 1, findEndOfPlainJSONCharacters(cursor + 1), builder) == words.sourceEnd);
    AOT_TEST_CHECK(builder.toString() == expected);

    AOT_TEST_CHECK(parseScalarString(fixture, cursor) == expected);
    fixture.runtime().abandon();
}

static void checkStringInEveryEncoding(DetachedInstance& fixture, const String& source)
{
    String expected = stringValue(fixture.parse(source));
    for (TextEncoding encoding : allTextEncodings) {
        if (encoding != TextEncoding::Latin1 || fitsLatin1(source))
            checkStringInEncoding(fixture, source, encoding, expected);
    }
}

static void testStringsInEveryEncoding()
{
    DetachedInstance fixture;
    static constexpr std::u16string_view texts[] = {
        u"\"\"",
        u"\"abc\"",
        u"\"\u00e9\"",
        u"\"\u00ff\"",
        u"\"a\u00ffb\u00fe\"",
        u"\"\u0100\"",
        u"\"\u65e5\u672c\"",
        u"\"\xd83d\xde00\"",
        u"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
        u"\"\\u0041\\u00e9\\u65e5\"",
        u"\"\\ud800\"",
        u"\"\\udc00x\"",
        u"\"\\ud83d\\ude00\"",
        u"\"\u65e5\\n\u672c\"",
    };
    for (std::u16string_view text : texts)
        checkStringInEveryEncoding(fixture, text16(text));

    static constexpr size_t lengths[] = { 15, 16, 17, 31, 32, 33, 61, 62, 63, 64, 65, 200 };
    for (size_t length : lengths) {
        checkStringInEveryEncoding(fixture, quotedRun(length, u""));
        checkStringInEveryEncoding(fixture, quotedRun(length, u"\u00e9"));
    }
}

static void checkMalformedString(DetachedInstance& fixture, const uint8_t* cursor)
{
    Instance& instance = fixture.instance();
    VM& vm = fixture.vm();
    const JSONWords& words = fixture.words();
    SourceWords before = sourceWordsOf(words);

    ParsedJSONString parsed = makeUncachedJSONString(instance, vm, cursor + 1);
    AOT_TEST_CHECK(!parsed.end && parsed.string && !parsed.string->length());

    StringBuilder builder;
    AOT_TEST_CHECK(!decodeJSONString(words, cursor + 1, findEndOfPlainJSONCharacters(cursor + 1), builder));

    auto scalar = tryParseJSONScalarAt(instance, vm, cursor);
    AOT_TEST_CHECK(scalar && !scalar->end && scalar->value.isUndefined());
    AOT_TEST_CHECK(sourceWordsOf(words) == before);
    fixture.runtime().abandon();
}

static void testMalformedStrings()
{
    DetachedInstance fixture;
    static constexpr std::u16string_view texts[] = { u"\"abc", u"\"", u"\"a\\", u"\"\\u12", u"\"\\u12\"", u"\"\\x\"", u"\"a\x01\"", u"\"a\n\"" };
    for (std::u16string_view characters : texts) {
        String text = text16(characters);
        for (TextEncoding encoding : allTextEncodings) {
            if (encoding != TextEncoding::UTF16 || text.length() > 1)
                checkMalformedString(fixture, fixture.open(text, encoding));
        }
    }

    static constexpr uint8_t truncatedSequence[] = { 0x22, 0xc3, 0x22 };
    static constexpr uint8_t invalidByte[] = { 0x22, 0xff, 0x22 };
    static constexpr uint8_t loneContinuation[] = { 0x22, 0x80, 0x22 };
    static constexpr uint8_t truncatedAfterEscape[] = { 0x22, 0x61, 0x5c, 0x6e, 0xc3, 0x22 };
    for (std::span<const uint8_t> bytes : { std::span<const uint8_t> { truncatedSequence }, std::span<const uint8_t> { invalidByte }, std::span<const uint8_t> { loneContinuation }, std::span<const uint8_t> { truncatedAfterEscape } })
        checkMalformedString(fixture, openBytes(fixture, bytes));
}

static bool isInStringCache(DetachedInstance& fixture, const uint8_t* quote)
{
    return !!fixture.vm().jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
}

static void testStringCacheEncoding()
{
    DetachedInstance fixture;
    JSONRuntime& runtime = fixture.runtime();

    const uint8_t* cursor = fixture.open(text16(u"\"\u00c3\u00a9\""), TextEncoding::Latin1);
    AOT_TEST_CHECK(parseScalarString(fixture, cursor) == text16(u"\u00c3\u00a9"));
    auto cached = findCachedJSONString(fixture.words(), fixture.vm(), cursor);
    AOT_TEST_CHECK(isInStringCache(fixture, cursor) && cached && cached->end == fixture.words().sourceEnd);
    runtime.abandon();
    static constexpr uint8_t utf8Text[] = { 0x22, 0xc3, 0xa9, 0x22 };
    cursor = openBytes(fixture, utf8Text);
    AOT_TEST_CHECK(isInStringCache(fixture, cursor) && !findCachedJSONString(fixture.words(), fixture.vm(), cursor));
    AOT_TEST_CHECK(parseScalarString(fixture, cursor) == text16(u"\u00e9"));
    runtime.abandon();

    AOT_TEST_CHECK(parseScalarString(fixture, fixture.open(text16(u"\"\u00ff\""), TextEncoding::Latin1)) == text16(u"\u00ff"));
    runtime.abandon();
    cursor = fixture.open(text16(u"\"\u0100\""), TextEncoding::UTF16);
    AOT_TEST_CHECK(isInStringCache(fixture, cursor) && !findCachedJSONString(fixture.words(), fixture.vm(), cursor));
    AOT_TEST_CHECK(parseScalarString(fixture, cursor) == text16(u"\u0100"));
    runtime.abandon();
    AOT_TEST_CHECK(parseScalarString(fixture, fixture.open(text16(u"\"\u00ff\""), TextEncoding::UTF16)) == text16(u"\u00ff"));
    runtime.abandon();
}

static JSValue parseScalar(DetachedInstance& fixture, ASCIILiteral text, ptrdiff_t length)
{
    const uint8_t* cursor = fixture.open(String(text), TextEncoding::Latin1);
    auto scalar = tryParseJSONScalarAt(fixture.instance(), fixture.vm(), cursor);
    AOT_TEST_CHECK(scalar && scalar->end == cursor + length);
    return scalar->value;
}

static void testScalars()
{
    DetachedInstance fixture;
    AOT_TEST_CHECK(parseScalar(fixture, "true"_s, 4) == jsBoolean(true));
    AOT_TEST_CHECK(parseScalar(fixture, "false"_s, 5) == jsBoolean(false));
    AOT_TEST_CHECK(parseScalar(fixture, "null"_s, 4) == jsNull());
    AOT_TEST_CHECK(parseScalar(fixture, "12"_s, 2).asNumber() == 12);
    AOT_TEST_CHECK(parseScalar(fixture, "-0.5,"_s, 4).asNumber() == -0.5);
    AOT_TEST_CHECK(stringValue(parseScalar(fixture, "\"ab\""_s, 4)) == "ab"_s);

    static constexpr ASCIILiteral notScalars[] = { ""_s, "{"_s, "["_s, "tru"_s, "fals"_s, "nul"_s, "-"_s, "x"_s };
    for (ASCIILiteral text : notScalars)
        AOT_TEST_CHECK(!tryParseJSONScalarAt(fixture.instance(), fixture.vm(), fixture.open(String(text), TextEncoding::Latin1)));
}

static void checkValueInEncoding(DetachedInstance& fixture, const String& source, TextEncoding encoding)
{
    const uint8_t* cursor = fixture.open(source, encoding);
    ParsedJSONValue parsed = parseJSONValueAt(fixture.instance(), fixture.vm(), fixture.globalObject(), cursor);
    AOT_TEST_CHECK(parsed.end == fixture.words().sourceEnd);
    AOT_TEST_CHECK(fixture.stringify(parsed.value) == fixture.stringify(fixture.parse(source)));
    fixture.runtime().abandon();
}

static void checkValueRejected(DetachedInstance& fixture, const uint8_t* cursor)
{
    SourceWords before = sourceWordsOf(fixture.words());
    ParsedJSONValue parsed = parseJSONValueAt(fixture.instance(), fixture.vm(), fixture.globalObject(), cursor);
    AOT_TEST_CHECK(!parsed.end && parsed.value.isUndefined());
    AOT_TEST_CHECK(sourceWordsOf(fixture.words()) == before);
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());
    fixture.runtime().abandon();
}

static void testValues()
{
    static constexpr unsigned deeperThanTheStack = 1000000;

    DetachedInstance fixture;
    static constexpr std::u16string_view texts[] = {
        u"{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5}}",
        u"[]",
        u"{}",
        u"[1,[2,[3]]]",
        u"\"s\"",
        u"12",
        u"true",
        u"{\"\u00e9\":\"\u00ff\"}",
        u"{\"\u65e5\":\"\u672c\"}",
    };
    for (std::u16string_view characters : texts) {
        String text = text16(characters);
        for (TextEncoding encoding : allTextEncodings) {
            if (encoding != TextEncoding::Latin1 || fitsLatin1(text))
                checkValueInEncoding(fixture, text, encoding);
        }
    }
    checkValueInEncoding(fixture, nestedArrays(maxSkippedJSONDepth), TextEncoding::Latin1);
    checkValueInEncoding(fixture, nestedArrays(maxSkippedJSONDepth + 1), TextEncoding::Latin1);
    checkValueInEncoding(fixture, nestedMixed(maxSkippedJSONDepth + 1), TextEncoding::Latin1);

    const uint8_t* cursor = fixture.open("[1,2] ,x"_s, TextEncoding::Latin1);
    AOT_TEST_CHECK(parseJSONValueAt(fixture.instance(), fixture.vm(), fixture.globalObject(), cursor).end == cursor + 5);
    fixture.runtime().abandon();

    static constexpr ASCIILiteral malformed[] = { "[1,"_s, "{\"a\"}"_s, "[1 2]"_s, "nul"_s };
    for (ASCIILiteral text : malformed)
        checkValueRejected(fixture, fixture.open(String(text), TextEncoding::Latin1));
    checkValueRejected(fixture, fixture.open(nestedArrays(deeperThanTheStack), TextEncoding::Latin1));
    checkValueRejected(fixture, fixture.open(nestedMixed(deeperThanTheStack), TextEncoding::Latin1));

    static constexpr uint8_t wellFormedForScanner[] = { 0x5b, 0x22, 0xff, 0x22, 0x5d };
    checkValueRejected(fixture, openBytes(fixture, wellFormedForScanner));
}

template<typename Check>
static void checkOperation(DetachedInstance& fixture, const String& text, const Check& check)
{
    const uint8_t* cursor = fixture.open(text, TextEncoding::Latin1);
    SourceWords before = sourceWordsOf(fixture.words());
    check(&fixture.instance(), cursor);
    AOT_TEST_CHECK(sourceWordsOf(fixture.words()) == before);
}

static void checkNumberOperations(DetachedInstance& fixture)
{
    checkOperation(fixture, "12.5,"_s, [](Instance* instance, const uint8_t* cursor) {
        UGPRPair pair = operationAOTJSONParseNumber(instance, cursor);
        AOT_TEST_CHECK(std::bit_cast<double>(uint64_t(pair.first)) == 12.5);
        AOT_TEST_CHECK(pair.second == std::bit_cast<UCPURegister>(cursor + 4));
    });
    checkOperation(fixture, "x"_s, [](Instance* instance, const uint8_t* cursor) {
        UGPRPair pair = operationAOTJSONParseNumber(instance, cursor);
        AOT_TEST_CHECK(!pair.first && !pair.second);
    });
    checkOperation(fixture, "-7]"_s, [](Instance* instance, const uint8_t* cursor) {
        UGPRPair pair = operationAOTJSONParseInteger(instance, cursor);
        AOT_TEST_CHECK(int64_t(pair.first) == -7);
        AOT_TEST_CHECK(pair.second == std::bit_cast<UCPURegister>(cursor + 2));
    });
    checkOperation(fixture, "1.0"_s, [](Instance* instance, const uint8_t* cursor) {
        AOT_TEST_CHECK(!operationAOTJSONParseInteger(instance, cursor).second);
    });
}

static void checkFindMemberOperation(DetachedInstance& fixture, ASCIILiteral text, JSONMemberSearch expected)
{
    checkOperation(fixture, String(text), [&](Instance* instance, const uint8_t* cursor) {
        UGPRPair pair = operationAOTJSONFindMember(instance, cursor + 1, 0);
        AOT_TEST_CHECK(pair.first == static_cast<UCPURegister>(expected));
        AOT_TEST_CHECK(!pair.second);
    });
}

static void testOperationsWithoutFrame()
{
    DetachedInstance fixture;
    checkNumberOperations(fixture);
    checkFindMemberOperation(fixture, "{}"_s, JSONMemberSearch::Absent);
    checkFindMemberOperation(fixture, "{a:1}"_s, JSONMemberSearch::Unknown);
    checkOperation(fixture, "x\"a\""_s, [](Instance* instance, const uint8_t* cursor) {
        UGPRPair pair = operationAOTJSONParseString(instance, cursor);
        AOT_TEST_CHECK(!pair.second);
        AOT_TEST_CHECK(!std::bit_cast<JSString*>(pair.first)->length());
    });
}

struct MemberCase {
    std::u16string_view text;
    std::u16string_view name;
};

static auto equalsText(const String& name)
{
    return [name](StringView text) {
        return text == StringView { name };
    };
}

template<typename Check>
static void checkMembersInEveryEncoding(DetachedInstance& fixture, std::span<const MemberCase> cases, const Check& check)
{
    for (const MemberCase& testCase : cases) {
        String text = text16(testCase.text);
        for (TextEncoding encoding : allTextEncodings) {
            if (encoding == TextEncoding::Latin1 && !fitsLatin1(text))
                continue;
            const uint8_t* brace = fixture.open(text, encoding);
            AOT_TEST_CHECK(*brace == '{');
            check(fixture.words(), brace + 1, text16(testCase.name));
            fixture.runtime().abandon();
        }
    }
}

static String memberAfterNestedArrays(unsigned depth)
{
    return makeString("{\"a\":"_s, nestedArrays(depth), ",\"b\":7}"_s);
}

static void checkMemberNotFound(DetachedInstance& fixture, std::span<const MemberCase> cases, JSONMemberSearch expected)
{
    checkMembersInEveryEncoding(fixture, cases, [&](const JSONWords& words, const uint8_t* cursor, const String& name) {
        FoundJSONMember member = findJSONMember(words, cursor, equalsText(name));
        AOT_TEST_CHECK(member.search == expected);
        AOT_TEST_CHECK(!member.value);
    });
}

static void testFindMember()
{
    DetachedInstance fixture;
    static constexpr MemberCase found[] = {
        { u"{\"b\":7}", u"b" },
        { u"{\"a\":1,\"b\":7,\"c\":3}", u"b" },
        { u"{ \"a\" : [1,{\"b\":2}] , \"b\" : 7 }", u"b" },
        { u"{\"b\":7,\"b\":8}", u"b" },
        { u"{\"bb\":1,\"B\":2,\"b\":7}", u"b" },
        { u"{\"a\":1,\"\\u0062\":7}", u"b" },
        { u"{\"\u00e9\":7}", u"\u00e9" },
        { u"{\"\u00fe\":1,\"\u00ff\":7}", u"\u00ff" },
        { u"{\"\u0100\":1,\"\u00ff\":7}", u"\u00ff" },
        { u"{\"\u00ff\":1,\"\u0100\":7}", u"\u0100" },
        { u"{\"\u65e5\":7}", u"\u65e5" },
        { u"{\"\\u65e5\":7}", u"\u65e5" },
        { u"{\"a\\n\u65e5\":7}", u"a\n\u65e5" },
    };
    checkMembersInEveryEncoding(fixture, found, [](const JSONWords& words, const uint8_t* cursor, const String& name) {
        FoundJSONMember member = findJSONMember(words, cursor, equalsText(name));
        AOT_TEST_CHECK(member.search == JSONMemberSearch::Found);
        AOT_TEST_CHECK(*member.value == '7');
    });

    static constexpr MemberCase absent[] = {
        { u"{}", u"b" }, { u"{\"a\":1}", u"b" }, { u"{\"a\":{\"b\":7}}", u"b" }, { u"{\"\u0100\":7}", u"\u00ff" },
    };
    checkMemberNotFound(fixture, absent, JSONMemberSearch::Absent);

    static constexpr MemberCase malformed[] = {
        { u"{\"a\" 1,\"b\":7}", u"b" }, { u"{\"a\":1 \"b\":7}", u"b" }, { u"{\"a\":,\"b\":7}", u"b" }, { u"{\"\\x\":1,\"b\":7}", u"b" },
        { u"{a:1,\"b\":7}", u"b" }, { u"{\"a\":1,", u"b" }, { u"{\"a\":1,}", u"b" }, { u"{\"a", u"b" },
    };
    checkMemberNotFound(fixture, malformed, JSONMemberSearch::Unknown);

    const uint8_t* brace = fixture.open(memberAfterNestedArrays(maxSkippedJSONDepth), TextEncoding::Latin1);
    AOT_TEST_CHECK(findJSONMember(fixture.words(), brace + 1, equalsText("b"_s)).search == JSONMemberSearch::Found);
    brace = fixture.open(memberAfterNestedArrays(maxSkippedJSONDepth + 1), TextEncoding::Latin1);
    AOT_TEST_CHECK(findJSONMember(fixture.words(), brace + 1, equalsText("b"_s)).search == JSONMemberSearch::Unknown);
}

static constexpr MemberCase unrepeated[] = {
    { u"{\"b\":1}", u"b" }, { u"{\"b\":1 }", u"b" }, { u"{\"b\":1,\"a\":2}", u"b" }, { u"{\"a\":0,\"b\":1,\"c\":2}", u"b" },
    { u"{\"b\":1 , \"a\" : {\"b\":2} , \"c\":[\"b\"]}", u"b" }, { u"{\"b\":1,\"\\u0063\":2}", u"b" },
    { u"{\"\u00ff\":1,\"\u0100\":2}", u"\u00ff" }, { u"{\"\u65e5\":1,\"\u672c\":2}", u"\u65e5" },
};

static constexpr MemberCase repeatedOrUnreadable[] = {
    { u"{\"b\":1,\"b\":2}", u"b" }, { u"{\"b\":1,\"a\":2,\"b\":3}", u"b" }, { u"{\"b\":1,\"a\":2,\"\\u0062\":3}", u"b" },
    { u"{\"\u00ff\":1,\"\u00ff\":2}", u"\u00ff" }, { u"{\"\u65e5\":1,\"\\u65e5\":2}", u"\u65e5" },
    { u"{\"b\":1", u"b" }, { u"{\"b\":1,", u"b" }, { u"{\"b\":1,}", u"b" }, { u"{\"b\":1]", u"b" }, { u"{\"b\":1 \"a\":2}", u"b" },
    { u"{\"b\":1,\"a\" 2}", u"b" }, { u"{\"b\":1,\"a\":}", u"b" }, { u"{\"b\":1,\"a\":2", u"b" }, { u"{\"b\":1,\"\\x\":2}", u"b" }, { u"{\"b\":1,a:2}", u"b" },
};

static const uint8_t* findMemberValue(const JSONWords& words, const uint8_t* cursor, const String& name)
{
    FoundJSONMember member = findJSONMember(words, cursor, equalsText(name));
    AOT_TEST_CHECK(member.search == JSONMemberSearch::Found);
    return member.value;
}

static const uint8_t* findMemberKey(const JSONWords& words, const uint8_t* cursor, const String& name)
{
    for (;;) {
        const uint8_t* key = skipJSONWhitespaceCharacters(cursor);
        ComparedJSONMember member = compareJSONMemberKey(words, key, equalsText(name));
        if (member.comparison == JSONKeyComparison::Equal)
            return key;
        AOT_TEST_CHECK(member.comparison == JSONKeyComparison::Different);
        const uint8_t* afterValue = skipJSONValue(member.value);
        AOT_TEST_CHECK(afterValue);
        const uint8_t* comma = skipJSONWhitespaceCharacters(afterValue);
        AOT_TEST_CHECK(*comma == ',');
        cursor = comma + 1;
    }
}

static bool memberRepeats(const JSONWords& words, const uint8_t* cursor, const String& name)
{
    const uint8_t* afterValue = skipJSONValue(findMemberValue(words, cursor, name));
    AOT_TEST_CHECK(afterValue);
    return jsonMemberRepeats(words, afterValue, equalsText(name));
}

static void testMemberRepeats()
{
    DetachedInstance fixture;
    checkMembersInEveryEncoding(fixture, unrepeated, [](const JSONWords& words, const uint8_t* cursor, const String& name) {
        AOT_TEST_CHECK(!memberRepeats(words, cursor, name));
    });
    checkMembersInEveryEncoding(fixture, repeatedOrUnreadable, [](const JSONWords& words, const uint8_t* cursor, const String& name) {
        AOT_TEST_CHECK(memberRepeats(words, cursor, name));
    });

    String deepTail = makeString("{\"b\":1,\"a\":"_s, nestedArrays(maxSkippedJSONDepth + 1), '}');
    AOT_TEST_CHECK(memberRepeats(fixture.words(), fixture.open(deepTail, TextEncoding::Latin1) + 1, "b"_s));
}

static void testKeyRepeats()
{
    DetachedInstance fixture;
    for (std::span<const MemberCase> cases : { std::span<const MemberCase> { unrepeated }, std::span<const MemberCase> { repeatedOrUnreadable } }) {
        checkMembersInEveryEncoding(fixture, cases, [](const JSONWords& words, const uint8_t* cursor, const String& name) {
            bool keyRepeats = jsonKeyRepeats(words, findMemberKey(words, cursor, name), findMemberValue(words, cursor, name));
            AOT_TEST_CHECK(keyRepeats == memberRepeats(words, cursor, name));
        });
    }

    static constexpr uint8_t plainKey[] = { '{', '"', 0xc3, 0x28, '"', ':', '1', '}' };
    static constexpr uint8_t escapedKey[] = { '{', '"', '\\', 'n', 0xc3, 0x28, '"', ':', '1', '}' };
    static constexpr uint8_t malformedKey[] = { '{', '"', '\\', 'x', '"', ':', '1', '}' };
    for (std::span<const uint8_t> bytes : { std::span<const uint8_t> { plainKey }, std::span<const uint8_t> { escapedKey }, std::span<const uint8_t> { malformedKey } }) {
        const uint8_t* brace = openBytes(fixture, bytes);
        AOT_TEST_CHECK(jsonKeyRepeats(fixture.words(), brace + 1, brace + bytes.size() - 2));
        fixture.runtime().abandon();
    }
}

static void testUndecodableMemberKeys()
{
    DetachedInstance fixture;
    static constexpr uint8_t plainKey[] = { '{', '"', 'b', '"', ':', '1', ',', '"', 0xc3, 0x28, '"', ':', '2', ',', '"', 'c', '"', ':', '7', '}' };
    static constexpr uint8_t escapedKey[] = { '{', '"', 'b', '"', ':', '1', ',', '"', '\\', 'n', 0xc3, 0x28, '"', ':', '2', ',', '"', 'c', '"', ':', '7', '}' };
    for (std::span<const uint8_t> bytes : { std::span<const uint8_t> { plainKey }, std::span<const uint8_t> { escapedKey } }) {
        const uint8_t* brace = openBytes(fixture, bytes);
        AOT_TEST_CHECK(fixture.words().sourceEncoding == JSONSourceEncoding::UTF8);
        AOT_TEST_CHECK(findMemberValue(fixture.words(), brace + 1, "b"_s) == brace + 5);
        FoundJSONMember afterUndecodableKey = findJSONMember(fixture.words(), brace + 1, equalsText("c"_s));
        AOT_TEST_CHECK(afterUndecodableKey.search == JSONMemberSearch::Unknown);
        AOT_TEST_CHECK(!afterUndecodableKey.value);
        AOT_TEST_CHECK(jsonMemberRepeats(fixture.words(), brace + 6, equalsText("b"_s)));
        AOT_TEST_CHECK(jsonKeyRepeats(fixture.words(), brace + 1, brace + 5));
        fixture.runtime().abandon();
    }
}

template<typename Operation>
static bool answerFor(DetachedInstance& fixture, const String& text, const Operation& operation)
{
    const uint8_t* brace = fixture.open(text, TextEncoding::Latin1);
    SourceWords before = sourceWordsOf(fixture.words());
    size_t answer = operation(&fixture.instance(), brace);
    AOT_TEST_CHECK(answer <= 1 && sourceWordsOf(fixture.words()) == before);
    fixture.runtime().abandon();
    return answer;
}

static void testRepeatOperations()
{
    DetachedInstance fixture;
    auto keyRepeats = [](Instance* instance, const uint8_t* brace) {
        return operationAOTJSONKeyRepeats(instance, brace + 1, brace + 5);
    };
    auto memberRepeats = [](Instance* instance, const uint8_t* brace) {
        return operationAOTJSONMemberRepeats(instance, brace + 5, 0);
    };
    String deepValue = makeString("{\"b\":"_s, nestedArrays(maxSkippedJSONDepth + 1), '}');

    AOT_TEST_CHECK(!answerFor(fixture, "{\"b\":1}"_s, keyRepeats));
    AOT_TEST_CHECK(!answerFor(fixture, "{\"b\":1,\"a\":2}"_s, keyRepeats));
    AOT_TEST_CHECK(answerFor(fixture, "{\"b\":1,\"b\":2}"_s, keyRepeats));
    AOT_TEST_CHECK(answerFor(fixture, "{\"b\":1,\"a\":2,\"\\u0062\":3}"_s, keyRepeats));
    AOT_TEST_CHECK(answerFor(fixture, "{\"b\":1,\"a\":2"_s, keyRepeats));
    AOT_TEST_CHECK(answerFor(fixture, deepValue, keyRepeats));

    AOT_TEST_CHECK(!answerFor(fixture, "{\"b\":1}"_s, memberRepeats));
    AOT_TEST_CHECK(!answerFor(fixture, "{\"b\":[1,{\"b\":2}] }"_s, memberRepeats));
    AOT_TEST_CHECK(answerFor(fixture, "{\"b\":1"_s, memberRepeats));
    AOT_TEST_CHECK(answerFor(fixture, "{\"b\":1]"_s, memberRepeats));
    AOT_TEST_CHECK(answerFor(fixture, deepValue, memberRepeats));
}

static void checkHooksDecline(TestRealm realm)
{
    DetachedInstance fixture(realm);
    JSGlobalObject* globalObject = fixture.globalObject();
    static constexpr ASCIILiteral text = "{\"a\":1}"_s;
    AOT_TEST_CHECK(!Instance::of(globalObject));
    AOT_TEST_CHECK(!tryParseJSON(globalObject, StringView(text)));
    AOT_TEST_CHECK(!tryParseUTF8JSON(globalObject, byteCast<uint8_t>(text.span())));
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());
    AOT_TEST_CHECK(sourceWordsOf(fixture.words()) == SourceWords { });
}

static void testHooksDeclineWithoutInstance()
{
    checkHooksDecline(TestRealm::Plain);
    checkHooksDecline(TestRealm::Locked);
}

std::span<const NativeTest> jsonScannerTests()
{
    static constexpr NativeTest tests[] = {
        { "json-scanner/white-space"_s, testWhiteSpace },
        { "json-scanner/plain-characters"_s, testPlainCharacters },
        { "json-scanner/escape-lengths"_s, testEscapeLengths },
        { "json-scanner/skip-string"_s, testSkipString },
        { "json-scanner/skip-number"_s, testSkipNumber },
        { "json-scanner/skip-value"_s, testSkipValue },
        { "json-scanner/damaged-texts"_s, testDamagedTexts },
        { "json-scanner/nesting-limit"_s, testNestingLimit },
        { "json-scanner/parse-number"_s, testParseNumber },
        { "json-scanner/parse-integer"_s, testParseInteger },
        { "json-scanner/short-text-encoding"_s, testShortTextEncoding },
        { "json-scanner/strings-in-every-encoding"_s, testStringsInEveryEncoding },
        { "json-scanner/malformed-strings"_s, testMalformedStrings },
        { "json-scanner/string-cache-respects-encoding"_s, testStringCacheEncoding },
        { "json-scanner/scalars"_s, testScalars },
        { "json-scanner/values"_s, testValues },
        { "json-scanner/operations-without-frame"_s, testOperationsWithoutFrame },
        { "json-scanner/find-member"_s, testFindMember },
        { "json-scanner/member-repeats"_s, testMemberRepeats },
        { "json-scanner/key-repeats"_s, testKeyRepeats },
        { "json-scanner/undecodable-member-keys"_s, testUndecodableMemberKeys },
        { "json-scanner/repeat-operations"_s, testRepeatOperations },
        { "json-scanner/hooks-decline-without-instance"_s, testHooksDeclineWithoutInstance },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
