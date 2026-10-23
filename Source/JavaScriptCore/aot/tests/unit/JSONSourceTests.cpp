/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTJSONSource.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include <algorithm>
#include <span>
#include <string>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/Latin1Character.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct SourceText {
    String text;
    TextEncoding encoding;
};

struct SourceState {
    SourceWords words;
    size_t capacity;
    size_t suspendedCount;

    friend bool operator==(const SourceState&, const SourceState&) = default;
};

struct CloseCase {
    ASCIILiteral text;
    size_t cursorOffset;
    bool isComplete;
};

} // namespace

static SourceText sourceTextAt(unsigned i)
{
    TextEncoding encoding = allTextEncodings[i % allTextEncodings.size()];
    std::u16string characters;
    characters.push_back(u'"');
    characters.append(i + 1, static_cast<char16_t>(u'a' + i % 26));
    characters.push_back(encoding == TextEncoding::UTF16 ? u'日' : u'é');
    characters.push_back(u'"');
    return { text16(characters), encoding };
}

static Vector<uint8_t> copyOf(const SourceText& source)
{
    auto characters = source.text.span16();
    switch (source.encoding) {
    case TextEncoding::Latin1:
        return Vector<uint8_t>(characters.size(), [&](size_t i) -> uint8_t {
            return static_cast<uint8_t>(characters[i]);
        });
    case TextEncoding::UTF16:
        return Vector<uint8_t>(characters.size(), [&](size_t i) -> uint8_t {
            return static_cast<uint8_t>(std::min<char16_t>(characters[i], saturatedJSONCharacter));
        });
    case TextEncoding::UTF8: {
        CString utf8 = source.text.utf8();
        return Vector<uint8_t>(byteCast<uint8_t>(utf8.span()));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSONSourceEncoding sourceEncodingOf(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Latin1:
        return JSONSourceEncoding::Latin1;
    case TextEncoding::UTF16:
        return JSONSourceEncoding::UTF16;
    case TextEncoding::UTF8:
        return JSONSourceEncoding::UTF8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::span<const uint8_t> sourceCopy(const JSONWords& words)
{
    return { words.sourceStart, words.sourceEnd };
}

static std::span<const uint8_t> bytesOf(ASCIILiteral text)
{
    return byteCast<uint8_t>(text.span());
}

static void checkCopy(const JSONWords& words, std::span<const uint8_t> expected)
{
    AOT_TEST_CHECK(equalSpans(sourceCopy(words), expected));
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
}

static bool openSource(DetachedInstance& fixture, unsigned i, unsigned depth)
{
    SourceText source = sourceTextAt(i);
    return fixture.runtime().open(fixture.text(source.text, source.encoding), stackPointerAtDepth(depth));
}

static void checkOpenSource(DetachedInstance& fixture, unsigned i, unsigned depth)
{
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();
    SourceText source = sourceTextAt(i);
    Vector<uint8_t> copy = copyOf(source);

    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(depth));
    checkCopy(words, copy.span());
    AOT_TEST_CHECK(words.sourceEncoding == sourceEncodingOf(source.encoding));
    AOT_TEST_CHECK(runtime.sourceCapacity() >= copy.size() + jsonSourcePadding);
    if (source.encoding != TextEncoding::UTF16) {
        AOT_TEST_CHECK(!runtime.wideText());
        return;
    }
    AOT_TEST_CHECK(runtime.wideText());
    AOT_TEST_CHECK(equalSpans(original16BitCharacters(words, sourceCopy(words)), source.text.span16()));
}

static String quotedText(size_t length)
{
    Vector<uint8_t> characters(length + 2, [&](size_t i) -> uint8_t {
        return !i || i == length + 1 ? '"' : 'a';
    });
    return String(byteCast<Latin1Character>(characters.span()));
}

static SourceState sourceStateOf(DetachedInstance& fixture)
{
    return { sourceWordsOf(fixture.words()), fixture.runtime().sourceCapacity(), fixture.runtime().numberOfSuspendedSources() };
}

static void openBytes(DetachedInstance& fixture, std::span<const uint8_t> bytes)
{
    AOT_TEST_CHECK(fixture.runtime().open(fixture.bytes(bytes), stackPointerAtDepth(1)));
}

static void testOpenAndClose()
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    AOT_TEST_CHECK(sourceWordsOf(words) == SourceWords { });
    AOT_TEST_CHECK(!runtime.sourceCapacity());
    AOT_TEST_CHECK(!runtime.numberOfSuspendedSources());

    const uint8_t* copy = fixture.open("{\"a\":1}"_s, TextEncoding::Latin1, 1);
    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(1));
    AOT_TEST_CHECK(copy == words.sourceStart);
    checkCopy(words, bytesOf("{\"a\":1}"_s));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    AOT_TEST_CHECK(runtime.sourceCapacity() >= minJSONSourceCapacity);
    AOT_TEST_CHECK(!runtime.numberOfSuspendedSources());
    AOT_TEST_CHECK(!runtime.wideText());

    size_t capacity = runtime.sourceCapacity();
    AOT_TEST_CHECK(runtime.close(words.sourceEnd));
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
    AOT_TEST_CHECK(words.sourceStart == copy);
    AOT_TEST_CHECK(runtime.sourceCapacity() == capacity);

    AOT_TEST_CHECK(runtime.open(jsEmptyString(fixture.vm()), stackPointerAtDepth(1)));
    AOT_TEST_CHECK(words.sourceStart);
    AOT_TEST_CHECK(words.sourceStart == words.sourceEnd);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    AOT_TEST_CHECK(runtime.close(words.sourceStart));
}

static void testCloseAnswers()
{
    static constexpr CloseCase cases[] = {
        { "1 \n\t\r "_s, 1, true },
        { "1"_s, 1, true },
        { "1 x"_s, 1, false },
        { "1"_s, 0, false },
    };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    for (const CloseCase& closeCase : cases) {
        const uint8_t* cursor = fixture.open(closeCase.text, TextEncoding::Latin1) + closeCase.cursorOffset;
        AOT_TEST_CHECK(runtime.close(cursor) == closeCase.isComplete);
        AOT_TEST_CHECK(!words.sourceOwnerStackPointer);

        cursor = fixture.open(closeCase.text, TextEncoding::Latin1) + closeCase.cursorOffset;
        AOT_TEST_CHECK(operationAOTJSONSourceClose(&fixture.instance(), cursor) == static_cast<size_t>(closeCase.isComplete));
        AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
    }

    fixture.open("1"_s, TextEncoding::Latin1);
    operationAOTJSONSourceAbandon(&fixture.instance());
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);

    SourceWords before = sourceWordsOf(words);
    AOT_TEST_CHECK(!runtime.close(words.sourceStart));
    AOT_TEST_CHECK(sourceWordsOf(words) == before);
    runtime.abandon();
    AOT_TEST_CHECK(sourceWordsOf(words) == before);
    AOT_TEST_CHECK(!operationAOTJSONSourceClose(&fixture.instance(), words.sourceStart));
    AOT_TEST_CHECK(sourceWordsOf(words) == before);
    operationAOTJSONSourceAbandon(&fixture.instance());
    AOT_TEST_CHECK(sourceWordsOf(words) == before);
}

static void checkWideTextEncoding(DetachedInstance& fixture)
{
    static constexpr uint8_t expected[] = { 0x22, 0x61, 0xe9, 0xff, 0xff, 0xff, 0xff, 0xff, 0x22 };

    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();
    String text = text16(u"\"aéÿĀ日\xD83D\xDE00\"");
    StringImpl* impl = text.impl();

    fixture.open(text, TextEncoding::UTF16);
    checkCopy(words, expected);
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF16);
    AOT_TEST_CHECK(runtime.wideText() == impl);
    AOT_TEST_CHECK(original16BitCharacters(words, sourceCopy(words)).data() == impl->span16().data());

    auto middle = original16BitCharacters(words, { words.sourceStart + 2, 3 });
    AOT_TEST_CHECK(middle.data() == impl->span16().data() + 2);
    AOT_TEST_CHECK(middle.size() == 3);

    AOT_TEST_CHECK(isLatin1AfterDecoding(words, { words.sourceStart, 3 }));
    AOT_TEST_CHECK(!isLatin1AfterDecoding(words, { words.sourceStart, 4 }));
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());
}

static void testEncodings()
{
    static constexpr uint8_t latin1Copy[] = { 0x22, 0xe9, 0xff, 0x22 };
    static constexpr uint8_t utf8Copy[] = { 0x22, 0xc3, 0xa9, 0x22 };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    fixture.open(text16(u"\"éÿ\""), TextEncoding::Latin1);
    checkCopy(words, latin1Copy);
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    AOT_TEST_CHECK(isLatin1(words, sourceCopy(words)));
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());

    checkWideTextEncoding(fixture);

    fixture.open(text16(u"[1]"), TextEncoding::UTF16);
    checkCopy(words, bytesOf("[1]"_s));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF16);
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());

    fixture.open(text16(u"\"é\""), TextEncoding::UTF8);
    checkCopy(words, utf8Copy);
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF8);
    AOT_TEST_CHECK(isLatin1AfterDecoding(words, { words.sourceStart, 1 }));
    AOT_TEST_CHECK(!isLatin1AfterDecoding(words, { words.sourceStart, 2 }));
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());

    openBytes(fixture, bytesOf("{\"a\":1}"_s));
    checkCopy(words, bytesOf("{\"a\":1}"_s));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());

    openBytes(fixture, { });
    AOT_TEST_CHECK(words.sourceStart == words.sourceEnd);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());
}

static void checkMarkNotStripped(std::span<const uint8_t> bytes)
{
    auto result = withoutByteOrderMark(bytes);
    AOT_TEST_CHECK(result.data() == bytes.data());
    AOT_TEST_CHECK(result.size() == bytes.size());
}

static void testByteOrderMark()
{
    static constexpr uint8_t mark[] = { 0xef, 0xbb, 0xbf };
    static constexpr uint8_t markAndObject[] = { 0xef, 0xbb, 0xbf, '{', '}' };
    static constexpr uint8_t twoMarksAndOne[] = { 0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf, '1' };
    static constexpr uint8_t markAndOne[] = { 0xef, 0xbb, 0xbf, '1' };
    static constexpr uint8_t partialMarkAndOne[] = { 0xef, 0xbb, '1' };
    static constexpr uint8_t oneAndMark[] = { '1', 0xef, 0xbb, 0xbf };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    openBytes(fixture, markAndObject);
    AOT_TEST_CHECK(sourceCopy(words).size() == 2);
    AOT_TEST_CHECK(*words.sourceStart == '{');
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    runtime.abandon();

    openBytes(fixture, mark);
    AOT_TEST_CHECK(words.sourceStart == words.sourceEnd);
    runtime.abandon();

    openBytes(fixture, twoMarksAndOne);
    checkCopy(words, markAndOne);
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF8);
    runtime.abandon();

    checkMarkNotStripped({ });
    checkMarkNotStripped(std::span<const uint8_t> { mark }.first(1));
    checkMarkNotStripped(std::span<const uint8_t> { mark }.first(2));
    checkMarkNotStripped(partialMarkAndOne);
    checkMarkNotStripped(oneAndMark);

    AOT_TEST_CHECK(withoutByteOrderMark(mark).empty());
    AOT_TEST_CHECK(withoutByteOrderMark(markAndOne).size() == 1);
    AOT_TEST_CHECK(withoutByteOrderMark(markAndOne).front() == '1');
}

static void testOwnsWideText()
{
    DetachedInstance fixture;
    JSONRuntime& runtime = fixture.runtime();
    String text = text16(u"\"日本\"");
    StringImpl* impl = text.impl();
    JSString* cell = fixture.string(text);
    unsigned before = impl->refCount();

    AOT_TEST_CHECK(runtime.open(cell, stackPointerAtDepth(1)));
    AOT_TEST_CHECK(impl->refCount() == before + 1);

    AOT_TEST_CHECK(openSource(fixture, 0, 2));
    AOT_TEST_CHECK(impl->refCount() == before + 1);
    AOT_TEST_CHECK(!runtime.wideText());
    AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 1);

    runtime.abandon();
    AOT_TEST_CHECK(runtime.wideText() == impl);

    runtime.abandon();
    AOT_TEST_CHECK(impl->refCount() == before);
}

static void checkOpenDeclined(DetachedInstance& fixture, JSValue value, unsigned depth)
{
    SourceState before = sourceStateOf(fixture);
    AOT_TEST_CHECK(!fixture.runtime().open(value, stackPointerAtDepth(depth)));
    AOT_TEST_CHECK(sourceStateOf(fixture) == before);
}

static void checkValueIsNotText(DetachedInstance& fixture, JSValue value)
{
    checkOpenDeclined(fixture, value, 1);
    AOT_TEST_CHECK(openSource(fixture, 1, 2));
    checkOpenDeclined(fixture, value, 3);
    checkOpenDeclined(fixture, value, 1);
    fixture.runtime().abandon();
}

static void testNonTextValues()
{
    static constexpr ASCIILiteral sources[] = {
        "({})"_s,
        "[]"_s,
        "new String('1')"_s,
        "Symbol()"_s,
        "10n ** 30n"_s,
        "new Uint16Array(2)"_s,
        "new Uint8ClampedArray(2)"_s,
        "new Int8Array(2)"_s,
        "new DataView(new ArrayBuffer(2))"_s,
    };

    DetachedInstance fixture;
    for (JSValue value : { jsNumber(1), jsUndefined(), jsNull(), jsBoolean(true) })
        checkValueIsNotText(fixture, value);
    for (ASCIILiteral source : sources)
        checkValueIsNotText(fixture, fixture.evaluate(source));
}

static uint64_t maxOpenSources(DetachedInstance& fixture)
{
    return runtimeLimitValue(fixture.vm(), LimitID::MaxOpenJSONSources);
}

static void testNestingLimit()
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();
    fixture.vm().aotState().counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();

    unsigned i = 1;
    for (; ; ++i) {
        AOT_TEST_CHECK(i <= maxOpenSources(fixture) + 1);
        AOT_TEST_CHECK(!limitHitsOf(fixture.vm(), LimitID::MaxOpenJSONSources));
        SourceState before = sourceStateOf(fixture);
        if (!openSource(fixture, i, i)) {
            AOT_TEST_CHECK(sourceStateOf(fixture) == before);
            break;
        }
        checkOpenSource(fixture, i, i);
        AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == i - 1);
    }
    AOT_TEST_CHECK(i - 1 == maxOpenSources(fixture));
    AOT_TEST_CHECK(limitHitsOf(fixture.vm(), LimitID::MaxOpenJSONSources) == 1);

    for (unsigned k = i - 1; k >= 1; --k) {
        checkOpenSource(fixture, k, k);
        AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == k - 1);
        if (k % 2)
            runtime.abandon();
        else
            AOT_TEST_CHECK(runtime.close(words.sourceEnd - !(k % 4)) == !!(k % 4));
    }
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
}

static void testDeadSources()
{
    {
        DetachedInstance fixture;
        AOT_TEST_CHECK(!mayHaveOpenSource(fixture.words(), stackPointerAtDepth(5)));
        AOT_TEST_CHECK(openSource(fixture, 1, 5));
        AOT_TEST_CHECK(!mayHaveOpenSource(fixture.words(), stackPointerAtDepth(5)));
        AOT_TEST_CHECK(openSource(fixture, 2, 5));
        AOT_TEST_CHECK(!fixture.runtime().numberOfSuspendedSources());
        checkOpenSource(fixture, 2, 5);
    }
    {
        DetachedInstance fixture;
        AOT_TEST_CHECK(openSource(fixture, 1, 5));
        AOT_TEST_CHECK(!mayHaveOpenSource(fixture.words(), stackPointerAtDepth(3)));
        AOT_TEST_CHECK(openSource(fixture, 2, 3));
        AOT_TEST_CHECK(!fixture.runtime().numberOfSuspendedSources());
        checkOpenSource(fixture, 2, 3);
    }
    {
        DetachedInstance fixture;
        AOT_TEST_CHECK(openSource(fixture, 1, 5));
        AOT_TEST_CHECK(mayHaveOpenSource(fixture.words(), stackPointerAtDepth(7)));
        AOT_TEST_CHECK(openSource(fixture, 2, 7));
        AOT_TEST_CHECK(fixture.runtime().numberOfSuspendedSources() == 1);
        fixture.runtime().abandon();
        checkOpenSource(fixture, 1, 5);
    }
    {
        DetachedInstance fixture;
        JSONRuntime& runtime = fixture.runtime();
        AOT_TEST_CHECK(openSource(fixture, 1, 2));
        AOT_TEST_CHECK(openSource(fixture, 2, 4));
        AOT_TEST_CHECK(openSource(fixture, 3, 6));
        AOT_TEST_CHECK(openSource(fixture, 4, 3));
        AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 1);
        checkOpenSource(fixture, 4, 3);
        runtime.abandon();
        checkOpenSource(fixture, 1, 2);
    }
}

static void testUnwinding()
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    SourceWords before = sourceWordsOf(words);
    words.didUnwindTo(stackPointerAtDepth(1));
    AOT_TEST_CHECK(sourceWordsOf(words) == before);

    AOT_TEST_CHECK(openSource(fixture, 1, 2));
    AOT_TEST_CHECK(openSource(fixture, 2, 4));
    AOT_TEST_CHECK(openSource(fixture, 3, 6));

    words.didUnwindTo(stackPointerAtDepth(7));
    checkOpenSource(fixture, 3, 6);
    AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 2);

    words.didUnwindTo(stackPointerAtDepth(3));
    checkOpenSource(fixture, 1, 2);
    AOT_TEST_CHECK(!runtime.numberOfSuspendedSources());

    words.didUnwindTo(stackPointerAtDepth(2));
    checkOpenSource(fixture, 1, 2);

    words.didUnwindTo(stackPointerAtDepth(1));
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
}

static void testScope()
{
    static constexpr uint8_t markAndOne[] = { 0xef, 0xbb, 0xbf, '1' };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    {
        JSONSourceScope scope(fixture.instance(), StringView("[1]"_s));
        AOT_TEST_CHECK(!!scope);
        AOT_TEST_CHECK(scope.start() == words.sourceStart);
        checkCopy(words, bytesOf("[1]"_s));
        AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
        AOT_TEST_CHECK(words.sourceOwnerStackPointer);
    }
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);

    String text = text16(u"[\"日\"]");
    unsigned before = text.impl()->refCount();
    {
        JSONSourceScope scope(fixture.instance(), StringView(text));
        AOT_TEST_CHECK(!!scope);
        AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF16);
        AOT_TEST_CHECK(runtime.wideText()->span16().data() == text.span16().data());
        AOT_TEST_CHECK(text.impl()->refCount() == before);
    }
    AOT_TEST_CHECK(!runtime.wideText());

    {
        JSONSourceScope scope(fixture.instance(), std::span<const uint8_t> { markAndOne });
        AOT_TEST_CHECK(!!scope);
        checkCopy(words, markAndOne);
        AOT_TEST_CHECK(*scope.start() == 0xef);
        AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF8);
    }
    AOT_TEST_CHECK(!words.sourceOwnerStackPointer);
}

static void testOpenTextAndBytes()
{
    static constexpr uint8_t markAndOne[] = { 0xef, 0xbb, 0xbf, '1' };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    AOT_TEST_CHECK(runtime.open(StringView("[1]"_s), stackPointerAtDepth(1)));
    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(1));
    checkCopy(words, bytesOf("[1]"_s));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    AOT_TEST_CHECK(!runtime.wideText());
    AOT_TEST_CHECK(runtime.close(words.sourceEnd));

    String text = text16(u"[\"日\"]");
    unsigned before = text.impl()->refCount();
    AOT_TEST_CHECK(runtime.open(StringView(text).substring(1, 3), stackPointerAtDepth(1)));
    AOT_TEST_CHECK(sourceCopy(words).size() == 3);
    AOT_TEST_CHECK(words.sourceStart[1] == saturatedJSONCharacter);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF16);
    AOT_TEST_CHECK(original16BitCharacters(words, sourceCopy(words)).data() == text.span16().data() + 1);
    AOT_TEST_CHECK(text.impl()->refCount() == before);

    AOT_TEST_CHECK(runtime.open(std::span<const uint8_t> { markAndOne }, stackPointerAtDepth(2)));
    AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 1);
    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(2));
    checkCopy(words, markAndOne);
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::UTF8);
    AOT_TEST_CHECK(!runtime.wideText());
    runtime.abandon();

    AOT_TEST_CHECK(words.sourceOwnerStackPointer == stackPointerAtDepth(1));
    AOT_TEST_CHECK(original16BitCharacters(words, sourceCopy(words)).data() == text.span16().data() + 1);
    runtime.abandon();
    AOT_TEST_CHECK(!runtime.wideText());

    AOT_TEST_CHECK(runtime.open(bytesOf("{}"_s), stackPointerAtDepth(1)));
    checkCopy(words, bytesOf("{}"_s));
    AOT_TEST_CHECK(words.sourceEncoding == JSONSourceEncoding::Latin1);
    runtime.abandon();

    AOT_TEST_CHECK(runtime.open(std::span<const uint8_t> { }, stackPointerAtDepth(1)));
    AOT_TEST_CHECK(words.sourceStart && words.sourceStart == words.sourceEnd);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    runtime.abandon();

    AOT_TEST_CHECK(runtime.open(StringView(), stackPointerAtDepth(1)));
    AOT_TEST_CHECK(words.sourceStart && words.sourceStart == words.sourceEnd);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    AOT_TEST_CHECK(runtime.close(words.sourceStart));
}

static void checkOnlyOuterListRemains(JSONKeyOrderLists& lists, std::span<const uint16_t> outerEntries)
{
    AOT_TEST_CHECK(lists.size() == 1);
    AOT_TEST_CHECK(equalSpans(lists.entries(0), outerEntries));
}

static void testKeyOrderListsEndWithTheirSource()
{
    static constexpr uint16_t outerEntries[] = { 3, 1, 2 };
    static constexpr uint16_t innerEntries[] = { 7 };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();
    JSONKeyOrderLists& lists = runtime.keyOrderLists();

    AOT_TEST_CHECK(openSource(fixture, 1, 1));
    AOT_TEST_CHECK(!lists.size());
    AOT_TEST_CHECK(!lists.add(outerEntries));

    AOT_TEST_CHECK(openSource(fixture, 2, 2));
    checkOnlyOuterListRemains(lists, outerEntries);
    AOT_TEST_CHECK(lists.add(innerEntries) == 1);
    AOT_TEST_CHECK(lists.add(innerEntries) == 2);
    lists.append(2, 9);
    AOT_TEST_CHECK(lists.entries(2).size() == 2);
    runtime.abandon();
    checkOnlyOuterListRemains(lists, outerEntries);

    AOT_TEST_CHECK(openSource(fixture, 2, 2));
    AOT_TEST_CHECK(lists.add(innerEntries) == 1);
    AOT_TEST_CHECK(runtime.close(words.sourceEnd));
    checkOnlyOuterListRemains(lists, outerEntries);

    AOT_TEST_CHECK(openSource(fixture, 2, 2));
    AOT_TEST_CHECK(lists.add(innerEntries) == 1);
    AOT_TEST_CHECK(openSource(fixture, 3, 3));
    AOT_TEST_CHECK(lists.add(innerEntries) == 2);
    words.didUnwindTo(stackPointerAtDepth(1));
    checkOpenSource(fixture, 1, 1);
    checkOnlyOuterListRemains(lists, outerEntries);

    AOT_TEST_CHECK(openSource(fixture, 2, 2));
    AOT_TEST_CHECK(lists.add(innerEntries) == 1);
    AOT_TEST_CHECK(openSource(fixture, 3, 2));
    AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 1);
    checkOnlyOuterListRemains(lists, outerEntries);
    runtime.abandon();

    AOT_TEST_CHECK(runtime.close(words.sourceEnd));
    AOT_TEST_CHECK(!lists.size());
}

static void testSourcesEndedInline()
{
    static constexpr uint16_t entries[] = { 1 };

    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();
    AOT_TEST_CHECK(!words.canEndSourceInline);

    for (TextEncoding encoding : { TextEncoding::Latin1, TextEncoding::UTF8 }) {
        fixture.open("[1]"_s, encoding);
        AOT_TEST_CHECK(words.canEndSourceInline);
        AOT_TEST_CHECK(!runtime.wideText() && !runtime.numberOfSuspendedSources());
        runtime.abandon();
        AOT_TEST_CHECK(!words.canEndSourceInline);
    }

    fixture.open(text16(u"[1]"), TextEncoding::UTF16);
    AOT_TEST_CHECK(runtime.wideText());
    AOT_TEST_CHECK(!words.canEndSourceInline);
    runtime.abandon();

    const uint8_t* outerCopy = fixture.open("[1]"_s, TextEncoding::Latin1, 1);
    fixture.open("[2]"_s, TextEncoding::Latin1, 2);
    AOT_TEST_CHECK(runtime.numberOfSuspendedSources() == 1);
    AOT_TEST_CHECK(!words.canEndSourceInline);
    AOT_TEST_CHECK(runtime.close(words.sourceEnd));
    AOT_TEST_CHECK(words.sourceStart == outerCopy);
    AOT_TEST_CHECK(words.canEndSourceInline);

    runtime.keyOrderLists().add(entries);
    words.canEndSourceInline = false;
    words.sourceOwnerStackPointer = nullptr;
    AOT_TEST_CHECK(!runtime.close(words.sourceEnd));
    AOT_TEST_CHECK(fixture.open("[3]"_s, TextEncoding::Latin1) == outerCopy);
    AOT_TEST_CHECK(!runtime.numberOfSuspendedSources());
    AOT_TEST_CHECK(!runtime.keyOrderLists().size());
    AOT_TEST_CHECK(words.canEndSourceInline);
    AOT_TEST_CHECK(runtime.close(words.sourceEnd));
}

static NEVER_INLINE unsigned openNestedScopes(DetachedInstance& fixture, unsigned level)
{
    AOT_TEST_CHECK(level <= maxOpenSources(fixture) + 1);
    SourceText source = sourceTextAt(level);
    String text = source.encoding == TextEncoding::UTF16 ? source.text : to8Bit(source.text);
    JSONSourceScope scope(fixture.instance(), StringView(text));
    if (!scope)
        return level - 1;

    unsigned deepest = openNestedScopes(fixture, level + 1);

    JSONWords& words = fixture.words();
    Vector<uint8_t> copy = copyOf(SourceText { source.text, source.encoding == TextEncoding::UTF16 ? TextEncoding::UTF16 : TextEncoding::Latin1 });
    AOT_TEST_CHECK(scope.start() == words.sourceStart);
    checkCopy(words, copy.span());
    return deepest;
}

static void testNestedScopes()
{
    DetachedInstance fixture;
    AOT_TEST_CHECK(openNestedScopes(fixture, 1) == maxOpenSources(fixture));
    AOT_TEST_CHECK(!fixture.words().sourceOwnerStackPointer);
    AOT_TEST_CHECK(!fixture.runtime().numberOfSuspendedSources());
}

static void testStorageReuse()
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    const uint8_t* block = fixture.open(quotedText(500), TextEncoding::Latin1);
    AOT_TEST_CHECK(runtime.sourceCapacity() == minJSONSourceCapacity);
    runtime.abandon();
    AOT_TEST_CHECK(fixture.open("1"_s, TextEncoding::Latin1) == block);
    AOT_TEST_CHECK(hasZeroPadding(words.sourceEnd));
    runtime.abandon();

    fixture.open(quotedText(2000), TextEncoding::Latin1);
    AOT_TEST_CHECK(runtime.sourceCapacity() >= 2002 + jsonSourcePadding);
    runtime.abandon();

    AOT_TEST_CHECK(openSource(fixture, 1, 1));
    const uint8_t* outer = words.sourceStart;
    AOT_TEST_CHECK(openSource(fixture, 2, 2));
    AOT_TEST_CHECK(words.sourceStart != outer);
    runtime.abandon();
    AOT_TEST_CHECK(words.sourceStart == outer);
    checkOpenSource(fixture, 1, 1);
}

#if ASAN_ENABLED
static void checkPoisonedAfterPadding(const JSONWords& words)
{
    const uint8_t* endOfPadding = words.sourceEnd + jsonSourcePadding;
    AOT_TEST_CHECK(!__asan_address_is_poisoned(words.sourceStart));
    AOT_TEST_CHECK(!__asan_address_is_poisoned(endOfPadding - 1));
    AOT_TEST_CHECK(__asan_address_is_poisoned(endOfPadding));
}

static void testUnusedCapacityIsPoisoned()
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    const uint8_t* block = fixture.open(quotedText(500), TextEncoding::Latin1);
    checkPoisonedAfterPadding(words);
    const uint8_t* lastByteOfLongText = words.sourceEnd + jsonSourcePadding - 1;
    AOT_TEST_CHECK(__asan_address_is_poisoned(block + runtime.sourceCapacity() - 1));
    runtime.abandon();

    AOT_TEST_CHECK(fixture.open("1"_s, TextEncoding::Latin1) == block);
    checkPoisonedAfterPadding(words);
    AOT_TEST_CHECK(__asan_address_is_poisoned(lastByteOfLongText));
    runtime.abandon();

    AOT_TEST_CHECK(fixture.open(quotedText(500), TextEncoding::Latin1) == block);
    checkPoisonedAfterPadding(words);
    AOT_TEST_CHECK(!__asan_address_is_poisoned(lastByteOfLongText));
}
#endif

static void checkLargeSourceRetained(DetachedInstance& fixture, const uint8_t* block, size_t capacity)
{
    JSONWords& words = fixture.words();
    AOT_TEST_CHECK(words.sourceStart == block);
    AOT_TEST_CHECK(fixture.runtime().sourceCapacity() == capacity);
    AOT_TEST_CHECK(block[0] == '"');
    AOT_TEST_CHECK(words.sourceEnd[-1] == '"');
}

static void checkNoSourceRetained(DetachedInstance& fixture)
{
    AOT_TEST_CHECK(!fixture.words().sourceStart);
    AOT_TEST_CHECK(!fixture.words().sourceEnd);
    AOT_TEST_CHECK(!fixture.runtime().sourceCapacity());
}

static void checkCollectionReleasesLargeSource(const String& largeText)
{
    DetachedInstance fixture;
    JSONRuntime& runtime = fixture.runtime();

    const uint8_t* block = fixture.open(largeText, TextEncoding::Latin1);
    size_t capacity = runtime.sourceCapacity();
    AOT_TEST_CHECK(capacity > maxRetainedJSONSourceCapacity);

    runtime.clear();
    checkLargeSourceRetained(fixture, block, capacity);
    fixture.finalizeCaches(CollectionScope::Eden);
    checkLargeSourceRetained(fixture, block, capacity);
    fixture.finalizeCaches(CollectionScope::Full);
    checkLargeSourceRetained(fixture, block, capacity);

    runtime.abandon();
    checkLargeSourceRetained(fixture, block, capacity);
    fixture.finalizeCaches(CollectionScope::Eden);
    checkNoSourceRetained(fixture);

    fixture.open("1"_s, TextEncoding::Latin1);
    checkCopy(fixture.words(), bytesOf("1"_s));
}

static void checkDirectRelease(const String& largeText)
{
    DetachedInstance fixture;
    JSONWords& words = fixture.words();
    JSONRuntime& runtime = fixture.runtime();

    fixture.open(largeText, TextEncoding::Latin1);
    runtime.abandon();
    runtime.clear();
    checkNoSourceRetained(fixture);
    runtime.clear();
    checkNoSourceRetained(fixture);

    const uint8_t* block = fixture.open("1"_s, TextEncoding::Latin1);
    size_t capacity = runtime.sourceCapacity();
    runtime.abandon();
    runtime.clear();
    AOT_TEST_CHECK(words.sourceStart == block);
    AOT_TEST_CHECK(runtime.sourceCapacity() == capacity);
}

static void checkLargeSourceReusePolicy(const String& largeText)
{
    DetachedInstance fixture;
    JSONRuntime& runtime = fixture.runtime();

    const uint8_t* block = fixture.open(largeText, TextEncoding::Latin1);
    size_t capacity = runtime.sourceCapacity();
    runtime.abandon();

    AOT_TEST_CHECK(fixture.open(quotedText(70 * KB), TextEncoding::Latin1) == block);
    AOT_TEST_CHECK(runtime.sourceCapacity() == capacity);
    runtime.abandon();

    fixture.open("1"_s, TextEncoding::Latin1);
    AOT_TEST_CHECK(runtime.sourceCapacity() == minJSONSourceCapacity);
}

static void testLargeTextRelease()
{
    String largeText = quotedText(100 * KB);
    checkCollectionReleasesLargeSource(largeText);
    checkDirectRelease(largeText);
    checkLargeSourceReusePolicy(largeText);
}

static void testDestructionWhileOpen()
{
    String text = text16(u"\"日\"");
    {
        DetachedInstance fixture;
        AOT_TEST_CHECK(fixture.runtime().open(fixture.string(text), stackPointerAtDepth(1)));
        AOT_TEST_CHECK(openSource(fixture, 0, 2));
        AOT_TEST_CHECK(openSource(fixture, 1, 3));
    }
    AOT_TEST_CHECK(text.impl()->hasOneRef());
}

std::span<const NativeTest> jsonSourceTests()
{
    static constexpr NativeTest tests[] = {
        { "json-source/open-and-close"_s, testOpenAndClose },
        { "json-source/close-answers"_s, testCloseAnswers },
        { "json-source/encodings"_s, testEncodings },
        { "json-source/byte-order-mark"_s, testByteOrderMark },
        { "json-source/owns-16-bit-text"_s, testOwnsWideText },
        { "json-source/non-text-values"_s, testNonTextValues },
        { "json-source/nesting-limit"_s, testNestingLimit },
        { "json-source/dead-sources"_s, testDeadSources },
        { "json-source/unwinding"_s, testUnwinding },
        { "json-source/scope"_s, testScope },
        { "json-source/open-text-and-bytes"_s, testOpenTextAndBytes },
        { "json-source/key-order-lists-end-with-their-source"_s, testKeyOrderListsEndWithTheirSource },
        { "json-source/sources-ended-inline"_s, testSourcesEndedInline },
        { "json-source/nested-scopes"_s, testNestedScopes },
        { "json-source/storage-reuse"_s, testStorageReuse },
#if ASAN_ENABLED
        { "json-source/unused-capacity-is-poisoned"_s, testUnusedCapacityIsPoisoned },
#endif
        { "json-source/large-text-release"_s, testLargeTextRelease },
        { "json-source/destruction-while-open"_s, testDestructionWhileOpen },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
