/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTJSONScanner.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
#include "AOTSchemaRuntimeOperations.h"
#include "ErrorInstance.h"
#include "TopExceptionScope.h"
#include <string_view>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::JSONMemberSearch;

std::span<const NativeTest> jsonParseOperationTests();

// The tests only look the names up in the tables. The object goes to a function that nobody knows: a store that no code of the program reads is removed, and the allocation with it.
static constexpr ASCIILiteral namesProgramText =
    "layout L0 name=\"Names\" module=m0 fields=[{key=\"a\" declared={type=anything} flags=[IsRequired]}, {key=\"b\" declared={type=anything} flags=[IsRequired]}, {key=\"\\xe6\\x97\\xa5\" declared={type=anything} flags=[IsRequired]}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0, v0 layout=L0 fields=[0, 1, 2]\n"
    "    v2 = Builtin builtin=GlobalThis\n"
    "    v3 = PropGet v2 key=\"publish\" [isFreeName]\n"
    "    v4 = Call v3, -, v1\n"
    "    Return\n"
    "}\n"_s;

namespace {

struct ParsedCell {
    JSString* string;
    const uint8_t* end;
};

struct KeyCase {
    std::u16string_view text;
    std::u16string_view key;
};

struct MemberCase {
    std::u16string_view text;
    std::u16string_view name;
    JSONMemberSearch search;
};

struct RepeatCase {
    std::u16string_view text;
    std::u16string_view name;
    bool mayRepeat;
};

} // anonymous namespace

static std::span<const uint8_t> sourceCopy(const JSONWords& words)
{
    return { words.sourceStart, words.sourceEnd };
}

static std::span<const uint8_t> bytesOf(ASCIILiteral text)
{
    return byteCast<uint8_t>(text.span());
}

static String textOf(JSString* string)
{
    AOT_TEST_CHECK(string);
    return string->tryGetValue().data;
}

static bool openByOperation(DetachedInstance& fixture, JSValue textOrBytes, unsigned depth)
{
    OperationCallScope scope(fixture.vm());
    size_t isOpen = operationAOTJSONSourceOpen(&fixture.instance(), JSValue::encode(textOrBytes), stackPointerAtDepth(depth));
    AOT_TEST_CHECK(isOpen <= 1);
    return isOpen;
}

static void checkOpenSource(DetachedInstance& fixture, std::span<const uint8_t> copy, JSONSourceEncoding encoding, unsigned depth)
{
    const JSONWords& words = fixture.words();
    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(depth));
    AOT_TEST_CHECK(equalSpans(sourceCopy(words), copy));
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    AOT_TEST_CHECK(words.sourceEncoding == encoding);
}

static void testSourceOpen()
{
    static constexpr uint8_t markAndObject[] = { 0xef, 0xbb, 0xbf, '{', '}' };
    static constexpr uint8_t saturatedCopy[] = { '"', saturatedJSONCharacter, '"' };
    static constexpr ASCIILiteral otherCells[] = { "({})"_s, "[]"_s, "new String('1')"_s, "Symbol()"_s, "new Uint16Array(2)"_s, "new ArrayBuffer(2)"_s };

    DetachedInstance fixture;
    Instance* instance = &fixture.instance();
    JSONWords& words = fixture.words();

    AOT_TEST_CHECK(openByOperation(fixture, fixture.string("[1]"_s), 1));
    checkOpenSource(fixture, bytesOf("[1]"_s), JSONSourceEncoding::Latin1, 1);
    AOT_TEST_CHECK(openByOperation(fixture, fixture.string(text16(u"\"日\"")), 2));
    checkOpenSource(fixture, saturatedCopy, JSONSourceEncoding::UTF16, 2);
    AOT_TEST_CHECK(fixture.runtime().numberOfSuspendedSources() == 1);
    AOT_TEST_CHECK(operationAOTJSONSourceClose(instance, words.sourceEnd) == 1);
    checkOpenSource(fixture, bytesOf("[1]"_s), JSONSourceEncoding::Latin1, 1);

    SourceWords before = sourceWordsOf(words);
    for (JSValue value : { jsNumber(1), jsUndefined(), jsNull(), jsBoolean(true) })
        AOT_TEST_CHECK(!openByOperation(fixture, value, 2) && sourceWordsOf(words) == before);
    for (ASCIILiteral source : otherCells)
        AOT_TEST_CHECK(!openByOperation(fixture, fixture.evaluate(source), 2) && sourceWordsOf(words) == before);
    operationAOTJSONSourceAbandon(instance);
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);

    AOT_TEST_CHECK(openByOperation(fixture, fixture.bytes(markAndObject), 1));
    checkOpenSource(fixture, bytesOf("{}"_s), JSONSourceEncoding::Latin1, 1);
    operationAOTJSONSourceAbandon(instance);

    JSString* rope = jsString(fixture.globalObject(), fixture.string("[1,"_s), fixture.string("2]"_s));
    AOT_TEST_CHECK(rope->isRope());
    AOT_TEST_CHECK(openByOperation(fixture, rope, 1));
    checkOpenSource(fixture, bytesOf("[1,2]"_s), JSONSourceEncoding::Latin1, 1);
    operationAOTJSONSourceAbandon(instance);
}

static JSValue decodeBytes(DetachedInstance& fixture, JSValue value)
{
    OperationCallScope scope(fixture.vm());
    auto decoded = operationAOTJSONDecodeBytes(&fixture.instance(), JSValue::encode(value));
    AOT_TEST_CHECK(!decoded.exception);
    return JSValue::decode(decoded.value);
}

static void checkDecodedBytes(DetachedInstance& fixture, std::span<const uint8_t> bytes, const String& expected)
{
    JSValue decoded = decodeBytes(fixture, fixture.bytes(bytes));
    AOT_TEST_CHECK(decoded.isString());
    AOT_TEST_CHECK(textOf(asString(decoded)) == expected);
}

static void checkEmptyBodyThrows(DetachedInstance& fixture, std::span<const uint8_t> bytes)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(fixture.vm());
    AOT_TEST_CHECK(!decodeJSONBody(fixture.globalObject(), fixture.bytes(bytes)));
    AOT_TEST_CHECK(scope.exception());
    auto* error = dynamicDowncast<ErrorInstance>(scope.exception()->value());
    scope.clearException();
    AOT_TEST_CHECK(error && error->errorType() == ErrorType::SyntaxError);
}

static void testDecodeBytes()
{
    static constexpr uint8_t markAlone[] = { 0xef, 0xbb, 0xbf };
    static constexpr uint8_t markAndOne[] = { 0xef, 0xbb, 0xbf, '1' };
    static constexpr uint8_t twoByteSequence[] = { '"', 0xc3, 0xa9, '"' };
    static constexpr uint8_t invalidByte[] = { '"', 0xff, '"' };
    static constexpr uint8_t littleEndianText[] = { 0xff, 0xfe, '"', 0x00, 0xe5, 0x65, '"', 0x00 };
    static constexpr uint8_t littleEndianMarkAlone[] = { 0xff, 0xfe };
    static constexpr ASCIILiteral otherCells[] = { "({})"_s, "[49]"_s, "new Uint16Array(2)"_s, "new Uint8ClampedArray(2)"_s, "new ArrayBuffer(2)"_s, "new DataView(new ArrayBuffer(2))"_s };

    DetachedInstance fixture;
    checkDecodedBytes(fixture, bytesOf("{\"a\":1}"_s), "{\"a\":1}"_s);
    checkDecodedBytes(fixture, markAndOne, "1"_s);
    checkDecodedBytes(fixture, twoByteSequence, text16(u"\"\u00e9\""));
    checkDecodedBytes(fixture, invalidByte, text16(u"\"\ufffd\""));
    checkDecodedBytes(fixture, littleEndianText, text16(u"\"日\""));
    checkDecodedBytes(fixture, littleEndianMarkAlone, text16(u"\ufffd\ufffd"));
    checkDecodedBytes(fixture, markAlone, emptyString());
    checkEmptyBodyThrows(fixture, { });

    for (JSValue value : { jsNumber(1), jsUndefined(), jsNull(), jsBoolean(false), JSValue(fixture.string("[1]"_s)) })
        AOT_TEST_CHECK(decodeBytes(fixture, value) == value);
    for (ASCIILiteral source : otherCells) {
        JSValue value = fixture.evaluate(source);
        AOT_TEST_CHECK(decodeBytes(fixture, value) == value);
    }
}

template<auto operation>
static ParsedCell parseCell(DetachedInstance& fixture, const uint8_t* cursor)
{
    SourceWords before = sourceWordsOf(fixture.words());
    OperationCallScope scope(fixture.vm());
    UGPRPair pair = operation(&fixture.instance(), cursor);
    AOT_TEST_CHECK(sourceWordsOf(fixture.words()) == before);
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());
    return { std::bit_cast<JSString*>(pair.first), std::bit_cast<const uint8_t*>(pair.second) };
}

template<auto operation>
static void checkCellNotParsed(DetachedInstance& fixture, const String& text, TextEncoding encoding)
{
    ParsedCell parsed = parseCell<operation>(fixture, fixture.open(text, encoding));
    AOT_TEST_CHECK(!parsed.end);
    AOT_TEST_CHECK(parsed.string && !parsed.string->length());
    fixture.runtime().abandon();
}

template<typename Functor>
static void forEachEncodingOf(const String& text, const Functor& functor)
{
    for (TextEncoding encoding : allTextEncodings) {
        if ((encoding != TextEncoding::Latin1 || fitsLatin1(text)) && (encoding != TextEncoding::UTF16 || text.length() > 1))
            functor(encoding);
    }
}

static void testParseString()
{
    static constexpr std::u16string_view texts[] = {
        u"\"\"", u"\"abc\"", u"\"\u00e9\u00ff\"", u"\"\u65e5\u672c\"", u"\"\xd83d\xde00\"", u"\"a\\n\\u0041\\\"\"", u"\"\\ud800\"", u"\"\u65e5\\t\"",
        u"\"a string that is longer than the texts that the cache of short strings keeps\"",
    };
    static constexpr std::u16string_view malformed[] = { u"x\"a\"", u"\"abc", u"\"a\\", u"\"\\x\"", u"\"\\u12\"", u"\"a\n\"" };

    DetachedInstance fixture;
    for (std::u16string_view characters : texts) {
        String text = text16(characters);
        String expected = textOf(asString(fixture.parse(text)));
        forEachEncodingOf(text, [&](TextEncoding encoding) {
            for (unsigned i = 0; i < 2; ++i) {
                ParsedCell parsed = parseCell<operationAOTJSONParseString>(fixture, fixture.open(makeString(text, ",1"_s), encoding));
                AOT_TEST_CHECK(parsed.end && *parsed.end == ',' && parsed.end + 2 == fixture.words().sourceEnd);
                AOT_TEST_CHECK(textOf(parsed.string) == expected);
                fixture.runtime().abandon();
            }
        });
    }
    for (std::u16string_view characters : malformed) {
        String text = text16(characters);
        forEachEncodingOf(text, [&](TextEncoding encoding) {
            checkCellNotParsed<operationAOTJSONParseString>(fixture, text, encoding);
        });
    }
}

static void testParseKey()
{
    static constexpr KeyCase keys[] = {
        { u"\"name\":1", u"name" }, { u"\"\":1", u"" }, { u"\"a b\" :1", u"a b" }, { u"\"\u00e9\":1", u"\u00e9" }, { u"\"\u65e5\u672c\":1", u"\u65e5\u672c" },
        { u"\"a key that is longer than the names that the cache keeps with their colon\":1", u"a key that is longer than the names that the cache keeps with their colon" },
    };
    static constexpr std::u16string_view notParsed[] = { u"name:1", u"\"name", u"\"a\\n\":1", u"\"\\u0061\":1", u"\"a\n\":1", u"1" };

    DetachedInstance fixture;
    for (const KeyCase& testCase : keys) {
        String text = text16(testCase.text);
        forEachEncodingOf(text, [&](TextEncoding encoding) {
            const uint8_t* cursor = fixture.open(text, encoding);
            ParsedCell parsed = parseCell<operationAOTJSONParseKey>(fixture, cursor);
            AOT_TEST_CHECK(parsed.end && cursor < parsed.end && parsed.end[-1] == '"');
            AOT_TEST_CHECK(*skipJSONWhitespaceCharacters(parsed.end) == ':');
            AOT_TEST_CHECK(textOf(parsed.string) == text16(testCase.key));
            fixture.runtime().abandon();
        });
    }
    for (std::u16string_view characters : notParsed) {
        String text = text16(characters);
        forEachEncodingOf(text, [&](TextEncoding encoding) {
            checkCellNotParsed<operationAOTJSONParseKey>(fixture, text, encoding);
        });
    }
}

static ParsedJSONValue parseValue(DetachedInstance& fixture, const uint8_t* cursor)
{
    SourceWords before = sourceWordsOf(fixture.words());
    OperationCallScope scope(fixture.vm());
    UGPRPair pair = operationAOTJSONParseValue(&fixture.instance(), cursor);
    AOT_TEST_CHECK(sourceWordsOf(fixture.words()) == before);
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());
    return { JSValue::decode(static_cast<EncodedJSValue>(pair.first)), std::bit_cast<const uint8_t*>(pair.second) };
}

static void testParseValue()
{
    static constexpr std::u16string_view texts[] = {
        u"{\"a\":[1,{\"b\":null}],\"c\":\"d\"}", u"[]", u"{}", u"[1.5,-0,1e21]", u"\"s\"", u"12", u"-0.5", u"true", u"false", u"null",
        u"{\"\u00e9\":\"\u00ff\"}", u"{\"\u65e5\":[\"\u672c\"]}", u"{\"a\":1,\"a\":2,\"__proto__\":3,\"0\":4}",
    };
    static constexpr ASCIILiteral malformed[] = { ""_s, "[1,"_s, "{\"a\"}"_s, "{\"a\":}"_s, "[1 2]"_s, "nul"_s, "\"a"_s, "-"_s, "x"_s };

    DetachedInstance fixture;
    for (std::u16string_view characters : texts) {
        String text = text16(characters);
        String expected = fixture.stringify(fixture.parse(text));
        forEachEncodingOf(text, [&](TextEncoding encoding) {
            ParsedJSONValue parsed = parseValue(fixture, fixture.open(makeString(text, " ,x"_s), encoding));
            AOT_TEST_CHECK(parsed.end && parsed.end + 3 == fixture.words().sourceEnd);
            AOT_TEST_CHECK(fixture.stringify(parsed.value) == expected);
            fixture.runtime().abandon();
        });
    }
    for (ASCIILiteral text : malformed) {
        ParsedJSONValue parsed = parseValue(fixture, fixture.open(String(text), TextEncoding::Latin1));
        AOT_TEST_CHECK(!parsed.end && parsed.value.isUndefined());
        fixture.runtime().abandon();
    }
}

static void testParseValueAfterFailure()
{
    static constexpr ASCIILiteral text = "[[1,x],[2],\"a]"_s;
    static constexpr size_t one = 2;
    static constexpr size_t noValue = 4;
    static constexpr size_t arrayOfTwo = 7;
    static constexpr size_t openString = 11;

    DetachedInstance fixture;
    for (size_t failing : { noValue, openString, size_t(0) }) {
        const uint8_t* start = fixture.open(String(text), TextEncoding::Latin1);
        AOT_TEST_CHECK(parseValue(fixture, start + arrayOfTwo).end == start + arrayOfTwo + 3);
        AOT_TEST_CHECK(!parseValue(fixture, start + failing).end);
        AOT_TEST_CHECK(!parseValue(fixture, start + arrayOfTwo).end);
        AOT_TEST_CHECK(!parseValue(fixture, start).end);
        AOT_TEST_CHECK(parseValue(fixture, start + one).end == start + one + 1);

        const uint8_t* nested = fixture.open("[3]"_s, TextEncoding::Latin1, 2);
        AOT_TEST_CHECK(parseValue(fixture, nested).end == nested + 3);
        fixture.runtime().abandon();
        AOT_TEST_CHECK(fixture.words().sourceStart == start);
        AOT_TEST_CHECK(!parseValue(fixture, start + arrayOfTwo).end);
        fixture.runtime().abandon();
    }

    const uint8_t* outer = fixture.open("[4]"_s, TextEncoding::Latin1);
    const uint8_t* nested = fixture.open("[x]"_s, TextEncoding::Latin1, 2);
    AOT_TEST_CHECK(!parseValue(fixture, nested).end);
    fixture.runtime().abandon();
    AOT_TEST_CHECK(parseValue(fixture, outer).end == outer + 3);
    fixture.runtime().abandon();
}

static uint32_t numberOfProgramString(LoadedProgram& program, const String& fieldName)
{
    const ProgramTables& tables = program.instance().tables();
    Identifier name = Identifier::fromString(program.vm(), fieldName);
    for (const AllocationData& allocation : tables.allocations()) {
        if (!tables.hasLayoutForShape(allocation.shape))
            continue;
        if (const FieldData* field = program.state().objects->layouts.field(program.vm(), tables.layoutForShape(allocation.shape), name.impl()))
            return ProgramStrings::identifierString(tables, field->name).index();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void withSourceInEveryEncoding(LoadedProgram& program, const String& text, const Functor& functor)
{
    if (fitsLatin1(text)) {
        String latin1 = to8Bit(text);
        JSONSourceScope source(program.instance(), StringView(latin1));
        AOT_TEST_CHECK(!!source);
        functor(source.start());
    }
    {
        JSONSourceScope source(program.instance(), StringView(text));
        AOT_TEST_CHECK(!!source && !text.is8Bit());
        functor(source.start());
    }
    CString utf8 = text.utf8();
    JSONSourceScope source(program.instance(), byteCast<uint8_t>(utf8.span()));
    AOT_TEST_CHECK(!!source);
    functor(source.start());
}

static void testFindMember(LoadedProgram& program)
{
    static constexpr MemberCase cases[] = {
        { u"{\"b\":7}", u"b", JSONMemberSearch::Found },
        { u"{\"a\":1,\"b\":7,\"c\":3}", u"b", JSONMemberSearch::Found },
        { u"{ \"a\" : [1,{\"b\":2}] , \"b\" : 7 }", u"b", JSONMemberSearch::Found },
        { u"{\"bb\":1,\"B\":2,\"\\u0062\":7}", u"b", JSONMemberSearch::Found },
        { u"{\"b\":1,\"\u65e5\":7}", u"\u65e5", JSONMemberSearch::Found },
        { u"{\"\u672c\":1,\"\\u65e5\":7}", u"\u65e5", JSONMemberSearch::Found },
        { u"{}", u"b", JSONMemberSearch::Absent },
        { u"{\"a\":{\"b\":7}}", u"b", JSONMemberSearch::Absent },
        { u"{\"\u672c\":7}", u"\u65e5", JSONMemberSearch::Absent },
        { u"{\"a\" 1,\"b\":7}", u"b", JSONMemberSearch::Unknown },
        { u"{\"a\":1,", u"b", JSONMemberSearch::Unknown },
        { u"{\"\\x\":1,\"b\":7}", u"b", JSONMemberSearch::Unknown },
    };
    for (const MemberCase& testCase : cases) {
        uint32_t name = numberOfProgramString(program, text16(testCase.name));
        withSourceInEveryEncoding(program, text16(testCase.text), [&](const uint8_t* brace) {
            UGPRPair pair = operationAOTJSONFindMember(&program.instance(), brace + 1, name);
            AOT_TEST_CHECK(pair.first == static_cast<UCPURegister>(testCase.search));
            const uint8_t* value = std::bit_cast<const uint8_t*>(pair.second);
            AOT_TEST_CHECK(testCase.search == JSONMemberSearch::Found ? value && *value == '7' : !value);
        });
    }
}

static void testMemberRepeats(LoadedProgram& program)
{
    static constexpr RepeatCase cases[] = {
        { u"{\"b\":1}", u"b", false },
        { u"{\"b\":1,\"a\":2}", u"b", false },
        { u"{\"b\":1 , \"a\" : {\"b\":2} , \"c\":[\"b\"]}", u"b", false },
        { u"{\"b\":1,\"\u65e5\":2}", u"b", false },
        { u"{\"b\":1,\"b\":2}", u"b", true },
        { u"{\"b\":1,\"a\":2,\"\\u0062\":3}", u"b", true },
        { u"{\"b\":1,\"a\":2", u"b", true },
        { u"{\"b\":1,\"\\x\":2}", u"b", true },
        { u"{\"b\":1,\"\u672c\":2,\"\u65e5\":3}", u"\u65e5", true },
    };
    for (const RepeatCase& testCase : cases) {
        uint32_t name = numberOfProgramString(program, text16(testCase.name));
        withSourceInEveryEncoding(program, text16(testCase.text), [&](const uint8_t* brace) {
            AOT_TEST_CHECK(operationAOTJSONMemberRepeats(&program.instance(), brace + 5, name) == static_cast<size_t>(testCase.mayRepeat));
        });
    }
}

template<LoadedProgramTest body>
static constexpr void (*withNamesProgram)() = runNativeTestWithLoadedProgram<jsonParseOperationTests, namesProgramText, body>;

std::span<const NativeTest> jsonParseOperationTests()
{
    static constexpr NativeTest tests[] = {
        { "json-operations/source-open"_s, testSourceOpen },
        { "json-operations/decode-bytes"_s, testDecodeBytes },
        { "json-operations/parse-string"_s, testParseString },
        { "json-operations/parse-key"_s, testParseKey },
        { "json-operations/parse-value"_s, testParseValue },
        { "json-operations/parse-value-after-a-failure"_s, testParseValueAfterFailure },
        { "json-operations/find-member"_s, withNamesProgram<testFindMember> },
        { "json-operations/member-repeats"_s, withNamesProgram<testMemberRepeats> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
