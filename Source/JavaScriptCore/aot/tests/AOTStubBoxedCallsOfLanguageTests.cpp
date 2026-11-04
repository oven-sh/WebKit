/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTStubRunner.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsRegExp.h"
#include "InternalFunction.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSString.h"
#include "JSTypeInfo.h"
#include "Structure.h"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubBoxedCallsOfLanguageTests();

using TextInLanguageCallTests = FakeObject<sizeof(JSRopeString)>;

static constexpr uint64_t nothingDoneInLanguageCallTests = 0;
static constexpr uint64_t resultOfHostFunctionInLanguageCallTests = static_cast<uint64_t>(JSValue::NumberTag) | 73;
static_assert(!JSValue::ValueEmpty);

static constexpr uint64_t int32InLanguageCallTests(int32_t number) { return static_cast<uint64_t>(JSValue::NumberTag) | static_cast<uint32_t>(number); }

static void makeFlatInLanguageCallTests(TextInLanguageCallTests& cell, StringImpl& impl)
{
    cell.setHeader(0, StringType);
    cell.at<uintptr_t>(JSString::offsetOfValue()) = std::bit_cast<uintptr_t>(&impl);
}

static void makeRopeInLanguageCallTests(TextInLanguageCallTests& cell, uint32_t length)
{
    cell.setHeader(0, StringType);
    cell.at<uintptr_t>(JSString::offsetOfValue()) = JSString::isRopeInPointer;
    cell.at<uint32_t>(JSRopeString::offsetOfLength()) = length;
}

static bool refersToInLanguageCallTests(const StubCatalog& catalog, StubKey from, StubKey target)
{
    return catalog.recordingOf(bodyOf(from)).references.containsIf([&](const StubReference& reference) {
        return reference.target == target;
    });
}

static bool callsOperationInLanguageCallTests(const StubCatalog& catalog, StubKey from, Entry entry)
{
    const StubRecording& recording = catalog.recordingOf(bodyOf(from));
    return recording.operationsCalled.get(indexOf(entry)) || recording.references.containsIf([&](const StubReference& reference) {
        return reference.target.immediate() == offsetInRuntimeTable(entry);
    });
}

// E1, M1. Where the program matches no RegExp the handlers of every image name nothing of RemovableFeature::RegExp.
static void testFormsThatMatchInLanguageCallTests()
{
    StubCatalog matches(StubOptions { });
    StubCatalog matchesNothing(StubOptions { .matchesRegExps = false });
    for (StubKey key : { callBoxedBuiltin(BoxedCallForm::RegExpExec, 1), callMethodOfBuiltinClass(BoxedCallForm::RegExpExec, 1), tryCallMethodOfBuiltinClass(BoxedCallForm::RegExpExec, 1) }) {
        AOT_TEST_CHECK(refersToInLanguageCallTests(matches, key, regExpMatch(RegExpMatchResult::Array)));
        AOT_TEST_CHECK(!refersToInLanguageCallTests(matchesNothing, key, regExpMatch(RegExpMatchResult::Array)));
    }
    for (StubKey key : { callBoxedBuiltin(BoxedCallForm::StringMatch, 1), callMethodOfBuiltinClass(BoxedCallForm::StringMatch, 1), tryCallMethodOfBuiltinClass(BoxedCallForm::StringMatch, 1) }) {
        AOT_TEST_CHECK(callsOperationInLanguageCallTests(matches, key, Entry::operationAOTStringMatchRegExp));
        AOT_TEST_CHECK(!callsOperationInLanguageCallTests(matchesNothing, key, Entry::operationAOTStringMatchRegExp));
    }
    for (BoxedCallForm form : { BoxedCallForm::RegExpExec, BoxedCallForm::StringMatch }) {
        AOT_TEST_CHECK(servesCallsByName(form, 1));
        for (unsigned count : { 0u, 2u, 3u, 4u })
            AOT_TEST_CHECK(!acceptsArgumentCount(form, count) && !servesCallsByName(form, count));
    }
}

// W1.
static void testIsWellFormedInLanguageCallTests()
{
    StubRunner runner;
    Ref<StringImpl> narrow = StringImpl::create("narrow"_s);
    constexpr std::array<char16_t, 2> wideCharacters { 0x100, 0xd800 };
    Ref<StringImpl> wide = StringImpl::create(std::span<const char16_t>(wideCharacters));
    AOT_TEST_CHECK(narrow->is8Bit() && !wide->is8Bit());
    TextInLanguageCallTests narrowText;
    TextInLanguageCallTests wideText;
    TextInLanguageCallTests rope;
    FakeObject<16> object;
    FakeObject<sizeof(JSFunction)> callee;
    makeFlatInLanguageCallTests(narrowText, narrow.get());
    makeFlatInLanguageCallTests(wideText, wide.get());
    makeRopeInLanguageCallTests(rope, 6);
    object.setHeader(0, ObjectType);

    AOT_TEST_CHECK(servesCallsByName(BoxedCallForm::StringIsWellFormed, 0) && !acceptsArgumentCount(BoxedCallForm::StringIsWellFormed, 1));
    const StubKey byName = tryCallMethodOfBuiltinClass(BoxedCallForm::StringIsWellFormed, 0);
    for (StubKey key : { byName, callBoxedBuiltin(BoxedCallForm::StringIsWellFormed, 0) }) {
        StubRun run = runner.runCall(key, callee.address(), narrowText.address(), { });
        AOT_TEST_CHECK(run.result == JSValue::ValueTrue && !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
    }
    for (uint64_t receiver : { wideText.address(), rope.address(), object.address(), int32InLanguageCallTests(1), static_cast<uint64_t>(JSValue::ValueUndefined) }) {
        StubRun run = runner.runCall(byName, callee.address(), receiver, { });
        AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
    }
}

// S1.
static void testSearchesFromPositionInLanguageCallTests()
{
    StubRunner runner;
    Ref<StringImpl> characters = StringImpl::create("abcdef"_s);
    Ref<StringImpl> charactersOfPart = StringImpl::create("cd"_s);
    TextInLanguageCallTests text;
    TextInLanguageCallTests rope;
    TextInLanguageCallTests part;
    FakeObject<16> object;
    FakeObject<sizeof(JSFunction)> callee;
    makeFlatInLanguageCallTests(text, characters.get());
    makeRopeInLanguageCallTests(rope, 9);
    makeFlatInLanguageCallTests(part, charactersOfPart.get());
    object.setHeader(0, ObjectType);
    constexpr uint64_t none = static_cast<uint64_t>(-1);

    struct Case {
        BoxedCallForm form;
        Entry operation;
        const TextInLanguageCallTests* receiver;
        int32_t position;
        uint32_t positionOfOperation;
        uint64_t answerOfOperation;
        uint64_t result;
    };
    const Case cases[] = {
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 0, 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 3, 3, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, -5, 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 6, 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 7, 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, std::numeric_limits<int32_t>::max(), 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, std::numeric_limits<int32_t>::min(), 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &rope, 100, 9, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, 2, 2, 1, JSValue::ValueTrue },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, -1, 0, 0, JSValue::ValueFalse },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, 7, 6, 0, JSValue::ValueFalse },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &text, 4, 4, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &text, -5, static_cast<uint32_t>(-5), none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &text, 100, 100, 2, int32InLanguageCallTests(2) },
    };
    for (const Case& testCase : cases) {
        AOT_TEST_CHECK(servesCallsByName(testCase.form, 1) && servesCallsByName(testCase.form, 2) && !acceptsArgumentCount(testCase.form, 3));
        runner.recordCallsOf(testCase.operation);
        runner.setResultOfRecordedOperation(testCase.answerOfOperation);
        for (StubKey key : { tryCallMethodOfBuiltinClass(testCase.form, 2), callBoxedBuiltin(testCase.form, 2) }) {
            StubRun run = runner.runCall(key, callee.address(), testCase.receiver->address(), { part.address(), int32InLanguageCallTests(testCase.position) });
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == testCase.result);
            AOT_TEST_CHECK(run.recordedArguments[0] == testCase.receiver->address() && run.recordedArguments[1] == part.address());
            AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[2]) == testCase.positionOfOperation);
        }
    }

    const uint64_t fraction = static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(1.5)));
    for (BoxedCallForm form : { BoxedCallForm::StringIndexOf, BoxedCallForm::StringStartsWith, BoxedCallForm::StringLastIndexOf }) {
        const StubKey key = tryCallMethodOfBuiltinClass(form, 2);
        const std::array<std::array<uint64_t, 3>, 6> leftAlone { {
            { text.address(), part.address(), fraction },
            { text.address(), part.address(), JSValue::ValueUndefined },
            { text.address(), part.address(), part.address() },
            { text.address(), object.address(), int32InLanguageCallTests(1) },
            { text.address(), int32InLanguageCallTests(1), int32InLanguageCallTests(1) },
            { object.address(), part.address(), int32InLanguageCallTests(1) },
        } };
        for (auto [receiver, searched, position] : leftAlone) {
            StubRun run = runner.runCall(key, callee.address(), receiver, { searched, position });
            AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
        }
    }
}

// P1.
static void testPushesOfSeveralValuesInLanguageCallTests()
{
    StubRunner runner;
    FakeObject<sizeof(JSArray)> list;
    FakeObject<sizeof(JSFunction)> callee;
    list.setHeader(0, ArrayType);
    list.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = IsPristine;
    constexpr uint64_t newLength = int32InLanguageCallTests(9);

    auto check = [&](Entry operation, std::initializer_list<uint64_t> values) {
        const unsigned count = static_cast<unsigned>(values.size());
        AOT_TEST_CHECK(acceptsArgumentCount(BoxedCallForm::ArrayPush, count) && servesCallsByName(BoxedCallForm::ArrayPush, count));
        runner.recordCallsOf(operation);
        runner.setResultOfRecordedOperation(newLength);
        for (StubKey key : { tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayPush, count), callBoxedBuiltin(BoxedCallForm::ArrayPush, count) }) {
            StubRun run = runner.runCall(key, callee.address(), list.address(), values);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == newLength && run.recordedArguments[0] == list.address());
            AOT_TEST_CHECK(std::ranges::equal(values, std::span(run.recordedArguments).subspan(1, count)));
        }
    };
    check(Entry::operationAOTArrayPushTwo, { int32InLanguageCallTests(1), int32InLanguageCallTests(2) });
    check(Entry::operationAOTArrayPushThree, { int32InLanguageCallTests(1), int32InLanguageCallTests(2), int32InLanguageCallTests(3) });
    check(Entry::operationAOTArrayPushFour, { int32InLanguageCallTests(1), int32InLanguageCallTests(2), int32InLanguageCallTests(3), int32InLanguageCallTests(4) });
    AOT_TEST_CHECK(!acceptsArgumentCount(BoxedCallForm::ArrayPush, 0));

    // A list that has names of its own may have a `push` of its own.
    list.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = 0;
    StubRun run = runner.runCall(tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayPush, 2), callee.address(), list.address(), { int32InLanguageCallTests(1), int32InLanguageCallTests(2) });
    AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
}

static unsigned numberOfHostCallsInLanguageCallTests;

static JSC_DECLARE_HOST_FUNCTION(countHostCallInLanguageCallTests);

JSC_DEFINE_HOST_FUNCTION(countHostCallInLanguageCallTests, (JSGlobalObject*, CallFrame*))
{
    ++numberOfHostCallsInLanguageCallTests;
    return static_cast<EncodedJSValue>(resultOfHostFunctionInLanguageCallTests);
}

// B1.
static void testBooleanCalledByValueInLanguageCallTests()
{
    StubRunner runner;
    Instance& instance = runner.instance();
    std::array<FakeObject<sizeof(Structure)>, 2> structures;
    FakeObject<16> classInfoOfBoolean;
    FakeObject<16> classInfoOfOtherFunction;
    instance.prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    AOT_TEST_CHECK((structures[1].address() & ~static_cast<uint64_t>(UINT32_MAX)) == instance.prefix().structureIDBase);
    structures[0].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfBoolean.address();
    structures[1].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfOtherFunction.address();
    StubEnvironment::of(&instance).runtimeTable[indexOf(Entry::BooleanConstructorClassInfo)] = std::bit_cast<void*>(static_cast<uintptr_t>(classInfoOfBoolean.address()));

    std::array<FakeObject<sizeof(InternalFunction)>, 2> functions;
    for (size_t i = 0; i < functions.size(); ++i) {
        functions[i].setHeader(static_cast<uint32_t>(structures[i].address()), InternalFunctionType);
        functions[i].at<const void*>(InternalFunction::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(countHostCallInLanguageCallTests).taggedPtr();
    }
    const uint64_t boolean = functions[0].address();
    const uint64_t otherFunction = functions[1].address();

    Ref<StringImpl> characters = StringImpl::create("x"_s);
    TextInLanguageCallTests text;
    TextInLanguageCallTests emptyText;
    TextInLanguageCallTests rope;
    FakeObject<16> object;
    FakeObject<16> objectThatPassesForUndefined;
    FakeObject<16> symbol;
    FakeObject<16> bigInt;
    makeFlatInLanguageCallTests(text, characters.get());
    makeFlatInLanguageCallTests(emptyText, *StringImpl::empty());
    makeRopeInLanguageCallTests(rope, 3);
    object.setHeader(0, ObjectType);
    objectThatPassesForUndefined.setHeader(0, ObjectType);
    objectThatPassesForUndefined.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = MasqueradesAsUndefined;
    symbol.setHeader(0, SymbolType);
    bigInt.setHeader(0, HeapBigIntType);
    constexpr uint64_t thisValue = JSValue::ValueUndefined;

    const std::pair<uint64_t, bool> answered[] = {
        { JSValue::ValueTrue, true }, { JSValue::ValueFalse, false }, { JSValue::ValueUndefined, false }, { JSValue::ValueNull, false },
        { int32InLanguageCallTests(0), false }, { int32InLanguageCallTests(1), true }, { int32InLanguageCallTests(-1), true }, { int32InLanguageCallTests(std::numeric_limits<int32_t>::min()), true },
        { object.address(), true }, { text.address(), true }, { emptyText.address(), false }, { rope.address(), true },
    };
    for (auto [value, answer] : answered) {
        const uint64_t expected = answer ? JSValue::ValueTrue : JSValue::ValueFalse;
        numberOfHostCallsInLanguageCallTests = 0;
        AOT_TEST_CHECK(runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 1), boolean, thisValue, { value }).result == expected);
        AOT_TEST_CHECK(runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 3), boolean, thisValue, { value, int32InLanguageCallTests(0), object.address() }).result == expected);
        AOT_TEST_CHECK(!numberOfHostCallsInLanguageCallTests);
        AOT_TEST_CHECK(runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 1), otherFunction, thisValue, { value }).result == resultOfHostFunctionInLanguageCallTests);
        AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 1);
    }
    numberOfHostCallsInLanguageCallTests = 0;
    AOT_TEST_CHECK(runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 0), boolean, thisValue, { }).result == JSValue::ValueFalse);
    AOT_TEST_CHECK(!numberOfHostCallsInLanguageCallTests);

    for (uint64_t value : { static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(0.5))), static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(-0.0))), static_cast<uint64_t>(JSValue::encode(jsNaN())), objectThatPassesForUndefined.address(), symbol.address(), bigInt.address() }) {
        numberOfHostCallsInLanguageCallTests = 0;
        AOT_TEST_CHECK(runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 1), boolean, thisValue, { value }).result == resultOfHostFunctionInLanguageCallTests);
        AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 1);
    }
}

std::span<const NativeTest> stubBoxedCallsOfLanguageTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/boxed-calls-of-language/exec-and-match-go-to-what-matches"_s, testFormsThatMatchInLanguageCallTests },
        { "stubs/boxed-calls-of-language/narrow-flat-text-is-well-formed"_s, ImageTests::whenExecutable<testIsWellFormedInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/search-from-a-position-lies-in-the-text"_s, ImageTests::whenExecutable<testSearchesFromPositionInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/several-values-are-pushed-by-one-operation"_s, ImageTests::whenExecutable<testPushesOfSeveralValuesInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/boolean-called-by-value-takes-no-frame"_s, ImageTests::whenExecutable<testBooleanCalledByValueInLanguageCallTests> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
