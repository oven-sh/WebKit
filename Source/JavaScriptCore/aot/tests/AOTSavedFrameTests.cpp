/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCollectionTestSupport.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTCoroutineTable.h"
#include "AOTFrameMaps.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTSavedFrame.h"
#include "AOTSavedFrameMap.h"
#include "AOTShellProcess.h"
#include "AOTStubFrames.h"
#include "AOTStubsCoroutines.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "MarkedSpaceInlines.h"
#include "MicrotaskQueue.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/NoTailCalls.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> savedFrameTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="waitFor" flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0
    v3 = Call v0
    v4 = PropGet v3 key="tag"
    v5 = Await v1
    v6 = PropGet v2 key="tag"
    v7 = NewArray v4, v5, v6 contents=anything
    Return v7
}
)IR"_s;

static constexpr ASCIILiteral programWithTwoAwaitsText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="waitTwice" flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0
    v3 = Call v0
    v4 = Call v0
    v5 = Call v0
    v6 = Call v0
    v7 = Await v1
    v8 = Call v0, -, v2, v3, v4, v5
    v9 = Await v1
    v10 = NewArray v6, v9 contents=anything
    Return v10
}
)IR"_s;

static Vector<uint32_t> valuesOf(const SavedFrameMap& map)
{
    Vector<uint32_t> locations;
    AOT_TEST_CHECK(map.forEachValue([&](uint32_t location) {
        locations.append(location);
        return true;
    }));
    return locations;
}

static Vector<uint32_t> narrowReferencesOf(const SavedFrameMap& map)
{
    Vector<uint32_t> locations;
    AOT_TEST_CHECK(map.forEachNarrowReference([&](uint32_t location) {
        locations.append(location);
        return true;
    }));
    return locations;
}

static Vector<uint32_t> otherLiveWordsOf(const SavedFrameMap& map)
{
    Vector<uint32_t> locations;
    AOT_TEST_CHECK(map.forEachOtherLiveWord([&](uint32_t location) {
        locations.append(location);
        return true;
    }));
    return locations;
}

static Vector<uint32_t> deadWordsOf(const SavedFrameMap& map, bool hasLocationZero, uint32_t first, uint32_t last)
{
    Vector<uint32_t> locations;
    AOT_TEST_CHECK(map.forEachDeadWord(hasLocationZero, first, last, [&](uint32_t location) {
        locations.append(location);
        return true;
    }));
    return locations;
}

static bool listsAsLive(const SavedFrameMap& map, uint32_t location)
{
    return map.listsValue(location) || map.listsOtherLiveWord(location) || map.listsNarrowReference(2 * location) || (location && map.listsNarrowReference(2 * location - 1));
}

static void checkLists(const SavedFrameMap& map, const Vector<uint32_t>& values, const Vector<uint32_t>& narrowReferences, const Vector<uint32_t>& otherLiveWords = { })
{
    AOT_TEST_CHECK(valuesOf(map) == values);
    AOT_TEST_CHECK(narrowReferencesOf(map) == narrowReferences);
    AOT_TEST_CHECK(otherLiveWordsOf(map) == otherLiveWords);
    for (uint32_t location = 0; location < 200; ++location) {
        AOT_TEST_CHECK(map.listsValue(location) == values.contains(location));
        AOT_TEST_CHECK(map.listsNarrowReference(location) == narrowReferences.contains(location));
        AOT_TEST_CHECK(map.listsOtherLiveWord(location) == otherLiveWords.contains(location));
    }
}

static void testMapRoundTrip()
{
    Vector<uint32_t> values { 0, 1, 31, 32, 70 };
    Vector<uint32_t> narrowReferences { 0, 3, 64 };
    SavedFrameMapBuilder builder;
    for (uint32_t location : { 70u, 0u, 32u, 1u, 31u })
        builder.addValue(location);
    for (uint32_t location : { 64u, 0u, 3u })
        builder.addNarrowReference(location);

    Vector<uint32_t, 8> words = builder.finish();
    AOT_TEST_CHECK(words.size() == 1 + 3 + 3);
    AOT_TEST_CHECK(words[0] == (3 | 3 << 8));

    std::optional<SavedFrameMap> map = SavedFrameMap::tryCreate(words.span(), 0);
    AOT_TEST_CHECK(map);
    checkLists(*map, values, narrowReferences);

    SavedFrameMapBuilder otherBuilder;
    otherBuilder.addValue(2);
    otherBuilder.addNarrowReference(5);
    Vector<uint32_t> twoMaps;
    twoMaps.appendVector(otherBuilder.finish());
    uint32_t offset = twoMaps.size();
    twoMaps.appendVector(words);

    std::optional<SavedFrameMap> first = SavedFrameMap::tryCreate(twoMaps.span(), 0);
    std::optional<SavedFrameMap> second = SavedFrameMap::tryCreate(twoMaps.span(), offset);
    AOT_TEST_CHECK(first && second);
    checkLists(*first, { 2 }, { 5 });
    checkLists(*second, values, narrowReferences);
}

static void testMapWithOnePlane()
{
    SavedFrameMapBuilder valuesOnly;
    valuesOnly.addValue(33);
    Vector<uint32_t, 8> valueWords = valuesOnly.finish();
    AOT_TEST_CHECK((valueWords == Vector<uint32_t, 8> { 2, 0, 2 }));
    checkLists(*SavedFrameMap::tryCreate(valueWords.span(), 0), { 33 }, { });

    SavedFrameMapBuilder narrowReferencesOnly;
    narrowReferencesOnly.addNarrowReference(33);
    Vector<uint32_t, 8> narrowWords = narrowReferencesOnly.finish();
    AOT_TEST_CHECK((narrowWords == Vector<uint32_t, 8> { 2 << 8, 0, 2 }));
    checkLists(*SavedFrameMap::tryCreate(narrowWords.span(), 0), { }, { 33 });
}

static void testMapThatListsEveryLiveWord()
{
    SavedFrameMapBuilder builder;
    for (uint32_t location : { 1u, 40u })
        builder.addValue(location);
    builder.addNarrowReference(70);
    for (uint32_t location : { 66u, 0u, 2u, 39u })
        builder.addOtherLiveWord(location);
    AOT_TEST_CHECK(builder.listsReferenceIn(1) && builder.listsReferenceIn(40) && builder.listsReferenceIn(35) && !builder.listsReferenceIn(0) && !builder.listsReferenceIn(2) && !builder.listsReferenceIn(34) && !builder.listsReferenceIn(36));

    Vector<uint32_t, 8> words = builder.finish();
    AOT_TEST_CHECK((words == Vector<uint32_t, 8> { 2 | 3 << 8 | 3 << 16, 1u << 1, 1u << 8, 0, 0, 1u << 6, 1u << 0 | 1u << 2, 1u << 7, 1u << 2 }));
    SavedFrameMap incomplete = *SavedFrameMap::tryCreate(words.span(), 0);
    checkLists(incomplete, { 1, 40 }, { 70 }, { 0, 2, 39, 66 });
    AOT_TEST_CHECK(!incomplete.listsEveryLiveWord() && deadWordsOf(incomplete, true, 1, 199).isEmpty());

    builder.setListsEveryLiveWord();
    words = builder.finish();
    AOT_TEST_CHECK(words[0] == (2 | 3 << 8 | 3 << 16 | SavedFrameMap::listsEveryLiveWordBit));
    SavedFrameMap map = *SavedFrameMap::tryCreate(words.span(), 0);
    checkLists(map, { 1, 40 }, { 70 }, { 0, 2, 39, 66 });
    AOT_TEST_CHECK(map.listsEveryLiveWord() && map.firstWord() == words.span().data());
    checkLists(SavedFrameMap::fromFirstWord(map.firstWord()), { 1, 40 }, { 70 }, { 0, 2, 39, 66 });

    for (bool hasLocationZero : { false, true }) {
        for (uint32_t first : { 1u, 2u, 3u, 31u, 32u, 33u, 64u, 70u }) {
            for (uint32_t last : { 0u, 1u, 2u, 30u, 31u, 32u, 63u, 64u, 65u, 95u, 96u, 130u }) {
                Vector<uint32_t> expected;
                for (uint32_t location = first; location <= last; ++location) {
                    if (!listsAsLive(map, location))
                        expected.append(location);
                }
                AOT_TEST_CHECK(deadWordsOf(map, hasLocationZero, first, last) == expected);
            }
        }
    }

    SavedFrameMapBuilder halves;
    halves.setListsEveryLiveWord();
    for (uint32_t narrowLocation : { 0u, 3u, 6u, 63u, 64u, 127u, 130u })
        halves.addNarrowReference(narrowLocation);
    Vector<uint32_t, 8> halfWords = halves.finish();
    AOT_TEST_CHECK((deadWordsOf(*SavedFrameMap::tryCreate(halfWords.span(), 0), true, 1, 66) == [] {
        Vector<uint32_t> locations;
        for (uint32_t location = 1; location <= 66; ++location) {
            if (location != 2 && location != 3 && location != 32 && location != 64 && location != 65)
                locations.append(location);
        }
        return locations;
    }()));

    SavedFrameMapBuilder nothingLive;
    nothingLive.setListsEveryLiveWord();
    Vector<uint32_t, 8> emptyWords = nothingLive.finish();
    AOT_TEST_CHECK((emptyWords == Vector<uint32_t, 8> { SavedFrameMap::listsEveryLiveWordBit }));
    AOT_TEST_CHECK((deadWordsOf(*SavedFrameMap::tryCreate(emptyWords.span(), 0), true, 3, 5) == Vector<uint32_t> { 0, 3, 4, 5 }));

    Vector<uint32_t> visited;
    AOT_TEST_CHECK(!SavedFrameMap::tryCreate(emptyWords.span(), 0)->forEachDeadWord(false, 1, 100, [&](uint32_t location) {
        visited.append(location);
        return location != 2;
    }));
    AOT_TEST_CHECK((visited == Vector<uint32_t> { 1, 2 }));
}

static void testEmptyMap()
{
    Vector<uint32_t, 8> words = SavedFrameMapBuilder().finish();
    AOT_TEST_CHECK((words == Vector<uint32_t, 8> { 0 }));
    std::optional<SavedFrameMap> map = SavedFrameMap::tryCreate(words.span(), 0);
    AOT_TEST_CHECK(map);
    checkLists(*map, { }, { });
    AOT_TEST_CHECK(!map->listsValue(UINT32_MAX) && !map->listsNarrowReference(UINT32_MAX));
}

static void testWalkStopsWhenTheFunctorDoes()
{
    SavedFrameMapBuilder builder;
    for (uint32_t location : { 1u, 40u, 41u }) {
        builder.addValue(location);
        builder.addNarrowReference(location + 1);
    }
    Vector<uint32_t, 8> words = builder.finish();
    SavedFrameMap map = *SavedFrameMap::tryCreate(words.span(), 0);

    Vector<uint32_t> visited;
    AOT_TEST_CHECK(!map.forEachValue([&](uint32_t location) {
        visited.append(location);
        return location != 40;
    }));
    AOT_TEST_CHECK((visited == Vector<uint32_t> { 1, 40 }));

    visited.clear();
    AOT_TEST_CHECK(!map.forEachNarrowReference([&](uint32_t location) {
        visited.append(location);
        return false;
    }));
    AOT_TEST_CHECK((visited == Vector<uint32_t> { 2 }));
}

static bool isRejected(std::initializer_list<uint32_t> words, uint32_t offset)
{
    Vector<uint32_t> copy(words);
    return !SavedFrameMap::tryCreate(copy.span(), offset);
}

static void testMapRejectsBadOffsets()
{
    constexpr uint32_t oneOfEach = 1 | 1 << 8;
    AOT_TEST_CHECK(!isRejected({ oneOfEach, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ 9, 9, oneOfEach, 2, 4 }, 2));
    AOT_TEST_CHECK(!isRejected({ 2, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ 2 << 16, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ 1 | 1 << 16 | SavedFrameMap::listsEveryLiveWordBit, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ oneOfEach, 2, 4, 0 }, 3));

    AOT_TEST_CHECK(isRejected({ oneOfEach, 2, 4 }, 3));
    AOT_TEST_CHECK(isRejected({ oneOfEach, 2, 4 }, 4));
    AOT_TEST_CHECK(isRejected({ oneOfEach, 2, 4 }, UINT32_MAX));
    AOT_TEST_CHECK(isRejected({ }, 0));
    AOT_TEST_CHECK(isRejected({ oneOfEach, 2, 1 }, 2));

    AOT_TEST_CHECK(isRejected({ 3, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 1 | 2 << 8, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ oneOfEach | 1 << 16, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 3 << 16, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 1 }, 0));
    AOT_TEST_CHECK(isRejected({ 1u << 26, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ UINT32_MAX, 2, 4 }, 0));

    constexpr uint32_t longHeader = SavedFrameMap::hasLongHeaderBit;
    AOT_TEST_CHECK(!isRejected({ longHeader, 1, 1, 0, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ longHeader | SavedFrameMap::listsEveryLiveWordBit, 0, 0, 2, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ longHeader, 0, 0, 0 }, 0));
    AOT_TEST_CHECK(isRejected({ longHeader, 0, 0 }, 0));
    AOT_TEST_CHECK(isRejected({ longHeader | 1, 1, 1, 0, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ longHeader, 3, 0, 0, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ longHeader, 1, 1, 1, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ longHeader, UINT32_MAX, UINT32_MAX, 2, 2, 4 }, 0));
}

static void testMapWithLongHeader()
{
    SavedFrameMapBuilder builder;
    builder.addValue(3);
    builder.addNarrowReference(32 * 256 + 1);
    builder.addOtherLiveWord(40);
    builder.setListsEveryLiveWord();
    Vector<uint32_t, 8> words = builder.finish();
    AOT_TEST_CHECK(words.size() == 4 + 1 + 257 + 2);
    AOT_TEST_CHECK(words[0] == (SavedFrameMap::hasLongHeaderBit | SavedFrameMap::listsEveryLiveWordBit) && words[1] == 1 && words[2] == 257 && words[3] == 2);
    SavedFrameMap map = *SavedFrameMap::tryCreate(words.span(), 0);
    AOT_TEST_CHECK(map.numberOfWords() == words.size() && map.listsEveryLiveWord());
    AOT_TEST_CHECK((valuesOf(map) == Vector<uint32_t> { 3 }) && (narrowReferencesOf(map) == Vector<uint32_t> { 32 * 256 + 1 }) && (otherLiveWordsOf(map) == Vector<uint32_t> { 40 }));
    AOT_TEST_CHECK((deadWordsOf(map, false, 16 * 256 - 1, 16 * 256 + 2) == Vector<uint32_t> { 16 * 256 - 1, 16 * 256, 16 * 256 + 2 }));
}

static void testSiteWord()
{
    constexpr uint32_t maxOffset = (1u << SavedFrame::liveSlotMapOffsetBits) - 1;
    constexpr uint32_t maxLocation = SavedFrame::maxNumberOfWords;
    static_assert(SavedFrame::liveSlotMapOffsetBits == 18 && SavedFrame::savedRegisterBits == 2);

    for (uint32_t offset : { 0u, 1u, 4097u, maxOffset }) {
        for (uint32_t promiseLocation : { 0u, 1u, 77u, 1u << 12, maxLocation }) {
            for (uint32_t savedRegisters : { 0u, 1u, 2u, 3u }) {
                std::optional<uint64_t> word = SavedFrame::siteWord(offset, promiseLocation, savedRegisters);
                AOT_TEST_CHECK(word);
                AOT_TEST_CHECK(SavedFrame::liveSlotMapOffsetOf(*word) == offset);
                AOT_TEST_CHECK(SavedFrame::promiseLocationOf(*word) == promiseLocation);
                AOT_TEST_CHECK(SavedFrame::savedRegistersOrPairsOf(*word) == savedRegisters);
                AOT_TEST_CHECK(!SavedFrame::skippedAtBottomOf(*word));
            }
        }
    }

    AOT_TEST_CHECK(!SavedFrame::siteWord(1u << 18, 0, 0));
    AOT_TEST_CHECK(!SavedFrame::siteWord(UINT32_MAX, 0, 0));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, 4));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, UINT32_MAX));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, maxLocation + 1, 0));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, UINT32_MAX, 0));

    AOT_TEST_CHECK(SavedFrame::savedRegistersOrPairsOf(SavedFrame::functionSavesPreservedRegister) == 1);
    AOT_TEST_CHECK(SavedFrame::savedRegistersOrPairsOf(SavedFrame::functionSavesR15) == 2);
}

static void testSkippedWordsInSiteWord()
{
    constexpr uint32_t maxOffset = (1u << SavedFrame::liveSlotMapOffsetBits) - 1;
    constexpr uint32_t maxLocation = SavedFrame::maxNumberOfWords;
    struct Case {
        uint32_t deadWordsAtBottom;
        uint32_t skippedAtBottom;
    };
    // The number of words kept stays even.
    static constexpr Case cases[] = { { 0, 0 }, { 1, 0 }, { 2, 2 }, { 5, 4 }, { 28, 28 }, { 29, 28 }, { 30, 30 }, { 31, 30 }, { 32, 32 }, { 126, 126 }, { 127, 126 }, { 128, 128 }, { 252, 252 }, { 253, 252 }, { 254, 254 }, { 255, 254 }, { 256, 256 }, { 4098, 4098 }, { maxLocation, maxLocation - 1 } };
    for (const Case& testCase : cases) {
        for (uint32_t promiseLocation : { 0u, 1u, maxLocation }) {
            uint64_t word = *SavedFrame::siteWord(maxOffset, promiseLocation, 3, testCase.deadWordsAtBottom);
            AOT_TEST_CHECK(SavedFrame::skippedAtBottomOf(word) == testCase.skippedAtBottom);
            AOT_TEST_CHECK(SavedFrame::liveSlotMapOffsetOf(word) == maxOffset && SavedFrame::promiseLocationOf(word) == promiseLocation && SavedFrame::savedRegistersOrPairsOf(word) == 3);
        }
    }
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, 0, maxLocation + 1));
    AOT_TEST_CHECK(!(*SavedFrame::siteWord(maxOffset, 15, 3, 28, 0, SavedFrame::maxClearedWords) >> 32) && *SavedFrame::siteWord(maxOffset, 15, 3, 30) >> 32 && *SavedFrame::siteWord(maxOffset, 16, 3, 28) >> 32);

    for (uint32_t savedRegistersOrPairs : { 0u, 1u, 2u, 3u }) {
        uint32_t skippedAtTop = SavedFrame::skippedAtTopOf(*SavedFrame::siteWord(maxOffset, maxLocation, savedRegistersOrPairs, 126));
#if CPU(X86_64)
        AOT_TEST_CHECK(skippedAtTop == (savedRegistersOrPairs == 3 ? 2u : 0u));
#else
        AOT_TEST_CHECK(skippedAtTop == 2 * savedRegistersOrPairs);
#endif
    }

    AOT_TEST_CHECK(SavedFrame::siteWord(0, 0, 0, 0, SavedFrame::maxNumberOfWords));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, 0, 0, SavedFrame::maxNumberOfWords + 1));
}

static void testClearedWordsInSiteWord()
{
    constexpr uint32_t maxOffset = (1u << SavedFrame::liveSlotMapOffsetBits) - 1;
    constexpr uint32_t maxLocation = SavedFrame::maxNumberOfWords;
    for (uint32_t clearedWords : { 0u, 1u, 6u, SavedFrame::maxClearedWords }) {
        for (uint32_t deadWordsAtBottom : { 0u, 30u, 32u, maxLocation - 1 }) {
            for (uint32_t promiseLocation : { 0u, 15u, maxLocation }) {
                uint64_t word = *SavedFrame::siteWord(maxOffset, promiseLocation, 3, deadWordsAtBottom, 0, clearedWords);
                AOT_TEST_CHECK(SavedFrame::clearedWordsOf(word) == clearedWords && SavedFrame::skippedAtBottomOf(word) == deadWordsAtBottom);
                AOT_TEST_CHECK(SavedFrame::liveSlotMapOffsetOf(word) == maxOffset && SavedFrame::promiseLocationOf(word) == promiseLocation && SavedFrame::savedRegistersOrPairsOf(word) == 3);
            }
        }
    }
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, 0, 0, 0, SavedFrame::maxClearedWords + 1));
}

static void testNarrowLocations()
{
    AOT_TEST_CHECK(SavedFrame::locationInFrame(-8) == 1 && SavedFrame::locationInFrame(-16) == 2 && SavedFrame::locationInFrame(-800) == 100);

    AOT_TEST_CHECK(SavedFrame::narrowLocationInFrame(-4) == 1);
    AOT_TEST_CHECK(SavedFrame::locationHolding(1) == 1 && SavedFrame::isInUpperHalf(1));
    AOT_TEST_CHECK(SavedFrame::narrowLocationInFrame(-8) == 2);
    AOT_TEST_CHECK(SavedFrame::locationHolding(2) == 1 && !SavedFrame::isInUpperHalf(2));
    AOT_TEST_CHECK(SavedFrame::narrowLocationInFrame(-12) == 3);
    AOT_TEST_CHECK(SavedFrame::locationHolding(3) == 2 && SavedFrame::isInUpperHalf(3));
    AOT_TEST_CHECK(SavedFrame::narrowLocationInFrame(-16) == 4);
    AOT_TEST_CHECK(SavedFrame::locationHolding(4) == 2 && !SavedFrame::isInUpperHalf(4));

    AOT_TEST_CHECK(SavedFrame::locationHolding(0) == SavedFrame::locationInRegister && !SavedFrame::isInUpperHalf(0));

    for (intptr_t offset = -8; offset >= -400; offset -= 8) {
        AOT_TEST_CHECK(SavedFrame::locationHolding(SavedFrame::narrowLocationInFrame(offset)) == SavedFrame::locationInFrame(offset));
        AOT_TEST_CHECK(SavedFrame::locationHolding(SavedFrame::narrowLocationInFrame(offset + 4)) == SavedFrame::locationInFrame(offset));
    }
}

static void testResumeModes()
{
    Vector<uint32_t> engineModes;
    for (ResumeMode mode : { ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return }) {
        uint32_t engineMode = static_cast<uint32_t>(engineResumeModeOf(mode));
        AOT_TEST_CHECK(resumeModeBitsOfEngineMode(engineMode) == static_cast<uint32_t>(mode));
        AOT_TEST_CHECK(!engineModes.contains(engineMode));
        engineModes.append(engineMode);
    }
    AOT_TEST_CHECK(engineResumeModeOf(ResumeMode::Next) == JSGenerator::ResumeMode::NormalMode);
    AOT_TEST_CHECK(engineResumeModeOf(ResumeMode::Throw) == JSGenerator::ResumeMode::ThrowMode);
    AOT_TEST_CHECK(engineResumeModeOf(ResumeMode::Return) == JSGenerator::ResumeMode::ReturnMode);
}

static unsigned numberOfSavedFrames(VM& vm)
{
    unsigned count = 0;
    HeapIterationScope iterationScope(vm.heap);
    vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
        if (isJSCellKind(kind) && static_cast<JSCell*>(cell)->inherits<SavedFrame>())
            ++count;
        return IterationStatus::Continue;
    });
    return count;
}

static void checkSavedFrameIsCallableForTheEngineOnly(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(!numberOfSavedFrames(vm));

    SavedFrame* saved = SavedFrame::create(vm, program.instance(), CoroutineKind::AsyncFunction, 2, 0, 0);
    AOT_TEST_CHECK(numberOfSavedFrames(vm) == 1);
    AOT_TEST_CHECK(saved->structure() == program.state().coroutines->savedFrameStructure());
    AOT_TEST_CHECK(saved->structure()->typeInfo().overridesGetCallData() && saved->getPrototypeDirect().isNull());
    AOT_TEST_CHECK(saved->kind() == CoroutineKind::AsyncFunction && saved->isOnStack() && saved->promiseOrWaiter() == saved && !saved->promise() && !saved->waiter());

    CallData callData = JSC::getCallData(saved);
    AOT_TEST_CHECK(callData.type == CallData::Type::Compiled);
    AOT_TEST_CHECK(callData.compiled.entrypoint == program.state().coroutines->savedFrameEntrypoint());
    AOT_TEST_CHECK(callData.compiled.entrypoint == savedFrameEntrypoint().taggedPtr());
    AOT_TEST_CHECK(callData.compiled.realm == program.realm());

    AOT_TEST_CHECK(JSC::getCallData(constructEmptyObject(program.realm())).type == CallData::Type::None);
}

namespace {

struct FunctionWithMap {
    uint32_t function;
    Vector<uint32_t> listedLocations;
};

struct HandMadeFrame {
    static constexpr unsigned numberOfCandidates = 64;
    static constexpr uint64_t integerWord = JSValue::NumberTag | 7;

    uint64_t& wordAt(uint32_t location) { return stack[numberOfWords - location]; }
    uint32_t firstKeptLocation() const { return SavedFrame::skippedAtTopOf(siteWord) + 1; }
    uint32_t lastKeptLocation() const { return numberOfWords - SavedFrame::skippedAtBottomOf(siteWord); }

    uint64_t siteWord { 0 };
    uint32_t numberOfWords { 0 };
    Vector<uint64_t> stack;
    Vector<uint32_t> candidateLocations;
    uint32_t locationOfLiveCell { 0 };
    uint32_t locationOfInteger { 0 };
    uint32_t locationOfZero { 0 };
};

} // anonymous namespace

static FunctionWithMap findFunctionWithMap(const Image& image)
{
    std::optional<uint32_t> found;
    for (uint32_t function = 0; function < image.header().counts.functions; ++function) {
        if (image.frameMapWords(function).empty())
            continue;
        AOT_TEST_CHECK(!found);
        found = function;
    }
    AOT_TEST_CHECK(found);

    std::span<const uint32_t> words = image.frameMapWords(*found);
    AOT_TEST_CHECK(SavedFrameMap::tryCreate(words, 0)->numberOfWords() == words.size());
    // The frames are made up, and the map of the compiled function does not list what is live in them.
    overwriteWordInImage(words[0], words[0] & ~SavedFrameMap::listsEveryLiveWordBit);
    std::optional<SavedFrameMap> map = SavedFrameMap::tryCreate(words, 0);
    AOT_TEST_CHECK(map && narrowReferencesOf(*map).isEmpty() && !map->listsEveryLiveWord());
    return { *found, valuesOf(*map) };
}

static NEVER_INLINE HandMadeFrame fillFrameWithUnreferencedObjects(LoadedProgram& program, const Vector<uint32_t>& listedLocations, uint64_t siteWord = *SavedFrame::siteWord(0, 1, 0))
{
    HandMadeFrame frame;
    frame.siteWord = siteWord;
    frame.numberOfWords = listedLocations.last() + HandMadeFrame::numberOfCandidates + 3 + SavedFrame::skippedAtBottomOf(siteWord);
    frame.stack.fill(0, frame.numberOfWords + 2);
    AOT_TEST_CHECK(listedLocations.first() >= frame.firstKeptLocation());

    Vector<uint32_t> unlistedLocations;
    for (uint32_t location = frame.firstKeptLocation(); location <= frame.lastKeptLocation(); ++location) {
        if (!listedLocations.contains(location))
            unlistedLocations.append(location);
    }
    frame.locationOfLiveCell = unlistedLocations.takeLast();
    frame.locationOfInteger = unlistedLocations.takeLast();
    frame.locationOfZero = unlistedLocations.takeLast();
    frame.candidateLocations = WTF::move(unlistedLocations);
    AOT_TEST_CHECK(frame.candidateLocations.size() >= HandMadeFrame::numberOfCandidates);

    for (uint32_t location = 1; location <= frame.numberOfWords; ++location)
        frame.wordAt(location) = std::bit_cast<uint64_t>(constructEmptyObject(program.realm()));
    frame.wordAt(frame.locationOfLiveCell) = std::bit_cast<uint64_t>(program.realm());
    frame.wordAt(frame.locationOfInteger) = HandMadeFrame::integerWord;
    frame.wordAt(frame.locationOfZero) = 0;
    return frame;
}

static SavedFrame* saveHandMadeFrame(LoadedProgram& program, HandMadeFrame& frame, uint32_t function)
{
    VM& vm = program.vm();
    SuspensionWords words { frame.siteWord, HandMadeFrame::integerWord };
    SuspendedMachineFrame machineFrame {
        frame.stack.mutableSpan().data() + frame.numberOfWords,
        frame.stack.span().data(),
        const_cast<uint8_t*>(Image::current()->codeOf(function)) + 1,
        &words,
    };
    AOT_TEST_CHECK(machineFrame.numberOfWords() == frame.lastKeptLocation() - frame.firstKeptLocation() + 1);
    AOT_TEST_CHECK(machineFrame.firstSavedWord() == &frame.wordAt(frame.lastKeptLocation()));

    SavedFrame* saved;
    {
        SetForScope validation(vm.aotState().validatesRuntime, true);
        saved = SavedFrame::create(vm, program.instance(), CoroutineKind::AsyncFunction, machineFrame);
    }
    saved->save(vm, machineFrame, SuspensionKind::Await);
    AOT_TEST_CHECK(!saved->isOnStack() && saved->suspensionKind() == SuspensionKind::Await && saved->resumeAddress() == machineFrame.resumeAddress);
    return saved;
}

static void checkWordsAfterCollection(LoadedProgram& program, HandMadeFrame& frame, const SavedFrame& saved, const Vector<uint32_t>& listedLocations)
{
    VM& vm = program.vm();
    for (uint32_t location : listedLocations) {
        AOT_TEST_CHECK(saved.wordAt(location) == frame.wordAt(location));
        AOT_TEST_CHECK(vm.heap.isMarked(std::bit_cast<JSCell*>(frame.wordAt(location))));
    }

    unsigned numberOfDeadCells = 0;
    for (uint32_t location : frame.candidateLocations) {
        bool isDead = !hasSurvivedCollection(vm, std::bit_cast<JSCell*>(frame.wordAt(location)));
        numberOfDeadCells += isDead;
        AOT_TEST_CHECK(saved.wordAt(location) == (isDead ? SavedFrame::poisonedWord : frame.wordAt(location)));
    }
    AOT_TEST_CHECK(numberOfDeadCells);

    AOT_TEST_CHECK(saved.wordAt(frame.locationOfLiveCell) == std::bit_cast<uint64_t>(program.realm()));
    AOT_TEST_CHECK(saved.wordAt(frame.locationOfInteger) == HandMadeFrame::integerWord);
    AOT_TEST_CHECK(saved.wordAt(frame.locationOfZero) == 0u);
}

static void checkUnlistedDeadWordIsPoisoned(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionWithMap compiled = findFunctionWithMap(*Image::current());
    Vector<uint32_t> listedInFrame = compiled.listedLocations;
    listedInFrame.removeAll(SavedFrame::locationInRegister);
    AOT_TEST_CHECK(!listedInFrame.isEmpty());

    HandMadeFrame frame = fillFrameWithUnreferencedObjects(program, listedInFrame);
    SavedFrame* saved = saveHandMadeFrame(program, frame, compiled.function);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.isMarked(saved));

    checkWordsAfterCollection(program, frame, *saved, listedInFrame);
}

// The map of the compiled function, made to list other words of the frame.
static void listValuesInMapOf(uint32_t function, const Vector<uint32_t>& locations)
{
    std::span<const uint32_t> words = Image::current()->frameMapWords(function);
    uint32_t numberOfValueWords = words[0] & 0xff;
    AOT_TEST_CHECK(numberOfValueWords && locations.last() < 32 * numberOfValueWords);
    for (uint32_t index = 0; index < numberOfValueWords; ++index) {
        uint32_t bits = 0;
        for (uint32_t location : locations) {
            if (location / 32 == index)
                bits |= 1u << location % 32;
        }
        overwriteWordInImage(words[1 + index], bits);
    }
    AOT_TEST_CHECK(valuesOf(*SavedFrameMap::tryCreate(words, 0)) == locations);
}

static void checkSkippedWords(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t function = findFunctionWithMap(*Image::current()).function;
    for (uint32_t savedRegistersOrPairs : { 0u, 1u, 3u }) {
        for (uint32_t skippedAtBottom : { 0u, 4u, 252u, 254u, 600u }) {
            uint64_t siteWord = *SavedFrame::siteWord(0, 1, savedRegistersOrPairs, skippedAtBottom);
            uint32_t skippedAtTop = SavedFrame::skippedAtTopOf(siteWord);
            AOT_TEST_CHECK(SavedFrame::skippedAtBottomOf(siteWord) == skippedAtBottom);
            Vector<uint32_t> listedLocations { skippedAtTop + 1, skippedAtTop + 2, skippedAtTop + 9 };
            listValuesInMapOf(function, listedLocations);

            HandMadeFrame frame = fillFrameWithUnreferencedObjects(program, listedLocations, siteWord);
            AOT_TEST_CHECK(frame.firstKeptLocation() == skippedAtTop + 1 && frame.lastKeptLocation() + skippedAtBottom == frame.numberOfWords);
            SavedFrame* saved = saveHandMadeFrame(program, frame, function);
            vm.heap.collectSync(CollectionScope::Full);
            AOT_TEST_CHECK(vm.heap.isMarked(saved));
            checkWordsAfterCollection(program, frame, *saved, listedLocations);

            for (uint32_t location = 1; location < frame.firstKeptLocation(); ++location)
                AOT_TEST_CHECK(!saved->wordAt(location));
            for (uint32_t location = frame.lastKeptLocation() + 1; location <= frame.numberOfWords + 2; ++location)
                AOT_TEST_CHECK(!saved->wordAt(location));
            AOT_TEST_CHECK(frame.locationOfLiveCell == frame.lastKeptLocation());
            AOT_TEST_CHECK(*reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(saved) + SavedFrame::offsetOfWords()) == frame.wordAt(frame.lastKeptLocation()));

            // The next round changes the map, and a word that it lists may be poisoned in this frame.
            saved->willBeRestoredToStack();
        }
    }
}

static void checkLowestWordsAreClearedAtTheFirstSave(LoadedProgram& program)
{
    uint32_t function = findFunctionWithMap(*Image::current()).function;
    for (uint32_t skippedAtBottom : { 0u, 4u, 40u }) {
        for (uint32_t clearedWords : { 0u, 1u, 3u, SavedFrame::maxClearedWords }) {
            uint64_t siteWord = *SavedFrame::siteWord(0, 1, 0, skippedAtBottom, 0, clearedWords);
            Vector<uint32_t> listedLocations { 1, 2 };
            listValuesInMapOf(function, listedLocations);
            HandMadeFrame frame = fillFrameWithUnreferencedObjects(program, listedLocations, siteWord);
            Vector<uint64_t> stackBefore = frame.stack;
            SavedFrame* saved = saveHandMadeFrame(program, frame, function);
            for (uint32_t location = 1; location <= frame.numberOfWords; ++location) {
                bool isCleared = location <= frame.lastKeptLocation() && location + clearedWords > frame.lastKeptLocation();
                AOT_TEST_CHECK(frame.wordAt(location) == (isCleared ? 0 : stackBefore[frame.numberOfWords - location]));
                if (location <= frame.lastKeptLocation())
                    AOT_TEST_CHECK(saved->wordAt(location) == frame.wordAt(location));
            }
            saved->willBeRestoredToStack();
        }
    }
}

static void checkFrameOnTheStackIsLeftAlone(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionWithMap compiled = findFunctionWithMap(*Image::current());
    Vector<uint32_t> listedInFrame = compiled.listedLocations;
    listedInFrame.removeAll(SavedFrame::locationInRegister);

    HandMadeFrame frame = fillFrameWithUnreferencedObjects(program, listedInFrame);
    SavedFrame* saved = saveHandMadeFrame(program, frame, compiled.function);
    saved->willBeRestoredToStack();
    AOT_TEST_CHECK(saved->isOnStack());
    vm.heap.collectSync(CollectionScope::Full);

    unsigned numberOfDeadCells = 0;
    for (uint32_t location = 1; location <= frame.numberOfWords; ++location) {
        AOT_TEST_CHECK(saved->wordAt(location) == frame.wordAt(location));
        if (frame.candidateLocations.contains(location) || listedInFrame.contains(location))
            numberOfDeadCells += !hasSurvivedCollection(vm, std::bit_cast<JSCell*>(frame.wordAt(location)));
    }
    AOT_TEST_CHECK(numberOfDeadCells);
}

static NEVER_INLINE Vector<uint64_t> makeStackOfUnreferencedObjects(LoadedProgram& program, uint32_t numberOfWords)
{
    Vector<uint64_t> stack;
    for (uint32_t index = 0; index < numberOfWords + 2; ++index)
        stack.append(std::bit_cast<uint64_t>(constructEmptyObject(program.realm())));
    return stack;
}

static void checkDeadWordsAreOverwrittenAtACollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    const Image& image = *Image::current();
    std::optional<uint32_t> function;
    for (uint32_t candidate = 0; candidate < image.header().counts.functions; ++candidate) {
        if (image.frameMapWords(candidate).empty())
            continue;
        AOT_TEST_CHECK(!function);
        function = candidate;
    }
    AOT_TEST_CHECK(function);

    std::span<const uint32_t> words = image.frameMapWords(*function);
    Vector<unsigned> numbersOfLiveWords;
    for (uint32_t offset = 0, nextOffset = 0; offset < words.size(); offset = nextOffset) {
        SavedFrameMap map = *SavedFrameMap::tryCreate(words, offset);
        nextOffset = offset + map.numberOfWords();
        AOT_TEST_CHECK(map.listsEveryLiveWord());
        Vector<uint32_t> values = valuesOf(map);
        Vector<uint32_t> otherLiveWords = otherLiveWordsOf(map);
        numbersOfLiveWords.append(values.size() + otherLiveWords.size());

        const uint32_t numberOfWords = WTF::roundUpToMultipleOf<2>(std::max(values.isEmpty() ? 0u : values.last(), otherLiveWords.isEmpty() ? 0u : otherLiveWords.last()) + 3);
        Vector<uint64_t> stack = makeStackOfUnreferencedObjects(program, numberOfWords);
        auto wordOnStack = [&](uint32_t location) { return stack[numberOfWords - location]; };
        SuspensionWords suspensionWords { *SavedFrame::siteWord(offset, 1, 0), stack[numberOfWords + 1] };
        SuspendedMachineFrame machineFrame {
            stack.mutableSpan().data() + numberOfWords,
            stack.span().data(),
            const_cast<uint8_t*>(image.codeOf(*function)) + 1,
            &suspensionWords,
        };
        auto checkWords = [&](const SavedFrame& saved, bool areDeadWordsOverwritten) {
            for (uint32_t location = 1; location <= numberOfWords; ++location)
                AOT_TEST_CHECK(saved.wordAt(location) == (areDeadWordsOverwritten && !listsAsLive(map, location) ? 0 : wordOnStack(location)));
            if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
                AOT_TEST_CHECK(saved.wordAt(SavedFrame::locationInRegister) == (areDeadWordsOverwritten && !listsAsLive(map, SavedFrame::locationInRegister) ? 0 : suspensionWords.preservedRegister));
        };

        SavedFrame* saved = SavedFrame::create(vm, program.instance(), CoroutineKind::AsyncFunction, machineFrame);
        for (CollectionScope scope : { CollectionScope::Full, CollectionScope::Eden, CollectionScope::Full }) {
            saved->save(vm, machineFrame, SuspensionKind::Await);
            checkWords(*saved, false);
            // An Eden collection marks nothing where nothing was allocated since the last collection.
            constructEmptyObject(program.realm());
            vm.heap.collectSync(scope);
            AOT_TEST_CHECK(vm.heap.isMarked(saved) && !saved->isOnStack());
            checkWords(*saved, true);
            for (uint32_t location : values) {
                if (location != SavedFrame::locationInRegister)
                    AOT_TEST_CHECK(vm.heap.isMarked(std::bit_cast<JSCell*>(wordOnStack(location))));
            }
            vm.heap.collectSync(CollectionScope::Full);
            checkWords(*saved, true);
            saved->willBeRestoredToStack();
        }

        saved->save(vm, machineFrame, SuspensionKind::Await);
        saved->willBeRestoredToStack();
        vm.heap.collectSync(CollectionScope::Full);
        checkWords(*saved, false);
    }
    // Four values are last read between the two awaits.
    AOT_TEST_CHECK(numbersOfLiveWords.size() == 2 && numbersOfLiveWords[0] >= numbersOfLiveWords[1] + 4);
}

static std::unique_ptr<CompileOutput> compileWithValidation(Wire::ReferenceWidth referenceWidth)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, "validate"_s));
        options.target.referenceWidth = referenceWidth;
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    });
    AOT_TEST_CHECK(!output->image.isEmpty() && output->entryModuleIDs.size() == 1);
    return output;
}

static constexpr ASCIILiteral scriptStart = R"js(
let start;
globalThis.register = callee => { start = callee; };
)js"_s;

static constexpr ASCIILiteral scriptEnd = R"js(.then(() => {
    let count = 0;
    let result = start(() => ({ tag: "object " + ++count }), Promise.resolve("resolved"));
    print("suspended after " + count + " calls");
    fullGC();
    let garbage = [];
    for (let i = 0; i < 100000; ++i)
        garbage.push({ tag: "garbage " + i });
    edenGC();
    return result;
}).then(value => print(JSON.stringify(value))).catch(error => print("the script threw " + error));
)js"_s;

static void testListedWordsSurviveACollection()
{
    for (Wire::ReferenceWidth referenceWidth : { Wire::ReferenceWidth::Wide, Wire::ReferenceWidth::Narrow }) {
        std::unique_ptr<CompileOutput> output = compileWithValidation(referenceWidth);
        TemporaryFile image(".image"_s, output->image.span());
        Vector<String> arguments {
            makeString("--aotImagePath="_s, image.path()),
            String("-e"_s),
            makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ')', scriptEnd),
        };

        String shell = shellPath();
        ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
        if (!result.exitedWith(EXIT_SUCCESS))
            dataLogLn(shell, " ", result.description(), "\n", result.output);
        AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));

        String printed = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
        if (printed != "suspended after 2 calls\n[\"object 2\",\"resolved\",\"object 1\"]\n"_s)
            dataLog(printed);
        AOT_TEST_CHECK(printed == "suspended after 2 calls\n[\"object 2\",\"resolved\",\"object 1\"]\n"_s);
    }
}

// An async function that keeps so many values alive over its await. They are block parameters, and calls of at most a thousand arguments read them behind it.
static String programWithRoots(unsigned numberOfRoots)
{
    constexpr unsigned firstRoot = 3;
    constexpr unsigned argumentsInCall = 1000;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\n"_s);
    text.append("function f0 name=\"waits\" role=CoroutineBody flags=[IsAsync] parameters=[{type=anything}, {type=anything}, {type=undefined|Promise}] results=[{type=anything}] creator=Coroutine{kind=AsyncFunction ramp=f1 body=f0 promiseType=Promise promiseParameter=2} {\n"_s);
    text.append("  b0(v0: anything @JSValue, v1: anything @JSValue, v2: undefined|Promise @JSValue):\n    Jump -> next b1("_s);
    for (unsigned root = 0; root < numberOfRoots; ++root)
        text.append(root ? ", v1"_s : "v1"_s);
    text.append(")\n  b1("_s);
    for (unsigned root = 0; root < numberOfRoots; ++root)
        text.append(root ? ", v"_s : "v"_s, firstRoot + root, " @JSValue"_s);
    unsigned awaited = firstRoot + numberOfRoots;
    text.append("):\n    v"_s, awaited, " @JSValue = AwaitBySavingFrame v0, v2\n"_s);
    unsigned last = awaited;
    for (unsigned root = 0; root < numberOfRoots; root += argumentsInCall) {
        text.append("    v"_s, ++last, " @JSValue = Call v"_s, awaited, ", -"_s);
        for (unsigned argument = root; argument < std::min(root + argumentsInCall, numberOfRoots); ++argument)
            text.append(", v"_s, firstRoot + argument);
        text.append('\n');
    }
    text.append("    Return v"_s, last, "\n}\n"_s);
    text.append("function f1 name=\"waits\" role=CoroutineRamp flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=Promise}] creator=Coroutine{kind=AsyncFunction ramp=f1 body=f0 promiseType=Promise promiseParameter=2}\n"_s);
    return text.toString();
}

static uint64_t rootLimitHitsWith(unsigned numberOfRoots)
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(programWithRoots(numberOfRoots), Typed::IRForm::Placed, build);
    AOT_TEST_CHECK(program);
    Typed::FunctionID body(0u);
    const Typed::Function& function = program->at(body);
    Typed::FunctionAnalyses analyses(Typed::AnalysisContext { build, *program, nullptr, nullptr, body }, function);
    Typed::FrameMaps maps = Typed::computeFrameMaps(*program, function, analyses);
    AOT_TEST_CHECK(maps.numberOfSuspensions() == 1);
    uint64_t hits = build.statistics().values()[static_cast<unsigned>(hitsOf(LimitID::MaxFrameMapRoots))];
    AOT_TEST_CHECK(build.diagnostics().hasErrors() == !!hits);
    return hits;
}

static void testRootLimit()
{
    constexpr unsigned limit = defaultLimitValue(LimitID::MaxFrameMapRoots, hostArchitecture());
    AOT_TEST_CHECK(!rootLimitHitsWith(limit));
    AOT_TEST_CHECK(rootLimitHitsWith(limit + 1) == 1);
}

static constexpr uint64_t wordOfDeadStack = 0x5ca1ab1e5ca1ab1eULL;
static unsigned numberOfSaveAreasRead;
static const Instance* instanceOfSaveArea;

static NEVER_INLINE void fillDeadStack()
{
    volatile uint64_t words[2048];
    for (volatile uint64_t& word : words)
        word = wordOfDeadStack;
}

static JSC_DECLARE_HOST_FUNCTION(readSaveAreaAboveResumedCaller);
JSC_DEFINE_HOST_FUNCTION(readSaveAreaAboveResumedCaller, (JSGlobalObject*, CallFrame* callFrame))
{
    // The call behind the first await is the one with arguments.
    if (!callFrame->argumentCount())
        return JSValue::encode(jsNumber(1));
    // The adapter's is the first frame above that holds the instance in its place.
    constexpr ptrdiff_t instanceWord = AdapterFrame::offsetOfInstance / static_cast<ptrdiff_t>(sizeof(uint64_t));
    auto* frameOfAdapter = reinterpret_cast<const uint64_t*>(callFrame);
    unsigned numberOfFrames = 0;
    do {
        frameOfAdapter = *reinterpret_cast<const uint64_t* const*>(frameOfAdapter);
        AOT_TEST_CHECK(++numberOfFrames < 6);
    } while (frameOfAdapter[instanceWord] != std::bit_cast<uintptr_t>(instanceOfSaveArea));
    for (ptrdiff_t word = 1; word <= static_cast<ptrdiff_t>(AdapterFrame::saveAreaSize / sizeof(uint64_t)); ++word)
        AOT_TEST_CHECK(frameOfAdapter[-word] != wordOfDeadStack);
    ++numberOfSaveAreasRead;
    return JSValue::encode(jsNumber(2));
}

static void checkAdapterAboveResumedFrameWritesItsSaveArea(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    program.realms().evaluate(realm, "globalThis.register = waitTwice => { globalThis.waitTwice = waitTwice; };"_s);
    std::optional<ModuleID> module = findModule("main.js"_s);
    AOT_TEST_CHECK(module);
    evaluateModuleSynchronously(realm, *module);
    AOT_TEST_CHECK(!scope.exception());

    MarkedArgumentBuffer arguments;
    arguments.append(JSFunction::create(vm, realm, 0, "readSaveArea"_s, readSaveAreaAboveResumedCaller, ImplementationVisibility::Public));
    arguments.append(program.realms().evaluate(realm, "Promise.resolve(3)"_s));
    numberOfSaveAreasRead = 0;
    instanceOfSaveArea = &program.instance();
    call(realm, program.realms().evaluate(realm, "waitTwice"_s), arguments, "waitTwice is a function"_s);
    AOT_TEST_CHECK(!scope.exception() && !numberOfSaveAreasRead);
    fillDeadStack();
    vm.drainMicrotasks();
    AOT_TEST_CHECK(!scope.exception() && numberOfSaveAreasRead == 1);
}

static const uint64_t* frameOfJobCaller;
static unsigned numberOfJobEntriesRead;

static JSC_DECLARE_HOST_FUNCTION(readFramesBelowJobCaller);
JSC_DEFINE_HOST_FUNCTION(readFramesBelowJobCaller, (JSGlobalObject*, CallFrame* callFrame))
{
    if (!callFrame->argumentCount())
        return JSValue::encode(jsNumber(1));
    auto* frame = reinterpret_cast<const uint64_t*>(callFrame);
    unsigned numberOfFrames = 0;
    while (*reinterpret_cast<const uint64_t* const*>(frame) != frameOfJobCaller) {
        frame = *reinterpret_cast<const uint64_t* const*>(frame);
        AOT_TEST_CHECK(++numberOfFrames < 12);
    }
    unsigned numberOfUnwrittenWords = 0;
    for (auto* word = reinterpret_cast<const uint64_t*>(callFrame); word < frame; ++word)
        numberOfUnwrittenWords += *word == wordOfDeadStack;
#if CPU(ARM64)
    // Registers are saved in pairs: a frame that saves an odd number of them has a word of padding.
    AOT_TEST_CHECK(numberOfUnwrittenWords <= numberOfFrames);
#else
    AOT_TEST_CHECK(!numberOfUnwrittenWords);
#endif
    ++numberOfJobEntriesRead;
    return JSValue::encode(jsNumber(2));
}

static NEVER_INLINE void runJobOverFilledStack(JSGlobalObject* realm, VM& vm, const QueuedTask& job)
{
    NO_TAIL_CALLS();
    frameOfJobCaller = static_cast<const uint64_t*>(__builtin_frame_address(0));
    fillDeadStack();
    auto arguments = job.arguments();
    runInternalMicrotask(realm, vm, job.job(), job.payload(), arguments[0], arguments[1], arguments[2], arguments[3]);
}

// A garbage collection reads every word of every frame. The job's own frame is gone when compiled code runs, and the frames on the way in write all of their words.
static void checkFramesBetweenJobAndCompiledCodeAreWritten(LoadedProgram& program)
{
#if HAVE(MUST_TAIL_CALL) && !ENABLE(EXCEPTION_SCOPE_VERIFICATION) && !ASAN_ENABLED
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    program.realms().evaluate(realm, "globalThis.register = waitTwice => { globalThis.waitTwice = waitTwice; };"_s);
    std::optional<ModuleID> module = findModule("main.js"_s);
    AOT_TEST_CHECK(module);
    evaluateModuleSynchronously(realm, *module);
    AOT_TEST_CHECK(!scope.exception());

    MarkedArgumentBuffer arguments;
    arguments.append(JSFunction::create(vm, realm, 0, "readFrames"_s, readFramesBelowJobCaller, ImplementationVisibility::Public));
    arguments.append(program.realms().evaluate(realm, "Promise.resolve(3)"_s));
    numberOfJobEntriesRead = 0;
    call(realm, program.realms().evaluate(realm, "waitTwice"_s), arguments, "waitTwice is a function"_s);
    AOT_TEST_CHECK(!scope.exception() && !numberOfJobEntriesRead);

    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    AOT_TEST_CHECK(queue.size() == 1);
    std::optional<QueuedTask> job;
    queue.allOf([&](const QueuedTask& queued) {
        job = queued;
        return true;
    });
    AOT_TEST_CHECK(job->job() == InternalMicrotask::CompiledAsyncFunctionResume);
    queue.clear();
    runJobOverFilledStack(realm, vm, *job);
    AOT_TEST_CHECK(!scope.exception() && numberOfJobEntriesRead == 1);
    vm.drainMicrotasks();
#else
    UNUSED_PARAM(program);
#endif
}

std::span<const NativeTest> savedFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "saved-frame/map-round-trip"_s, testMapRoundTrip },
        { "saved-frame/map-with-one-plane"_s, testMapWithOnePlane },
        { "saved-frame/map-that-lists-every-live-word"_s, testMapThatListsEveryLiveWord },
        { "saved-frame/empty-map"_s, testEmptyMap },
        { "saved-frame/walk-stops-when-the-functor-does"_s, testWalkStopsWhenTheFunctorDoes },
        { "saved-frame/map-rejects-bad-offsets"_s, testMapRejectsBadOffsets },
        { "saved-frame/map-with-long-header"_s, testMapWithLongHeader },
        { "saved-frame/site-word"_s, testSiteWord },
        { "saved-frame/skipped-words-in-site-word"_s, testSkippedWordsInSiteWord },
        { "saved-frame/cleared-words-in-site-word"_s, testClearedWordsInSiteWord },
        { "saved-frame/narrow-locations"_s, testNarrowLocations },
        { "saved-frame/resume-modes"_s, testResumeModes },
        { "saved-frame/root-limit"_s, testRootLimit },
        { "saved-frame/callable-for-the-engine-only"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkSavedFrameIsCallableForTheEngineOnly> },
        { "saved-frame/unlisted-dead-word-is-poisoned"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkUnlistedDeadWordIsPoisoned> },
        { "saved-frame/frame-on-the-stack-is-left-alone"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkFrameOnTheStackIsLeftAlone> },
        { "saved-frame/skipped-words"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkSkippedWords> },
        { "saved-frame/lowest-words-are-cleared-at-the-first-save"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkLowestWordsAreClearedAtTheFirstSave> },
        { "saved-frame/dead-words-are-overwritten-at-a-collection"_s, runNativeTestWithLoadedProgram<savedFrameTests, programWithTwoAwaitsText, checkDeadWordsAreOverwrittenAtACollection> },
        { "saved-frame/adapter-above-a-resumed-frame-writes-its-save-area"_s, runNativeTestWithLoadedProgram<savedFrameTests, programWithTwoAwaitsText, checkAdapterAboveResumedFrameWritesItsSaveArea> },
        { "saved-frame/frames-between-job-and-compiled-code-are-written"_s, runNativeTestWithLoadedProgram<savedFrameTests, programWithTwoAwaitsText, checkFramesBetweenJobAndCompiledCodeAreWritten> },
        { "saved-frame/listed-words-survive-a-collection"_s, testListedWordsSurviveACollection },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
