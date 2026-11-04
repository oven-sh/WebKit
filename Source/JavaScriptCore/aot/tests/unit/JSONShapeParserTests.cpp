/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTJSONHooks.h"
#include "AOTRuntimeCounters.h"
#include "AOTVector.h"
#include "JSONCache.h"
#include "PropertyNameArray.h"
#include <wtf/Noncopyable.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr unsigned timesParsed = 3;

namespace {

class HookCaller {
    WTF_MAKE_NONCOPYABLE(HookCaller);
public:
    HookCaller() = default;

    DetachedInstance& fixture() { return m_fixture; }
    JSValue tryParse(const String& text, TextEncoding = TextEncoding::Latin1);

private:
    DetachedInstance m_fixture;
};

NEVER_INLINE JSValue HookCaller::tryParse(const String& text, TextEncoding encoding)
{
    JSGlobalObject* globalObject = m_fixture.globalObject();
    globalObject->setAOTInstance(&m_fixture.instance());
    JSValue parsed;
    switch (encoding) {
    case TextEncoding::Latin1: {
        String latin1 = to8Bit(text);
        parsed = tryParseJSON(globalObject, latin1);
        break;
    }
    case TextEncoding::UTF16:
        AOT_TEST_CHECK(!text.is8Bit());
        parsed = tryParseJSON(globalObject, text);
        break;
    case TextEncoding::UTF8: {
        CString utf8 = text.utf8();
        parsed = tryParseUTF8JSON(globalObject, byteCast<uint8_t>(utf8.span()));
        break;
    }
    }
    globalObject->setAOTInstance(nullptr);
    AOT_TEST_CHECK(!m_fixture.vm().exceptionForInspection());
    return parsed;
}

} // anonymous namespace

static void checkSameStructure(Structure* actual, Structure* expected)
{
    if (!expected->isDictionary()) {
        AOT_TEST_CHECK(actual->id() == expected->id());
        return;
    }
    AOT_TEST_CHECK(actual->isCacheableDictionary() == expected->isCacheableDictionary());
    AOT_TEST_CHECK(actual->isUncacheableDictionary() == expected->isUncacheableDictionary());
    AOT_TEST_CHECK(actual->indexingType() == expected->indexingType());
    AOT_TEST_CHECK(actual->storedPrototype() == expected->storedPrototype());
}

static void checkSameValue(DetachedInstance& fixture, JSValue actual, JSValue expected)
{
    if (expected.isString()) {
        AOT_TEST_CHECK(actual.isString());
        AOT_TEST_CHECK(asString(actual)->tryGetValue().data == asString(expected)->tryGetValue().data);
        return;
    }
    if (!expected.isObject()) {
        AOT_TEST_CHECK(actual == expected);
        return;
    }

    AOT_TEST_CHECK(actual.isObject());
    JSGlobalObject* globalObject = fixture.globalObject();
    JSObject* actualObject = asObject(actual);
    JSObject* expectedObject = asObject(expected);
    checkSameStructure(actualObject->structure(), expectedObject->structure());

    PropertyNameArrayBuilder actualNames(fixture.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    PropertyNameArrayBuilder expectedNames(fixture.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    actualObject->methodTable()->getOwnPropertyNames(actualObject, globalObject, actualNames, DontEnumPropertiesMode::Include);
    expectedObject->methodTable()->getOwnPropertyNames(expectedObject, globalObject, expectedNames, DontEnumPropertiesMode::Include);
    AOT_TEST_CHECK(actualNames.size() == expectedNames.size());
    for (unsigned i = 0; i < expectedNames.size(); ++i) {
        AOT_TEST_CHECK(actualNames[i] == expectedNames[i]);
        checkSameValue(fixture, actualObject->get(globalObject, actualNames[i]), expectedObject->get(globalObject, expectedNames[i]));
    }
}

static void checkAgrees(HookCaller& hooks, const String& text, TextEncoding encoding = TextEncoding::Latin1)
{
    DetachedInstance& fixture = hooks.fixture();
    for (unsigned i = 0; i < timesParsed; ++i) {
        JSValue parsed = hooks.tryParse(text, encoding);
        AOT_TEST_CHECK(parsed);
        JSValue expected = fixture.parse(text);
        AOT_TEST_CHECK(expected);
        checkSameValue(fixture, parsed, expected);
        AOT_TEST_CHECK(fixture.stringify(parsed) == fixture.stringify(expected));
    }
}

static void checkAllAgree(HookCaller& hooks, std::span<const ASCIILiteral> texts)
{
    for (ASCIILiteral text : texts)
        checkAgrees(hooks, String(text));
    for (ASCIILiteral text : texts)
        checkAgrees(hooks, String(text));
}

static void checkDeclines(HookCaller& hooks, ASCIILiteral text)
{
    AOT_TEST_CHECK(!hooks.fixture().parse(String(text)));
    AOT_TEST_CHECK(!hooks.tryParse(String(text)));
    AOT_TEST_CHECK(!hooks.fixture().words().sourceOwnerStackPointer);
}

static String objectWithMembers(unsigned count)
{
    StringBuilder builder;
    builder.append('{');
    for (unsigned i = 0; i < count; ++i)
        builder.append(i ? ","_s : ""_s, "\"k"_s, i, "\":"_s, i);
    builder.append('}');
    return builder.toString();
}

static String arrayOfObjectKinds(unsigned count)
{
    StringBuilder builder;
    builder.append('[');
    for (unsigned i = 0; i < count; ++i)
        builder.append(i ? ","_s : ""_s, "{\"a"_s, i, "\":"_s, i, '}');
    builder.append(']');
    return builder.toString();
}

static String textOfKind(unsigned kind)
{
    return makeString("{\"type\":\"k"_s, kind, "\",\"value"_s, kind, "\":"_s, kind, ",\"last\":null}"_s);
}

static String quotedText(size_t length)
{
    Vector<uint8_t> characters(length + 2, [&](size_t i) -> uint8_t {
        return !i || i == length + 1 ? '"' : 'a';
    });
    return String(byteCast<Latin1Character>(characters.span()));
}

static String emptyNestedArrays(unsigned depth)
{
    StringBuilder builder;
    for (unsigned i = 0; i < depth; ++i)
        builder.append('[');
    for (unsigned i = 0; i < depth; ++i)
        builder.append(']');
    return builder.toString();
}

static constexpr ASCIILiteral memberOrderTexts[] = {
    "{\"a\":1,\"b\":\"x\",\"c\":true}"_s,
    "{\"c\":false,\"a\":2,\"b\":\"y\"}"_s,
    "{\"b\":\"z\",\"c\":null,\"a\":3}"_s,
    "{\"a\":4,\"c\":true}"_s,
    "{\"b\":\"w\"}"_s,
    "{\"c\":true,\"b\":\"v\"}"_s,
    "{\"a\":5,\"b\":\"u\",\"c\":false,\"d\":[1]}"_s,
    "{\"a\":6,\"e\":1}"_s,
    "{ \"a\" : 7 , \"b\" : \"t\" , \"c\" : true }"_s,
    "\n{\"a\":8,\t\"b\":\"s\"}\r\n"_s,
    "{}"_s,
    "{ }"_s,
};

static constexpr ASCIILiteral nestedObjectTexts[] = {
    "{\"id\":1,\"owner\":{\"name\":\"a\",\"tags\":[\"x\",\"y\"]},\"next\":null}"_s,
    "{\"id\":2,\"owner\":{\"name\":\"b\",\"tags\":[]},\"next\":{\"id\":3,\"owner\":null,\"next\":null}}"_s,
    "{\"id\":4,\"owner\":\"nobody\",\"next\":[{\"id\":5}]}"_s,
    "{\"id\":\"six\",\"owner\":{\"tags\":[1,{\"name\":\"c\"}],\"name\":7}}"_s,
};

static constexpr ASCIILiteral textsOfSeveralKinds[] = {
    "[{\"a\":1},[1],\"s\",2,null,true,{\"b\":[{\"a\":2}]},[[\"t\"]]]"_s,
    "{\"v\":1}"_s,
    "[1]"_s,
    "\"s\""_s,
    "12"_s,
    "-0.5e1"_s,
    "true"_s,
    "false"_s,
    "null"_s,
    "{\"v\":[1]}"_s,
    "{\"v\":{\"v\":\"s\"}}"_s,
};

static constexpr ASCIILiteral specialKeyTexts[] = {
    "{\"a\":1,\"a\":2}"_s,
    "{\"a\":1,\"b\":2,\"a\":3}"_s,
    "{\"a\":{\"x\":1},\"a\":[2]}"_s,
    "{\"__proto__\":{\"x\":1},\"a\":1}"_s,
    "{\"a\":1,\"__proto__\":null}"_s,
    "{\"__proto__\":1,\"__proto__\":2}"_s,
    "{\"1\":\"a\",\"0\":\"b\",\"x\":1}"_s,
    "{\"x\":1,\"4294967294\":2,\"4294967295\":3,\"-1\":4,\"01\":5}"_s,
    "{\"0\":1}"_s,
    "{\"\":1,\" \":2}"_s,
    "{\"a\\u0062\":1,\"ab\":2}"_s,
    "{\"\\u0061\":1,\"b\":2}"_s,
    "{\"\\n\":1,\"\\\\\":2,\"\\\"\":3,\"\\/\":4}"_s,
    "{\"toString\":1,\"constructor\":2,\"hasOwnProperty\":3}"_s,
};

static constexpr ASCIILiteral malformedTexts[] = {
    ""_s, " "_s, "{"_s, "{\"a\""_s, "{\"a\":"_s, "{\"a\":}"_s, "{\"a\":1,}"_s, "{\"a\":1,\"b\"}"_s, "{\"a\":1,\"b\":}"_s, "{\"a\":1 \"b\":2}"_s,
    "{\"a\":1,\"b\":2"_s, "{\"a\":1,\"b\":2]"_s, "{\"a\":1,\"b\":2}x"_s, "{\"a\":1,\"b\":2}}"_s, "{\"a\":1,\"b\":2} {}"_s, "{\"a\":1,\"b\":2},"_s,
    "{\"a\" 1}"_s, "{\"a\":1,,\"b\":2}"_s, "{,\"a\":1}"_s, "{a:1}"_s, "{'a':1}"_s, "{\"a\":01}"_s, "{\"a\":+1}"_s, "{\"a\":1.}"_s, "{\"a\":.5}"_s,
    "{\"a\":tru}"_s, "{\"a\":nul}"_s, "{\"a\":fals}"_s, "{\"a\":undefined}"_s, "{\"a\":\"x}"_s, "{\"a\":\"\\x\"}"_s, "{\"a\":\"\\u12\"}"_s, "{\"a\":\"\t\"}"_s,
    "["_s, "[1,"_s, "[1,]"_s, "[,1]"_s, "[1 2]"_s, "[1,2}"_s, "[1,2]]"_s, "[1,2] 3"_s, "[{\"a\":1,\"b\":}]"_s,
    "nul"_s, "tru"_s, "fals"_s, "nulll"_s, "\"abc"_s, "01"_s, "-"_s, "1e"_s, "NaN"_s,
};

template<const auto& texts>
static void testTexts()
{
    HookCaller hooks;
    checkAllAgree(hooks, texts);
}

static void testManyMembers()
{
    static constexpr unsigned counts[] = {
        1, JSFinalObject::defaultInlineCapacity - 1, JSFinalObject::defaultInlineCapacity, JSFinalObject::defaultInlineCapacity + 1, 2 * JSFinalObject::defaultInlineCapacity,
        JSFinalObject::maxInlineCapacity - 1, JSFinalObject::maxInlineCapacity, JSFinalObject::maxInlineCapacity + 1, 70, 2 * JSFinalObject::maxInlineCapacity, 1000,
    };
    static_assert(JSFinalObject::maxInlineCapacity < 70);
    HookCaller ascending;
    for (unsigned count : counts)
        checkAgrees(ascending, objectWithMembers(count));
    for (unsigned count : counts)
        checkAgrees(ascending, makeString('[', objectWithMembers(count), ',', objectWithMembers(count), ']'));

    HookCaller startingPastInlineCapacity;
    checkAgrees(startingPastInlineCapacity, objectWithMembers(70));
    checkAgrees(startingPastInlineCapacity, objectWithMembers(1));
}

static void testManyShapes()
{
    static constexpr unsigned numberOfShapes = 300;
    HookCaller hooks;
    checkAgrees(hooks, arrayOfObjectKinds(9));
    checkAgrees(hooks, arrayOfObjectKinds(numberOfShapes));
    for (unsigned kind = 0; kind < numberOfShapes; ++kind)
        checkAgrees(hooks, textOfKind(kind));
    for (unsigned kind = 0; kind < numberOfShapes; ++kind)
        checkAgrees(hooks, textOfKind(kind));
}

static void checkIndexingShape(HookCaller& hooks, ASCIILiteral text, IndexingType expectedShape)
{
    checkAgrees(hooks, String(text));
    JSValue parsed = hooks.tryParse(String(text));
    AOT_TEST_CHECK(parsed && isJSArray(parsed));
    AOT_TEST_CHECK((asArray(parsed)->indexingType() & IndexingShapeMask) == expectedShape);
}

static void testArrays()
{
    static constexpr ASCIILiteral nested[] = {
        "[]"_s,
        "[ ]"_s,
        "[[1,2],[1.5,2],[\"a\",1],[]]"_s,
        "[[[1]],[[2.5,[null]]]]"_s,
        "[ 1 , 2 ,\n3 ]"_s,
        "[2147483647,2147483648,-2147483648,-2147483649,1e21,-0]"_s,
    };
    static constexpr unsigned lengths[] = { 31, 32, 33, 128, 5000 };
    HookCaller hooks;
    checkIndexingShape(hooks, "[1,2,3]"_s, Int32Shape);
    checkIndexingShape(hooks, "[1.5,2,3]"_s, DoubleShape);
    checkIndexingShape(hooks, "[1,2.5]"_s, DoubleShape);
    checkIndexingShape(hooks, "[1,\"a\",null]"_s, ContiguousShape);
    checkIndexingShape(hooks, "[1.5,true]"_s, ContiguousShape);
    checkAllAgree(hooks, nested);

    for (unsigned length : lengths) {
        StringBuilder longArray;
        longArray.append('[');
        for (unsigned i = 0; i < length; ++i)
            longArray.append(i ? ","_s : ""_s, i);
        longArray.append(']');
        checkAgrees(hooks, longArray.toString());
        checkAgrees(hooks, makeString("[[0],"_s, longArray.toString(), ",[1,"_s, longArray.toString(), "]]"_s));
    }
}

static void testSpecialKeys()
{
    for (ASCIILiteral text : specialKeyTexts) {
        HookCaller hooks;
        checkAgrees(hooks, String(text));
    }
    {
        HookCaller hooks;
        checkAllAgree(hooks, specialKeyTexts);
        for (ASCIILiteral text : specialKeyTexts)
            checkAgrees(hooks, makeString('[', text, ']'));
        for (ASCIILiteral text : specialKeyTexts)
            checkAgrees(hooks, makeString('[', text, ',', text, ']'));
    }

    HookCaller hooks;
    JSValue parsed = hooks.tryParse("{\"__proto__\":{\"x\":1}}"_s);
    AOT_TEST_CHECK(parsed && parsed.isObject());
    AOT_TEST_CHECK(asObject(parsed)->getPrototypeDirect() == hooks.fixture().globalObject()->objectPrototype());
}

static String objectWithNameLengths(std::span<const unsigned> lengths)
{
    StringBuilder builder;
    builder.append('{');
    for (unsigned length : lengths) {
        builder.append(builder.length() > 1 ? ","_s : ""_s, '"');
        for (unsigned i = 0; i < length; ++i)
            builder.append('n');
        builder.append("\":"_s, length);
    }
    builder.append('}');
    return builder.toString();
}

static void testNameLengths()
{
    static constexpr unsigned longestCachedName = JSONCache::maxNameTextLength - 3;
    static constexpr unsigned shortNames[] = { 1, 4, 5, 6, 7 };
    static constexpr unsigned mediumNames[] = { 12, 13, 14, 15, 16, 17 };
    static constexpr unsigned namesAtCacheLimit[] = { longestCachedName - 1, longestCachedName, longestCachedName + 1, 2 * JSONCache::maxNameTextLength, jsonSourcePadding, 300 };
    static constexpr unsigned uncachedNameFirst[] = { longestCachedName + 1, 1 };
    for (std::span<const unsigned> lengths : { std::span<const unsigned> { shortNames }, std::span<const unsigned> { mediumNames }, std::span<const unsigned> { namesAtCacheLimit }, std::span<const unsigned> { uncachedNameFirst } }) {
        HookCaller hooks;
        checkAgrees(hooks, objectWithNameLengths(lengths));
        checkAgrees(hooks, objectWithNameLengths(lengths.first(1)));
        checkAgrees(hooks, objectWithNameLengths(lengths.subspan(1)));
    }
}

static void testEveryEncoding()
{
    static constexpr std::u16string_view texts[] = {
        u"{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5}}",
        u"{\"a\":\"\\u00e9\",\"b\":[\"\\u00ff\"],\"c\":{\"d\":\"plain\"}}",
        u"{\"\u00e9\":\"\u00ff\",\"a\":\"\u00e9\"}",
        u"{\"a\":\"\u65e5\u672c\",\"\u65e5\":1}",
        u"[\"\u65e5\u672c\",\"a\",\"\u00e9\",\"\\ud83d\\ude00\"]",
        u"{\"a\":\"\U0001f600\",\"b\":\"\\ud83d\"}",
    };
    for (TextEncoding encoding : allTextEncodings) {
        HookCaller hooks;
        for (std::u16string_view characters : texts) {
            String text = text16(characters);
            if (encoding != TextEncoding::Latin1 || fitsLatin1(text))
                checkAgrees(hooks, text, encoding);
        }
    }

    HookCaller shared;
    for (std::u16string_view characters : texts) {
        String text = text16(characters);
        for (TextEncoding encoding : allTextEncodings) {
            if (encoding != TextEncoding::Latin1 || fitsLatin1(text))
                checkAgrees(shared, text, encoding);
        }
    }
}

static void testInvalidBytes()
{
    static constexpr uint8_t invalidValue[] = { '{', '"', 'a', '"', ':', '"', 0xff, '"', '}' };
    static constexpr uint8_t invalidName[] = { '{', '"', 0xc3, 0x28, '"', ':', '1', '}' };
    static constexpr uint8_t truncatedSequence[] = { '[', '"', 0xe6, 0x97, '"', ']' };
    HookCaller hooks;
    JSGlobalObject* globalObject = hooks.fixture().globalObject();
    globalObject->setAOTInstance(&hooks.fixture().instance());
    for (std::span<const uint8_t> bytes : { std::span<const uint8_t> { invalidValue }, std::span<const uint8_t> { invalidName }, std::span<const uint8_t> { truncatedSequence } }) {
        AOT_TEST_CHECK(!tryParseUTF8JSON(globalObject, bytes));
        AOT_TEST_CHECK(!hooks.fixture().vm().exceptionForInspection());
        AOT_TEST_CHECK(!hooks.fixture().words().sourceOwnerStackPointer);
    }
    globalObject->setAOTInstance(nullptr);
    checkAgrees(hooks, "{\"a\":\"b\"}"_s, TextEncoding::UTF8);
}

static void testMalformedTexts()
{
    HookCaller hooks;
    for (ASCIILiteral text : malformedTexts)
        checkDeclines(hooks, text);
    checkAgrees(hooks, "{\"a\":1,\"b\":2}"_s);
    checkAgrees(hooks, "[{\"a\":1,\"b\":2}]"_s);
    for (ASCIILiteral text : malformedTexts)
        checkDeclines(hooks, text);
    checkAgrees(hooks, "{\"a\":1,\"b\":2}"_s);
}

static void testFailuresLeaveNoState()
{
    static constexpr unsigned numberOfFailures = 1000;
    HookCaller hooks;
    SourceWords wordsBeforeFirstText = sourceWordsOf(hooks.fixture().words());
    AOT_TEST_CHECK(wordsBeforeFirstText == SourceWords { });
    for (unsigned i = 0; i < numberOfFailures; ++i) {
        AOT_TEST_CHECK(!hooks.tryParse(makeString("{\"id\":"_s, i, ",\"name\":\"n\",\"tags\":[1,2,}"_s)));
        AOT_TEST_CHECK(!hooks.fixture().words().sourceOwnerStackPointer);
        AOT_TEST_CHECK(!hooks.fixture().runtime().numberOfSuspendedSources());
    }
    checkAgrees(hooks, "{\"id\":1000,\"name\":\"n\",\"tags\":[1,2]}"_s);

    for (unsigned i = 0; i < numberOfFailures; ++i)
        checkDeclines(hooks, malformedTexts[i % std::size(malformedTexts)]);
    checkAllAgree(hooks, memberOrderTexts);
}

static void testCollectionBetweenParses()
{
    HookCaller hooks;
    VM& vm = hooks.fixture().vm();
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Full }) {
        checkAllAgree(hooks, memberOrderTexts);
        checkAllAgree(hooks, nestedObjectTexts);
        checkAllAgree(hooks, specialKeyTexts);
        checkAgrees(hooks, objectWithMembers(70));
        checkAgrees(hooks, arrayOfObjectKinds(40));
        vm.heap.collectSync(scope);
        hooks.fixture().finalizeCaches(scope);
    }
    checkAllAgree(hooks, memberOrderTexts);
    checkAllAgree(hooks, nestedObjectTexts);
}

static void testParsedValuesSurviveCollection()
{
    HookCaller hooks;
    DetachedInstance& fixture = hooks.fixture();
    String text = makeString('[', objectWithMembers(70), ',', arrayOfObjectKinds(40), ",{\"s\":\"a string that is made for this text\",\"d\":1.5}]"_s);
    JSValue parsed = hooks.tryParse(text);
    AOT_TEST_CHECK(parsed);
    fixture.vm().heap.collectSync(CollectionScope::Full);
    checkSameValue(fixture, parsed, fixture.parse(text));
}

static void testTextLengths()
{
    static constexpr TextEncoding encodings[] = { TextEncoding::Latin1, TextEncoding::UTF8 };
    static constexpr size_t lengths[] = { 0, 1, jsonSourcePadding - 2, jsonSourcePadding, minJSONSourceCapacity, maxRetainedJSONSourceCapacity, 4 * MB };
    HookCaller hooks;
    for (TextEncoding encoding : encodings) {
        AOT_TEST_CHECK(!hooks.tryParse(emptyString(), encoding));
        for (size_t length : lengths)
            checkAgrees(hooks, quotedText(length), encoding);
    }
}

static void testNesting()
{
    static constexpr unsigned deeperThanTheStack = 1000000;
    HookCaller hooks;
    for (unsigned depth : { 1u, 2u, 64u, 65u, 1000u })
        checkAgrees(hooks, emptyNestedArrays(depth));
    AOT_TEST_CHECK(!hooks.tryParse(emptyNestedArrays(deeperThanTheStack)));
    checkAgrees(hooks, emptyNestedArrays(3));
}

static void testOpenSources()
{
    HookCaller hooks;
    DetachedInstance& fixture = hooks.fixture();
    JSONWords& words = fixture.words();
    String text = "{\"a\":1}"_s;
    {
        JSONSourceScope outer(fixture.instance(), StringView("[1]"_s));
        AOT_TEST_CHECK(!!outer);
        SourceWords before = sourceWordsOf(words);
        checkAgrees(hooks, text);
        AOT_TEST_CHECK(!hooks.tryParse("{\"a\":"_s));
        AOT_TEST_CHECK(sourceWordsOf(words) == before);
        AOT_TEST_CHECK(equalSpans(std::span { words.sourceStart, words.sourceEnd }, byteCast<uint8_t>("[1]"_s.span())));
        AOT_TEST_CHECK(!fixture.runtime().numberOfSuspendedSources());
    }
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);

    fixture.open("1"_s, TextEncoding::Latin1);
    AOT_TEST_CHECK(!mayHaveOpenSource(words, currentStackPointer()));
    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(1));
    checkAgrees(hooks, text);
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
}

static NEVER_INLINE void checkHookAtEveryNestingLevel(HookCaller& hooks, unsigned level)
{
    uint64_t limit = runtimeLimitValue(hooks.fixture().vm(), LimitID::MaxOpenJSONSources);
    AOT_TEST_CHECK(level <= limit);
    JSONSourceScope scope(hooks.fixture().instance(), StringView("[1]"_s));
    AOT_TEST_CHECK(!!scope);
    if (level == limit) {
        AOT_TEST_CHECK(!hooks.tryParse("{\"a\":1}"_s));
        return;
    }
    checkAgrees(hooks, "{\"a\":1}"_s);
    checkHookAtEveryNestingLevel(hooks, level + 1);
    checkAgrees(hooks, "{\"a\":1}"_s);
}

static void testNestingLimitOfSources()
{
    HookCaller hooks;
    checkHookAtEveryNestingLevel(hooks, 1);
    AOT_TEST_CHECK(!hooks.fixture().words().sourceOwnerStackPointer);
    checkAgrees(hooks, "{\"a\":1}"_s);
}

static void testStockParserFirst()
{
    HookCaller hooks;
    DetachedInstance& fixture = hooks.fixture();
    for (std::span<const ASCIILiteral> texts : { std::span<const ASCIILiteral> { memberOrderTexts }, std::span<const ASCIILiteral> { nestedObjectTexts }, std::span<const ASCIILiteral> { textsOfSeveralKinds }, std::span<const ASCIILiteral> { specialKeyTexts } }) {
        for (ASCIILiteral text : texts)
            AOT_TEST_CHECK(fixture.parse(String(text)));
        checkAllAgree(hooks, texts);
    }
}

std::span<const NativeTest> jsonShapeParserTests()
{
    static constexpr NativeTest tests[] = {
        { "json-parser/values/member-orders"_s, testTexts<memberOrderTexts> },
        { "json-parser/values/nested-objects"_s, testTexts<nestedObjectTexts> },
        { "json-parser/values/many-members"_s, testManyMembers },
        { "json-parser/values/many-shapes"_s, testManyShapes },
        { "json-parser/values/values-of-several-kinds"_s, testTexts<textsOfSeveralKinds> },
        { "json-parser/values/arrays"_s, testArrays },
        { "json-parser/values/special-keys"_s, testSpecialKeys },
        { "json-parser/values/name-lengths"_s, testNameLengths },
        { "json-parser/values/every-encoding"_s, testEveryEncoding },
        { "json-parser/values/invalid-bytes"_s, testInvalidBytes },
        { "json-parser/values/malformed-texts"_s, testMalformedTexts },
        { "json-parser/values/failures-leave-no-state"_s, testFailuresLeaveNoState },
        { "json-parser/values/collection-between-parses"_s, testCollectionBetweenParses },
        { "json-parser/values/parsed-values-survive-collection"_s, testParsedValuesSurviveCollection },
        { "json-parser/values/text-lengths"_s, testTextLengths },
        { "json-parser/values/nesting"_s, testNesting },
        { "json-parser/values/open-sources"_s, testOpenSources },
        { "json-parser/values/nesting-limit-of-sources"_s, testNestingLimitOfSources },
        { "json-parser/values/stock-parser-first"_s, testStockParserFirst },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
