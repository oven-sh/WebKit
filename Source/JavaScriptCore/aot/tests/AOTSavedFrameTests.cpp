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

    SavedFrame* saved = SavedFrame::create(vm, program.instance(), CoroutineKind::AsyncFunction, 2);
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

static NEVER_INLINE HandMadeFrame fillFrameWithUnreferencedObjects(LoadedProgram& program, const Vector<uint32_t>& listedLocations)
{
    HandMadeFrame frame;
    frame.numberOfWords = listedLocations.last() + HandMadeFrame::numberOfCandidates + 3;
    frame.stack.fill(0, frame.numberOfWords + 2);

    Vector<uint32_t> unlistedLocations;
    for (uint32_t location = 1; location <= frame.numberOfWords; ++location) {
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
    SavedFrame* saved;
    {
        SetForScope validation(vm.aotState().validatesRuntime, true);
        saved = SavedFrame::create(vm, program.instance(), CoroutineKind::AsyncFunction, frame.numberOfWords);
    }

    SuspensionWords words { *SavedFrame::siteWord(0, 1, 0), HandMadeFrame::integerWord };
    SuspendedMachineFrame machineFrame {
        frame.stack.mutableSpan().data() + frame.numberOfWords,
        frame.stack.span().data(),
        const_cast<uint8_t*>(Image::current()->codeOf(function)) + 1,
        &words,
    };
    AOT_TEST_CHECK(machineFrame.numberOfWords() == frame.numberOfWords);
    saved->save(vm, machineFrame, SuspensionKind::Await);
    AOT_TEST_CHECK(!saved->isOnStack() && saved->suspensionKind() == SuspensionKind::Await && saved->resumeAddress() == machineFrame.resumeAddress);
    return saved;
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

    const uint64_t* savedWords = reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(saved) + SavedFrame::offsetOfWords());
    auto savedWordAt = [&](uint32_t location) {
        return savedWords[frame.numberOfWords - location];
    };

    for (uint32_t location : listedInFrame) {
        AOT_TEST_CHECK(savedWordAt(location) == frame.wordAt(location));
        AOT_TEST_CHECK(vm.heap.isMarked(std::bit_cast<JSCell*>(frame.wordAt(location))));
    }

    unsigned numberOfDeadCells = 0;
    for (uint32_t location : frame.candidateLocations) {
        bool isDead = !vm.heap.isMarked(std::bit_cast<JSCell*>(frame.wordAt(location)));
        numberOfDeadCells += isDead;
        AOT_TEST_CHECK(savedWordAt(location) == (isDead ? SavedFrame::poisonedWord : frame.wordAt(location)));
    }
    AOT_TEST_CHECK(numberOfDeadCells);

    AOT_TEST_CHECK(savedWordAt(frame.locationOfLiveCell) == std::bit_cast<uint64_t>(program.realm()));
    AOT_TEST_CHECK(savedWordAt(frame.locationOfInteger) == HandMadeFrame::integerWord);
    AOT_TEST_CHECK(!savedWordAt(frame.locationOfZero));
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

    const uint64_t* savedWords = reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(saved) + SavedFrame::offsetOfWords());
    unsigned numberOfDeadCells = 0;
    for (uint32_t location = 1; location <= frame.numberOfWords; ++location) {
        AOT_TEST_CHECK(savedWords[frame.numberOfWords - location] == frame.wordAt(location));
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

std::span<const NativeTest> savedFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "saved-frame/map-round-trip"_s, testMapRoundTrip },
        { "saved-frame/map-with-one-plane"_s, testMapWithOnePlane },
        { "saved-frame/empty-map"_s, testEmptyMap },
        { "saved-frame/walk-stops-when-the-functor-does"_s, testWalkStopsWhenTheFunctorDoes },
        { "saved-frame/map-rejects-bad-offsets"_s, testMapRejectsBadOffsets },
        { "saved-frame/site-word"_s, testSiteWord },
        { "saved-frame/narrow-locations"_s, testNarrowLocations },
        { "saved-frame/resume-modes"_s, testResumeModes },
        { "saved-frame/callable-for-the-engine-only"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkSavedFrameIsCallableForTheEngineOnly> },
        { "saved-frame/unlisted-dead-word-is-poisoned"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkUnlistedDeadWordIsPoisoned> },
        { "saved-frame/frame-on-the-stack-is-left-alone"_s, runNativeTestWithLoadedProgram<savedFrameTests, programText, checkFrameOnTheStackIsLeftAlone> },
        { "saved-frame/listed-words-survive-a-collection"_s, testListedWordsSurviveACollection },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
