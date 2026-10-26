/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTOperationCallScope.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStringBuilderStorage.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "ErrorInstance.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
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
using StringUse = StringBuilderStorage::StringUse;

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

    JSString* toString(StringUse use = StringUse::Unknown)
    {
        JSString* result = storage()->toString(m_program.realm(), m_program.instance(), use);
        AOT_TEST_CHECK(result);
        return result;
    }
    JSString* finish(StringUse use = StringUse::Unknown)
    {
        JSString* result = storage()->finish(m_program.realm(), m_program.instance(), use);
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

static uint32_t limitValue(LoadedProgram& program, LimitID limit)
{
    return static_cast<uint32_t>(runtimeLimitValue(program.vm(), limit));
}

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

    JSString* concatenated = StringBuilderStorage::concatenate(realm, program.instance(), encoded.span(), StringUse::Unknown);
    AOT_TEST_CHECK(concatenated && expectedText.endsWith(textOf(program, concatenated)) && concatenated->length() * 2 < expectedText.length() + 200);
}

static void testLimitIsRoom(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t byReference = limitValue(program, LimitID::MinLengthAppendedByReference);
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
    uint32_t adopted = limitValue(program, LimitID::MinLengthAdoptedByFinish);
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
    uint32_t retained = limitValue(program, LimitID::MaxRetainedStringBuilderCapacity);
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
    uint32_t retained = limitValue(program, LimitID::MaxRetainedStringBuilderCapacity);
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
    uint32_t least = limitValue(program, LimitID::MinLengthAdoptedByFinish);
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

static void testSegmentsCap(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t cap = limitValue(program, LimitID::MaxStringBuilderSegments);
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::MaxStringBuilderSegments));
    Builder builder(program);
    WTF::StringBuilder expected;
    for (uint32_t i = 0; i < cap; ++i) {
        builder.append(std::span(japan));
        builder.append("-"_s);
        expected.append(std::span(japan));
        expected.append('-');
    }
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::MaxStringBuilderSegments));
    for (uint32_t i = 0; i < 10; ++i) {
        builder.append(std::span(japan));
        expected.append(std::span(japan));
    }
    AOT_TEST_CHECK(limitHitsOf(vm, LimitID::MaxStringBuilderSegments) == 1);
    AOT_TEST_CHECK(builder->length() == 3 * cap + 20);
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

static void testPoolDepth(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    uint32_t maxPooled = limitValue(program, LimitID::MaxPooledStringBuilders);
    AOT_TEST_CHECK(!instance.stringBuilder.freeStorage && !limitHitsOf(vm, LimitID::MaxPooledStringBuilders));

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
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::MaxPooledStringBuilders));

    (*builders[maxPooled])->release(instance);
    AOT_TEST_CHECK(limitHitsOf(vm, LimitID::MaxPooledStringBuilders) == 1);
    AOT_TEST_CHECK(instance.stringBuilder.freeStorage == builders[maxPooled - 1]->storage());
    AOT_TEST_CHECK(!builders[maxPooled]->characters() && !(*builders[maxPooled])->length());

    for (uint32_t i = maxPooled; i--;)
        AOT_TEST_CHECK(StringBuilderStorage::acquire(instance) == builders[i]->storage());
    AOT_TEST_CHECK(!instance.stringBuilder.freeStorage);
    StringBuilderStorage* fresh = StringBuilderStorage::acquire(instance);
    for (auto& builder : builders)
        AOT_TEST_CHECK(fresh != builder->storage());
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

static JSString* buildKey(LoadedProgram& program, const String& text, StringUse use)
{
    Builder builder(program);
    AOT_TEST_CHECK(builder->append(program.realm(), text.span8()) == AppendResult::Appended);
    JSString* result = builder.finish(use);
    AOT_TEST_CHECK(textOf(program, result) == text);
    return result;
}

static void testKeysThatAreIntegers(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(buildKey(program, "123"_s, StringUse::LikelyKey) == int32ToString(vm, 123, 10));
    AOT_TEST_CHECK(buildKey(program, "40000"_s, StringUse::LikelyKey) == int32ToString(vm, 40000, 10));
    AOT_TEST_CHECK(buildKey(program, "012"_s, StringUse::LikelyKey) != int32ToString(vm, 12, 10));
    AOT_TEST_CHECK(buildKey(program, "12a"_s, StringUse::LikelyKey) != int32ToString(vm, 12, 10));
    AOT_TEST_CHECK(buildKey(program, "7"_s, StringUse::LikelyKey) == vm.smallStrings.singleCharacterString('7'));
    AOT_TEST_CHECK(buildKey(program, "7"_s, StringUse::Unknown) == vm.smallStrings.singleCharacterString('7'));
}

namespace {

class StackBuilderForTest {
public:
    explicit StackBuilderForTest(LoadedProgram& program)
        : m_program(program)
    {
        m_builder.limit = m_builder.buffer + StackStringBuilder::capacity;
        m_builder.lengthOrigin = m_builder.buffer;
        m_cursor = m_builder.buffer;
    }

    bool isOnStack() const { return m_builder.isOnStack(); }
    size_t length() const { return m_cursor - m_builder.lengthOrigin; }
    const String expected() { return m_expected.toString(); }

    void appendInline(StringView text)
    {
        AOT_TEST_CHECK(text.length() <= StackStringBuilder::maxBytesWrittenPastLimit && m_cursor <= m_builder.limit);
        memset(m_cursor, '?', StackStringBuilder::maxBytesWrittenPastLimit);
        text.getCharacters(std::span<Latin1Character> { m_cursor, text.length() });
        m_cursor += text.length();
        m_expected.append(text);
        if (m_cursor > m_builder.limit) {
            AOT_TEST_CHECK(m_builder.moveToStorage(m_program.realm(), m_program.instance(), m_cursor));
            didChangeStorage();
        }
    }

    void appendToStorage(JSString* string)
    {
        StringBuilderStorage* storage = m_builder.moveToStorage(m_program.realm(), m_program.instance(), m_cursor);
        AOT_TEST_CHECK(storage && storage->append(m_program.realm(), string) == AppendResult::Appended);
        m_expected.append(textOf(m_program, string));
        didChangeStorage();
    }

    JSString* finish(StringUse use)
    {
        AOT_TEST_CHECK(length() == m_expected.length());
        JSString* result = m_builder.finish(m_program.realm(), m_program.instance(), m_cursor, use);
        AOT_TEST_CHECK(result && textOf(m_program, result) == m_expected.toString());
        return result;
    }

private:
    void didChangeStorage()
    {
        m_cursor = m_builder.didChangeStorage(m_program.realm());
        AOT_TEST_CHECK(m_cursor && m_cursor <= m_builder.limit && !m_builder.isOnStack());
    }

    LoadedProgram& m_program;
    StackStringBuilder m_builder;
    Latin1Character* m_cursor;
    WTF::StringBuilder m_expected;
};

} // anonymous namespace

static void testStackBuilder(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (size_t length : { 0u, 1u, 2u, 63u, 64u, 65u, 127u, 128u, 129u, 143u, 144u, 145u, 4095u, 4096u, 4097u, 300000u }) {
        for (size_t step : { 1u, 7u, 16u }) {
            StackBuilderForTest builder(program);
            String piece = repeated('a' + step, step);
            while (builder.length() + step <= length)
                builder.appendInline(piece);
            builder.appendInline(StringView(piece).left(length - builder.length()));
            AOT_TEST_CHECK(builder.isOnStack() == (length <= StackStringBuilder::capacity));
            JSString* key = builder.finish(StringUse::LikelyKey);
            if (length >= 2 && length <= 64)
                AOT_TEST_CHECK(key == buildKey(program, builder.expected(), StringUse::LikelyKey));
        }
    }

    StackBuilderForTest index(program);
    index.appendInline("40000"_s);
    AOT_TEST_CHECK(index.finish(StringUse::LikelyKey) == int32ToString(vm, 40000, 10));

    JSString* longString = jsString(vm, repeated('x', 100000));
    StackBuilderForTest only(program);
    only.appendToStorage(longString);
    AOT_TEST_CHECK(only.finish(StringUse::Unknown) == longString);

    static constexpr char16_t japan[] = { 0x65e5, 0x672c };
    StackBuilderForTest mixed(program);
    for (unsigned round = 0; round < 40; ++round) {
        mixed.appendInline("0123456789"_s);
        if (round % 8 == 3)
            mixed.appendToStorage(longString);
        if (round % 8 == 5)
            mixed.appendToStorage(jsString(vm, String(std::span(japan))));
        if (round % 8 == 7)
            mixed.appendToStorage(jsString(vm, repeated('y', 200)));
    }
    mixed.finish(StringUse::Unknown);
}

static void testParseIndex()
{
    static constexpr std::array<uint8_t, 12> alphabet { '0', '0', '1', '5', '9', '9', '/', ':', ',', 'a', 0xb0, 0xff };
    std::array<Latin1Character, StringKeyCache::maxIndexLength + StringKeyCache::minReadableBytes> buffer;
    auto check = [&](size_t length) {
        std::span<const Latin1Character> text = std::span { buffer }.first(length);
        AOT_TEST_CHECK(StringKeyCache::tryParseIndex(text) == parseIndex(text));
    };
    uint64_t state = 1;
    for (size_t length = 1; length <= StringKeyCache::maxIndexLength; ++length) {
        for (unsigned round = 0; round < 50000; ++round) {
            for (Latin1Character& character : buffer) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                unsigned digitsOnly = round % 2 * 6;
                character = static_cast<Latin1Character>(alphabet[(state >> 33) % (alphabet.size() - digitsOnly)]);
            }
            check(length);
        }
    }
    for (uint64_t value : { 0ull, 1ull, 9ull, 10ull, 99ull, 100ull, 65535ull, 9999999ull, 10000000ull, 99999999ull, 100000000ull, 999999999ull, 1000000000ull, 2147483647ull, 2147483648ull, 4294967294ull, 4294967295ull, 4294967296ull, 9999999999ull }) {
        String text = String::number(value);
        buffer.fill(static_cast<Latin1Character>('7'));
        memcpySpan(std::span { buffer }, text.span8());
        check(text.length());
    }
}

static void testKeysUpToTheCachedLength(LoadedProgram& program)
{
    uint32_t cached = limitValue(program, LimitID::MaxCachedKeyLength);
    AOT_TEST_CHECK(cached == 64);
    for (uint32_t length : { 2u, 10u, cached }) {
        JSString* first = buildKey(program, repeated('k', length), StringUse::LikelyKey);
        AOT_TEST_CHECK(buildKey(program, repeated('k', length), StringUse::LikelyKey) == first);
        AOT_TEST_CHECK(buildKey(program, repeated('j', length), StringUse::LikelyKey) != first);
        AOT_TEST_CHECK(buildKey(program, repeated('k', length), StringUse::Unknown) != buildKey(program, repeated('k', length), StringUse::Unknown));
    }
    AOT_TEST_CHECK(buildKey(program, repeated('k', cached + 1), StringUse::LikelyKey) != buildKey(program, repeated('k', cached + 1), StringUse::LikelyKey));
}

static JSString* concatenate(LoadedProgram& program, std::initializer_list<JSValue> parts, StringUse use)
{
    Vector<EncodedJSValue, 8> encoded;
    WTF::StringBuilder expected;
    for (JSValue part : parts) {
        encoded.append(JSValue::encode(part));
        expected.append(part.toWTFString(program.realm()));
    }
    JSString* result = StringBuilderStorage::concatenate(program.realm(), program.instance(), encoded.span(), use);
    AOT_TEST_CHECK(result && textOf(program, result) == expected.toString());
    return result;
}

static void testConcatenatedKeys(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    uint32_t cached = limitValue(program, LimitID::MaxCachedKeyLength);
    JSString* row = jsString(vm, String("row"_s));
    JSString* colon = jsString(vm, String(":"_s));

    JSString* key = concatenate(program, { row, colon, jsNumber(12) }, StringUse::LikelyKey);
    AOT_TEST_CHECK(!key->isRope() && key->tryGetValueImpl()->hasHash());
    AOT_TEST_CHECK(concatenate(program, { row, colon, jsNumber(12) }, StringUse::LikelyKey) == key);
    AOT_TEST_CHECK(concatenate(program, { row, colon, jsDoubleNumber(12.0) }, StringUse::LikelyKey) == key);
    AOT_TEST_CHECK(buildKey(program, "row:12"_s, StringUse::LikelyKey) == key);
    AOT_TEST_CHECK(concatenate(program, { row, colon, jsNumber(13) }, StringUse::LikelyKey) != key);
    AOT_TEST_CHECK(concatenate(program, { row, colon, jsNumber(12) }, StringUse::Unknown) != key);

    // concatenate() above reads the text of each part, which resolves a rope.
    JSString* rope = jsString(realm, row, colon);
    AOT_TEST_CHECK(rope->isRope());
    std::array<EncodedJSValue, 2> ropeAndNumber { JSValue::encode(rope), JSValue::encode(jsNumber(12)) };
    AOT_TEST_CHECK(StringBuilderStorage::concatenate(realm, program.instance(), ropeAndNumber, StringUse::LikelyKey) == key);
    AOT_TEST_CHECK(rope->isRope());

    AOT_TEST_CHECK(concatenate(program, { jsString(vm, String("1"_s)), jsNumber(23) }, StringUse::LikelyKey) == int32ToString(vm, 123, 10));
    AOT_TEST_CHECK(concatenate(program, { jsString(vm, String("0"_s)), jsNumber(12) }, StringUse::LikelyKey) != int32ToString(vm, 12, 10));
    AOT_TEST_CHECK(concatenate(program, { jsEmptyString(vm), jsEmptyString(vm) }, StringUse::LikelyKey) == jsEmptyString(vm));
    AOT_TEST_CHECK(concatenate(program, { jsEmptyString(vm), colon }, StringUse::LikelyKey) == vm.smallStrings.singleCharacterString(':'));

    concatenate(program, { row, jsNumber(INT32_MIN), jsNumber(INT32_MAX), jsDoubleNumber(-0.0) }, StringUse::LikelyKey);
    concatenate(program, { row, jsNumber(1.5), jsDoubleNumber(PNaN), jsNumber(4294967296.0), jsNumber(-2147483649.0) }, StringUse::LikelyKey);
    concatenate(program, { row, jsUndefined(), jsNull(), jsBoolean(true) }, StringUse::LikelyKey);
    concatenate(program, { row, jsString(vm, String(std::span(japan))) }, StringUse::LikelyKey);

    JSString* longest = jsString(vm, repeated('k', cached - 3));
    AOT_TEST_CHECK(concatenate(program, { row, longest }, StringUse::LikelyKey) == concatenate(program, { row, longest }, StringUse::LikelyKey));
    AOT_TEST_CHECK(concatenate(program, { row, longest, colon }, StringUse::LikelyKey) != concatenate(program, { row, longest, colon }, StringUse::LikelyKey));
    AOT_TEST_CHECK(concatenate(program, { longest, jsNumber(123) }, StringUse::LikelyKey) == concatenate(program, { longest, jsNumber(123) }, StringUse::LikelyKey));
}

static JSString* concatenateAsCompiledCodeDoes(LoadedProgram& program, std::initializer_list<JSValue> parts)
{
    Vector<EncodedJSValue, 8> encoded;
    WTF::StringBuilder expected;
    for (JSValue part : parts) {
        encoded.append(JSValue::encode(part));
        expected.append(part.toWTFString(program.realm()));
    }
    JSString* found = StringBuilderStorage::tryFindKeyByParts(program.instance(), encoded.span());
    AOT_TEST_CHECK(!found || textOf(program, found) == expected.toString());
    return found ? found : concatenate(program, parts, StringUse::LikelyKey);
}

static NEVER_INLINE void makeKeysOfPartsNobodyElseHolds(LoadedProgram& program, unsigned round)
{
    VM& vm = program.vm();
    for (unsigned i = 0; i < 2000; ++i) {
        JSString* part = jsString(vm, makeString("part"_s, round, 'x', i));
        concatenateAsCompiledCodeDoes(program, { part, jsNumber(i) });
        concatenateAsCompiledCodeDoes(program, { jsNumber(i), part, jsNumber(round) });
        concatenateAsCompiledCodeDoes(program, { jsNumber(100000 + round * 2000 + i), jsEmptyString(vm) });
    }
}

static void testKeysFoundByTheirParts(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto find = [&](std::initializer_list<JSValue> parts) {
        Vector<EncodedJSValue, 8> encoded;
        for (JSValue part : parts)
            encoded.append(JSValue::encode(part));
        return StringBuilderStorage::tryFindKeyByParts(program.instance(), encoded.span());
    };
    JSString* row = jsString(vm, String("row"_s));
    JSString* otherRow = jsString(vm, String("row"_s));
    AOT_TEST_CHECK(!find({ row, jsNumber(12) }));
    JSString* key = concatenateAsCompiledCodeDoes(program, { row, jsNumber(12) });
    AOT_TEST_CHECK(find({ row, jsNumber(12) }) == key);
    AOT_TEST_CHECK(!find({ row, jsNumber(13) }) && !find({ jsNumber(12), row }) && !find({ row, jsNumber(12), jsEmptyString(vm) }));
    AOT_TEST_CHECK(!find({ otherRow, jsNumber(12) }));
    AOT_TEST_CHECK(concatenateAsCompiledCodeDoes(program, { otherRow, jsNumber(12) }) == key);
    AOT_TEST_CHECK(find({ otherRow, jsNumber(12) }) == key);

    JSString* integerKey = concatenateAsCompiledCodeDoes(program, { jsNumber(40000), jsEmptyString(vm) });
    AOT_TEST_CHECK(integerKey == int32ToString(vm, 40000, 10));
    AOT_TEST_CHECK(find({ jsNumber(40000), jsEmptyString(vm) }) == integerKey && find({ jsNumber(40000) }) == integerKey);
    AOT_TEST_CHECK(!find({ jsNumber(40001), jsEmptyString(vm) }) && !find({ jsNumber(-40000), jsEmptyString(vm) }));
    AOT_TEST_CHECK(textOf(program, concatenateAsCompiledCodeDoes(program, { jsNumber(-40000), jsEmptyString(vm) })) == "-40000"_s);
    AOT_TEST_CHECK(!find({ row, jsUndefined() }) && !find({ row, jsNumber(1.5) }));
    AOT_TEST_CHECK(find({ row, jsEmptyString(vm) }) == row && find({ jsEmptyString(vm), row }) == row && find({ row }) == row);
    AOT_TEST_CHECK(!find({ jsEmptyString(vm), jsEmptyString(vm) }) && !find({ jsUndefined(), jsEmptyString(vm) }));
    AOT_TEST_CHECK(find({ jsEmptyString(vm), jsNumber(40000) }) == integerKey);
    AOT_TEST_CHECK(concatenate(program, { jsEmptyString(vm), row }, StringUse::LikelyKey) == row);
    AOT_TEST_CHECK(!concatenate(program, { jsEmptyString(vm), jsEmptyString(vm) }, StringUse::LikelyKey)->length());
    AOT_TEST_CHECK(textOf(program, concatenate(program, { jsNumber(12), jsNumber(34) }, StringUse::LikelyKey)) == "1234"_s);
    AOT_TEST_CHECK(concatenate(program, { jsNumber(412), jsNumber(34) }, StringUse::LikelyKey) == int32ToString(vm, 41234, 10));
    AOT_TEST_CHECK(textOf(program, concatenate(program, { jsNumber(2147483647), jsNumber(1) }, StringUse::LikelyKey)) == "21474836471"_s);
    AOT_TEST_CHECK(textOf(program, concatenate(program, { jsNumber(0), jsNumber(7) }, StringUse::LikelyKey)) == "07"_s);
    AOT_TEST_CHECK(textOf(program, concatenate(program, { jsNumber(2147483647), jsEmptyString(vm) }, StringUse::LikelyKey)) == "2147483647"_s);
    AOT_TEST_CHECK(textOf(program, concatenate(program, { jsNumber(0), jsEmptyString(vm) }, StringUse::LikelyKey)) == "0"_s);
    AOT_TEST_CHECK(!find({ row, row, row, row, row, row, row }));

    for (unsigned round = 0; round < 24; ++round) {
        makeKeysOfPartsNobodyElseHolds(program, round);
        vm.heap.collectNow(Sync, round % 3 ? CollectionScope::Eden : CollectionScope::Full);
    }
    JSString* foundAfterwards = find({ row, jsNumber(12) });
    AOT_TEST_CHECK(!foundAfterwards || foundAfterwards == key);
    foundAfterwards = find({ jsNumber(40000), jsEmptyString(vm) });
    AOT_TEST_CHECK(!foundAfterwards || foundAfterwards == integerKey);
    concatenateAsCompiledCodeDoes(program, { row, jsNumber(12) });
    concatenateAsCompiledCodeDoes(program, { jsNumber(40000), jsEmptyString(vm) });
    ensureStillAliveHere(row);
    ensureStillAliveHere(otherRow);
    ensureStillAliveHere(key);
    ensureStillAliveHere(integerKey);
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

static JSString* toLowerCase(LoadedProgram& program, JSString* string)
{
    return uncheckedDowncast<JSString>(callOperationFromTest(program.vm(), operationAOTStringToLowerCase, &program.instance(), string).value);
}

static JSString* toUpperCase(LoadedProgram& program, JSString* string)
{
    return uncheckedDowncast<JSString>(callOperationFromTest(program.vm(), operationAOTStringToUpperCase, &program.instance(), string).value);
}

static NEVER_INLINE void convertAStringThatDies(LoadedProgram& program, uintptr_t& addressOfSource)
{
    JSString* source = jsNontrivialString(program.vm(), "X-Dies-With-The-Request"_s);
    addressOfSource = std::bit_cast<uintptr_t>(source);
    AOT_TEST_CHECK(textOf(program, toLowerCase(program, source)) == "x-dies-with-the-request"_s);
    AOT_TEST_CHECK(program.state().propertyCaches->stringKeys().tryFindCaseConversion(source, StringKeyCache::LetterCase::Lower));
}

// The conversion of one and the same string is remembered for each case. Nothing is remembered of a string that died: another string may get its cell.
static void testCaseConversionOfTheSameStringIsRemembered(LoadedProgram& program)
{
    VM& vm = program.vm();
    StringKeyCache& keys = program.state().propertyCaches->stringKeys();
    using LetterCase = StringKeyCache::LetterCase;
    Strong<JSString> source(vm, jsNontrivialString(vm, "Content-Type"_s));
    Strong<JSString> twin(vm, jsNontrivialString(vm, "Content-Type"_s));
    AOT_TEST_CHECK(!keys.tryFindCaseConversion(source.get(), LetterCase::Lower) && !keys.tryFindCaseConversion(source.get(), LetterCase::Upper));

    Strong<JSString> lower(vm, toLowerCase(program, source.get()));
    AOT_TEST_CHECK(textOf(program, lower.get()) == "content-type"_s);
    AOT_TEST_CHECK(keys.tryFindCaseConversion(source.get(), LetterCase::Lower) == lower.get() && !keys.tryFindCaseConversion(source.get(), LetterCase::Upper));
    AOT_TEST_CHECK(!keys.tryFindCaseConversion(twin.get(), LetterCase::Lower));
    AOT_TEST_CHECK(toLowerCase(program, source.get()) == lower.get() && toLowerCase(program, twin.get()) == lower.get());

    Strong<JSString> upper(vm, toUpperCase(program, source.get()));
    AOT_TEST_CHECK(textOf(program, upper.get()) == "CONTENT-TYPE"_s);
    AOT_TEST_CHECK(keys.tryFindCaseConversion(source.get(), LetterCase::Upper) == upper.get() && keys.tryFindCaseConversion(source.get(), LetterCase::Lower) == lower.get());
    AOT_TEST_CHECK(toLowerCase(program, lower.get()) == lower.get() && !keys.tryFindCaseConversion(lower.get(), LetterCase::Lower));

    uintptr_t addressOfDeadSource = 0;
    convertAStringThatDies(program, addressOfDeadSource);
    clobberStack();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!keys.tryFindCaseConversion(std::bit_cast<JSString*>(addressOfDeadSource), LetterCase::Lower));
    AOT_TEST_CHECK(keys.tryFindCaseConversion(source.get(), LetterCase::Lower) == lower.get() && keys.tryFindCaseConversion(source.get(), LetterCase::Upper) == upper.get());
}

// Where strings lie decides which of them share a hash. As many as a hash has places for are all remembered, however often they take turns.
static void testCaseConversionsOfStringsWithOneHashAreRemembered(LoadedProgram& program)
{
    VM& vm = program.vm();
    StringKeyCache& keys = program.state().propertyCaches->stringKeys();
    using LetterCase = StringKeyCache::LetterCase;
    auto hashOf = [&](JSString* string) { return StringKeyCache::indexOfCaseConversion(MarkedBlockCage::encode(keys.cageBase(), string), LetterCase::Lower); };
    Vector<Strong<JSString>> sources;
    sources.append(Strong<JSString>(vm, jsNontrivialString(vm, "X-Request-Id"_s)));
    Vector<Strong<JSString>> others;
    constexpr size_t numberOfSources = 4;
    static_assert(numberOfSources <= StringKeyCache::waysOfCaseConversions);
    while (sources.size() < numberOfSources) {
        Strong<JSString> string(vm, jsNontrivialString(vm, "X-Request-Id"_s));
        (hashOf(string.get()) == hashOf(sources[0].get()) ? sources : others).append(WTF::move(string));
    }
    for (unsigned turn = 0; turn < 3; ++turn) {
        for (auto& source : sources) {
            AOT_TEST_CHECK(!!keys.tryFindCaseConversion(source.get(), LetterCase::Lower) == !!turn);
            AOT_TEST_CHECK(textOf(program, toLowerCase(program, source.get())) == "x-request-id"_s);
        }
    }
}

template<LoadedProgramTest body>
static constexpr void (*withInstance)() = runNativeTestWithLoadedProgram<stringBuilderStorageTests, programText, body>;

std::span<const NativeTest> stringBuilderStorageTests()
{
    static constexpr NativeTest tests[] = {
        { "string-builder-storage/append-latin1"_s, withInstance<testAppendLatin1> },
        { "string-builder-storage/case-conversion-of-the-same-string-is-remembered"_s, withInstance<testCaseConversionOfTheSameStringIsRemembered> },
        { "string-builder-storage/case-conversions-of-strings-with-one-hash-are-remembered"_s, withInstance<testCaseConversionsOfStringsWithOneHashAreRemembered> },
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
        { "string-builder-storage/pool-depth"_s, withInstance<testPoolDepth> },
        { "string-builder-storage/pool-cleared-by-collection"_s, withInstance<testPoolClearedByCollection> },
        { "string-builder-storage/keys-that-are-integers"_s, withInstance<testKeysThatAreIntegers> },
        { "string-builder-storage/parse-index"_s, testParseIndex },
        { "string-builder-storage/stack-builder"_s, withInstance<testStackBuilder> },
        { "string-builder-storage/keys-up-to-the-cached-length"_s, withInstance<testKeysUpToTheCachedLength> },
        { "string-builder-storage/concatenated-keys"_s, withInstance<testConcatenatedKeys> },
        { "string-builder-storage/keys-found-by-their-parts"_s, withInstance<testKeysFoundByTheirParts> },
        { collectorTest, testCollector },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
