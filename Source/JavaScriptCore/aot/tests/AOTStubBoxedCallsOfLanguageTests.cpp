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

static bool callsOperationInLanguageCallTests(const StubCatalog& catalog, StubKey from, Entry entry)
{
    const StubRecording& recording = catalog.recordingOf(bodyOf(from));
    return recording.operationsCalled.get(indexOf(entry)) || recording.references.containsIf([&](const StubReference& reference) {
        return reference.target.immediate() == offsetInRuntimeTable(entry);
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
        AOT_TEST_CHECK(callsOperationInLanguageCallTests(matches, key, Entry::operationAOTStringMatchRegExp));
        AOT_TEST_CHECK(!callsOperationInLanguageCallTests(matchesNothing, key, Entry::operationAOTStringMatchRegExp));
    }
    for (BoxedCallForm form : { BoxedCallForm::RegExpExec, BoxedCallForm::StringMatch }) {
        AOT_TEST_CHECK(servesCallsByName(form, 1));
        for (unsigned count : { 0u, 2u, 3u, 4u })
            AOT_TEST_CHECK(!acceptsArgumentCount(form, count) && !servesCallsByName(form, count));
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
}

static void testSearchesFromPositionInLanguageCallTests()
{
    StubRunner runner;
    constexpr std::array<char16_t, 6> wideCharacters { 'a', 'b', 'c', 'd', 'e', 0x100 };
    Ref<StringImpl> characters = StringImpl::create(std::span<const char16_t>(wideCharacters));
    Ref<StringImpl> charactersOfPart = *StringImpl::empty();
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
        { BoxedCallForm::StringStartsWith, 1, Wide, Nothing, unused, yes },
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
        { "stubs/boxed-calls-of-language/short-text-that-can-stand-at-one-place-alone-is-compared-there"_s, ImageTests::whenExecutable<testTextAtOnePlaceInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/several-values-are-pushed-by-one-operation"_s, ImageTests::whenExecutable<testPushesOfSeveralValuesInLanguageCallTests> },
        { "stubs/boxed-calls-of-language/boolean-called-by-value-takes-no-frame"_s, ImageTests::whenExecutable<testBooleanCalledByValueInLanguageCallTests> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
