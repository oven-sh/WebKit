/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTOperationCallScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTStringBuilderStorage.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "ErrorInstance.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "Options.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <array>
#include <wtf/ConcurrentPtrHashSet.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> stringBuilderStorageTests();

using AppendResult = StringBuilderStorage::AppendResult;

static constexpr ASCIILiteral collectorTest = "string-builder-storage/collector"_s;

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

namespace {

class Builder {
public:
    explicit Builder(LoadedProgram& program)
        : m_program(program)
        , m_storage(program.vm(), StringBuilderStorage::acquire(program.instance()))
    {
    }

    StringBuilderStorage* storage() { return m_storage.get(); }
    StringBuilderStorage* operator->() { return storage(); }

    const Latin1Character* characters() { return word<const Latin1Character*>(StringBuilderStorage::offsetOfCharacters()); }
    uint32_t position() { return word<uint32_t>(StringBuilderStorage::offsetOfPosition()); }
    uint32_t limit() { return word<uint32_t>(StringBuilderStorage::offsetOfLimit()); }
    uint32_t lengthOutsideBuffer() { return word<uint32_t>(StringBuilderStorage::offsetOfLengthOutsideBuffer()); }
    StringBuilderStorage* nextFree() { return word<StringBuilderStorage*>(StringBuilderStorage::offsetOfNextFree()); }

    void append(ASCIILiteral text) { AOT_TEST_CHECK(storage()->append(m_program.realm(), text.span8()) == AppendResult::Appended); }
    void append(std::span<const char16_t> text) { AOT_TEST_CHECK(storage()->append(m_program.realm(), text) == AppendResult::Appended); }
    void append(JSString* string) { AOT_TEST_CHECK(storage()->append(m_program.realm(), string) == AppendResult::Appended); }
    void append(JSValue primitive) { AOT_TEST_CHECK(storage()->append(m_program.realm(), primitive) == AppendResult::Appended); }
    void appendRepeated(Latin1Character character, size_t count)
    {
        Vector<Latin1Character> characters(FillWith { }, count, character);
        AOT_TEST_CHECK(storage()->append(m_program.realm(), characters.span()) == AppendResult::Appended);
    }

    JSString* toString()
    {
        JSString* result = storage()->toString(m_program.realm(), m_program.instance(), StringBuilderStorage::StringUse::Unknown);
        AOT_TEST_CHECK(result);
        return result;
    }
    JSString* finish()
    {
        JSString* result = storage()->finish(m_program.realm(), m_program.instance());
        AOT_TEST_CHECK(result);
        return result;
    }
    String text() { return toString()->value(m_program.realm()).data; }

private:
    template<typename T> T word(ptrdiff_t offset) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(storage()) + offset); }

    LoadedProgram& m_program;
    Strong<StringBuilderStorage> m_storage;
};

} // anonymous namespace

static constexpr char16_t japan[] = { 0x65e5, 0x672c };

static String textOf(LoadedProgram& program, JSString* string)
{
    return string->value(program.realm()).data;
}

static String repeated(char character, size_t count)
{
    Vector<Latin1Character> characters(FillWith { }, count, static_cast<Latin1Character>(character));
    return String(characters.span());
}

static const Latin1Character* charactersOf(JSString* string)
{
    const StringImpl* impl = string->tryGetValueImpl();
    AOT_TEST_CHECK(impl && impl->is8Bit());
    return impl->span8().data();
}

static bool takeException(LoadedProgram& program, const Invocable<bool(ErrorInstance&)> auto& isExpected)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    Exception* exception = scope.exception();
    if (!exception)
        return false;
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    bool result = error && isExpected(*error);
    scope.clearException();
    return result;
}

static bool takeOutOfMemoryError(LoadedProgram& program)
{
    return takeException(program, [](ErrorInstance& error) { return error.isOutOfMemoryError(); });
}

static NEVER_INLINE void clobberStack()
{
    volatile uint8_t bytes[16 * KB];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = 0;
}

static void testAppendLatin1(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Builder builder(program);
    WTF::StringBuilder expected;

    builder.append("plain span, "_s);
    expected.append("plain span, "_s);
    const Latin1Character accents[] = { 0xe9, 0xff, 0x80 };
    AOT_TEST_CHECK(builder->append(realm, std::span(accents)) == AppendResult::Appended);
    expected.append(std::span(accents));

    builder.append(jsString(vm, String("a string"_s)));
    expected.append("a string"_s);
    builder.append(jsEmptyString(vm));
    JSString* rope = jsString(realm, jsString(vm, String("left "_s)), jsString(vm, String("middle "_s)), jsString(vm, String("right"_s)));
    AOT_TEST_CHECK(rope->isRope());
    builder.append(rope);
    AOT_TEST_CHECK(rope->isRope());
    expected.append("left middle right"_s);
    JSString* substring = jsSubstring(realm, jsString(vm, String("take the middle of this"_s)), 9, 6);
    builder.append(substring);
    expected.append("middle"_s);

    const JSValue primitives[] = {
        jsNumber(0), jsNumber(-1), jsNumber(INT32_MAX), jsNumber(INT32_MIN), jsNumber(1.5), jsDoubleNumber(-0.0), jsDoubleNumber(PNaN),
        jsDoubleNumber(std::numeric_limits<double>::infinity()), jsDoubleNumber(-std::numeric_limits<double>::infinity()), jsNumber(1e21), jsNumber(1e-7), jsNumber(123456789012.0),
        jsBoolean(true), jsBoolean(false), jsUndefined(), jsNull(), program.realms().evaluate(program.realms().plain(), "-(2n ** 70n)"_s), jsString(vm, String("last"_s)),
    };
    for (JSValue primitive : primitives) {
        builder.append(primitive);
        expected.append(primitive.toWTFString(realm));
    }
    Vector<EncodedJSValue> encoded;
    for (JSValue primitive : primitives) {
        encoded.append(JSValue::encode(primitive));
        expected.append(primitive.toWTFString(realm));
    }
    AOT_TEST_CHECK(builder->append(realm, encoded.span()) == AppendResult::Appended);

    String expectedText = expected.toString();
    AOT_TEST_CHECK(expectedText.contains("0-12147483647-21474836481.50NaNInfinity-Infinity1e+211e-7123456789012truefalseundefinednull-1180591620717411303424last"_s));
    AOT_TEST_CHECK(builder->length() == expectedText.length() && !builder.lengthOutsideBuffer());
    AOT_TEST_CHECK(builder.text() == expectedText);
    JSString* result = builder.finish();
    AOT_TEST_CHECK(result->is8Bit() && !result->isRope() && textOf(program, result) == expectedText);
}

static NEVER_INLINE void readAndDropTheString(Builder& builder)
{
    AOT_TEST_CHECK(builder.toString()->tryGetValueImpl()->isSubString());
}

static void testSharersDie(LoadedProgram& program)
{
    Builder builder(program);
    builder.appendRepeated('a', 5000);
    const Latin1Character* buffer = builder.characters();
    readAndDropTheString(builder);
    builder->rollBack(100);
    clobberStack();
    program.vm().heap.collectNow(Sync, CollectionScope::Full);
    builder.append("other text"_s);
    AOT_TEST_CHECK(builder.characters() == buffer);
    AOT_TEST_CHECK(builder.text() == makeString(repeated('a', 100), "other text"_s));
    builder->release(program.instance());
}

static void testFinishGivesLargeBufferAway(LoadedProgram& program)
{
    uint32_t retained = StringBuilderStorage::maxRetainedCapacity;
    uint32_t large = roundUpToMultipleOf<sizeof(uint64_t)>(retained + 1);
    Builder builder(program);
    AOT_TEST_CHECK(builder->reserve(program.realm(), large));
    AOT_TEST_CHECK(builder.limit() == large);
    for (unsigned i = 0; i < 5; ++i)
        builder.appendRepeated('a', 40000);
    AOT_TEST_CHECK(builder.limit() == large);
    const Latin1Character* buffer = builder.characters();
    JSString* result = builder.finish();
    AOT_TEST_CHECK(charactersOf(result) == buffer && textOf(program, result) == repeated('a', 200000));
    AOT_TEST_CHECK(program.instance().stringBuilder.freeStorage == builder.storage());
    AOT_TEST_CHECK(builder.characters() != buffer && !builder->length());

    Builder again(program);
    AOT_TEST_CHECK(again.storage() == builder.storage());
    again.append("next"_s);
    AOT_TEST_CHECK(again.text() == "next"_s && textOf(program, result) == repeated('a', 200000));
    again->release(program.instance());

    Builder full(program);
    AOT_TEST_CHECK(full->reserve(program.realm(), large));
    full.appendRepeated('b', large);
    const Latin1Character* fullBuffer = full.characters();
    JSString* whole = full.finish();
    AOT_TEST_CHECK(charactersOf(whole) == fullBuffer && !whole->tryGetValueImpl()->isSubString() && whole->length() == large);
}

static void testNextLargeTextGrowsOnce(LoadedProgram& program)
{
    uint32_t least = StringBuilderStorage::minCapacity;
    uint32_t retained = StringBuilderStorage::maxRetainedCapacity;
    uint32_t large = retained * 4;
    auto write = [&](Builder& builder, uint32_t length) {
        for (uint32_t written = 0; written < length; written += 1000)
            builder.appendRepeated('a', 1000);
    };

    Builder first(program);
    write(first, large - retained);
    AOT_TEST_CHECK(first.limit() == large);
    first.finish();

    Builder shortText(program);
    shortText.append("short"_s);
    AOT_TEST_CHECK(shortText.limit() == least);
    shortText.finish();

    Builder second(program);
    second.appendRepeated('a', least);
    AOT_TEST_CHECK(second.limit() == least);
    second.append("a"_s);
    AOT_TEST_CHECK(second.limit() == large);
    const Latin1Character* buffer = second.characters();
    write(second, large - retained);
    AOT_TEST_CHECK(second.characters() == buffer && charactersOf(second.finish()) == buffer);

    program.instance().stringBuilder.freeStorage = nullptr;
    Builder half(program);
    AOT_TEST_CHECK(half.storage() != second.storage());
    write(half, retained + 1000);
    AOT_TEST_CHECK(half.limit() == large);
    buffer = half.characters();
    JSString* result = half.finish();
    AOT_TEST_CHECK(charactersOf(result) != buffer && !result->tryGetValueImpl()->isSubString());

    Builder fitting(program);
    write(fitting, retained + 1000);
    AOT_TEST_CHECK(fitting.limit() == retained * 2);
    buffer = fitting.characters();
    AOT_TEST_CHECK(charactersOf(fitting.finish()) == buffer);

    Builder shorter(program);
    write(shorter, retained / 4 + 1000);
    AOT_TEST_CHECK(shorter.limit() == retained * 2);
    buffer = shorter.characters();
    AOT_TEST_CHECK(charactersOf(shorter.finish()) != buffer);
    AOT_TEST_CHECK(!shorter.limit() && !shorter->length());

    Builder kept(program);
    AOT_TEST_CHECK(kept.storage() == shorter.storage());
    kept.append("a"_s);
    AOT_TEST_CHECK(kept.limit() == retained / 2);
    buffer = kept.characters();
    write(kept, retained / 4 + 1000);
    AOT_TEST_CHECK(kept.characters() == buffer);
    kept.finish();
    AOT_TEST_CHECK(kept.characters() == buffer);

    Builder wide(program);
    write(wide, large - retained);
    wide.append(std::span(japan));
    AOT_TEST_CHECK(wide.limit() == large);
    wide.finish();
    Builder afterWide(program);
    write(afterWide, least + 1000);
    AOT_TEST_CHECK(afterWide.limit() == large);
    afterWide->release(program.instance());
}

static void testDiscardLeavesNextBufferToNextWrite(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t retained = StringBuilderStorage::maxRetainedCapacity;
    uint32_t large = retained * 4;
    auto write = [&](Builder& builder, uint32_t length) {
        for (uint32_t written = 0; written < length; written += 1000)
            builder.appendRepeated('a', 1000);
    };
    auto discardWhileCollectionIsAskedFor = [&](Builder& builder) {
        AOT_TEST_CHECK(!vm.heap.isDeferred());
        vm.heap.collectAsync(CollectionScope::Full);
        HeapVersion version = vm.heap.objectSpace().markingVersion();
        {
            AssertNoGC assertNoGC;
            operationAOTStringBuilderDiscard(&program.instance(), builder.storage());
        }
        AOT_TEST_CHECK(vm.heap.objectSpace().markingVersion() == version);
        vm.heap.collectSync(CollectionScope::Full);
        AOT_TEST_CHECK(vm.heap.objectSpace().markingVersion() != version);
    };
    auto rememberLargeBuffer = [&] {
        Builder builder(program);
        write(builder, large - retained);
        AOT_TEST_CHECK(builder.limit() == large);
        builder.finish();
    };

    rememberLargeBuffer();
    Builder discarded(program);
    write(discarded, retained / 4 + 1000);
    AOT_TEST_CHECK(discarded.limit() == large);
    discardWhileCollectionIsAskedFor(discarded);
    AOT_TEST_CHECK(!discarded.limit() && !discarded.characters() && !discarded->length());
    discarded.append("a"_s);
    AOT_TEST_CHECK(discarded.limit() == retained / 2);
    discarded->release(program.instance());
}

static String fillWithPiecesAndSegments(LoadedProgram& program, Builder& builder, Vector<uint32_t>& lengths)
{
    VM& vm = program.vm();
    static constexpr char16_t wide[] = { 0x6f22, 0x5b57, 0x3042 };
    WTF::StringBuilder expected;
    auto remember = [&] {
        lengths.append(builder->length());
    };
    builder.append("abc"_s);
    expected.append("abc"_s);
    remember();
    builder.append(std::span(japan));
    expected.append(std::span(japan));
    remember();
    builder.append(jsString(vm, String(std::span(wide))));
    expected.append(std::span(wide));
    remember();
    builder.append("de"_s);
    expected.append("de"_s);
    remember();
    builder.append(jsNumber(-17));
    expected.append("-17"_s);
    remember();
    builder.append(std::span(wide));
    expected.append(std::span(wide));
    remember();
    builder.append("f"_s);
    expected.append('f');
    remember();
    return expected.toString();
}

static void testRollBackEverywhere(LoadedProgram& program)
{
    for (bool collapsesFirst : { false, true }) {
        uint32_t total;
        {
            Builder builder(program);
            Vector<uint32_t> lengths;
            total = fillWithPiecesAndSegments(program, builder, lengths).length();
            AOT_TEST_CHECK(lengths == Vector<uint32_t>({ 3, 5, 8, 10, 13, 16, 17 }) && total == 17);
            builder->release(program.instance());
        }
        for (uint32_t length = 0; length <= total; ++length) {
            Builder builder(program);
            Vector<uint32_t> lengths;
            String expected = fillWithPiecesAndSegments(program, builder, lengths);
            if (collapsesFirst)
                AOT_TEST_CHECK(builder.text() == expected);
            builder->rollBack(length);
            AOT_TEST_CHECK(builder->length() == length && builder.position() + builder.lengthOutsideBuffer() == length);
            AOT_TEST_CHECK(builder.text() == expected.left(length));
            builder.append("+"_s);
            builder.append(std::span(japan));
            AOT_TEST_CHECK(builder.text() == makeString(expected.left(length), '+', String(std::span(japan))));
            builder->rollBack(length);
            AOT_TEST_CHECK(builder.text() == expected.left(length));
            builder->rollBack(length / 2);
            AOT_TEST_CHECK(textOf(program, builder.finish()) == expected.left(length / 2));
        }
    }
}

namespace {

class BuilderInFrame {
public:
    explicit BuilderInFrame(LoadedProgram& program)
        : m_program(program)
    {
        m_record.savedCursor = m_record.buffer;
        m_record.limit = m_record.buffer + StackStringBuilder::capacity;
        m_record.lengthOrigin = m_record.buffer;
        m_cursor = m_record.buffer;
    }

    void write(char character)
    {
        if (m_cursor >= m_record.limit)
            takeCursorBehind([](StringBuilderStorage&) { });
        *m_cursor++ = static_cast<Latin1Character>(character);
        m_expected.append(character);
    }
    void append(std::span<const char16_t> wide)
    {
        takeCursorBehind([&](StringBuilderStorage& storage) {
            AOT_TEST_CHECK(storage.append(m_program.realm(), jsString(m_program.vm(), String(wide))) == AppendResult::Appended);
        });
        m_expected.append(wide);
    }
    JSString* toString()
    {
        JSString* result = m_record.toString(m_program.realm(), m_program.instance(), m_cursor);
        AOT_TEST_CHECK(result && static_cast<size_t>(m_cursor - m_record.lengthOrigin) == result->length());
        return result;
    }
    JSString* finish() { return m_record.finish(m_program.realm(), m_program.instance(), m_cursor, StringBuilderStorage::StringUse::Unknown); }
    StringBuilderStorage& moveToStorage()
    {
        StringBuilderStorage* storage = m_record.moveToStorage(m_program.realm(), m_program.instance(), m_cursor);
        AOT_TEST_CHECK(storage);
        return *storage;
    }
    String expected() { return m_expected.toString(); }

private:
    void takeCursorBehind(const Invocable<void(StringBuilderStorage&)> auto& change)
    {
        StringBuilderStorage& storage = moveToStorage();
        change(storage);
        m_cursor = m_record.didChangeStorage(m_program.realm(), storage);
        AOT_TEST_CHECK(m_cursor);
    }

    LoadedProgram& m_program;
    StackStringBuilder m_record;
    Latin1Character* m_cursor;
    WTF::StringBuilder m_expected;
};

} // anonymous namespace

static void testTextInFrameIsReadAtEachTurn(LoadedProgram& program)
{
    for (unsigned turns : { 40u, 9000u }) {
        BuilderInFrame builder(program);
        Vector<std::pair<JSString*, String>> read;
        MarkedArgumentBuffer kept;
        for (unsigned turn = 0; turn < turns; ++turn) {
            if (turn % 3 == 1)
                builder.append(std::span(japan));
            else
                builder.write('a' + turn % 26);
            JSString* text = builder.toString();
            AOT_TEST_CHECK(turn >= 40 || !text->isRope());
            AOT_TEST_CHECK(textOf(program, text) == builder.expected());
            AOT_TEST_CHECK(builder.toString() == text);
            if (turn && turn < 40) {
                kept.append(text);
                read.append({ text, builder.expected() });
            }
        }
        for (unsigned part = 0; part < StringBuilderStorage::maxSegments + 8; ++part) {
            builder.append(std::span(japan));
            builder.write('-');
        }
        AOT_TEST_CHECK(textOf(program, builder.toString()) == builder.expected());
        JSString* whole = builder.finish();
        AOT_TEST_CHECK(whole && textOf(program, whole) == builder.expected());
        for (auto& [text, expected] : read)
            AOT_TEST_CHECK(textOf(program, text) == expected);
    }
}

static void testRollBackBehindReadOfTextInFrame(LoadedProgram& program)
{
    auto fill = [](BuilderInFrame& builder) {
        for (char character : { 'a', 'b', 'c' })
            builder.write(character);
        builder.append(std::span(japan));
        builder.write('d');
        builder.toString();
        builder.write('e');
        builder.append(std::span(japan));
        builder.write('f');
        builder.toString();
        builder.write('g');
        builder.write('h');
    };
    for (uint32_t length = 0; length <= 12; ++length) {
        BuilderInFrame builder(program);
        fill(builder);
        String expected = builder.expected();
        AOT_TEST_CHECK(expected.length() == 12);
        StringBuilderStorage& storage = builder.moveToStorage();
        storage.rollBack(length);
        AOT_TEST_CHECK(storage.length() == length);
        AOT_TEST_CHECK(storage.append(program.realm(), "+"_s.span8()) == AppendResult::Appended);
        AOT_TEST_CHECK(storage.append(program.realm(), std::span(japan)) == AppendResult::Appended);
        JSString* result = storage.finish(program.realm(), program.instance());
        AOT_TEST_CHECK(result && textOf(program, result) == makeString(expected.left(length), '+', String(std::span(japan))));
    }
}

static void testAppendValuesIsAtomic(LoadedProgram& program)
{
    VM& vm = program.vm();
    Builder builder(program);
    builder.append("kept"_s);
    builder.append(std::span(japan));
    const EncodedJSValue primitives[] = { JSValue::encode(jsNumber(1)), JSValue::encode(jsString(vm, String(std::span(japan)))), JSValue::encode(Symbol::create(vm)) };
    AOT_TEST_CHECK(builder->append(program.realm(), std::span(primitives)) == AppendResult::Failed);
    AOT_TEST_CHECK(takeException(program, [](ErrorInstance& error) { return error.errorType() == ErrorType::TypeError; }));
    AOT_TEST_CHECK(builder->length() == 6 && builder.text() == makeString("kept"_s, String(std::span(japan))));

    AOT_TEST_CHECK(builder->append(program.realm(), JSValue(Symbol::create(vm))) == AppendResult::Failed);
    AOT_TEST_CHECK(takeException(program, [](ErrorInstance& error) { return error.errorType() == ErrorType::TypeError; }));
    AOT_TEST_CHECK(builder->length() == 6);

    AOT_TEST_CHECK(builder->append(program.realm(), std::span(primitives).first(2)) == AppendResult::Appended);
    AOT_TEST_CHECK(builder.text() == makeString("kept"_s, String(std::span(japan)), '1', String(std::span(japan))));
    builder->release(program.instance());
}

static void testRoomThatMayStayUnused(LoadedProgram& program)
{
    VM& vm = program.vm();
    constexpr uint32_t left = 100;
    auto fillUpTo = [&](Builder& builder) {
        JSString* piece = jsString(vm, repeated('x', 1u << 16));
        for (unsigned bit = 16; bit < 30; ++bit) {
            builder.append(piece);
            piece = jsString(program.realm(), piece, piece);
        }
        builder.append(piece);
        builder.appendRepeated('y', (1u << 16) - 1 - left);
        AOT_TEST_CHECK(builder->length() == String::MaxLength - left && builder.limit() - builder.position() <= left);
    };
    auto writeBehind = [&](Builder& builder, uint32_t length) {
        Latin1Character* cursor = builder->reserve(program.realm(), { .length = 0, .maxLengthOfRest = maxStringBuilderReservation });
        AOT_TEST_CHECK(cursor == builder.characters() + builder.position() && builder.limit() - builder.position() >= maxStringBuilderReservation);
        memset(cursor, 'z', length);
        builder->commit(cursor + length);
    };

    Builder exact(program);
    fillUpTo(exact);
    AOT_TEST_CHECK(!exact->reserve(program.realm(), left + 1));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(!exact->reserve(program.realm(), { .length = left + 1, .maxLengthOfRest = 0 }));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    writeBehind(exact, left);
    AOT_TEST_CHECK(exact->length() == String::MaxLength);
    AOT_TEST_CHECK(exact.finish()->length() == String::MaxLength);

    Builder tooLong(program);
    fillUpTo(tooLong);
    uint32_t lengthBefore = tooLong->length();
    writeBehind(tooLong, left + 1);
    AOT_TEST_CHECK(tooLong->length() == String::MaxLength + 1);
    AOT_TEST_CHECK(!tooLong->toString(program.realm(), program.instance(), StringBuilderStorage::StringUse::Unknown));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(!tooLong->reserve(program.realm(), { .length = 0, .maxLengthOfRest = 2 * maxStringBuilderReservation }));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(tooLong->append(program.realm(), jsString(vm, repeated('w', 1u << 16))) == AppendResult::Failed);
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    tooLong->rollBack(lengthBefore);
    AOT_TEST_CHECK(tooLong->length() == lengthBefore && tooLong.limit() - tooLong.position() == left);
    AOT_TEST_CHECK(!tooLong->reserve(program.realm(), left + 1));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    writeBehind(tooLong, 1);
    AOT_TEST_CHECK(tooLong.finish()->length() == lengthBefore + 1);
}

static void testPoolDepth(LoadedProgram& program)
{
    Instance& instance = program.instance();
    uint32_t maxPooled = StringBuilderStorage::maxPooledBuilders;
    AOT_TEST_CHECK(!instance.stringBuilder.freeStorage);

    Vector<std::unique_ptr<Builder>> builders;
    for (uint32_t i = 0; i <= maxPooled; ++i) {
        builders.append(makeUniqueWithoutFastMallocCheck<Builder>(program));
        builders.last()->append("content"_s);
        for (uint32_t earlier = 0; earlier < i; ++earlier)
            AOT_TEST_CHECK(builders[earlier]->storage() != builders[i]->storage());
    }
    for (uint32_t i = 0; i < maxPooled; ++i) {
        (*builders[i])->release(instance);
        AOT_TEST_CHECK(instance.stringBuilder.freeStorage == builders[i]->storage() && !(*builders[i])->length());
        AOT_TEST_CHECK(builders[i]->nextFree() == (i ? builders[i - 1]->storage() : nullptr));
    }

    (*builders[maxPooled])->release(instance);
    AOT_TEST_CHECK(instance.stringBuilder.freeStorage == builders[maxPooled - 1]->storage());
    AOT_TEST_CHECK(!builders[maxPooled]->characters() && !(*builders[maxPooled])->length());

    for (uint32_t i = maxPooled; i--;)
        AOT_TEST_CHECK(StringBuilderStorage::acquire(instance) == builders[i]->storage());
    AOT_TEST_CHECK(!instance.stringBuilder.freeStorage);
    StringBuilderStorage* fresh = StringBuilderStorage::acquire(instance);
    for (auto& builder : builders)
        AOT_TEST_CHECK(fresh != builder->storage());
}

static NEVER_INLINE String fillWithStringsNobodyElseHolds(LoadedProgram& program, Builder& builder)
{
    VM& vm = program.vm();
    WTF::StringBuilder expected;
    for (unsigned i = 0; i < 50; ++i) {
        String wide = makeString(String(std::span(japan)), i);
        builder.append(jsString(vm, wide));
        builder.append("|"_s);
        expected.append(wide, '|');
    }
    String large = repeated('L', 70000);
    builder.append(jsString(vm, large));
    expected.append(large);
    return expected.toString();
}

static void checkCollector(LoadedProgram& program)
{
    AOT_TEST_CHECK(Options::verifyGC());
    VM& vm = program.vm();
    Builder builder(program);
    String expected = fillWithStringsNobodyElseHolds(program, builder);
    clobberStack();
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden }) {
        vm.heap.collectNow(Sync, scope);
        for (unsigned i = 0; i < 2000; ++i)
            jsString(vm, makeString("garbage"_s, i));
    }
    AOT_TEST_CHECK(builder->length() == expected.length());
    builder.append(jsString(vm, String(std::span(japan))));
    vm.heap.collectNow(Sync, CollectionScope::Eden);
    AOT_TEST_CHECK(builder.text() == makeString(expected, String(std::span(japan))));
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(textOf(program, builder.finish()) == makeString(expected, String(std::span(japan))));
}

static void testCollector()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(collectorTest, programText, checkCollector);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(collectorTest), makeString("--aotImagePath="_s, image.path()), "--verifyGC=1"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that verifies collections ", result.description());
        failNativeTest(__FILE__, __LINE__, collectorTest.characters());
    }
}

template<LoadedProgramTest body>
static constexpr void (*withInstance)() = runNativeTestWithLoadedProgram<stringBuilderStorageTests, programText, body>;

std::span<const NativeTest> stringBuilderStorageTests()
{
    static constexpr NativeTest tests[] = {
        { "string-builder-storage/append-latin1"_s, withInstance<testAppendLatin1> },
        { "string-builder-storage/sharers-die"_s, withInstance<testSharersDie> },
        { "string-builder-storage/finish-gives-large-buffer-away"_s, withInstance<testFinishGivesLargeBufferAway> },
        { "string-builder-storage/next-large-text-grows-once"_s, withInstance<testNextLargeTextGrowsOnce> },
        { "string-builder-storage/roll-back-everywhere"_s, withInstance<testRollBackEverywhere> },
        { "string-builder-storage/text-in-frame-is-read-at-each-turn"_s, withInstance<testTextInFrameIsReadAtEachTurn> },
        { "string-builder-storage/roll-back-behind-read-of-text-in-frame"_s, withInstance<testRollBackBehindReadOfTextInFrame> },
        { "string-builder-storage/append-values-is-atomic"_s, withInstance<testAppendValuesIsAtomic> },
        { "string-builder-storage/room-that-may-stay-unused"_s, withInstance<testRoomThatMayStayUnused> },
        { "string-builder-storage/pool-depth"_s, withInstance<testPoolDepth> },
        { "string-builder-storage/discard-leaves-next-buffer-to-next-write"_s, withInstance<testDiscardLeavesNextBufferToNextWrite> },
        { collectorTest, testCollector },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
