/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTIRTestSupport.h"
#include "AOTJSONTextWriter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStringBuilderCursor.h"
#include "DateInstance.h"
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <wtf/MathExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::JSONOutcome;
using Typed::JSONPosition;

struct OutcomesByPosition {
    JSONOutcome root;
    JSONOutcome property;
    JSONOutcome element;
};

static constexpr OutcomesByPosition parsedEverywhere { JSONOutcome::Parsed, JSONOutcome::Parsed, JSONOutcome::Parsed };
static constexpr OutcomesByPosition parsedAsElementOnly { JSONOutcome::Rejected, JSONOutcome::Rejected, JSONOutcome::Parsed };
static constexpr OutcomesByPosition unknownEverywhere { JSONOutcome::Unknown, JSONOutcome::Unknown, JSONOutcome::Unknown };

static constexpr size_t poisonedFrameSize = 64;
static constexpr uint8_t poisonByte = 0xaa;

template<typename T>
static T wordAt(const StringBuilderStorage& builder, ptrdiff_t offset)
{
    return *std::bit_cast<const T*>(std::bit_cast<const uint8_t*>(&builder) + offset);
}

static uint32_t lengthOutsideBuffer(const StringBuilderStorage& builder)
{
    return wordAt<uint32_t>(builder, StringBuilderStorage::offsetOfLengthOutsideBuffer());
}

class Writer {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(Writer);
public:
    explicit Writer(DetachedInstance& fixture)
        : m_fixture(fixture)
        , m_builder(*fixture.acquireBuilder())
        , m_cursor(m_builder.reserve(fixture.globalObject(), maxStringBuilderReservation))
    {
        checkReservation();
    }

    const StringBuilderStorage& builder() const { return m_builder; }
    Latin1Character* cursor() { return m_cursor; }
    void setCursor(Latin1Character* cursor) { m_cursor = cursor; }

    void appendLiteral(ASCIILiteral text) { m_cursor = appendJSONLiteral(m_cursor, text); }

    void appendString(const String& text) { appendString(m_fixture.string(text)); }

    void appendString(JSString* string)
    {
        OperationCallScope scope(m_fixture.vm());
        auto appended = operationAOTJSONAppendString(&m_fixture.instance(), &m_builder, byteCast<uint8_t>(m_cursor), string);
        AOT_TEST_CHECK(!appended.exception);
        m_cursor = byteCast<Latin1Character>(appended.value);
        checkReservation();
    }

    JSONOutcome appendValue(JSValue value, JSONPosition position)
    {
        OperationCallScope scope(m_fixture.vm());
        UGPRPair cursorAndOutcome = operationAOTJSONAppendValue(&m_fixture.instance(), &m_builder, byteCast<uint8_t>(m_cursor), JSValue::encode(value), static_cast<uint32_t>(position));
        m_cursor = std::bit_cast<Latin1Character*>(cursorAndOutcome.first);
        checkReservation();
        return static_cast<JSONOutcome>(cursorAndOutcome.second);
    }

    String finish()
    {
        m_builder.commit(m_cursor);
        return m_fixture.finish(&m_builder);
    }

private:
    void checkReservation()
    {
        AOT_TEST_CHECK(m_cursor);
        const Latin1Character* start = wordAt<Latin1Character*>(m_builder, StringBuilderStorage::offsetOfCharacters());
        const Latin1Character* limit = start + wordAt<uint32_t>(m_builder, StringBuilderStorage::offsetOfLimit());
        AOT_TEST_CHECK(start <= m_cursor && m_cursor <= limit);
        AOT_TEST_CHECK(static_cast<size_t>(limit - m_cursor) >= maxStringBuilderReservation);
        memset(m_cursor, poisonByte, maxStringBuilderReservation);
    }

    DetachedInstance& m_fixture;
    StringBuilderStorage& m_builder;
    Latin1Character* m_cursor;
};

static String writeOneString(DetachedInstance& fixture, const String& text)
{
    Writer writer(fixture);
    writer.appendString(text);
    return writer.finish();
}

static String engineText(DetachedInstance& fixture, const String& text)
{
    return fixture.stringify(fixture.string(text));
}

static String repeatedCharacter(size_t length, Latin1Character character)
{
    Vector<Latin1Character> characters(FillWith { }, length, character);
    return String { characters.span() };
}

static void testEmptyText()
{
    DetachedInstance fixture;
    Writer writer(fixture);
    String text = writer.finish();
    AOT_TEST_CHECK(!text.isNull());
    AOT_TEST_CHECK(text.isEmpty());
}

static void testStrings()
{
    static constexpr std::array<uint8_t, 8> specialBytes { '"', '\\', 0x0a, 0x01, 0x1f, 0x7f, 0x80, 0xff };
    static constexpr size_t maxLength = 40;
    DetachedInstance fixture;
    std::array<Latin1Character, maxLength> storage;
    storage.fill(Latin1Character('a'));
    auto checkText = [&](size_t length) {
        String text { std::span<const Latin1Character> { storage }.first(length) };
        String written = writeOneString(fixture, text);
        AOT_TEST_CHECK(written.is8Bit());
        AOT_TEST_CHECK(written == engineText(fixture, text));
    };
    for (size_t length = 0; length <= maxLength; ++length) {
        checkText(length);
        for (uint8_t special : specialBytes) {
            for (size_t position = 0; position < length; ++position) {
                storage[position] = Latin1Character(special);
                checkText(length);
                storage[position] = Latin1Character('a');
            }
        }
    }
}

static void testLongStrings()
{
    static constexpr std::array<size_t, 5> lengths { maxStringBuilderReservation - 3, maxStringBuilderReservation - 2, maxStringBuilderReservation, maxStringBuilderReservation + 1, 100 * KB };
    DetachedInstance fixture;
    for (size_t length : lengths) {
        Vector<Latin1Character> characters(FillWith { }, length, Latin1Character('a'));
        String plain { characters.span() };
        String written = writeOneString(fixture, plain);
        AOT_TEST_CHECK(written.length() == length + 2);
        AOT_TEST_CHECK(written == engineText(fixture, plain));

        characters.last() = Latin1Character('"');
        String withOneEscape { characters.span() };
        AOT_TEST_CHECK(writeOneString(fixture, withOneEscape) == engineText(fixture, withOneEscape));

        characters.fill(Latin1Character(0x01));
        String withLongestEscapes { characters.span() };
        written = writeOneString(fixture, withLongestEscapes);
        AOT_TEST_CHECK(written.length() == 6 * length + 2);
        AOT_TEST_CHECK(written == engineText(fixture, withLongestEscapes));
    }
}

static void testSeveralStrings()
{
    DetachedInstance fixture;
    Vector<Latin1Character> characters(FillWith { }, 3000, Latin1Character('b'));
    String longText { characters.span() };
    StringBuilder expected;
    Writer writer(fixture);
    writer.appendLiteral("["_s);
    expected.append('[');
    for (unsigned i = 0; i < 50; ++i) {
        String text = i % 2 ? longText : makeString("a\""_s, i);
        if (i) {
            writer.appendLiteral(","_s);
            expected.append(',');
        }
        writer.appendString(text);
        expected.append(engineText(fixture, text));
    }
    writer.appendLiteral("]"_s);
    expected.append(']');
    AOT_TEST_CHECK(!lengthOutsideBuffer(writer.builder()));
    AOT_TEST_CHECK(writer.finish() == expected.toString());
}

static bool hasRoomForEscaped(Writer& writer, const String& text)
{
    return writer.builder().hasRoomAt(writer.cursor(), 6 * text.length() + 2 + maxStringBuilderReservation);
}

static String longTextWithOneEscape()
{
    return makeString(repeatedCharacter(1000, 'e'), '"');
}

static void testPositionAfterStrings()
{
    DetachedInstance fixture;
    Writer writer(fixture);
    StringBuilder expected;
    auto append = [&](const String& text) {
        writer.appendString(text);
        expected.append(engineText(fixture, text));
    };

    append("the first buffer holds one reservation"_s);
    uint32_t position = writer.builder().length();
    for (const String& plainText : { String("a"_s), emptyString(), String("plain text"_s), repeatedCharacter(1000, 'p') }) {
        AOT_TEST_CHECK(writer.builder().hasRoomAt(writer.cursor(), plainText.length() + 2 + maxStringBuilderReservation));
        append(plainText);
        AOT_TEST_CHECK(writer.builder().length() == position);
    }
    for (const String& escapedText : { String("a\"b"_s), String("\\\n\x01"_s), repeatedCharacter(100, '"') }) {
        AOT_TEST_CHECK(hasRoomForEscaped(writer, escapedText));
        append(escapedText);
        AOT_TEST_CHECK(writer.builder().length() == position);
    }

    String escapedTextWithoutRoom = longTextWithOneEscape();
    AOT_TEST_CHECK(writer.builder().hasRoomAt(writer.cursor(), escapedTextWithoutRoom.length() + 2 + maxStringBuilderReservation));
    AOT_TEST_CHECK(!hasRoomForEscaped(writer, escapedTextWithoutRoom));
    for (const String& otherText : { escapedTextWithoutRoom, repeatedCharacter(100 * KB, 'l'), text16(u"日本") }) {
        append(otherText);
        AOT_TEST_CHECK(writer.builder().length() > position);
        position = writer.builder().length();
    }
    AOT_TEST_CHECK(writer.finish() == expected.toString());
}

static void testPositionAfterValues()
{
    DetachedInstance fixture;
    Writer writer(fixture);
    StringBuilder expected;
    auto append = [&](JSValue value) {
        AOT_TEST_CHECK(writer.appendValue(value, JSONPosition::Element) == JSONOutcome::Parsed);
        String text = fixture.stringify(value);
        expected.append(text.isNull() ? String("null"_s) : text);
    };

    append(fixture.string("the first buffer holds one reservation"_s));
    uint32_t position = writer.builder().length();
    for (JSValue value : { jsNumber(1), jsNumber(1.5), jsBoolean(true), jsNull(), JSValue(fixture.string("plain text"_s)), JSValue(fixture.string("a\"b"_s)) }) {
        append(value);
        AOT_TEST_CHECK(writer.builder().length() == position);
    }

    String escapedTextWithoutRoom = longTextWithOneEscape();
    AOT_TEST_CHECK(!hasRoomForEscaped(writer, escapedTextWithoutRoom));
    for (JSValue value : { jsUndefined(), JSValue(fixture.string(escapedTextWithoutRoom)), JSValue(fixture.string(text16(u"日本"))) }) {
        append(value);
        AOT_TEST_CHECK(writer.builder().length() > position);
        position = writer.builder().length();
    }
    AOT_TEST_CHECK(writer.finish() == expected.toString());
}

static void test16BitStrings()
{
    DetachedInstance fixture;
    String first = "abc"_s;
    String second = text16(u"日本");
    String third = text16(u"a\"日\n\xd800");

    Writer writer(fixture);
    writer.appendLiteral("["_s);
    writer.appendString(first);
    AOT_TEST_CHECK(!lengthOutsideBuffer(writer.builder()));
    writer.appendLiteral(","_s);
    writer.appendString(second);
    AOT_TEST_CHECK(lengthOutsideBuffer(writer.builder()) == engineText(fixture, second).length());
    writer.appendLiteral(","_s);
    writer.appendString(third);
    AOT_TEST_CHECK(lengthOutsideBuffer(writer.builder()) == engineText(fixture, second).length() + engineText(fixture, third).length());
    writer.appendLiteral("]"_s);
    String written = writer.finish();
    AOT_TEST_CHECK(!written.is8Bit());
    AOT_TEST_CHECK(written == makeString('[', engineText(fixture, first), ',', engineText(fixture, second), ',', engineText(fixture, third), ']'));

    AOT_TEST_CHECK(writeOneString(fixture, second) == engineText(fixture, second));

    String latin1In16Bits = text16(u"a\"é");
    AOT_TEST_CHECK(!latin1In16Bits.is8Bit());
    Writer latin1Writer(fixture);
    latin1Writer.appendString(latin1In16Bits);
    AOT_TEST_CHECK(!lengthOutsideBuffer(latin1Writer.builder()));
    written = latin1Writer.finish();
    AOT_TEST_CHECK(written.is8Bit());
    AOT_TEST_CHECK(written == engineText(fixture, latin1In16Bits));

    std::u16string repeated(5000, u'日');
    String overInlineCapacity = text16(repeated);
    AOT_TEST_CHECK(writeOneString(fixture, overInlineCapacity) == engineText(fixture, overInlineCapacity));

    std::u16string pairs;
    for (unsigned i = 0; i < 5000; ++i)
        pairs += u"日\"";
    String overInlineCapacityWithEscapes = text16(pairs);
    AOT_TEST_CHECK(writeOneString(fixture, overInlineCapacityWithEscapes) == engineText(fixture, overInlineCapacityWithEscapes));
}

static void testRopes()
{
    DetachedInstance fixture;
    const String parts[] = { "abcdefgh"_s, "ij\"kl\n"_s, text16(u"日本"), repeatedCharacter(5000, 'r') };
    for (const String& left : parts) {
        for (const String& right : parts) {
            JSString* rope = jsString(fixture.globalObject(), fixture.string(left), fixture.string(right));
            AOT_TEST_CHECK(rope->isRope());
            Writer writer(fixture);
            writer.appendString(rope);
            AOT_TEST_CHECK(writer.finish() == engineText(fixture, makeString(left, right)));
        }
    }
}

template<typename Append>
static void checkLeafOperation(DetachedInstance& fixture, const String& expected, size_t maxLength, size_t reservedBytes, const Append& append)
{
    Writer writer(fixture);
    uint8_t* cursor = byteCast<uint8_t>(writer.cursor());
    uint8_t* end = append(cursor);
    AOT_TEST_CHECK(cursor < end && static_cast<size_t>(end - cursor) <= maxLength);
    for (size_t i = reservedBytes; i < poisonedFrameSize; ++i)
        AOT_TEST_CHECK(cursor[i] == poisonByte);
    writer.setCursor(byteCast<Latin1Character>(end));
    AOT_TEST_CHECK(writer.finish() == expected);
}

static void testIntegers()
{
    static constexpr std::array<int64_t, 29> integers {
        0, 1, 9, 10, 99, 100, 999, 1000, 9999, 10000, 10001, 10100, 99999, 100000, 1000007, 9999999, 10000000, 99999999, 100000000, 123456789, -1, -9, -10,
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
        (1LL << 53) - 1, -((1LL << 53) - 1),
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
    };
    DetachedInstance fixture;
    for (int64_t integer : integers) {
        checkLeafOperation(fixture, String::number(static_cast<long long>(integer)), maxJSONIntegerLength, jsonIntegerBytes, [&](uint8_t* cursor) {
            return operationAOTJSONAppendInteger(cursor, integer);
        });
    }
}

static void testNumbers()
{
    static constexpr std::array<double, 19> numbers {
        0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 0.1, 1e21, 1e-7,
        123456789012345680000.0, 2147483648.0, -2147483649.0, 9007199254740993.0, 5e-324,
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
        PNaN, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
    };
    DetachedInstance fixture;
    for (double number : numbers) {
        checkLeafOperation(fixture, fixture.stringify(jsNumber(number)), maxJSONNumberLength, jsonNumberBytes, [&](uint8_t* cursor) {
            return operationAOTJSONAppendNumber(cursor, number);
        });
    }
}

static void testTimeValues()
{
    static constexpr std::array<double, 12> timeValues {
        0, 1, -1, 86399999, 951782400000, 253402300799999, 253402300800000,
        -62167219200000, -62167219200001, 8.64e15, -8.64e15, PNaN,
    };
    DetachedInstance fixture;
    for (double timeValue : timeValues) {
        JSValue date = DateInstance::create(fixture.vm(), fixture.globalObject()->dateStructure(), timeValue);
        checkLeafOperation(fixture, fixture.stringify(date), maxJSONDateLength, jsonDateBytes, [&](uint8_t* cursor) {
            return operationAOTJSONAppendDate(cursor, timeValue);
        });
    }
}

static void checkValue(DetachedInstance& fixture, JSValue value, JSONPosition position, JSONOutcome expected)
{
    Writer writer(fixture);
    writer.appendLiteral("["_s);
    AOT_TEST_CHECK(writer.appendValue(value, position) == expected);
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());
    String written = writer.finish();
    if (expected != JSONOutcome::Parsed) {
        AOT_TEST_CHECK(written == "["_s);
        return;
    }
    String text = fixture.stringify(value);
    AOT_TEST_CHECK(written == makeString('[', text.isNull() ? String("null"_s) : text));
}

static void checkValueEverywhere(DetachedInstance& fixture, JSValue value, const OutcomesByPosition& expected)
{
    checkValue(fixture, value, JSONPosition::Root, expected.root);
    checkValue(fixture, value, JSONPosition::Property, expected.property);
    checkValue(fixture, value, JSONPosition::Element, expected.element);
}

static void checkValuesByPosition(TestRealm realm)
{
    static constexpr ASCIILiteral cellsWithoutText[] = {
        "(function () { })"_s,
        "(class extends (class { }) { })"_s,
        "(function () { }).bind(null)"_s,
        "({ a: 1, b: \"x\", c: [1, { d: null }] })"_s,
        "({})"_s,
        "[1, 2]"_s,
        "[]"_s,
        "1n"_s,
        "10n ** 30n"_s,
        "Object.assign(function () { }, { toJSON() { globalThis.didRunUserCode = true; return 1 } })"_s,
        "({ toJSON() { globalThis.didRunUserCode = true; return 1 } })"_s,
        "({ get a() { globalThis.didRunUserCode = true; return 1 } })"_s,
        "new Proxy({}, { get() { globalThis.didRunUserCode = true } })"_s,
        "new Date(0)"_s,
        "new String(\"s\")"_s,
        "new Number(1)"_s,
        "new Boolean(true)"_s,
        "Object(Symbol())"_s,
        "Object(1n)"_s,
    };
    DetachedInstance fixture(realm);
    const JSValue primitives[] = {
        jsNull(), jsBoolean(true), jsBoolean(false), jsNumber(1), jsNumber(-0.0), jsNumber(1.5), jsNumber(PNaN), jsNumber(-std::numeric_limits<double>::max()),
        fixture.string("abc"_s), fixture.string(emptyString()), fixture.string(text16(u"日本")), fixture.string("a\"b"_s),
    };
    for (JSValue value : primitives)
        checkValueEverywhere(fixture, value, parsedEverywhere);
    for (JSONPosition position : { JSONPosition::Root, JSONPosition::Property, JSONPosition::Element }) {
        JSString* rope = jsString(fixture.globalObject(), fixture.string("abcdefgh"_s), fixture.string("ij\"kl"_s));
        AOT_TEST_CHECK(rope->isRope());
        checkValue(fixture, rope, position, JSONOutcome::Parsed);
    }

    checkValueEverywhere(fixture, jsUndefined(), parsedAsElementOnly);
    checkValueEverywhere(fixture, fixture.evaluate("Symbol()"_s), parsedAsElementOnly);
    checkValueEverywhere(fixture, fixture.evaluate("Symbol.iterator"_s), parsedAsElementOnly);

    for (ASCIILiteral source : cellsWithoutText)
        checkValueEverywhere(fixture, fixture.evaluate(source), unknownEverywhere);
    AOT_TEST_CHECK(fixture.evaluate("globalThis.didRunUserCode"_s).isUndefined());
}

static void testValuesByPosition()
{
    checkValuesByPosition(TestRealm::Plain);
    checkValuesByPosition(TestRealm::Locked);
}

static Typed::Value appendKeyRow(Typed::TestProgram& test, const String& name, ASCIILiteral prefix = ""_s)
{
    Typed::Value row(Typed::Opcode::JSONAppendKey);
    row.setString(test.program.strings.intern(StringView(name)));
    row.setOpensObject(prefix == "{"_s);
    row.setFollowsMember(prefix == ","_s);
    return row;
}

static void checkKeyBytes(Typed::TestProgram& test, const String& name, std::span<const uint8_t> expectedEscapedName)
{
    for (ASCIILiteral prefix : { ""_s, "{"_s, ","_s }) {
        Typed::Value row = appendKeyRow(test, name, prefix);
        AOT_TEST_CHECK(Typed::canWriteJSONKeyAtCursor(test.program, row.string()));

        Vector<uint8_t> expected(byteCast<uint8_t>(prefix.span()));
        expected.append('"');
        expected.append(expectedEscapedName);
        expected.append(byteCast<uint8_t>("\":"_s.span()));
        Typed::CursorBytes bytes = Typed::bytesOfJSONKey(test.program, row);
        AOT_TEST_CHECK(equalSpans(byteCast<uint8_t>(bytes.span()), expected.span()));

        uint32_t taken = Typed::bytesTakenFromReservation(test.program, row);
        AOT_TEST_CHECK(taken == WTF::roundUpToMultipleOf<8>(expectedEscapedName.size() + 4));
        AOT_TEST_CHECK(bytes.size() <= taken && taken <= maxStringBuilderReservation);
    }
}

static void testEscapedKeys()
{
    static constexpr uint8_t latin1Name[] = { 0xe9 };
    Typed::TestProgram test;
    auto canWrite = [&](const String& name) {
        Typed::Value row = appendKeyRow(test, name);
        bool canWrite = Typed::canWriteJSONKeyAtCursor(test.program, row.string());
        AOT_TEST_CHECK(canWrite == (Typed::bytesTakenFromReservation(test.program, row) <= maxStringBuilderReservation));
        return canWrite;
    };

    checkKeyBytes(test, "a"_s, byteCast<uint8_t>("a"_s.span()));
    checkKeyBytes(test, emptyString(), { });
    checkKeyBytes(test, "a\"b"_s, byteCast<uint8_t>("a\\\"b"_s.span()));
    checkKeyBytes(test, "\x01"_s, byteCast<uint8_t>("\\u0001"_s.span()));
    checkKeyBytes(test, "\\\n"_s, byteCast<uint8_t>("\\\\\\n"_s.span()));
    checkKeyBytes(test, text16(u"é"), latin1Name);

    AOT_TEST_CHECK(!canWrite(text16(u"日")));
    AOT_TEST_CHECK(!canWrite(text16(u"aĀ")));
    AOT_TEST_CHECK(!Typed::canWriteJSONKeyAtCursor(test.program, Typed::StringID()));

    AOT_TEST_CHECK(canWrite(repeatedCharacter(maxStringBuilderReservation - 4, 'k')));
    AOT_TEST_CHECK(!canWrite(repeatedCharacter(maxStringBuilderReservation - 3, 'k')));
    AOT_TEST_CHECK(!canWrite(repeatedCharacter(maxStringBuilderReservation + 1, 'k')));
    AOT_TEST_CHECK(canWrite(repeatedCharacter((maxStringBuilderReservation - 4) / 6, 0x01)));
    AOT_TEST_CHECK(!canWrite(repeatedCharacter((maxStringBuilderReservation - 4) / 6 + 1, 0x01)));
}

static void checkValueWrittenByHost(DetachedInstance& fixture, JSValue value, JSValue property)
{
    JSObject* textOfProperty = asObject(fixture.evaluate("((value, key) => { const text = JSON.stringify({ [key]: value }); return text.length === 2 ? undefined : text.slice(JSON.stringify(String(key)).length + 2, -1) })"_s));
    MarkedArgumentBuffer arguments;
    arguments.append(value);
    arguments.append(property);
    JSValue expected = call(fixture.globalObject(), textOfProperty, jsUndefined(), arguments, "not callable"_s);
    AOT_TEST_CHECK(!fixture.vm().exceptionForInspection());

    StringBuilderStorage* builder = fixture.acquireBuilder();
    AOT_TEST_CHECK(builder->append(fixture.globalObject(), "before:"_s.span8()) == StringBuilderStorage::AppendResult::Appended);
    bool isAppended;
    {
        OperationCallScope scope(fixture.vm());
        auto appended = operationAOTStringBuilderAppendJSON(&fixture.instance(), builder, JSValue::encode(value), JSValue::encode(property));
        AOT_TEST_CHECK(!appended.exception);
        isAppended = appended.value;
    }
    AOT_TEST_CHECK(isAppended == expected.isString());
    String written = fixture.finish(builder);
    String expectedText = expected.isString() ? asString(expected)->value(fixture.globalObject()).data : emptyString();
    AOT_TEST_CHECK(written == makeString("before:"_s, expectedText));
}

static void testValuesWrittenByHost()
{
    static constexpr ASCIILiteral values[] = {
        "undefined"_s,
        "null"_s,
        "1.5"_s,
        "\"text\""_s,
        "Symbol()"_s,
        "(function () { })"_s,
        "({})"_s,
        "[]"_s,
        "({ a: 1, b: \"x\", c: [1, { d: null }, undefined, () => 1], e: undefined })"_s,
        "[[\"a\"], [], [[1.5]]]"_s,
        "({ wide: \"\\u65e5\\u672c\" })"_s,
        "[\"\\u65e5\", \"x\".repeat(9000)]"_s,
        "Array.from({ length: 3000 }, (_, i) => ({ i }))"_s,
        "new Date(0)"_s,
        "new String(\"s\")"_s,
        "new Number(1)"_s,
        "new Map([[1, 2]])"_s,
        "({ toJSON(key) { return typeof key + \":\" + key } })"_s,
        "({ toJSON() { return undefined } })"_s,
        "({ toJSON() { return () => 1 } })"_s,
        "({ inner: { toJSON(key) { return key } }, list: [{ toJSON(key) { return key } }] })"_s,
        "Object.assign([\"x\".repeat(9000)], { toJSON(key) { return key } })"_s,
        "({ long: \"x\".repeat(9000), toJSON(key) { return key } })"_s,
        "({ wide: \"\\u65e5\", toJSON(key) { return key } })"_s,
        "({ wide: \"\\u65e5\", long: \"x\".repeat(9000), toJSON(key) { return key } })"_s,
        "({ long: \"x\".repeat(5000), wide: \"\\u65e5\", toJSON(key) { return key } })"_s,
        "({ get a() { return 1 } })"_s,
        "new Proxy([1, 2], {})"_s,
    };
    DetachedInstance fixture;
    const JSValue properties[] = { fixture.string("k"_s), fixture.string("a\"b"_s), fixture.string(text16(u"\u65e5")), jsNumber(0), jsNumber(3), jsNumber(4294967294.0) };
    for (ASCIILiteral source : values) {
        for (JSValue property : properties)
            checkValueWrittenByHost(fixture, fixture.evaluate(source), property);
    }
}

std::span<const NativeTest> jsonWriterTests()
{
    static constexpr NativeTest tests[] = {
        { "json-writer/empty-text"_s, testEmptyText },
        { "json-writer/strings"_s, testStrings },
        { "json-writer/long-strings"_s, testLongStrings },
        { "json-writer/several-strings"_s, testSeveralStrings },
        { "json-writer/position-after-strings"_s, testPositionAfterStrings },
        { "json-writer/position-after-values"_s, testPositionAfterValues },
        { "json-writer/16-bit-strings"_s, test16BitStrings },
        { "json-writer/ropes"_s, testRopes },
        { "json-writer/integers"_s, testIntegers },
        { "json-writer/numbers"_s, testNumbers },
        { "json-writer/time-values"_s, testTimeValues },
        { "json-writer/values-by-position"_s, testValuesByPosition },
        { "json-writer/values-written-by-host"_s, testValuesWrittenByHost },
        { "json-writer/escaped-keys"_s, testEscapedKeys },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
