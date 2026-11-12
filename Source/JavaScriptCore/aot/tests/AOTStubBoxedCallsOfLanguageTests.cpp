/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTStubRunner.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCalls.h"
#include "AOTStubsRegExp.h"
#include "InternalFunction.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSString.h"
#include "JSTypeInfo.h"
#include "NativeExecutable.h"
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

static void makePieceInLanguageCallTests(TextInLanguageCallTests& cell, const TextInLanguageCallTests& base, uint64_t offset, uint64_t length)
{
    using CompactFibers = JSRopeString::CompactFibers;
    cell.setHeader(0, StringType);
    cell.at<uint64_t>(sizeof(uint64_t)) = JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    cell.at<uint64_t>(2 * sizeof(uint64_t)) = base.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | length;
    cell.at<uint64_t>(3 * sizeof(uint64_t)) = base.address() >> CompactFibers::widthOfFiber1LowerInLengthWord | offset << CompactFibers::shiftOfFiber2InUpperWord;
}

static void makeJoinedInLanguageCallTests(TextInLanguageCallTests& cell, uint64_t length, std::initializer_list<const TextInLanguageCallTests*> parts)
{
    using CompactFibers = JSRopeString::CompactFibers;
    std::array<uint64_t, 3> fibers { };
    std::ranges::transform(parts, fibers.begin(), [](const TextInLanguageCallTests* part) {
        return part->address();
    });
    cell.setHeader(0, StringType);
    cell.at<uint64_t>(sizeof(uint64_t)) = fibers[0] | JSString::isRopeInPointer | JSRopeString::is8BitInPointer;
    cell.at<uint64_t>(2 * sizeof(uint64_t)) = fibers[1] << CompactFibers::shiftOfFiber1LowerInLengthWord | length;
    cell.at<uint64_t>(3 * sizeof(uint64_t)) = fibers[1] >> CompactFibers::widthOfFiber1LowerInLengthWord | fibers[2] << CompactFibers::shiftOfFiber2InUpperWord;
}

static bool refersToInLanguageCallTests(const StubCatalog& catalog, StubKey from, StubKey target)
{
    return catalog.recordingOf(bodyOf(from)).references.containsIf([&](const StubReference& reference) {
        return reference.target == target;
    });
}

static void testFormsThatMatchInLanguageCallTests()
{
    StubCatalog matches(StubOptions { });
    StubCatalog matchesNothing(StubOptions { .matchesRegExps = false });
    for (StubKey key : { callBoxedBuiltin(BoxedCallForm::RegExpExec, 1), callMethodOfBuiltinClass(BoxedCallForm::RegExpExec, 1), tryCallMethodOfBuiltinClass(BoxedCallForm::RegExpExec, 1) }) {
        AOT_TEST_CHECK(refersToInLanguageCallTests(matches, key, regExpMatch(RegExpMatchResult::Array)));
        AOT_TEST_CHECK(!refersToInLanguageCallTests(matchesNothing, key, regExpMatch(RegExpMatchResult::Array)));
    }
    for (StubKey key : { callBoxedBuiltin(BoxedCallForm::StringMatch, 1), callMethodOfBuiltinClass(BoxedCallForm::StringMatch, 1), tryCallMethodOfBuiltinClass(BoxedCallForm::StringMatch, 1) }) {
        AOT_TEST_CHECK(refersToInLanguageCallTests(matches, key, regExpMatch(RegExpMatchResult::ArrayForStringMatch)));
        AOT_TEST_CHECK(!refersToInLanguageCallTests(matchesNothing, key, regExpMatch(RegExpMatchResult::ArrayForStringMatch)));
    }
    for (BoxedCallForm form : { BoxedCallForm::RegExpExec, BoxedCallForm::StringMatch }) {
        AOT_TEST_CHECK(servesCallsByName(form, 1));
        for (unsigned count : { 0u, 2u, 3u, 4u })
            AOT_TEST_CHECK(!acceptsArgumentCount(form, count) && !servesCallsByName(form, count));
    }
}

static void testCallsOfBuiltinsThatProgramCannotHoldInLanguageCallTests()
{
    StubCatalog holdsAll(StubOptions { });
    StubOptions options { .callsNumberOrStringByValue = false, .callsArrayByValue = false };
    options.boxedCallsOutOfReach.setAll();
    StubCatalog holdsNone(options);
    auto uses = [](const StubCatalog& catalog, StubKey key, Entry entry) {
        return catalog.recordingOf(bodyOf(key)).entriesUsed.get(static_cast<size_t>(entry));
    };
    StubKey then = callBoxedBuiltin(BoxedCallForm::PromiseThen, 2);
    AOT_TEST_CHECK(uses(holdsAll, then, Entry::operationAOTPromiseThen) && !uses(holdsNone, then, Entry::operationAOTPromiseThen));
    StubKey constructor = callInternalFunction(CodeSpecializationKind::CodeForCall, 1);
    for (Entry entry : { Entry::operationAOTNumberOfAnyValue, Entry::operationAOTStringOfAnyValue })
        AOT_TEST_CHECK(uses(holdsAll, constructor, entry) && !uses(holdsNone, constructor, entry));
    const StubKey makesArray = newArrayOfLength(ArrayElementKind::Contiguous);
    AOT_TEST_CHECK(refersToInLanguageCallTests(holdsAll, constructor, makesArray) && !refersToInLanguageCallTests(holdsNone, constructor, makesArray));
    for (unsigned form = 0; form < numberOfBuiltinBoxedStubs; ++form) {
        for (unsigned count = BoxedCallEntry::minArgumentCount; count <= BoxedCallEntry::maxArgumentCount; ++count) {
            StubKey key = callBoxedBuiltin(static_cast<BoxedCallForm>(form), count);
            AOT_TEST_CHECK(holdsNone.recordingOf(bodyOf(key)).entriesUsed.isEmpty());
            AOT_TEST_CHECK(refersToInLanguageCallTests(holdsNone, key, hostCallLeftTo(static_cast<BoxedCallForm>(form), count)));
        }
    }
}

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

    constexpr uint32_t slimClosureStructureID = 0x1080;
    runner.instance().slimClosureHeaders[0] = slimClosureStructureID;
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> method;
    method.setHeader(slimClosureStructureID, SlimClosureType);
    method.at<uint64_t>(offsetOfSlimClosureWord) = runner.closureWord(0);
    TextInLanguageCallTests cellOfOtherKind;
    makeFlatInLanguageCallTests(cellOfOtherKind, narrow.get());
    cellOfOtherKind.setHeader(0, ObjectType);
    const StubKey mayBeUnread = callMethodThatMayBeUnread(BoxedCallForm::StringIsWellFormed, 0);
    StubRun unread = runner.runCall(mayBeUnread, JSValue::ValueEmpty, narrowText.address(), { });
    AOT_TEST_CHECK(unread.result == JSValue::ValueTrue && !unread.calledOperation && unread.landedCall.place == LandedCall::Place::Nowhere);
    runner.recordCallsOf(Entry::operationAOTMethodCalledBoxed);
    runner.setResultOfRecordedOperation(method.address());
    for (uint64_t receiver : { cellOfOtherKind.address(), int32InLanguageCallTests(1), static_cast<uint64_t>(JSValue::ValueUndefined) }) {
        StubRun run = runner.runCall(mayBeUnread, JSValue::ValueEmpty, receiver, { });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.landedCall.place == LandedCall::Place::CompiledFunction);
        AOT_TEST_CHECK(run.landedCall.values.callee == method.address() && run.landedCall.values.thisValue == receiver);
    }
}

static void testSearchesFromPositionInLanguageCallTests()
{
    StubRunner runner;
    constexpr std::array<char16_t, 6> wideCharacters { 'a', 'b', 'c', 'd', 'e', 0x100 };
    Ref<StringImpl> characters = StringImpl::create(std::span<const char16_t>(wideCharacters));
    Ref<StringImpl> charactersOfPart = *StringImpl::empty();
    TextInLanguageCallTests text;
    TextInLanguageCallTests rope;
    TextInLanguageCallTests shortRope;
    TextInLanguageCallTests part;
    FakeObject<16> object;
    FakeObject<sizeof(JSFunction)> callee;
    makeFlatInLanguageCallTests(text, characters.get());
    makeRopeInLanguageCallTests(rope, 9);
    makeRopeInLanguageCallTests(shortRope, 6);
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
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, 0, 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, 3, 3, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, -5, 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, 6, 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, 7, 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, std::numeric_limits<int32_t>::max(), 6, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &shortRope, std::numeric_limits<int32_t>::min(), 0, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &rope, 100, 9, none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &shortRope, 2, 2, 1, JSValue::ValueTrue },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &shortRope, -1, 0, 0, JSValue::ValueFalse },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &shortRope, 7, 6, 0, JSValue::ValueFalse },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &shortRope, 4, 4, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &shortRope, -5, static_cast<uint32_t>(-5), none, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &shortRope, 100, 100, 2, int32InLanguageCallTests(2) },
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

    const Case answeredByStub[] = {
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 6, 0, none, int32InLanguageCallTests(6) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, 7, 0, none, int32InLanguageCallTests(6) },
        { BoxedCallForm::StringIndexOf, Entry::operationAOTStringIndexOfFrom, &text, std::numeric_limits<int32_t>::max(), 0, none, int32InLanguageCallTests(6) },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, 2, 0, 0, JSValue::ValueTrue },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, -1, 0, 0, JSValue::ValueTrue },
        { BoxedCallForm::StringStartsWith, Entry::operationAOTStringHasAt, &text, 7, 0, 0, JSValue::ValueTrue },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &text, 0, 0, none, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, Entry::operationAOTStringLastIndexOf, &text, -5, 0, none, int32InLanguageCallTests(0) },
    };
    for (const Case& testCase : answeredByStub) {
        runner.recordCallsOf(testCase.operation);
        runner.setResultOfRecordedOperation(testCase.answerOfOperation);
        for (StubKey key : { tryCallMethodOfBuiltinClass(testCase.form, 2), callBoxedBuiltin(testCase.form, 2) }) {
            StubRun run = runner.runCall(key, callee.address(), testCase.receiver->address(), { part.address(), int32InLanguageCallTests(testCase.position) });
            AOT_TEST_CHECK(!run.numberOfRecordedCalls && run.result == testCase.result);
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

static void testTextAtOnePlaceInLanguageCallTests()
{
    StubRunner runner;
    std::array<char, 40> manyCharacters;
    manyCharacters.fill('a');
    constexpr std::array<char16_t, 2> wideCharacters { 'e', 'f' };
    Ref<StringImpl> impls[] = {
        StringImpl::create("abcdef"_s), StringImpl::create("ab"_s), StringImpl::create("cd"_s), StringImpl::create("ef"_s), Ref<StringImpl> { *StringImpl::empty() }, StringImpl::create("abcdefg"_s), StringImpl::create("xabcdef"_s),
        StringImpl::create(std::span<const char>(manyCharacters)), StringImpl::create(std::span<const char>(manyCharacters).first(32)), StringImpl::create(std::span<const char>(manyCharacters).first(33)),
        StringImpl::create(std::span<const char16_t>(wideCharacters)),
    };
    enum Text : unsigned { Whole, Head, Middle, Tail, Nothing, Longer, Shifted, Forty, ThirtyTwo, ThirtyThree, Wide, Rope, PieceWhole, PieceTail, PieceOfWide, HeadAndMiddle, MiddleAndTail, HeadMiddleAndTail, HeadAndTail, MiddleAndPieceTail, ThirtyTwoOfTwoParts, ThirtyTwoAndHead, MiddleAndWide, HeadAndJoined, WholeOfThreeParts, HeadMiddleAndHead, numberOfTexts };
    std::array<TextInLanguageCallTests, numberOfTexts> texts;
    for (unsigned index = 0; index < std::size(impls); ++index)
        makeFlatInLanguageCallTests(texts[index], impls[index].get());
    makePieceInLanguageCallTests(texts[PieceWhole], texts[Shifted], 1, 6);
    makePieceInLanguageCallTests(texts[PieceTail], texts[Shifted], 5, 2);
    makePieceInLanguageCallTests(texts[PieceOfWide], texts[Wide], 0, 1);
    Ref<StringImpl> sixteen = StringImpl::create(std::span<const char>(manyCharacters).first(16));
    TextInLanguageCallTests textOfSixteen;
    makeFlatInLanguageCallTests(textOfSixteen, sixteen.get());
    makeJoinedInLanguageCallTests(texts[HeadAndMiddle], 4, { &texts[Head], &texts[Middle] });
    makeJoinedInLanguageCallTests(texts[MiddleAndTail], 4, { &texts[Middle], &texts[Tail] });
    makeJoinedInLanguageCallTests(texts[HeadMiddleAndTail], 6, { &texts[Head], &texts[Middle], &texts[Tail] });
    makeJoinedInLanguageCallTests(texts[HeadAndTail], 4, { &texts[Head], &texts[Tail] });
    makeJoinedInLanguageCallTests(texts[MiddleAndPieceTail], 4, { &texts[Middle], &texts[PieceTail] });
    makeJoinedInLanguageCallTests(texts[ThirtyTwoOfTwoParts], 32, { &textOfSixteen, &textOfSixteen });
    makeJoinedInLanguageCallTests(texts[ThirtyTwoAndHead], 18, { &textOfSixteen, &texts[Head] });
    makeJoinedInLanguageCallTests(texts[MiddleAndWide], 4, { &texts[Middle], &texts[Wide] });
    Ref<StringImpl> one = StringImpl::create("e"_s);
    Ref<StringImpl> other = StringImpl::create("f"_s);
    TextInLanguageCallTests textOfOne;
    TextInLanguageCallTests textOfOther;
    TextInLanguageCallTests joinedOfBoth;
    makeFlatInLanguageCallTests(textOfOne, one.get());
    makeFlatInLanguageCallTests(textOfOther, other.get());
    makeJoinedInLanguageCallTests(joinedOfBoth, 2, { &textOfOne, &textOfOther });
    makeJoinedInLanguageCallTests(texts[Rope], 2, { &joinedOfBoth, &texts[Nothing] });
    makeJoinedInLanguageCallTests(texts[HeadAndJoined], 6, { &texts[Head], &texts[MiddleAndTail] });
    makeJoinedInLanguageCallTests(texts[WholeOfThreeParts], 6, { &texts[Head], &texts[Middle], &texts[Tail] });
    makeJoinedInLanguageCallTests(texts[HeadMiddleAndHead], 6, { &texts[Head], &texts[Middle], &texts[Head] });
    FakeObject<sizeof(JSFunction)> callee;
    constexpr uint64_t yes = JSValue::ValueTrue;
    constexpr uint64_t no = JSValue::ValueFalse;
    constexpr int32_t unused = 0;

    struct Case {
        BoxedCallForm form;
        unsigned count;
        Text receiver;
        Text sought;
        int32_t position;
        uint64_t result;
    };
    const Case answeredByStub[] = {
        { BoxedCallForm::StringStartsWith, 1, Whole, Head, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, Middle, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Whole, Nothing, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, Whole, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, Longer, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Nothing, Nothing, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Forty, ThirtyTwo, unused, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Middle, 2, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Middle, 3, no },
        { BoxedCallForm::StringStartsWith, 2, Whole, Tail, 4, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Tail, 5, no },
        { BoxedCallForm::StringStartsWith, 2, Whole, Nothing, 6, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Nothing, 100, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Middle, -1, no },
        { BoxedCallForm::StringStartsWith, 2, Whole, Head, -7, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Tail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Middle, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Whole, Nothing, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Whole, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Shifted, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Shifted, Whole, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Longer, Whole, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Forty, ThirtyTwo, unused, yes },
        { BoxedCallForm::StringIndexOf, 2, Whole, Tail, 4, int32InLanguageCallTests(4) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Middle, 4, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Tail, 5, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Tail, 100, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Whole, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Whole, -3, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Longer, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Shifted, Whole, 1, int32InLanguageCallTests(1) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Nothing, 6, int32InLanguageCallTests(6) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Nothing, 100, int32InLanguageCallTests(6) },
        { BoxedCallForm::StringIndexOf, 2, Nothing, Nothing, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Head, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Middle, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Head, -3, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Nothing, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Longer, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Whole, std::numeric_limits<int32_t>::min(), int32InLanguageCallTests(0) },
        { BoxedCallForm::StringEndsWith, 1, Whole, PieceTail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, PieceWhole, Tail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, PieceWhole, Middle, unused, no },
        { BoxedCallForm::StringEndsWith, 1, PieceWhole, PieceTail, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, PieceWhole, Head, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, PieceWhole, PieceTail, unused, no },
        { BoxedCallForm::StringStartsWith, 2, PieceWhole, PieceTail, 4, yes },
        { BoxedCallForm::StringIndexOf, 2, PieceWhole, PieceTail, 4, int32InLanguageCallTests(4) },
        { BoxedCallForm::StringLastIndexOf, 2, PieceWhole, Head, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringStartsWith, 1, Wide, Whole, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Wide, Nothing, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Wide, Nothing, unused, yes },
        { BoxedCallForm::StringIndexOf, 2, Wide, Nothing, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringLastIndexOf, 2, Wide, Nothing, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringStartsWith, 2, Whole, Wide, 5, no },
        { BoxedCallForm::StringEndsWith, 1, Rope, Whole, unused, no },
        { BoxedCallForm::StringIndexOf, 2, Rope, Whole, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Wide, 5, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, 2, Wide, Whole, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringStartsWith, 1, Whole, HeadAndMiddle, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, MiddleAndTail, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Whole, HeadAndTail, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Whole, HeadMiddleAndTail, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Longer, HeadMiddleAndTail, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, HeadMiddleAndHead, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Shifted, HeadMiddleAndHead, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Shifted, HeadMiddleAndTail, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Forty, ThirtyTwoOfTwoParts, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Forty, ThirtyTwoAndHead, unused, no },
        { BoxedCallForm::StringStartsWith, 2, Whole, MiddleAndTail, 2, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, MiddleAndTail, 1, no },
        { BoxedCallForm::StringStartsWith, 2, Whole, MiddleAndTail, 3, no },
        { BoxedCallForm::StringStartsWith, 2, PieceWhole, MiddleAndPieceTail, 2, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, MiddleAndTail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, HeadAndMiddle, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Whole, HeadAndTail, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Whole, MiddleAndPieceTail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Shifted, HeadMiddleAndTail, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Forty, ThirtyTwoOfTwoParts, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Head, HeadAndMiddle, unused, no },
        { BoxedCallForm::StringIndexOf, 2, Whole, MiddleAndTail, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, 2, Whole, HeadAndTail, 2, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringIndexOf, 2, Shifted, HeadMiddleAndTail, 1, int32InLanguageCallTests(1) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, HeadAndMiddle, 0, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, MiddleAndTail, 0, int32InLanguageCallTests(-1) },
    };
    const Case leftToOperation[] = {
        { BoxedCallForm::StringStartsWith, 1, Forty, ThirtyThree, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, Wide, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Whole, Rope, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Rope, Head, unused, no },
        { BoxedCallForm::StringStartsWith, 1, Whole, PieceOfWide, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Whole, PieceOfWide, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Whole, MiddleAndWide, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, Whole, HeadAndJoined, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, HeadAndJoined, unused, yes },
        { BoxedCallForm::StringStartsWith, 1, WholeOfThreeParts, Head, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, WholeOfThreeParts, MiddleAndTail, unused, yes },
        { BoxedCallForm::StringStartsWith, 2, Forty, ThirtyThree, 1, yes },
        { BoxedCallForm::StringStartsWith, 2, Whole, Wide, 4, yes },
        { BoxedCallForm::StringEndsWith, 1, Forty, ThirtyThree, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Wide, unused, yes },
        { BoxedCallForm::StringEndsWith, 1, Whole, Rope, unused, no },
        { BoxedCallForm::StringEndsWith, 1, Rope, Tail, unused, no },
        { BoxedCallForm::StringIndexOf, 2, Whole, Tail, 3, int32InLanguageCallTests(4) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Nothing, 2, int32InLanguageCallTests(2) },
        { BoxedCallForm::StringIndexOf, 2, Whole, Wide, 4, int32InLanguageCallTests(4) },
        { BoxedCallForm::StringIndexOf, 2, Forty, ThirtyThree, 7, int32InLanguageCallTests(7) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Head, 1, int32InLanguageCallTests(0) },
        { BoxedCallForm::StringLastIndexOf, 2, Whole, Wide, 0, int32InLanguageCallTests(-1) },
        { BoxedCallForm::StringLastIndexOf, 2, Rope, Head, 0, int32InLanguageCallTests(-1) },
    };
    auto operationOf = [](BoxedCallForm form) {
        switch (form) {
        case BoxedCallForm::StringStartsWith:
            return Entry::operationAOTStringHasAt;
        case BoxedCallForm::StringEndsWith:
            return Entry::operationAOTStringEndsWith;
        case BoxedCallForm::StringIndexOf:
            return Entry::operationAOTStringIndexOfFrom;
        default:
            return Entry::operationAOTStringLastIndexOf;
        }
    };
    auto answerInFormOfOperation = [](BoxedCallForm form, uint64_t result) -> uint64_t {
        if (form == BoxedCallForm::StringStartsWith)
            return result == yes;
        if (form == BoxedCallForm::StringEndsWith)
            return result;
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(result)));
    };
    auto otherAnswer = [](BoxedCallForm form, uint64_t result) -> uint64_t {
        if (form == BoxedCallForm::StringStartsWith || form == BoxedCallForm::StringEndsWith)
            return result == yes ? no : yes;
        return int32InLanguageCallTests(static_cast<int32_t>(result) + 2);
    };
    auto run = [&](const Case& testCase, StubKey key) {
        if (testCase.count == 1)
            return runner.runCall(key, callee.address(), texts[testCase.receiver].address(), { texts[testCase.sought].address() });
        return runner.runCall(key, callee.address(), texts[testCase.receiver].address(), { texts[testCase.sought].address(), int32InLanguageCallTests(testCase.position) });
    };
    for (const Case& testCase : answeredByStub) {
        runner.recordCallsOf(operationOf(testCase.form));
        runner.setResultOfRecordedOperation(answerInFormOfOperation(testCase.form, otherAnswer(testCase.form, testCase.result)));
        for (StubKey key : { tryCallMethodOfBuiltinClass(testCase.form, testCase.count), callBoxedBuiltin(testCase.form, testCase.count) }) {
            StubRun answer = run(testCase, key);
            AOT_TEST_CHECK(!answer.numberOfRecordedCalls && answer.result == testCase.result);
        }
    }
    for (const Case& testCase : leftToOperation) {
        runner.recordCallsOf(operationOf(testCase.form));
        runner.setResultOfRecordedOperation(answerInFormOfOperation(testCase.form, testCase.result));
        for (StubKey key : { tryCallMethodOfBuiltinClass(testCase.form, testCase.count), callBoxedBuiltin(testCase.form, testCase.count) }) {
            StubRun answer = run(testCase, key);
            AOT_TEST_CHECK(answer.numberOfRecordedCalls == 1 && answer.result == testCase.result);
            AOT_TEST_CHECK(answer.recordedArguments[0] == texts[testCase.receiver].address() && answer.recordedArguments[1] == texts[testCase.sought].address());
        }
    }
}

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

    list.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = 0;
    StubRun run = runner.runCall(tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayPush, 2), callee.address(), list.address(), { int32InLanguageCallTests(1), int32InLanguageCallTests(2) });
    AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
}

namespace {

template<uint32_t numberOfWords>
struct ListOfCapacityInLanguageCallTests {
    static constexpr uint32_t capacity = numberOfWords;

    ListOfCapacityInLanguageCallTests(IndexingType shapeAndWritability, std::span<const uint64_t> values, uint8_t flags = IsPristine, uint32_t structureID = 0)
    {
        AOT_TEST_CHECK(values.size() <= capacity);
        cell.setHeader(structureID, ArrayType);
        cell.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = IsArray | shapeAndWritability;
        cell.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = flags;
        cell.at<uintptr_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(&words[1]);
        words.fill(0);
        words[0] = static_cast<uint64_t>(capacity) << 32 | values.size();
        std::ranges::copy(values, words.begin() + 1);
    }

    uint32_t length() const { return static_cast<uint32_t>(words[0]); }
    std::span<const uint64_t> elements() const { return std::span(words).subspan(1); }

    FakeObject<sizeof(JSArray)> cell;
    alignas(16) std::array<uint64_t, capacity + 1> words;
};
using ListInLanguageCallTests = ListOfCapacityInLanguageCallTests<40>;
static_assert(Butterfly::offsetOfPublicLength() == -8 && Butterfly::offsetOfVectorLength() == -4);

} // anonymous namespace

static Vector<uint64_t, 20> integersInLanguageCallTests(uint32_t count)
{
    Vector<uint64_t, 20> values;
    for (uint32_t number = 1; number <= count; ++number)
        values.append(int32InLanguageCallTests(static_cast<int32_t>(number)));
    return values;
}

static void testShiftOfFewValuesInLanguageCallTests()
{
    StubRunner runner;
    FakeObject<sizeof(JSFunction)> callee;
    constexpr StubKey key = callBoxedBuiltin(BoxedCallForm::ArrayShift, 0);
    constexpr uint64_t answerOfOperation = int32InLanguageCallTests(99);
    runner.install(Entry::operationAOTMoveBytes, operationAOTMoveBytes);
    runner.recordCallsOf(Entry::operationAOTArrayShiftGeneric);
    runner.setResultOfRecordedOperation(answerOfOperation);

    auto isShiftedByStub = [&](auto& list, uint64_t first) {
        Vector<uint64_t, 20> before;
        before.append(list.elements().first(list.length()));
        StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { });
        AOT_TEST_CHECK(!run.calledOperation && run.result == first && list.length() + 1 == before.size());
        AOT_TEST_CHECK(std::ranges::equal(list.elements().first(list.length()), before.span().subspan(1)) && !list.elements()[list.length()]);
    };
    auto isLeftToOperation = [&](auto& list) {
        Vector<uint64_t, 20> before;
        before.append(list.elements().first(list.length()));
        StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == answerOfOperation && run.recordedArguments[0] == list.cell.address());
        AOT_TEST_CHECK(list.length() == before.size() && std::ranges::equal(list.elements().first(list.length()), before.span()));
    };

    for (IndexingType shape : { Int32Shape, ContiguousShape }) {
        for (uint32_t count : { 1u, 2u, 3u, 9u, 17u, 18u, 40u }) {
            ListInLanguageCallTests list(shape, integersInLanguageCallTests(count).span());
            isShiftedByStub(list, int32InLanguageCallTests(1));
        }
        using LongList = ListOfCapacityInLanguageCallTests<JSArray::shiftThreshold + 1>;
        LongList longestMoved(shape, integersInLanguageCallTests(JSArray::shiftThreshold).span());
        isShiftedByStub(longestMoved, int32InLanguageCallTests(1));
        LongList ofOneMore(shape, integersInLanguageCallTests(JSArray::shiftThreshold + 1).span());
        isLeftToOperation(ofOneMore);

        ListInLanguageCallTests empty(shape, { });
        StubRun run = runner.runCall(key, callee.address(), empty.cell.address(), { });
        AOT_TEST_CHECK(!run.calledOperation && run.result == static_cast<uint64_t>(JSValue::encode(jsUndefined())) && !empty.length());

        const std::array<uint64_t, 3> holeInFront { 0, int32InLanguageCallTests(2), int32InLanguageCallTests(3) };
        ListInLanguageCallTests withHoleInFront(shape, holeInFront);
        isLeftToOperation(withHoleInFront);

        const std::array<uint64_t, 3> holeBehind { int32InLanguageCallTests(1), 0, int32InLanguageCallTests(3) };
        ListInLanguageCallTests withHoleBehind(shape, holeBehind);
        isShiftedByStub(withHoleBehind, int32InLanguageCallTests(1));
        ListInLanguageCallTests withHoleBehindThatMayLookUp(shape, holeBehind, 0);
        isLeftToOperation(withHoleBehindThatMayLookUp);
        ListInLanguageCallTests aloneThoughNotPristine(shape, integersInLanguageCallTests(1).span(), 0);
        isShiftedByStub(aloneThoughNotPristine, int32InLanguageCallTests(1));

        ListInLanguageCallTests copiedWhenWritten(shape | CopyOnWrite, integersInLanguageCallTests(2).span());
        isLeftToOperation(copiedWhenWritten);
    }
    ListInLanguageCallTests ofDoubles(DoubleShape, integersInLanguageCallTests(2).span());
    isLeftToOperation(ofDoubles);

    runner.instance().mutatorShouldBeFenced = true;
    for (uint32_t count : { 3u, 18u }) {
        ListInLanguageCallTests ofValuesWhileCollectorMayVisit(ContiguousShape, integersInLanguageCallTests(count).span());
        isLeftToOperation(ofValuesWhileCollectorMayVisit);
    }
    ListInLanguageCallTests ofIntegersWhileCollectorMayVisit(Int32Shape, integersInLanguageCallTests(3).span());
    isShiftedByStub(ofIntegersWhileCollectorMayVisit, int32InLanguageCallTests(1));
    ListInLanguageCallTests aloneWhileCollectorMayVisit(ContiguousShape, integersInLanguageCallTests(1).span());
    isShiftedByStub(aloneWhileCollectorMayVisit, int32InLanguageCallTests(1));
    runner.instance().mutatorShouldBeFenced = false;
}

static void testSearchesOfListsInLanguageCallTests()
{
    using LongList = ListOfCapacityInLanguageCallTests<72>;
    StubRunner runner;
    FakeObject<sizeof(JSFunction)> callee;
    auto isAnswered = [&](auto& list, uint64_t sought, std::optional<int32_t> place) {
        for (StubKey key : { tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIndexOf, 1), callBoxedBuiltin(BoxedCallForm::ArrayIndexOf, 1) }) {
            StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == int32InLanguageCallTests(place.value_or(-1)) && !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
        }
        for (StubKey key : { tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIncludes, 1), callBoxedBuiltin(BoxedCallForm::ArrayIncludes, 1) }) {
            StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == static_cast<uint64_t>(place ? JSValue::ValueTrue : JSValue::ValueFalse) && !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
        }
    };
    auto isLeftToHostFunction = [&](auto& list, uint64_t sought) {
        for (BoxedCallForm form : { BoxedCallForm::ArrayIndexOf, BoxedCallForm::ArrayIncludes }) {
            StubRun run = runner.runCall(tryCallMethodOfBuiltinClass(form, 1), callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
        }
    };

    constexpr uint32_t numberOfTexts = 70;
    Vector<Ref<StringImpl>> characters;
    std::array<TextInLanguageCallTests, numberOfTexts + 2> texts;
    auto makeText = [&](TextInLanguageCallTests& cell, uint32_t number) {
        const std::array<char, 3> letters { 'k', static_cast<char>('0' + number / 10), static_cast<char>('0' + number % 10) };
        characters.append(StringImpl::create(std::span<const char>(letters)));
        makeFlatInLanguageCallTests(cell, characters.last().get());
    };
    Vector<uint64_t, 20> cells;
    for (uint32_t index = 0; index < numberOfTexts; ++index) {
        makeText(texts[index], index);
        cells.append(texts[index].address());
    }
    TextInLanguageCallTests& lastInAnotherCell = texts[numberOfTexts];
    TextInLanguageCallTests& absent = texts[numberOfTexts + 1];
    makeText(lastInAnotherCell, numberOfTexts - 1);
    makeText(absent, 99);
    for (uint32_t count : { 32u, 33u, 64u, 65u, numberOfTexts }) {
        LongList list(ContiguousShape, cells.span().first(count));
        isAnswered(list, texts[count - 1].address(), count - 1);
        isAnswered(list, absent.address(), std::nullopt);
    }
    LongList allTexts(ContiguousShape, cells.span());
    isAnswered(allTexts, lastInAnotherCell.address(), numberOfTexts - 1);

    LongList mostWords(Int32Shape, integersInLanguageCallTests(64).span());
    LongList tooManyWords(Int32Shape, integersInLanguageCallTests(65).span());
    isAnswered(mostWords, int32InLanguageCallTests(64), 63);
    isAnswered(mostWords, int32InLanguageCallTests(65), std::nullopt);
    isLeftToHostFunction(tooManyWords, int32InLanguageCallTests(65));

}

static void testSearchesOfListsWithoutBoxedValuesInLanguageCallTests()
{
    using LongList = ListOfCapacityInLanguageCallTests<72>;
    StubRunner runner;
    FakeObject<sizeof(JSFunction)> callee;
    Ref<StringImpl> characters = StringImpl::create("12"_s);
    TextInLanguageCallTests text;
    makeFlatInLanguageCallTests(text, characters.get());
    constexpr uint64_t undefined = JSValue::ValueUndefined;
    auto boxed = [](double number) { return static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(number))); };
    auto raw = [](double number) { return std::bit_cast<uint64_t>(number); };
    auto says = [&](auto& list, uint64_t sought, int32_t place, bool isIncluded) {
        for (StubKey key : { tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIndexOf, 1), callBoxedBuiltin(BoxedCallForm::ArrayIndexOf, 1) }) {
            StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == int32InLanguageCallTests(place) && !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
        }
        for (StubKey key : { tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIncludes, 1), callBoxedBuiltin(BoxedCallForm::ArrayIncludes, 1) }) {
            StubRun run = runner.runCall(key, callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == static_cast<uint64_t>(isIncluded ? JSValue::ValueTrue : JSValue::ValueFalse) && !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
        }
    };
    auto isNowhere = [&](auto& list, uint64_t sought) { says(list, sought, -1, false); };
    auto isLeftToHostFunction = [&](auto& list, uint64_t sought) {
        for (BoxedCallForm form : { BoxedCallForm::ArrayIndexOf, BoxedCallForm::ArrayIncludes }) {
            StubRun run = runner.runCall(tryCallMethodOfBuiltinClass(form, 1), callee.address(), list.cell.address(), { sought });
            AOT_TEST_CHECK(run.result == nothingDoneInLanguageCallTests && !run.calledOperation);
        }
    };
    const std::array<uint64_t, 5> whatIsNoNumber { static_cast<uint64_t>(JSValue::ValueNull), static_cast<uint64_t>(JSValue::ValueTrue), static_cast<uint64_t>(JSValue::ValueFalse), text.address(), callee.address() };
    const std::array<double, 7> whatIsNoInt32 { 2.25, -0.5, 4294967308.0, 1e300, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), PNaN };

    const std::array<uint64_t, 4> wholeNumbers { int32InLanguageCallTests(0), int32InLanguageCallTests(12), int32InLanguageCallTests(-7), int32InLanguageCallTests(3) };
    ListInLanguageCallTests integers(Int32Shape, wholeNumbers);
    says(integers, boxed(-0.0), 0, true);
    says(integers, boxed(12), 1, true);
    says(integers, boxed(-7), 2, true);
    for (double number : whatIsNoInt32)
        isNowhere(integers, boxed(number));
    for (uint64_t value : whatIsNoNumber)
        isNowhere(integers, value);
    isNowhere(integers, undefined);
    const std::array<uint64_t, 4> wholeNumbersAndHole { int32InLanguageCallTests(5), int32InLanguageCallTests(1), 0, int32InLanguageCallTests(12) };
    ListInLanguageCallTests holedIntegers(Int32Shape, wholeNumbersAndHole);
    says(holedIntegers, undefined, -1, true);
    says(holedIntegers, boxed(12), 3, true);
    isNowhere(holedIntegers, boxed(0));

    const std::array<uint64_t, 5> rawFractions { raw(0.5), raw(2.5), raw(-0.0), raw(3), raw(1e300) };
    ListInLanguageCallTests fractions(DoubleShape, rawFractions);
    says(fractions, boxed(0.5), 0, true);
    says(fractions, boxed(2.5), 1, true);
    says(fractions, boxed(0), 2, true);
    says(fractions, boxed(-0.0), 2, true);
    says(fractions, int32InLanguageCallTests(0), 2, true);
    says(fractions, int32InLanguageCallTests(3), 3, true);
    says(fractions, boxed(3), 3, true);
    says(fractions, boxed(1e300), 4, true);
    for (uint64_t absent : { boxed(2.25), boxed(PNaN), boxed(std::numeric_limits<double>::infinity()), int32InLanguageCallTests(2), int32InLanguageCallTests(-3) })
        isNowhere(fractions, absent);
    for (uint64_t value : whatIsNoNumber)
        isNowhere(fractions, value);
    isNowhere(fractions, undefined);
    const std::array<uint64_t, 3> rawFractionsAndHole { raw(0.5), raw(PNaN), raw(2.5) };
    ListInLanguageCallTests holedFractions(DoubleShape, rawFractionsAndHole);
    says(holedFractions, undefined, -1, true);
    says(holedFractions, boxed(2.5), 2, true);
    isNowhere(holedFractions, boxed(PNaN));

    ListInLanguageCallTests empty(UndecidedShape, std::span<const uint64_t>());
    const std::array<uint64_t, 3> wordsThatNobodyWrote { int32InLanguageCallTests(7), int32InLanguageCallTests(7), int32InLanguageCallTests(7) };
    ListInLanguageCallTests holes(UndecidedShape, wordsThatNobodyWrote);
    for (auto* list : { &empty, &holes }) {
        for (uint64_t value : whatIsNoNumber)
            isNowhere(*list, value);
        isNowhere(*list, boxed(2.5));
        isNowhere(*list, boxed(7));
        isNowhere(*list, int32InLanguageCallTests(7));
    }
    isNowhere(empty, undefined);
    says(holes, undefined, -1, true);

    Vector<uint64_t, 20> manyFractions;
    for (uint32_t index = 0; index < 65; ++index)
        manyFractions.append(raw(index + 0.5));
    LongList mostFractions(DoubleShape, manyFractions.span().first(64));
    LongList tooManyFractions(DoubleShape, manyFractions.span());
    says(mostFractions, boxed(63.5), 63, true);
    isNowhere(mostFractions, boxed(64.5));
    isNowhere(mostFractions, undefined);
    isLeftToHostFunction(tooManyFractions, boxed(64.5));
    isLeftToHostFunction(tooManyFractions, int32InLanguageCallTests(1));
    isNowhere(tooManyFractions, text.address());
    LongList tooManyWholeNumbers(Int32Shape, integersInLanguageCallTests(65).span());
    isNowhere(tooManyWholeNumbers, boxed(2.5));
    isNowhere(tooManyWholeNumbers, text.address());
    isLeftToHostFunction(tooManyWholeNumbers, boxed(65));
    for (auto* list : { &tooManyFractions, &tooManyWholeNumbers }) {
        StubRun indexOf = runner.runCall(tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIndexOf, 1), callee.address(), list->cell.address(), { undefined });
        StubRun includes = runner.runCall(tryCallMethodOfBuiltinClass(BoxedCallForm::ArrayIncludes, 1), callee.address(), list->cell.address(), { undefined });
        AOT_TEST_CHECK(indexOf.result == int32InLanguageCallTests(-1) && includes.result == nothingDoneInLanguageCallTests);
    }

    for (IndexingType sparseKind : { ArrayStorageShape, SlowPutArrayStorageShape, NoIndexingShape }) {
        ListInLanguageCallTests sparse(sparseKind, wholeNumbers);
        for (uint64_t sought : { boxed(2.5), int32InLanguageCallTests(12), text.address(), undefined })
            isLeftToHostFunction(sparse, sought);
    }
    for (IndexingType shape : { Int32Shape, DoubleShape, UndecidedShape }) {
        ListInLanguageCallTests notAsMade(shape, wholeNumbers, 0);
        for (uint64_t sought : { boxed(2.5), int32InLanguageCallTests(12), text.address(), undefined })
            isLeftToHostFunction(notAsMade, sought);
    }
}

static void testSpliceOfOneItemInLanguageCallTests()
{
    StubRunner runner;
    static FakeObject<sizeof(JSGlobalObject)> realm;
    uintptr_t& speciesOfLists = realm.at<uintptr_t>(OBJECT_OFFSETOF(JSGlobalObject, m_arraySpeciesWatchpointSet) + InlineWatchpointSet::offsetOfData());
    speciesOfLists = InlineWatchpointSet::encodeState(IsWatched);
    runner.instance().globalObject = std::bit_cast<JSGlobalObject*>(static_cast<uintptr_t>(realm.address()));
    runner.instance().barrierThreshold = blackThreshold;
    FakeObject<sizeof(JSFunction)> callee;
    callee.at<uint64_t>(JSCallee::offsetOfScopeChain()) = realm.address();
    std::array<FakeObject<sizeof(Structure)>, 2> structures;
    runner.instance().prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    AOT_TEST_CHECK((structures[1].address() & ~static_cast<uint64_t>(UINT32_MAX)) == runner.instance().prefix().structureIDBase);
    structures[0].at<uint64_t>(Structure::realmOffset()) = realm.address();
    structures[1].at<uint64_t>(Structure::realmOffset()) = callee.address();
    const uint32_t ofThisRealm = static_cast<uint32_t>(structures[0].address());
    const uint32_t ofAnotherRealm = static_cast<uint32_t>(structures[1].address());

    constexpr uint64_t headerOfValues = 0x0101010100000001ULL;
    constexpr uint64_t headerOfIntegers = 0x0202020200000002ULL;
    runner.instance().prefix().newArrayWithContiguousHeader = headerOfValues;
    runner.instance().prefix().newArrayWithInt32Header = headerOfIntegers;
    FakeAllocator allocatorOfOne { static_cast<uint32_t>(sizeOfArrayWithStorage(arrayLiteralVectorLength(1))) };
    FakeAllocator allocatorOfNone { static_cast<uint32_t>(sizeOfArrayWithStorage(arrayLiteralVectorLength(0))) };
    runner.instance().shortArrays.allocators[ShortArrayWords::indexOfAllocator(arrayLiteralVectorLength(1))] = &allocatorOfOne;
    runner.instance().shortArrays.allocators[ShortArrayWords::indexOfAllocator(arrayLiteralVectorLength(0))] = &allocatorOfNone;
    alignas(16) std::array<uint64_t, 16> made;

    constexpr uint64_t answerOfOperation = int32InLanguageCallTests(99);
    constexpr uint64_t one = int32InLanguageCallTests(1);
    constexpr uint64_t none = int32InLanguageCallTests(0);
    constexpr uint64_t come = int32InLanguageCallTests(77);
    runner.recordCallsOf(Entry::operationAOTArraySpliceGeneric);
    runner.recordCallsOf(Entry::operationAOTArraySpliceItemGeneric);
    runner.setResultOfRecordedOperation(answerOfOperation);
    constexpr StubKey takesOut = callBoxedBuiltin(BoxedCallForm::ArraySplice, 2);
    constexpr StubKey putsIn = callBoxedBuiltin(BoxedCallForm::ArraySplice, 3);

    constexpr uint32_t mostValuesMoved = 256;
    using LongList = ListOfCapacityInLanguageCallTests<mostValuesMoved + 3>;

    auto copyOf = [](auto& list) {
        Vector<uint64_t, 20> values;
        values.append(list.elements().first(list.length()));
        return values;
    };
    auto goesOutInStub = [&](auto& list, int32_t start, uint32_t place, uint64_t header) {
        Vector<uint64_t, 20> expected = copyOf(list);
        uint64_t gone = expected[place];
        expected.removeAt(place);
        made.fill(0);
        allocatorOfOne.makeRoomAt(std::bit_cast<uintptr_t>(made.data()), sizeOfArrayWithStorage(arrayLiteralVectorLength(1)));
        StubRun run = runner.runCall(takesOut, callee.address(), list.cell.address(), { int32InLanguageCallTests(start), one });
        AOT_TEST_CHECK(!run.calledOperation && run.result == std::bit_cast<uintptr_t>(made.data()) && made[0] == header);
        AOT_TEST_CHECK(static_cast<uint32_t>(made[2]) == 1 && made[3] == gone);
        AOT_TEST_CHECK(list.length() == expected.size() && std::ranges::equal(list.elements().first(list.length()), expected.span()) && !list.elements()[list.length()]);
    };
    auto comesInInStub = [&](auto& list, int32_t start, uint32_t place, uint64_t item) {
        Vector<uint64_t, 20> expected = copyOf(list);
        expected.insert(place, item);
        made.fill(0);
        allocatorOfNone.makeRoomAt(std::bit_cast<uintptr_t>(made.data()), sizeOfArrayWithStorage(arrayLiteralVectorLength(0)));
        StubRun run = runner.runCall(putsIn, callee.address(), list.cell.address(), { int32InLanguageCallTests(start), none, item });
        AOT_TEST_CHECK(!run.calledOperation && run.result == std::bit_cast<uintptr_t>(made.data()) && made[0] == headerOfValues && !static_cast<uint32_t>(made[2]));
        AOT_TEST_CHECK(list.length() == expected.size() && std::ranges::equal(list.elements().first(list.length()), expected.span()));
    };
    auto nothingGoesOutInStub = [&](auto& list, uint64_t start) {
        Vector<uint64_t, 20> before = copyOf(list);
        made.fill(0);
        allocatorOfNone.makeRoomAt(std::bit_cast<uintptr_t>(made.data()), sizeOfArrayWithStorage(arrayLiteralVectorLength(0)));
        StubRun run = runner.runCall(takesOut, callee.address(), list.cell.address(), { start, none });
        AOT_TEST_CHECK(!run.calledOperation && run.result == std::bit_cast<uintptr_t>(made.data()) && made[0] == headerOfValues && !static_cast<uint32_t>(made[2]));
        AOT_TEST_CHECK(list.length() == before.size() && std::ranges::equal(list.elements().first(list.length()), before.span()));
    };
    auto isLeftToOperation = [&](auto& list, uint64_t start, uint64_t deleteCount, std::optional<uint64_t> item = std::nullopt) {
        Vector<uint64_t, 20> before = copyOf(list);
        StubRun run = item ? runner.runCall(putsIn, callee.address(), list.cell.address(), { start, deleteCount, *item }) : runner.runCall(takesOut, callee.address(), list.cell.address(), { start, deleteCount });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == answerOfOperation);
        AOT_TEST_CHECK(run.recordedArguments[0] == list.cell.address() && run.recordedArguments[1] == start && run.recordedArguments[2] == deleteCount && (!item || run.recordedArguments[3] == *item));
        AOT_TEST_CHECK(list.length() == before.size() && std::ranges::equal(list.elements().first(list.length()), before.span()));
    };
    auto listOf = [&](IndexingType shape, uint32_t count, uint8_t flags = IsPristine) { return ListInLanguageCallTests(shape, integersInLanguageCallTests(count).span(), flags, ofThisRealm); };
    using StartsAndPlaces = std::initializer_list<std::pair<int32_t, uint32_t>>;

    for (IndexingType shape : { Int32Shape, ContiguousShape }) {
        const uint64_t header = shape == Int32Shape ? headerOfIntegers : headerOfValues;
        for (auto [start, place] : StartsAndPlaces { { 0, 0 }, { 2, 2 }, { 4, 4 }, { -1, 4 }, { -5, 0 }, { -9, 0 }, { INT32_MIN, 0 } }) {
            auto list = listOf(shape, 5);
            goesOutInStub(list, start, place, header);
        }
        for (auto [start, place] : StartsAndPlaces { { 0, 0 }, { 2, 2 }, { 5, 5 }, { -1, 4 }, { -5, 0 }, { -9, 0 }, { INT32_MIN, 0 } }) {
            auto list = listOf(shape, 5);
            comesInInStub(list, start, place, come);
        }
        auto alone = listOf(shape, 1);
        goesOutInStub(alone, 0, 0, header);
        auto empty = listOf(shape, 0);
        comesInInStub(empty, 0, 0, come);
        auto stillEmpty = listOf(shape, 0);
        isLeftToOperation(stillEmpty, none, one);

        for (uint32_t count : { 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 33u, 34u }) {
            for (uint32_t place : { 0u, 1u, count - 5, count - 4, count - 1 }) {
                auto takenOutOf = listOf(shape, count);
                goesOutInStub(takenOutOf, place, place, header);
                auto putInto = listOf(shape, count);
                comesInInStub(putInto, place, place, come);
            }
        }
        LongList longestTakenOutOf(shape, integersInLanguageCallTests(mostValuesMoved + 1).span(), IsPristine, ofThisRealm);
        goesOutInStub(longestTakenOutOf, 0, 0, header);
        LongList longestPutInto(shape, integersInLanguageCallTests(mostValuesMoved).span(), IsPristine, ofThisRealm);
        comesInInStub(longestPutInto, 0, 0, come);
        LongList tooLongToTakeOutOf(shape, integersInLanguageCallTests(mostValuesMoved + 2).span(), IsPristine, ofThisRealm);
        isLeftToOperation(tooLongToTakeOutOf, none, one);
        LongList tooLongToPutInto(shape, integersInLanguageCallTests(mostValuesMoved + 1).span(), IsPristine, ofThisRealm);
        isLeftToOperation(tooLongToPutInto, none, none, come);
        auto full = listOf(shape, ListInLanguageCallTests::capacity);
        isLeftToOperation(full, int32InLanguageCallTests(ListInLanguageCallTests::capacity - 1), none, come);

        auto five = listOf(shape, 5);
        isLeftToOperation(five, int32InLanguageCallTests(5), one);
        isLeftToOperation(five, int32InLanguageCallTests(INT32_MAX), one);
        isLeftToOperation(five, int32InLanguageCallTests(6), none, come);
        nothingGoesOutInStub(five, one);
        nothingGoesOutInStub(five, int32InLanguageCallTests(-9));
        isLeftToOperation(five, static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(1))), none);
        isLeftToOperation(five, one, int32InLanguageCallTests(2));
        isLeftToOperation(five, one, one, come);
        isLeftToOperation(five, one, static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(1))));
        isLeftToOperation(five, static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(1))), one);
        isLeftToOperation(five, static_cast<uint64_t>(JSValue::encode(jsUndefined())), none, come);

        auto notAsMade = listOf(shape, 5, 0);
        isLeftToOperation(notAsMade, one, one);
        isLeftToOperation(notAsMade, one, none, come);
        auto copiedWhenWritten = listOf(shape | CopyOnWrite, 5);
        isLeftToOperation(copiedWhenWritten, one, one);
        isLeftToOperation(copiedWhenWritten, one, none, come);

        const std::array<uint64_t, 4> holes { int32InLanguageCallTests(1), 0, int32InLanguageCallTests(3), 0 };
        ListInLanguageCallTests hereIsHole(shape, holes, IsPristine, ofThisRealm);
        isLeftToOperation(hereIsHole, one, one);
        goesOutInStub(hereIsHole, 0, 0, header);
        ListInLanguageCallTests holesMove(shape, holes, IsPristine, ofThisRealm);
        comesInInStub(holesMove, 1, 1, come);
    }
    for (IndexingType shape : { UndecidedShape, DoubleShape, ArrayStorageShape, SlowPutArrayStorageShape }) {
        auto other = listOf(shape, 3);
        isLeftToOperation(other, one, one);
        isLeftToOperation(other, one, none, come);
    }

    for (JSType type : { ArrayType, DerivedArrayType }) {
        FakeObject<sizeof(JSArray)> withoutStorage;
        withoutStorage.setHeader(0, type);
        withoutStorage.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = ArrayClass;
        StubRun run = runner.runCall(takesOut, callee.address(), withoutStorage.address(), { none, one });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == answerOfOperation && run.recordedArguments[0] == withoutStorage.address());
        run = runner.runCall(putsIn, callee.address(), withoutStorage.address(), { none, none, come });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == answerOfOperation && run.recordedArguments[0] == withoutStorage.address() && run.recordedArguments[3] == come);
    }

    FakeObject<16> cell;
    auto integers = listOf(Int32Shape, 3);
    isLeftToOperation(integers, one, none, cell.address());
    isLeftToOperation(integers, one, none, static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(1.5))));
    auto young = listOf(ContiguousShape, 3);
    young.cell.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
    comesInInStub(young, 1, 1, cell.address());
    auto old = listOf(ContiguousShape, 3);
    old.cell.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::PossiblyBlack);
    isLeftToOperation(old, one, none, cell.address());
    comesInInStub(old, 1, 1, come);
    goesOutInStub(old, 1, 1, headerOfValues);

    ListInLanguageCallTests listOfAnotherRealm(ContiguousShape, integersInLanguageCallTests(3).span(), IsPristine, ofAnotherRealm);
    isLeftToOperation(listOfAnotherRealm, one, one);
    isLeftToOperation(listOfAnotherRealm, one, none, come);

    speciesOfLists = InlineWatchpointSet::encodeState(IsInvalidated);
    auto whileSpeciesMayBeAnother = listOf(ContiguousShape, 3);
    isLeftToOperation(whileSpeciesMayBeAnother, one, one);
    isLeftToOperation(whileSpeciesMayBeAnother, one, none, come);
    speciesOfLists = InlineWatchpointSet::encodeState(ClearWatchpoint);
    isLeftToOperation(whileSpeciesMayBeAnother, one, one);
    speciesOfLists = InlineWatchpointSet::encodeState(IsWatched);

    runner.instance().mutatorShouldBeFenced = true;
    auto ofValuesWhileCollectorMayVisit = listOf(ContiguousShape, 3);
    isLeftToOperation(ofValuesWhileCollectorMayVisit, one, one);
    isLeftToOperation(ofValuesWhileCollectorMayVisit, one, none, come);
    auto ofIntegersWhileCollectorMayVisit = listOf(Int32Shape, 3);
    goesOutInStub(ofIntegersWhileCollectorMayVisit, 1, 1, headerOfIntegers);
    comesInInStub(ofIntegersWhileCollectorMayVisit, 1, 1, come);
    runner.instance().mutatorShouldBeFenced = false;
}

template<uint32_t count, uint32_t budget>
static void testInstructionsOfShiftInLanguageCallTests()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    StubRunner runner;
    runner.setCountsInstructions(true);
    FakeObject<sizeof(JSFunction)> callee;
    ListInLanguageCallTests list(ContiguousShape, integersInLanguageCallTests(count).span());
    StubRun run = runner.runCall(callBoxedBuiltin(BoxedCallForm::ArrayShift, 0), callee.address(), list.cell.address(), { });
    AOT_TEST_CHECK(!run.calledOperation && run.result == int32InLanguageCallTests(1) && list.length() + 1 == count && run.instructionsInStubs);
    if (*run.instructionsInStubs != budget)
        dataLogLn("a shift of a list of ", count, ": ", *run.instructionsInStubs, " instructions, the budget is ", budget);
    AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
#endif
}

static unsigned numberOfHostCallsInLanguageCallTests;

static JSC_DECLARE_HOST_FUNCTION(countHostCallInLanguageCallTests);

JSC_DEFINE_HOST_FUNCTION(countHostCallInLanguageCallTests, (JSGlobalObject*, CallFrame*))
{
    ++numberOfHostCallsInLanguageCallTests;
    return static_cast<EncodedJSValue>(resultOfHostFunctionInLanguageCallTests);
}

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

    FakeObject<16> realm;
    FakeObject<sizeof(NativeExecutable)> executable;
    executable.setHeader(0, NativeExecutableType);
    executable.at<const void*>(NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(countHostCallInLanguageCallTests).taggedPtr();
    FakeObject<sizeof(JSFunction)> booleanConstructor;
    booleanConstructor.setHeader(static_cast<uint32_t>(structures[0].address()), JSFunctionType);
    booleanConstructor.at<uint64_t>(JSCallee::offsetOfScopeChain()) = realm.address();
    booleanConstructor.at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = executable.address();
    FakeObject<sizeof(InternalFunction)> internalFunction;
    internalFunction.setHeader(static_cast<uint32_t>(structures[1].address()), InternalFunctionType);
    internalFunction.at<const void*>(InternalFunction::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(countHostCallInLanguageCallTests).taggedPtr();
    const uint64_t boolean = booleanConstructor.address();
    const uint64_t otherFunction = internalFunction.address();

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
        AOT_TEST_CHECK(runner.runCall(callValue(1), boolean, thisValue, { value }).result == expected);
        AOT_TEST_CHECK(runner.runCall(callValue(3), boolean, thisValue, { value, int32InLanguageCallTests(0), object.address() }).result == expected);
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
        AOT_TEST_CHECK(runner.runCall(callValue(1), boolean, thisValue, { value }).result == resultOfHostFunctionInLanguageCallTests);
        AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 1);
    }
}

static void testNumberAndStringCalledByValueWithoutTheirArmInLanguageCallTests()
{
    StubRunner runner(StubOptions { .callsNumberOrStringByValue = false, .targetFeatures = CPUFeatureSet::buildTarget() });
    Instance& instance = runner.instance();
    std::array<FakeObject<sizeof(Structure)>, 2> structures;
    FakeObject<16> classInfoOfNumber;
    FakeObject<16> classInfoOfString;
    instance.prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    AOT_TEST_CHECK((structures[1].address() & ~static_cast<uint64_t>(UINT32_MAX)) == instance.prefix().structureIDBase);
    structures[0].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfNumber.address();
    structures[1].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfString.address();
    StubEnvironment::of(&instance).runtimeTable[indexOf(Entry::NumberConstructorClassInfo)] = std::bit_cast<void*>(static_cast<uintptr_t>(classInfoOfNumber.address()));
    StubEnvironment::of(&instance).runtimeTable[indexOf(Entry::StringConstructorClassInfo)] = std::bit_cast<void*>(static_cast<uintptr_t>(classInfoOfString.address()));

    FakeObject<16> realm;
    FakeObject<sizeof(NativeExecutable)> executable;
    executable.setHeader(0, NativeExecutableType);
    executable.at<const void*>(NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(countHostCallInLanguageCallTests).taggedPtr();
    std::array<FakeObject<sizeof(JSFunction)>, 2> constructors;
    for (size_t index = 0; index < constructors.size(); ++index) {
        constructors[index].setHeader(static_cast<uint32_t>(structures[index].address()), JSFunctionType);
        constructors[index].at<uint64_t>(JSCallee::offsetOfScopeChain()) = realm.address();
        constructors[index].at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = executable.address();
    }

    Ref<StringImpl> characters = StringImpl::create("12"_s);
    TextInLanguageCallTests text;
    FakeObject<16> object;
    makeFlatInLanguageCallTests(text, characters.get());
    object.setHeader(0, ObjectType);
    constexpr uint64_t thisValue = JSValue::ValueUndefined;
    const uint64_t fraction = static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(0.5)));

    numberOfHostCallsInLanguageCallTests = 0;
    for (uint64_t callee : { constructors[0].address(), constructors[1].address() }) {
        for (uint64_t value : { int32InLanguageCallTests(7), fraction, text.address(), object.address() }) {
            const std::array<StubRun, 4> runs {
                runner.runCall(callValue(1), callee, thisValue, { value }),
                runner.runCall(callValue(3), callee, thisValue, { value, int32InLanguageCallTests(0), object.address() }),
                runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 1), callee, thisValue, { value }),
                runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 3), callee, thisValue, { value, int32InLanguageCallTests(0), object.address() }),
            };
            for (const StubRun& run : runs)
                AOT_TEST_CHECK(!run.calledOperation && run.result == resultOfHostFunctionInLanguageCallTests);
        }
        AOT_TEST_CHECK(runner.runCall(callValue(0), callee, thisValue, { }).result == resultOfHostFunctionInLanguageCallTests);
    }
    AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 34);
}

static void testNumberAndStringCalledByValueInLanguageCallTests()
{
    StubRunner runner;
    Instance& instance = runner.instance();
    std::array<FakeObject<sizeof(Structure)>, 2> structures;
    FakeObject<16> classInfoOfNumber;
    FakeObject<16> classInfoOfString;
    instance.prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    AOT_TEST_CHECK((structures[1].address() & ~static_cast<uint64_t>(UINT32_MAX)) == instance.prefix().structureIDBase);
    structures[0].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfNumber.address();
    structures[1].at<uint64_t>(Structure::classInfoOffset()) = classInfoOfString.address();
    StubEnvironment::of(&instance).runtimeTable[indexOf(Entry::NumberConstructorClassInfo)] = std::bit_cast<void*>(static_cast<uintptr_t>(classInfoOfNumber.address()));
    StubEnvironment::of(&instance).runtimeTable[indexOf(Entry::StringConstructorClassInfo)] = std::bit_cast<void*>(static_cast<uintptr_t>(classInfoOfString.address()));

    FakeObject<16> realm;
    FakeObject<sizeof(NativeExecutable)> executable;
    executable.setHeader(0, NativeExecutableType);
    executable.at<const void*>(NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(countHostCallInLanguageCallTests).taggedPtr();
    std::array<FakeObject<sizeof(JSFunction)>, 2> constructors;
    for (size_t index = 0; index < constructors.size(); ++index) {
        constructors[index].setHeader(static_cast<uint32_t>(structures[index].address()), JSFunctionType);
        constructors[index].at<uint64_t>(JSCallee::offsetOfScopeChain()) = realm.address();
        constructors[index].at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = executable.address();
    }
    const uint64_t number = constructors[0].address();
    const uint64_t string = constructors[1].address();

    Ref<StringImpl> characters = StringImpl::create("12"_s);
    TextInLanguageCallTests text;
    FakeObject<16> object;
    FakeObject<16> symbol;
    FakeObject<16> bigInt;
    makeFlatInLanguageCallTests(text, characters.get());
    object.setHeader(0, ObjectType);
    symbol.setHeader(0, SymbolType);
    bigInt.setHeader(0, HeapBigIntType);
    constexpr uint64_t thisValue = JSValue::ValueUndefined;
    const uint64_t fraction = static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(0.5)));

    auto runsOf = [&](uint64_t callee, uint64_t value) {
        return std::array<StubRun, 4> {
            runner.runCall(callValue(1), callee, thisValue, { value }),
            runner.runCall(callValue(3), callee, thisValue, { value, int32InLanguageCallTests(0), object.address() }),
            runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 1), callee, thisValue, { value }),
            runner.runCall(callInternalFunction(CodeSpecializationKind::CodeForCall, 3), callee, thisValue, { value, int32InLanguageCallTests(0), object.address() }),
        };
    };
    auto checkLeftToOperation = [&](uint64_t callee, uint64_t value, Entry entry) {
        constexpr uint64_t resultOfOperation = 0x7777;
        runner.recordCallsOf(entry);
        runner.setResultOfRecordedOperation(resultOfOperation);
        for (const StubRun& run : runsOf(callee, value))
            AOT_TEST_CHECK(run.calledOperation && run.numberOfRecordedCalls == 1 && run.result == resultOfOperation && run.recordedArguments[0] == value);
        runner.countCallsOf(entry);
    };
    numberOfHostCallsInLanguageCallTests = 0;
    for (uint64_t value : { int32InLanguageCallTests(7), int32InLanguageCallTests(-1), fraction }) {
        for (const StubRun& run : runsOf(number, value))
            AOT_TEST_CHECK(!run.calledOperation && run.result == value);
        checkLeftToOperation(string, value, Entry::operationAOTStringOfAnyValue);
    }
    for (const StubRun& run : runsOf(string, text.address()))
        AOT_TEST_CHECK(!run.calledOperation && run.result == text.address());
    checkLeftToOperation(number, text.address(), Entry::operationAOTNumberOfAnyValue);
    AOT_TEST_CHECK(!numberOfHostCallsInLanguageCallTests);

    for (uint64_t callee : { number, string }) {
        for (uint64_t value : { static_cast<uint64_t>(JSValue::ValueUndefined), static_cast<uint64_t>(JSValue::ValueNull), static_cast<uint64_t>(JSValue::ValueTrue), object.address(), symbol.address(), bigInt.address() }) {
            numberOfHostCallsInLanguageCallTests = 0;
            AOT_TEST_CHECK(runner.runCall(callValue(1), callee, thisValue, { value }).result == resultOfHostFunctionInLanguageCallTests);
            AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 1);
        }
        numberOfHostCallsInLanguageCallTests = 0;
        AOT_TEST_CHECK(runner.runCall(callValue(0), callee, thisValue, { }).result == resultOfHostFunctionInLanguageCallTests);
        AOT_TEST_CHECK(numberOfHostCallsInLanguageCallTests == 1);
    }
}

std::span<const NativeTest> stubBoxedCallsOfLanguageTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/boxed-calls-of-language/exec-and-match-go-to-what-matches"_s, testFormsThatMatchInLanguageCallTests },
        { "stubs/boxed-calls-of-language/number-and-string-called-by-value-without-their-arm-go-to-the-host-function"_s, ImageTests::whenExecutable<testNumberAndStringCalledByValueWithoutTheirArmInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/narrow-flat-text-is-well-formed"_s, ImageTests::whenExecutable<testIsWellFormedInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/search-from-a-position-lies-in-the-text"_s, ImageTests::whenExecutable<testSearchesFromPositionInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/short-text-that-can-stand-at-one-place-alone-is-compared-there"_s, ImageTests::whenExecutable<testTextAtOnePlaceInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/several-values-are-pushed-by-one-operation"_s, ImageTests::whenExecutable<testPushesOfSeveralValuesInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/a-list-of-few-values-is-shifted-without-the-operation"_s, ImageTests::whenExecutable<testShiftOfFewValuesInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/one-item-is-spliced-in-or-out-without-the-operation"_s, ImageTests::whenExecutable<testSpliceOfOneItemInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/a-text-is-searched-for-in-a-list-of-any-length"_s, ImageTests::whenExecutable<testSearchesOfListsInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/a-list-that-holds-no-boxed-values-is-searched-without-the-host"_s, ImageTests::whenExecutable<testSearchesOfListsWithoutBoxedValuesInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/instructions/a-shift-of-a-list-of-one"_s, ImageTests::whenExecutable<testInstructionsOfShiftInLanguageCallTests<1, 26>> },
        { "stubs/boxed-calls-of-language/instructions/a-shift-of-a-list-of-two"_s, ImageTests::whenExecutable<testInstructionsOfShiftInLanguageCallTests<2, 38>> },
        { "stubs/boxed-calls-of-language/instructions/a-shift-of-a-list-of-nine"_s, ImageTests::whenExecutable<testInstructionsOfShiftInLanguageCallTests<9, 73>> },
        { "stubs/boxed-calls-of-language/instructions/a-shift-of-a-list-of-seventeen"_s, ImageTests::whenExecutable<testInstructionsOfShiftInLanguageCallTests<17, 113>> },
        { "stubs/boxed-calls-of-language/boolean-called-by-value-takes-no-frame"_s, ImageTests::whenExecutable<testBooleanCalledByValueInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/number-and-string-called-by-value-go-to-the-operations-of-their-direct-calls"_s, ImageTests::whenExecutable<testNumberAndStringCalledByValueInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/a-call-of-a-built-in-that-the-program-cannot-hold-goes-to-the-host-function"_s, testCallsOfBuiltinsThatProgramCannotHoldInLanguageCallTests },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
