/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTLoadedProgramTestSupport.h"
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

class RecordingVisitor final : public AbstractSlotVisitor {
public:
    explicit RecordingVisitor(VM& vm)
        : AbstractSlotVisitor(vm.heap.collector(), "Recording"_s, m_opaqueRootStorage)
    {
    }

    const Vector<JSCell*>& cells() const { return m_cells; }

    void appendUnbarriered(JSCell* cell) final
    {
        if (cell)
            m_cells.append(cell);
    }

    void append(const ConservativeRoots&) final { RELEASE_ASSERT_NOT_REACHED(); }
    void appendHiddenUnbarriered(JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void markAuxiliary(const void*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void visitAsConstraint(const JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }

    bool isFirstVisit() const final { return true; }
    bool isMarked(const void*) const final { return false; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return false; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return false; }
    bool mutatorsAreStopped() const final { return true; }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    void didRace(const VisitRaceKey&) final { }

private:
    ConcurrentPtrHashSet m_opaqueRootStorage;
    Vector<JSCell*> m_cells;
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

static void collectParts(VM& vm, JSString* string, Vector<JSString*>& parts)
{
    if (!string->isRope() || string->isSubstring()) {
        parts.append(string);
        return;
    }
    RecordingVisitor visitor(vm);
    AbstractSlotVisitor& abstractVisitor = visitor;
    string->methodTable()->visitChildren(string, abstractVisitor);
    for (JSCell* cell : visitor.cells()) {
        if (auto* part = dynamicDowncast<JSString>(cell))
            collectParts(vm, part, parts);
    }
}

static Vector<JSString*> partsOf(LoadedProgram& program, JSString* string)
{
    Vector<JSString*> parts;
    collectParts(program.vm(), string, parts);
    return parts;
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

static void testLimitIsRoom(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t byReference = StringBuilderStorage::minLengthAppendedByReference;
    {
        Builder builder(program);
        AOT_TEST_CHECK(builder.limit() > builder.position());
        builder.append("ab"_s);
        builder.append(std::span(japan));
        AOT_TEST_CHECK(builder.limit() > builder.position() && builder.lengthOutsideBuffer() == 2);
        builder->release(program.instance());
    }
    {
        Builder builder(program);
        builder.append("ab"_s);
        builder.append(jsString(vm, repeated('x', byReference)));
        AOT_TEST_CHECK(builder.limit() > builder.position() && builder.lengthOutsideBuffer() == byReference && builder.position() == 2);
        builder->release(program.instance());
    }
    {
        Builder builder(program);
        builder.append(jsString(vm, repeated('x', 3 * byReference)));
        AOT_TEST_CHECK(builder.limit() > builder.position() && !builder.position());
        builder.append("tail"_s);
        AOT_TEST_CHECK(builder.position() == 4 && builder->length() == 3 * byReference + 4);
        builder->release(program.instance());
    }
    {
        Builder builder(program);
        builder.append("ab"_s);
        builder.append(jsString(vm, repeated('x', byReference - 1)));
        AOT_TEST_CHECK(builder.position() == byReference + 1 && !builder.lengthOutsideBuffer() && builder.limit() >= builder.position());
        builder->release(program.instance());
    }
}

static void testLengthIsTwoWords(LoadedProgram& program)
{
    VM& vm = program.vm();
    Builder builder(program);
    uint32_t expected = 0;
    auto check = [&](uint32_t added) {
        expected += added;
        AOT_TEST_CHECK(builder->length() == expected && builder.position() + builder.lengthOutsideBuffer() == expected);
        AOT_TEST_CHECK(builder.position() <= builder.limit());
    };
    check(0);
    builder.append("abc"_s);
    check(3);
    builder.append(std::span(japan));
    check(2);
    builder.append(jsNumber(12345));
    check(5);
    builder.append(jsString(vm, repeated('y', 70000)));
    check(70000);
    builder.append(jsString(vm, String(std::span(japan))));
    check(2);
    builder.appendRepeated('z', 10000);
    check(10000);
    builder.toString();
    check(0);
    builder.append("d"_s);
    check(1);
    builder->rollBack(4);
    expected = 4;
    check(0);
    builder->rollBack(0);
    expected = 0;
    check(0);
    builder->release(program.instance());
}

static void testFirstStringComesBack(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSString* const strings[] = { jsString(vm, String("short"_s)), jsString(vm, repeated('x', 5000)), jsString(vm, repeated('x', 70000)), jsString(vm, String(std::span(japan))) };
    for (JSString* string : strings) {
        {
            Builder builder(program);
            builder.append(string);
            AOT_TEST_CHECK(builder.toString() == string);
            AOT_TEST_CHECK(builder.finish() == string);
        }
        {
            Builder builder(program);
            builder.append(string);
            builder.append("!"_s);
            JSString* result = builder.finish();
            AOT_TEST_CHECK(result != string && result->length() == string->length() + 1);
        }
        {
            Builder builder(program);
            builder.append("!"_s);
            builder.append(string);
            AOT_TEST_CHECK(builder.finish() != string);
        }
        {
            Builder builder(program);
            builder.append(string);
            builder->rollBack(string->length() - 1);
            JSString* result = builder.finish();
            AOT_TEST_CHECK(result != string && result->length() == string->length() - 1);
        }
    }
}

static void testShareRead(LoadedProgram& program)
{
    Builder builder(program);
    builder.appendRepeated('a', 5000);
    JSString* read = builder.toString();
    const StringImpl* impl = read->tryGetValueImpl();
    AOT_TEST_CHECK(impl && impl->isSubString() && impl->length() == 5000);
    AOT_TEST_CHECK(charactersOf(read) == builder.characters());

    const Latin1Character* buffer = builder.characters();
    builder.append("more"_s);
    AOT_TEST_CHECK(builder.characters() == buffer);
    AOT_TEST_CHECK(textOf(program, read) == repeated('a', 5000));
    AOT_TEST_CHECK(builder.text() == makeString(repeated('a', 5000), "more"_s));
    builder->release(program.instance());
}

static void testShareThenRollBack(LoadedProgram& program)
{
    Builder builder(program);
    builder.appendRepeated('a', 5000);
    JSString* read = builder.toString();
    const Latin1Character* buffer = builder.characters();
    AOT_TEST_CHECK(charactersOf(read) == buffer);

    builder->rollBack(100);
    AOT_TEST_CHECK(builder->length() == 100 && builder.limit() == builder.position());
    builder.append("other text"_s);
    AOT_TEST_CHECK(builder.characters() != buffer);
    AOT_TEST_CHECK(textOf(program, read) == repeated('a', 5000));
    AOT_TEST_CHECK(builder.text() == makeString(repeated('a', 100), "other text"_s));
    ensureStillAliveHere(read);
    builder->release(program.instance());
}

static void testShareThenRelease(LoadedProgram& program)
{
    Builder builder(program);
    builder.appendRepeated('a', 5000);
    JSString* read = builder.toString();
    const Latin1Character* buffer = builder.characters();
    builder->release(program.instance());

    Builder again(program);
    AOT_TEST_CHECK(again.storage() == builder.storage() && !again->length());
    again.appendRepeated('b', 5000);
    AOT_TEST_CHECK(again.characters() != buffer);
    AOT_TEST_CHECK(textOf(program, read) == repeated('a', 5000));
    AOT_TEST_CHECK(again.text() == repeated('b', 5000));
    ensureStillAliveHere(read);
    again->release(program.instance());
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

static void testBuilderKeepsTheStringItReadLast(LoadedProgram& program)
{
    Builder builder(program);
    builder.appendRepeated('a', 5000);
    const Latin1Character* buffer = builder.characters();
    readAndDropTheString(builder);
    clobberStack();
    program.vm().heap.collectNow(Sync, CollectionScope::Full);
    JSString* read = builder.toString();
    AOT_TEST_CHECK(charactersOf(read) == buffer && read->length() == 5000);
    builder->rollBack(100);
    builder.append("other text"_s);
    AOT_TEST_CHECK(builder.characters() != buffer && textOf(program, read) == repeated('a', 5000));
    builder->release(program.instance());
}

static void testShortReadCopies(LoadedProgram& program)
{
    uint32_t adopted = StringBuilderStorage::minLengthAdoptedByFinish;
    for (uint32_t length : { 2u, 100u, adopted - 1 }) {
        Builder builder(program);
        builder.appendRepeated('a', length);
        JSString* read = builder.toString();
        AOT_TEST_CHECK(!read->tryGetValueImpl()->isSubString() && charactersOf(read) != builder.characters());
        const Latin1Character* buffer = builder.characters();
        builder->rollBack(1);
        builder.append("b"_s);
        AOT_TEST_CHECK(builder.characters() == buffer && textOf(program, read) == repeated('a', length));
        builder->release(program.instance());
    }
    Builder builder(program);
    builder.appendRepeated('a', adopted);
    AOT_TEST_CHECK(builder.toString()->tryGetValueImpl()->isSubString());
    builder->release(program.instance());
}

static void testFinishKeepsPooledBuffer(LoadedProgram& program)
{
    uint32_t retained = StringBuilderStorage::maxRetainedCapacity;
    Builder builder(program);
    AOT_TEST_CHECK(builder->reserve(program.realm(), retained));
    AOT_TEST_CHECK(builder.limit() == retained);
    for (unsigned i = 0; i < 5; ++i)
        builder.appendRepeated('a', 40000);
    AOT_TEST_CHECK(builder.limit() == retained);
    const Latin1Character* buffer = builder.characters();
    JSString* result = builder.finish();
    AOT_TEST_CHECK(charactersOf(result) != buffer && !result->tryGetValueImpl()->isSubString() && textOf(program, result) == repeated('a', 200000));
    AOT_TEST_CHECK(program.instance().stringBuilder.freeStorage == builder.storage());
    AOT_TEST_CHECK(builder.characters() == buffer && !builder->length() && builder.limit() == retained);
}

static void testFinishGivesLargeBufferAway(LoadedProgram& program)
{
    uint32_t retained = StringBuilderStorage::maxRetainedCapacity;
    // reserve() makes room for whole words.
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

// A collection that has been asked for runs at the next place where the collector may run.
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

// Pieces of one width that follow each other become one flat part. Where the width changes a new part begins.
static void testSegmentsKeepTheirWidth(LoadedProgram& program)
{
    Builder builder(program);
    WTF::StringBuilder expected;
    for (unsigned i = 0; i < 100; ++i) {
        builder.append("ab"_s);
        builder.append(std::span(japan));
        expected.append("ab"_s);
        expected.append(std::span(japan));
    }
    AOT_TEST_CHECK(builder.position() == 200 && builder.lengthOutsideBuffer() == 200);
    JSString* result = builder.finish();
    AOT_TEST_CHECK(result->isRope() && !result->is8Bit() && result->length() == 400);
    Vector<JSString*> parts = partsOf(program, result);
    AOT_TEST_CHECK(parts.size() == 200);
    for (size_t i = 0; i < parts.size(); ++i) {
        AOT_TEST_CHECK(!parts[i]->isRope() && parts[i]->length() == 2 && parts[i]->is8Bit() == !(i % 2));
        AOT_TEST_CHECK(textOf(program, parts[i]) == (i % 2 ? String(std::span(japan)) : String("ab"_s)));
    }
    AOT_TEST_CHECK(textOf(program, result) == expected.toString());
    AOT_TEST_CHECK(!result->isRope() && !result->is8Bit());

    Builder runs(program);
    runs.append("ab"_s);
    runs.append(jsNumber(12));
    runs.append(std::span(japan));
    runs.append(std::span(japan));
    runs.append("cd"_s);
    AOT_TEST_CHECK(runs.position() == 6 && runs.lengthOutsideBuffer() == 4);
    JSString* joined = runs.finish();
    AOT_TEST_CHECK(joined->isRope() && !joined->is8Bit() && joined->length() == 10);
    Vector<JSString*> joinedParts = partsOf(program, joined);
    AOT_TEST_CHECK(joinedParts.size() == 3);
    AOT_TEST_CHECK(!joinedParts[0]->isRope() && joinedParts[0]->is8Bit() && textOf(program, joinedParts[0]) == "ab12"_s);
    AOT_TEST_CHECK(!joinedParts[1]->isRope() && !joinedParts[1]->is8Bit() && textOf(program, joinedParts[1]) == makeString(String(std::span(japan)), String(std::span(japan))));
    AOT_TEST_CHECK(!joinedParts[2]->isRope() && joinedParts[2]->is8Bit() && textOf(program, joinedParts[2]) == "cd"_s);
}

static void testSegmentsByReference(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSString* first = jsString(vm, repeated('x', 70000));
    JSString* second = jsString(vm, repeated('y', 70000));
    Builder builder(program);
    builder.append(first);
    builder.append(" between "_s);
    builder.append(second);
    JSString* result = builder.finish();
    AOT_TEST_CHECK(result->isRope() && result->length() == 140009);

    RecordingVisitor visitor(vm);
    AbstractSlotVisitor& abstractVisitor = visitor;
    result->methodTable()->visitChildren(result, abstractVisitor);
    AOT_TEST_CHECK(visitor.cells().contains(first) && visitor.cells().contains(second));
    AOT_TEST_CHECK(textOf(program, result) == makeString(repeated('x', 70000), " between "_s, repeated('y', 70000)));
}

static void testLongFirstStringIsNotCopied(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t least = StringBuilderStorage::minLengthAdoptedByFinish;
    {
        JSString* first = jsString(vm, repeated('x', least));
        Builder builder(program);
        builder.append(first);
        AOT_TEST_CHECK(!builder.position() && builder.lengthOutsideBuffer() == least);
        builder.append("tail"_s);
        builder.append(jsString(vm, repeated('y', least)));
        AOT_TEST_CHECK(builder.position() == 4 + least && builder.lengthOutsideBuffer() == least);
        JSString* result = builder.finish();
        AOT_TEST_CHECK(result->isRope() && result->length() == 2 * least + 4);

        RecordingVisitor visitor(vm);
        AbstractSlotVisitor& abstractVisitor = visitor;
        result->methodTable()->visitChildren(result, abstractVisitor);
        AOT_TEST_CHECK(visitor.cells().contains(first));
        AOT_TEST_CHECK(textOf(program, result) == makeString(repeated('x', least), "tail"_s, repeated('y', least)));
    }
    {
        Builder builder(program);
        builder.append(jsString(vm, repeated('x', least - 1)));
        AOT_TEST_CHECK(builder.position() == least - 1 && !builder.lengthOutsideBuffer());
    }
}

// Joining the segments takes the text that lies in the buffer out of it.
static void testSegmentsCap(LoadedProgram& program)
{
    uint32_t cap = StringBuilderStorage::maxSegments;
    Builder builder(program);
    WTF::StringBuilder expected;
    for (uint32_t i = 0; i < cap; ++i) {
        builder.append(std::span(japan));
        builder.append("-"_s);
        expected.append(std::span(japan));
        expected.append('-');
    }
    AOT_TEST_CHECK(builder.position() == cap && builder.lengthOutsideBuffer() == 2 * cap);
    for (uint32_t i = 0; i < 10; ++i) {
        builder.append(std::span(japan));
        expected.append(std::span(japan));
        AOT_TEST_CHECK(!builder.position());
    }
    AOT_TEST_CHECK(builder->length() == 3 * cap + 20 && builder.lengthOutsideBuffer() == 3 * cap + 20);
    AOT_TEST_CHECK(textOf(program, builder.finish()) == expected.toString());
}

static void testReadWithSegmentsTwice(LoadedProgram& program)
{
    Builder builder(program);
    builder.append("ab"_s);
    builder.append(std::span(japan));
    builder.append("cd"_s);
    JSString* first = builder.toString();
    AOT_TEST_CHECK(builder.toString() == first);
    builder.append("e"_s);
    JSString* second = builder.toString();
    AOT_TEST_CHECK(second != first && second->length() == 7 && builder.toString() == second);
    AOT_TEST_CHECK(first->length() == 6);
    AOT_TEST_CHECK(builder.finish() == second);

    Builder plain(program);
    plain.append("only the buffer"_s);
    JSString* read = plain.toString();
    AOT_TEST_CHECK(plain.toString() == read && plain.finish() == read);
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

// A 16-bit string stays a 16-bit part of the text, whatever its units are. What is cut out of one as an 8-bit string lies in the buffer.
static void testWideStringsStayOutsideTheBuffer(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    static constexpr char16_t text[] = { 0x3c0, 'a', 'b', 'c', 0xe9, ',', 'd' };
    Strong<JSString> wide(vm, jsString(vm, String(std::span(text))));
    AOT_TEST_CHECK(!wide->is8Bit());

    Builder builder(program);
    builder.append("<"_s);
    JSString* part = jsSubstring(realm, wide.get(), 1, 4);
    AOT_TEST_CHECK(!part->is8Bit());
    builder.append(part);
    AOT_TEST_CHECK(builder.position() == 1 && builder.lengthOutsideBuffer() == 4);
    builder.append(jsString(vm, String(std::span(text).subspan(5))));
    AOT_TEST_CHECK(builder.position() == 1 && builder.lengthOutsideBuffer() == 6);
    const char16_t narrowInWideUnits[] = { 'w', 0xe9 };
    builder.append(std::span(narrowInWideUnits));
    AOT_TEST_CHECK(builder.position() == 1 && builder.lengthOutsideBuffer() == 8);

    JSString* twoPlainUnits = jsSubstring(realm, wide.get(), 1, 2);
    JSString* oneNarrowUnit = jsSubstring(realm, wide.get(), 4, 1);
    AOT_TEST_CHECK(twoPlainUnits->is8Bit() && oneNarrowUnit->is8Bit());
    builder.append(twoPlainUnits);
    builder.append(oneNarrowUnit);
    AOT_TEST_CHECK(builder.position() == 4 && builder.lengthOutsideBuffer() == 8);

    JSString* built = builder.toString();
    AOT_TEST_CHECK(built->isRope() && !built->is8Bit() && built->length() == 12);
    Vector<JSString*> parts = partsOf(program, built);
    AOT_TEST_CHECK(parts.size() == 3);
    AOT_TEST_CHECK(!parts[0]->isRope() && parts[0]->is8Bit() && textOf(program, parts[0]) == "<"_s);
    AOT_TEST_CHECK(!parts[1]->isRope() && !parts[1]->is8Bit() && textOf(program, parts[1]) == String::fromUTF8("abc\xc3\xa9,dw\xc3\xa9"));
    AOT_TEST_CHECK(!parts[2]->isRope() && parts[2]->is8Bit() && textOf(program, parts[2]) == String::fromUTF8("ab\xc3\xa9"));
    AOT_TEST_CHECK(!builder.position() && builder.lengthOutsideBuffer() == 12);
    AOT_TEST_CHECK(builder.text() == String::fromUTF8("<abc\xc3\xa9,dw\xc3\xa9" "ab\xc3\xa9"));

    builder.append(jsSubstring(realm, wide.get(), 0, 2));
    AOT_TEST_CHECK(!builder.position() && builder.lengthOutsideBuffer() == 14);
    AOT_TEST_CHECK(builder.text() == String::fromUTF8("<abc\xc3\xa9,dw\xc3\xa9" "ab\xc3\xa9\xcf\x80" "a"));
    builder->release(program.instance());
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

static void testReserveCommit(LoadedProgram& program)
{
    Builder builder(program);
    builder.append("start:"_s);
    Latin1Character* cursor = builder->reserve(program.realm(), 10);
    AOT_TEST_CHECK(cursor == builder.characters() + 6 && builder.limit() - builder.position() >= 10);
    memcpy(cursor, "12345", 5);
    AOT_TEST_CHECK(builder->length() == 6);
    builder->commit(cursor + 5);
    AOT_TEST_CHECK(builder->length() == 11 && builder.text() == "start:12345"_s);

    const Latin1Character* buffer = builder.characters();
    uint32_t capacity = builder.limit();
    cursor = builder->reserve(program.realm(), capacity);
    AOT_TEST_CHECK(cursor && builder.characters() != buffer && cursor == builder.characters() + 11);
    AOT_TEST_CHECK(builder.limit() - builder.position() >= capacity);
    AOT_TEST_CHECK(builder->length() == 11 && builder.text() == "start:12345"_s);

    builder->commit(builder.characters() + 6);
    AOT_TEST_CHECK(builder.text() == "start:"_s);

    AOT_TEST_CHECK(!builder->reserve(program.realm(), String::MaxLength));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(builder.text() == "start:"_s);
    builder->release(program.instance());
}

static void testLengthLimit(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSString* large = jsString(vm, repeated('x', 65536));
    while (large->length() < 1u << 30)
        large = jsString(program.realm(), large, large);
    AOT_TEST_CHECK(large->length() == 1u << 30 && large->isRope());

    Builder builder(program);
    builder.append("head"_s);
    builder.append(large);
    AOT_TEST_CHECK(builder->length() == (1u << 30) + 4);
    AOT_TEST_CHECK(builder->append(program.realm(), large) == AppendResult::Failed);
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(builder->length() == (1u << 30) + 4 && builder.position() == 4 && builder.lengthOutsideBuffer() == 1u << 30);

    builder.append("tail"_s);
    AOT_TEST_CHECK(builder->length() == (1u << 30) + 8);
    builder->rollBack(4);
    AOT_TEST_CHECK(builder.text() == "head"_s);
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

// A builder without text may be laid into the empty pool by one store, which writes no depth.
static void layIntoEmptyPool(LoadedProgram& program, Builder& builder)
{
    StringBuilderStorage*& pool = program.instance().stringBuilder.freeStorage;
    AOT_TEST_CHECK(!pool && !builder->length() && !builder.nextFree());
    pool = builder.storage();
}

static void testPoolDepthAboveBuilderLaidIntoEmptyPool(LoadedProgram& program)
{
    Instance& instance = program.instance();
    uint32_t maxPooled = StringBuilderStorage::maxPooledBuilders;
    static_assert(StringBuilderStorage::maxPooledBuilders >= 2);

    Vector<std::unique_ptr<Builder>> builders;
    for (uint32_t i = 0; i <= maxPooled; ++i)
        builders.append(makeUniqueWithoutFastMallocCheck<Builder>(program));
    auto fillPoolAbove = [&](uint32_t bottom) {
        layIntoEmptyPool(program, *builders[bottom]);
        uint32_t numberPooled = 1;
        for (uint32_t i = 0; i <= maxPooled; ++i) {
            if (i == bottom)
                continue;
            StringBuilderStorage* head = instance.stringBuilder.freeStorage;
            builders[i]->append("content"_s);
            (*builders[i])->release(instance);
            if (numberPooled < maxPooled) {
                ++numberPooled;
                AOT_TEST_CHECK(instance.stringBuilder.freeStorage == builders[i]->storage() && builders[i]->nextFree() == head && builders[i]->characters());
            } else
                AOT_TEST_CHECK(instance.stringBuilder.freeStorage == head && !builders[i]->nextFree() && !builders[i]->characters());
        }
        while (instance.stringBuilder.freeStorage)
            StringBuilderStorage::acquire(instance);
    };
    // None has been in the pool. Then the one that lay on top of the full pool is at the bottom, then one from the middle.
    fillPoolAbove(0);
    fillPoolAbove(maxPooled - 1);
    fillPoolAbove(maxPooled / 2);

    // Two are open at once, and the one that was taken first goes back first.
    layIntoEmptyPool(program, *builders[0]);
    (*builders[1])->release(instance);
    for (uint32_t turn = 0; turn < 3 * maxPooled; ++turn) {
        Builder first(program);
        Builder second(program);
        AOT_TEST_CHECK((first.storage() == builders[0]->storage() && second.storage() == builders[1]->storage()) || (first.storage() == builders[1]->storage() && second.storage() == builders[0]->storage()));
        second.append("content"_s);
        layIntoEmptyPool(program, first);
        second->release(instance);
        AOT_TEST_CHECK(instance.stringBuilder.freeStorage == second.storage() && second.nextFree() == first.storage() && second.characters());
    }
}

static void testPoolClearedByCollection(LoadedProgram& program)
{
    Instance& instance = program.instance();
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        {
            Builder builder(program);
            builder.append("content"_s);
            builder->release(instance);
            AOT_TEST_CHECK(instance.stringBuilder.freeStorage == builder.storage() && instance.stringBuilder.isPoolRegistered);
        }
        program.vm().heap.collectSync(scope);
        AOT_TEST_CHECK(!instance.stringBuilder.freeStorage);
    }
    Builder builder(program);
    builder.append("after"_s);
    AOT_TEST_CHECK(textOf(program, builder.finish()) == "after"_s);
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
        { "string-builder-storage/limit-is-room"_s, withInstance<testLimitIsRoom> },
        { "string-builder-storage/length-is-two-words"_s, withInstance<testLengthIsTwoWords> },
        { "string-builder-storage/first-string-comes-back"_s, withInstance<testFirstStringComesBack> },
        { "string-builder-storage/share-read"_s, withInstance<testShareRead> },
        { "string-builder-storage/share-then-roll-back"_s, withInstance<testShareThenRollBack> },
        { "string-builder-storage/share-then-release"_s, withInstance<testShareThenRelease> },
        { "string-builder-storage/sharers-die"_s, withInstance<testSharersDie> },
        { "string-builder-storage/builder-keeps-the-string-it-read-last"_s, withInstance<testBuilderKeepsTheStringItReadLast> },
        { "string-builder-storage/short-read-copies"_s, withInstance<testShortReadCopies> },
        { "string-builder-storage/finish-keeps-pooled-buffer"_s, withInstance<testFinishKeepsPooledBuffer> },
        { "string-builder-storage/finish-gives-large-buffer-away"_s, withInstance<testFinishGivesLargeBufferAway> },
        { "string-builder-storage/next-large-text-grows-once"_s, withInstance<testNextLargeTextGrowsOnce> },
        { "string-builder-storage/segments-keep-their-width"_s, withInstance<testSegmentsKeepTheirWidth> },
        { "string-builder-storage/segments-by-reference"_s, withInstance<testSegmentsByReference> },
        { "string-builder-storage/long-first-string-is-not-copied"_s, withInstance<testLongFirstStringIsNotCopied> },
        { "string-builder-storage/segments-cap"_s, withInstance<testSegmentsCap> },
        { "string-builder-storage/read-with-segments-twice"_s, withInstance<testReadWithSegmentsTwice> },
        { "string-builder-storage/wide-strings-stay-outside-the-buffer"_s, withInstance<testWideStringsStayOutsideTheBuffer> },
        { "string-builder-storage/roll-back-everywhere"_s, withInstance<testRollBackEverywhere> },
        { "string-builder-storage/append-values-is-atomic"_s, withInstance<testAppendValuesIsAtomic> },
        { "string-builder-storage/reserve-commit"_s, withInstance<testReserveCommit> },
        { "string-builder-storage/length-limit"_s, withInstance<testLengthLimit> },
        { "string-builder-storage/room-that-may-stay-unused"_s, withInstance<testRoomThatMayStayUnused> },
        { "string-builder-storage/pool-depth"_s, withInstance<testPoolDepth> },
        { "string-builder-storage/pool-depth-above-a-builder-laid-into-the-empty-pool"_s, withInstance<testPoolDepthAboveBuilderLaidIntoEmptyPool> },
        { "string-builder-storage/pool-cleared-by-collection"_s, withInstance<testPoolClearedByCollection> },
        { "string-builder-storage/discard-leaves-next-buffer-to-next-write"_s, withInstance<testDiscardLeavesNextBufferToNextWrite> },
        { collectorTest, testCollector },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
