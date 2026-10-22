/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTCoroutineTable.h"
#include "AOTFrameMaps.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTSavedFrame.h"
#include "AOTSavedFrameMap.h"
#include "AOTShellProcess.h"
#include "AOTStubsCoroutines.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVMState.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "ObjectConstructor.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> savedFrameTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="waitFor" flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Call v0
    v3 = Call v0
    v4 = PropGet v3 key="tag"
    v5 = Await v1
    v6 = PropGet v2 key="tag"
    v7 = NewArray v4, v5, v6 contents=anything
    Return v7
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

static void checkLists(const SavedFrameMap& map, const Vector<uint32_t>& values, const Vector<uint32_t>& narrowReferences)
{
    AOT_TEST_CHECK(valuesOf(map) == values);
    AOT_TEST_CHECK(narrowReferencesOf(map) == narrowReferences);
    for (uint32_t location = 0; location < 200; ++location) {
        AOT_TEST_CHECK(map.listsValue(location) == values.contains(location));
        AOT_TEST_CHECK(map.listsNarrowReference(location) == narrowReferences.contains(location));
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
    AOT_TEST_CHECK(words.size() == 2 + 3 + 3);
    AOT_TEST_CHECK(words[0] == words.size() - 1 && words[1] == 3);

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
    AOT_TEST_CHECK((valueWords == Vector<uint32_t, 8> { 3, 2, 0, 2 }));
    checkLists(*SavedFrameMap::tryCreate(valueWords.span(), 0), { 33 }, { });

    SavedFrameMapBuilder narrowReferencesOnly;
    narrowReferencesOnly.addNarrowReference(33);
    Vector<uint32_t, 8> narrowWords = narrowReferencesOnly.finish();
    AOT_TEST_CHECK((narrowWords == Vector<uint32_t, 8> { 3, 0, 0, 2 }));
    checkLists(*SavedFrameMap::tryCreate(narrowWords.span(), 0), { }, { 33 });
}

static void testEmptyMap()
{
    Vector<uint32_t, 8> words = SavedFrameMapBuilder().finish();
    AOT_TEST_CHECK((words == Vector<uint32_t, 8> { 1, 0 }));
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
    AOT_TEST_CHECK(!isRejected({ 3, 1, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ 9, 9, 3, 1, 2, 4 }, 2));
    AOT_TEST_CHECK(!isRejected({ 3, 2, 2, 4 }, 0));
    AOT_TEST_CHECK(!isRejected({ 3, 1, 2, 4, 1, 0 }, 4));

    AOT_TEST_CHECK(isRejected({ 3, 1, 2, 4 }, 4));
    AOT_TEST_CHECK(isRejected({ 3, 1, 2, 4 }, 5));
    AOT_TEST_CHECK(isRejected({ 3, 1, 2, 4 }, UINT32_MAX));
    AOT_TEST_CHECK(isRejected({ }, 0));
    AOT_TEST_CHECK(isRejected({ 3, 1, 2, 1 }, 3));

    AOT_TEST_CHECK(isRejected({ 4, 1, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ UINT32_MAX, 1, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 3, 1, 2, 4, 2, 0 }, 4));

    AOT_TEST_CHECK(isRejected({ 3, 3, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 3, UINT32_MAX, 2, 4 }, 0));
    AOT_TEST_CHECK(isRejected({ 1, 1 }, 0));

    AOT_TEST_CHECK(isRejected({ 0, 0 }, 0));
    AOT_TEST_CHECK(isRejected({ 0, 0, 0, 0 }, 0));
}

static void testSiteWord()
{
    constexpr uint32_t maxOffset = (1u << SavedFrame::liveSlotMapOffsetBits) - 1;
    static_assert(SavedFrame::liveSlotMapOffsetBits == 18 && SavedFrame::savedRegisterBits == 2);

    for (uint32_t offset : { 0u, 1u, 4097u, maxOffset }) {
        for (uint32_t promiseLocation : { 0u, 1u, 77u, 1u << 20, UINT32_MAX }) {
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

    AOT_TEST_CHECK(SavedFrame::savedRegistersOrPairsOf(SavedFrame::functionSavesPreservedRegister) == 1);
    AOT_TEST_CHECK(SavedFrame::savedRegistersOrPairsOf(SavedFrame::functionSavesR15) == 2);
}

static void testSkippedWordsInSiteWord()
{
    constexpr uint32_t maxOffset = (1u << SavedFrame::liveSlotMapOffsetBits) - 1;
    struct Case {
        uint32_t deadWordsAtBottom;
        uint32_t skippedAtBottom;
    };
    // The number of words kept stays even, and what does not fit the field is kept.
    static constexpr Case cases[] = { { 0, 0 }, { 1, 0 }, { 2, 2 }, { 5, 4 }, { 254, 254 }, { 255, 254 }, { 256, 254 }, { UINT32_MAX, 254 } };
    for (const Case& testCase : cases) {
        uint64_t word = *SavedFrame::siteWord(maxOffset, UINT32_MAX, 3, testCase.deadWordsAtBottom);
        AOT_TEST_CHECK(SavedFrame::skippedAtBottomOf(word) == testCase.skippedAtBottom);
        AOT_TEST_CHECK(SavedFrame::liveSlotMapOffsetOf(word) == maxOffset && SavedFrame::promiseLocationOf(word) == UINT32_MAX && SavedFrame::savedRegistersOrPairsOf(word) == 3);
    }

    for (uint32_t savedRegistersOrPairs : { 0u, 1u, 2u, 3u }) {
        uint32_t skippedAtTop = SavedFrame::skippedAtTopOf(*SavedFrame::siteWord(maxOffset, UINT32_MAX, savedRegistersOrPairs, 254));
#if CPU(X86_64)
        AOT_TEST_CHECK(skippedAtTop == (savedRegistersOrPairs == 3 ? 2u : 0u));
#else
        AOT_TEST_CHECK(skippedAtTop == 2 * savedRegistersOrPairs);
#endif
    }

    AOT_TEST_CHECK(SavedFrame::siteWord(0, 0, 0, 0, SavedFrame::maxNumberOfWords));
    AOT_TEST_CHECK(!SavedFrame::siteWord(0, 0, 0, 0, SavedFrame::maxNumberOfWords + 1));
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
    AOT_TEST_CHECK(words[0] == words.size() - 1);
    std::optional<SavedFrameMap> map = SavedFrameMap::tryCreate(words, 0);
    AOT_TEST_CHECK(map && narrowReferencesOf(*map).isEmpty());
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
        bool isDead = !vm.heap.isMarked(std::bit_cast<JSCell*>(frame.wordAt(location)));
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
    uint32_t numberOfValueWords = words[1];
    AOT_TEST_CHECK(numberOfValueWords && locations.last() < 32 * numberOfValueWords);
    for (uint32_t index = 0; index < numberOfValueWords; ++index) {
        uint32_t bits = 0;
        for (uint32_t location : locations) {
            if (location / 32 == index)
                bits |= 1u << location % 32;
        }
        overwriteWordInImage(words[2 + index], bits);
    }
    AOT_TEST_CHECK(valuesOf(*SavedFrameMap::tryCreate(words, 0)) == locations);
}

static void checkSkippedWords(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t function = findFunctionWithMap(*Image::current()).function;
    for (uint32_t savedRegistersOrPairs : { 0u, 1u, 3u }) {
        for (uint32_t skippedAtBottom : { 0u, 4u }) {
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
            numberOfDeadCells += !vm.heap.isMarked(std::bit_cast<JSCell*>(frame.wordAt(location)));
    }
    AOT_TEST_CHECK(numberOfDeadCells);
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

std::span<const NativeTest> savedFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "saved-frame/map-round-trip"_s, testMapRoundTrip },
        { "saved-frame/map-with-one-plane"_s, testMapWithOnePlane },
        { "saved-frame/empty-map"_s, testEmptyMap },
        { "saved-frame/walk-stops-when-the-functor-does"_s, testWalkStopsWhenTheFunctorDoes },
        { "saved-frame/map-rejects-bad-offsets"_s, testMapRejectsBadOffsets },
        { "saved-frame/site-word"_s, testSiteWord },
        { "saved-frame/skipped-words-in-site-word"_s, testSkippedWordsInSiteWord },
        { "saved-frame/narrow-locations"_s, testNarrowLocations },
        { "saved-frame/resume-modes"_s, testResumeModes },
        { "saved-frame/root-limit"_s, testRootLimit },
        { "saved-frame/callable-for-the-engine-only"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkSavedFrameIsCallableForTheEngineOnly> },
        { "saved-frame/unlisted-dead-word-is-poisoned"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkUnlistedDeadWordIsPoisoned> },
        { "saved-frame/frame-on-the-stack-is-left-alone"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkFrameOnTheStackIsLeftAlone> },
        { "saved-frame/skipped-words"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkSkippedWords> },
        { "saved-frame/listed-words-survive-a-collection"_s, testListedWordsSurviveACollection },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
