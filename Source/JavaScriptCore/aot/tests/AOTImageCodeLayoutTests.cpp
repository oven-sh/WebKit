/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageBuildSteps.h"
#include "AOTImageTestBodies.h"
#include "AOTImageTestSupport.h"
#include "AOTRegExpSection.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTWasmSection.h"
#include "CPU.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT::ImageTests {

static constexpr uint32_t numberOfFarBodies = 75;
static constexpr unsigned pinnedUnitsInLargeBody = 16;
static constexpr uint32_t numberOfLargeBodies = 48;
static_assert(numberOfLargeBodies * pinnedUnitsInLargeBody * wordsInPinnedUnit * sizeof(uint32_t) >= 3 * loweredSharedCodeReach);
static constexpr uint32_t lastBodyAtEdgeOfReach = 24;
static constexpr unsigned thunkCallsInBody = 100;
static constexpr size_t maxRunSize = 1 * MB;
static constexpr uint16_t secondEntry = 1;

namespace {

struct SelectedBytes {
    Vector<uint8_t> blob;
    Vector<uint8_t> thunks;
    Vector<uint32_t> thunkOffsets;
};

} // anonymous namespace

static CodeSketch bigSketch(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(function, wordsInPinnedUnit);
    return sketch;
}

static CompiledCode big(uint32_t function)
{
    CodeSketch sketch = bigSketch(function);
    sketch.end();
    return sketch.take();
}

static CompiledCode tail(uint32_t function, uint32_t target)
{
    CodeSketch sketch(function);
    sketch.plain(function);
    sketch.jumpToFunction(target);
    return sketch.take();
}

static CompiledCode framed(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.end();
    return sketch.take();
}

static CompiledCode framedWithPrefix(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(0x0105);
    sketch.code().body.entries[0].offset = sketch.size();
    sketch.pinned(0x1000 + function);
    sketch.end();
    return sketch.take();
}

static void addSecondEntry(CodeSketch& sketch)
{
    sketch.code().body.entries.append(CodeEntry { sketch.pinned(9), secondEntry });
}

static CompiledCode stubCaller(uint32_t function, StubKey key)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(key);
    sketch.end();
    return sketch.take();
}

static CompiledCode thunkCaller(uint32_t function, const ArgumentMoves& moves)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callThunk(moves);
    sketch.end();
    return sketch.take();
}

static void callRegExp(CodeSketch& sketch, uint32_t regExp, uint16_t entry)
{
    sketch.callStub(writeBarrier());
    Relocation& call = sketch.lastRelocation();
    call.space = SymbolSpace::RegExp;
    call.symbol = regExp;
    call.entry = entry;
}

static void fillUpTo(CodeSketch& sketch, uint32_t size)
{
    AOT_TEST_CHECK(sketch.size() <= size);
    if (uint32_t oddBytes = (size - sketch.size()) % sizeof(uint32_t))
        sketch.pinnedBytes(oddBytes);
    while (sketch.size() < size)
        sketch.pinned(7, std::min<uint32_t>((size - sketch.size()) / sizeof(uint32_t), wordsInPinnedUnit));
}

static CompiledCode sizedCaller(uint32_t function, uint32_t callee, uint32_t size)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    AOT_TEST_CHECK(!sketch.callFunction(callee));
    fillUpTo(sketch, size - sizeof(uint32_t));
    sketch.end();
    AOT_TEST_CHECK(sketch.size() == size);
    return sketch.take();
}

static Vector<CompiledCode> bodiesAtEdgeOfReach(uint32_t calleeOfSecond)
{
    constexpr uint32_t sizeOfFirst = loweredReach / 2;
    constexpr uint32_t sizeOfSecond = loweredReach - sizeOfFirst - sizeof(uint32_t);
    Vector<CompiledCode> result = bodies(sizedCaller(1, 3, sizeOfFirst), sizedCaller(2, calleeOfSecond, sizeOfSecond));
    for (uint32_t function = 3; function <= lastBodyAtEdgeOfReach; ++function)
        result.append(big(function));
    return result;
}

static Vector<ArgumentMoves> moveLists(unsigned count)
{
    Vector<ArgumentMoves> lists;
    for (uint64_t constant = 1; constant <= count; ++constant)
        lists.append(stubArgumentMoves(writeBarrier(), constant));
    return lists;
}

static Vector<CompiledCode> bodiesWithMoves(unsigned count, std::span<const ArgumentMoves> lists)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(count);
    for (uint32_t function = 1; function <= count; ++function) {
        CodeSketch sketch = framedSketch(function);
        for (unsigned call = 0; call < thunkCallsInBody; ++call) {
            sketch.callThunk(lists[function % lists.size()]);
            sketch.lastRelocation().symbol = 0;
            sketch.code().body.argumentMoves.shrink(1);
        }
        sketch.end();
        result.append(sketch.take());
    }
    return result;
}

template<typename AddCalls>
static Vector<CompiledCode> farBodies(const AddCalls& addCalls)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(numberOfFarBodies);
    for (uint32_t function = 1; function <= numberOfFarBodies; ++function) {
        CodeSketch sketch = bigSketch(function);
        addCalls(function, sketch);
        sketch.callStub(writeBarrier());
        sketch.end();
        result.append(sketch.take());
    }
    return result;
}

static Vector<CompiledCode> largeStubCallers()
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(numberOfLargeBodies);
    for (uint32_t function = 1; function <= numberOfLargeBodies; ++function) {
        CodeSketch sketch = bigSketch(function);
        for (unsigned unit = 1; unit < pinnedUnitsInLargeBody; ++unit)
            sketch.pinned(function, wordsInPinnedUnit);
        sketch.callStub(writeBarrier());
        sketch.end();
        result.append(sketch.take());
    }
    return result;
}

static uint64_t distanceBetween(uint64_t first, uint64_t second)
{
    return std::max(first, second) - std::min(first, second);
}

static void checkCodeIsDisjoint(const Steps& steps)
{
    Vector<std::pair<uint64_t, uint64_t>> ranges;
    for (size_t position = 0; position < steps.functions.size(); ++position) {
        uint64_t start = steps.layout.bodyStarts[position];
        ranges.append({ start, start + steps.functions[position].body.bytes.size() });
    }
    for (uint64_t copy : steps.layout.sharedCodeCopies)
        ranges.append({ copy, copy + steps.layout.sharedCodeSize });
    std::ranges::sort(ranges);
    for (size_t index = 1; index < ranges.size(); ++index)
        AOT_TEST_CHECK(ranges[index - 1].second <= ranges[index].first);
}

static void checkStubCallsInReach(const Steps& steps)
{
    for (size_t position = 0; position < steps.functions.size(); ++position) {
        uint64_t copy = steps.layout.sharedCodeCopyByBody[position];
        AOT_TEST_CHECK(steps.layout.sharedCodeCopies.contains(copy));
        for (const Relocation& relocation : steps.functions[position].body.relocations) {
            if (relocation.space != SymbolSpace::Stub)
                continue;
            uint64_t from = static_cast<uint64_t>(steps.layout.bodyStarts[position]) + relocation.offset;
            AOT_TEST_CHECK(distanceBetween(from, copy) <= loweredSharedCodeReach);
            AOT_TEST_CHECK(distanceBetween(from, copy + steps.layout.sharedCodeSize) <= loweredSharedCodeReach);
        }
    }
}

static void checkMoveLists(const Steps& steps, std::span<const ArgumentMoves> lists)
{
    const SharedCode& sharedCode = steps.sharedCode;
    AOT_TEST_CHECK(sharedCode.argumentMoves.size() == lists.size());
    for (size_t index = 0; index < lists.size(); ++index) {
        AOT_TEST_CHECK(*sharedCode.argumentMoves[index] == lists[index]);
        AOT_TEST_CHECK(sharedCode.thunkOffset(lists[index]) == sharedCode.argumentThunks.offsets[index]);
    }
    AOT_TEST_CHECK(sharedCode.blob.offsetOf(writeBarrier()));
}

static void checkJumpRetained(Steps& steps)
{
    steps.rewrite();
    Vector<uint8_t> bytes = steps.functions[0].body.bytes;
    size_t numberOfRelocations = steps.functions[0].body.relocations.size();
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.layout.fallsIntoNext.bitCount());
    AOT_TEST_CHECK(steps.functions[0].body.bytes == bytes);
    AOT_TEST_CHECK(steps.functions[0].body.relocations.size() == numberOfRelocations);
    AOT_TEST_CHECK(!steps.build.statistic(StatisticID::JumpsToNextFunctionRemoved));
}

static void checkJumpRetained(Vector<CompiledCode>&& code)
{
    Steps steps(WTF::move(code));
    checkJumpRetained(steps);
}

static void checkFallsThrough(const Steps& steps, size_t position)
{
    AOT_TEST_CHECK(steps.layout.fallsIntoNext.get(position));
    uint64_t end = static_cast<uint64_t>(steps.layout.bodyStarts[position]) + steps.functions[position].body.bytes.size();
    AOT_TEST_CHECK(steps.layout.bodyStarts[position + 1] == end);
}

static void testCalledKeys()
{
    Steps steps(bodies(stubCaller(1, writeBarrier()), stubCaller(2, toPropertyKey()), stubCaller(3, writeBarrier())));
    steps.select();
    AOT_TEST_CHECK(steps.sharedCode.blob.offsetOf(writeBarrier()));
    AOT_TEST_CHECK(steps.sharedCode.blob.offsetOf(toPropertyKey()));
}

static void testKeyOfOutlinedJump()
{
    Steps steps(bodies(framed(1)));
    steps.rewrite();
    OutlinedBodies& sharedBodies = steps.outlinedCode.sharedBodies;
    AOT_TEST_CHECK(sharedBodies.bytes.isEmpty());
    sharedBodies.bytes = testTemplates().nearJump;
    sharedBodies.offsets.append(0);
    sharedBodies.relocations.append(Relocation { 0, RelocationKind::Jump, SymbolSpace::Stub, primaryEntry, static_cast<uint32_t>(steps.outlinedCode.stubKeys.size()) });
    steps.outlinedCode.stubKeys.append(toPropertyKey());
    steps.select();
    AOT_TEST_CHECK(steps.sharedCode.blob.offsetOf(toPropertyKey()));
    AOT_TEST_CHECK(steps.sharedCode.veneersOffset >= steps.sharedCode.sharedBodiesOffset + sizeOfJump);
}

static void checkSelectedMoveLists(unsigned numberOfLists)
{
    Vector<ArgumentMoves> lists = moveLists(numberOfLists);
    Steps steps(bodiesWithMoves(100, lists.span()));
    steps.select();
    checkMoveLists(steps, lists.span());
}

static void testUncalledKey()
{
    Steps steps(bodies(stubCaller(1, writeBarrier())));
    steps.select();
    AOT_TEST_CHECK(steps.sharedCode.blob.offsetOf(writeBarrier()));
    AOT_TEST_CHECK(!steps.sharedCode.blob.offsetOf(toPropertyKey()));
}

static void checkMissingStub(CompiledCode&& caller)
{
    FunctionID function = caller.function;
    Steps steps(bodies(WTF::move(caller)));
    steps.select();
    AOT_TEST_CHECK(steps.build.count(InternalErrorKind::CallToStubNotGenerated) == 1);
    bool namesCaller = std::ranges::any_of(steps.build.context().diagnostics().internalErrors(), [&](const InternalError& error) {
        return error.kind == InternalErrorKind::CallToStubNotGenerated && error.function == function;
    });
    AOT_TEST_CHECK(namesCaller);
    AOT_TEST_CHECK(steps.sharedCode.blob.code.bytes.isEmpty());
}

static SelectedBytes selectOnThreads(unsigned threadCount, std::span<const ArgumentMoves> lists)
{
    Steps steps(bodiesWithMoves(2000, lists), TestOptions { .threadCount = threadCount });
    steps.select();
    checkMoveLists(steps, lists);
    return { WTF::move(steps.sharedCode.blob.code.bytes), WTF::move(steps.sharedCode.argumentThunks.code.bytes), WTF::move(steps.sharedCode.argumentThunks.offsets) };
}

static void testSelectionThreads()
{
    Vector<ArgumentMoves> lists = moveLists(3);
    SelectedBytes oneThread = selectOnThreads(1, lists.span());
    SelectedBytes eightThreads = selectOnThreads(8, lists.span());
    AOT_TEST_CHECK(oneThread.blob == eightThreads.blob);
    AOT_TEST_CHECK(oneThread.thunks == eightThreads.thunks);
    AOT_TEST_CHECK(oneThread.thunkOffsets == eightThreads.thunkOffsets);
}

static void testOneCopy()
{
    Steps steps(repeatedBodies(5));
    steps.layOut();
    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.sharedCodeCopies.size() == 1);
    AOT_TEST_CHECK(!layout.sharedCodeCopies[0]);
    AOT_TEST_CHECK(layout.bodyStarts.size() == 5);
    for (size_t position = 0; position < layout.bodyStarts.size(); ++position) {
        AOT_TEST_CHECK(layout.bodyStarts[position]);
        AOT_TEST_CHECK(!(layout.bodyStarts[position] % imageFunctionAlignment));
        AOT_TEST_CHECK(!position || layout.bodyStarts[position - 1] < layout.bodyStarts[position]);
        AOT_TEST_CHECK(layout.sharedCodeCopyByBody[position] == layout.sharedCodeCopies[0]);
    }
    checkCodeIsDisjoint(steps);
    AOT_TEST_CHECK(layout.constantsOffset == WTF::roundUpToMultipleOf<imageConstantsAlignment>(layout.functionsEnd));
    AOT_TEST_CHECK(layout.codeSize == layout.constantsOffset);
}

static void testSeveralCopies()
{
    Steps steps(largeStubCallers(), sharedCodeReachOptions());
    steps.layOut();
    AOT_TEST_CHECK(steps.layout.sharedCodeSize < loweredSharedCodeReach / 2);
    AOT_TEST_CHECK(steps.layout.sharedCodeCopies.size() > 1);
    for (uint32_t copy : steps.layout.sharedCodeCopies)
        AOT_TEST_CHECK(!(copy % imageStubsAlignment));
    AOT_TEST_CHECK(steps.layout.bodyStarts[0]);
    checkStubCallsInReach(steps);
    checkCodeIsDisjoint(steps);
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::StubCopiesEmitted) == steps.layout.sharedCodeCopies.size());
}

static void testFarCallsGetVeneers()
{
    Steps steps(farBodies([](uint32_t function, CodeSketch& sketch) {
        if (function == 1) {
            sketch.callFunction(numberOfFarBodies);
            sketch.callFunction(numberOfFarBodies);
        }
        if (function == 2)
            sketch.callFunction(3);
    }), reachOptions(LimitID::FunctionCallReach));
    steps.layOut();
    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.veneersByBody[0].size() == 1);
    AOT_TEST_CHECK(layout.veneersByBody[0][0] == (VeneerTarget { SymbolSpace::Function, primaryEntry, numberOfFarBodies - 1, { } }));
    AOT_TEST_CHECK(layout.veneersByBody[1].isEmpty());
    AOT_TEST_CHECK(layout.sharedCodeVeneers.isEmpty());
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::Veneers) == 1);
    size_t firstVeneer = layout.firstVeneerOffset(0, steps.functions[0].body.bytes.size());
    AOT_TEST_CHECK(firstVeneer >= layout.bodyStarts[0] + steps.functions[0].body.bytes.size());
    AOT_TEST_CHECK(layout.bodyStarts[1] >= firstVeneer + testTemplates().sizeOfVeneer());
}

static void testTwoEntriesTwoVeneers()
{
    Steps steps(farBodies([](uint32_t function, CodeSketch& sketch) {
        if (function == numberOfFarBodies)
            addSecondEntry(sketch);
        if (function != 1)
            return;
        sketch.callFunction(numberOfFarBodies, { }, secondEntry);
        sketch.callFunction(numberOfFarBodies);
    }), reachOptions(LimitID::FunctionCallReach));
    steps.layOut();
    const auto& veneers = steps.layout.veneersByBody[0];
    AOT_TEST_CHECK(veneers.size() == 2);
    AOT_TEST_CHECK(veneers[0] == (VeneerTarget { SymbolSpace::Function, secondEntry, numberOfFarBodies - 1, { } }));
    AOT_TEST_CHECK(veneers[1] == (VeneerTarget { SymbolSpace::Function, primaryEntry, numberOfFarBodies - 1, { } }));
}

static void testVeneerPushesCallOutOfReach()
{
    Steps withoutFarCall(bodiesAtEdgeOfReach(3), reachOptions(LimitID::FunctionCallReach));
    withoutFarCall.layOut();
    const CodeLayout& layoutInReach = withoutFarCall.layout;
    AOT_TEST_CHECK(layoutInReach.bodyStarts[2] - layoutInReach.bodyStarts[0] == loweredReach - sizeof(uint32_t));
    AOT_TEST_CHECK(layoutInReach.veneersByBody[0].isEmpty());
    AOT_TEST_CHECK(layoutInReach.veneersByBody[1].isEmpty());

    Steps withFarCall(bodiesAtEdgeOfReach(lastBodyAtEdgeOfReach), reachOptions(LimitID::FunctionCallReach));
    withFarCall.layOut();
    const CodeLayout& layout = withFarCall.layout;
    AOT_TEST_CHECK(layout.veneersByBody[1].size() == 1);
    AOT_TEST_CHECK(layout.veneersByBody[1][0].index == lastBodyAtEdgeOfReach - 1);
    AOT_TEST_CHECK(layout.veneersByBody[0].size() == 1);
    AOT_TEST_CHECK(layout.veneersByBody[0][0].index == 2);
    AOT_TEST_CHECK(layout.bodyStarts[2] - layout.bodyStarts[0] >= loweredReach);
    AOT_TEST_CHECK(withFarCall.build.statistic(StatisticID::Veneers) == 2);
}

static void testFarRegExpEntries()
{
    Steps steps(farBodies([](uint32_t function, CodeSketch& sketch) {
        if (function == 1 || function == numberOfFarBodies)
            callRegExp(sketch, 0, 1);
    }), reachOptions(LimitID::FunctionCallReach));
    steps.regExpSection.bytes.fill(0, 64);
    steps.regExpSection.entries.append(std::array<uint32_t, 2> { 0, 16 });
    steps.select();
    AOT_TEST_CHECK(steps.sharedCode.blob.code.relocations.isEmpty());
    steps.sharedCode.blob.code.relocations.append(Relocation { 0, RelocationKind::Call, SymbolSpace::RegExp, 0, 0 });
    steps.layOut();

    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.veneersByBody[0].size() == 1);
    AOT_TEST_CHECK(layout.veneersByBody[0][0] == (VeneerTarget { SymbolSpace::RegExp, 1, 0, { } }));
    AOT_TEST_CHECK(layout.veneersByBody.last().isEmpty());
    AOT_TEST_CHECK(layout.sharedCodeVeneers.size() == 1);
    AOT_TEST_CHECK(layout.sharedCodeVeneers[0] == (VeneerTarget { SymbolSpace::RegExp, 0, 0, { } }));
    AOT_TEST_CHECK(layout.sharedCodeSize == steps.sharedCode.veneersOffset + testTemplates().sizeOfVeneer());
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::Veneers) == 1 + layout.sharedCodeCopies.size());
    checkCodeIsDisjoint(steps);
}

static size_t firstFunctionShift(const Steps& steps)
{
    size_t sharedCodeEnd = static_cast<size_t>(steps.layout.sharedCodeCopies[0]) + steps.layout.sharedCodeSize;
    return steps.layout.bodyStarts[0] - WTF::roundUpToMultipleOf(static_cast<size_t>(1) << steps.shift, sharedCodeEnd);
}

static void testSeed()
{
    Steps unseeded(repeatedBodies(5));
    Steps seeded(repeatedBodies(5), TestOptions { .layoutSeed = 1 });
    unseeded.layOut();
    seeded.layOut();
    AOT_TEST_CHECK(!firstFunctionShift(unseeded));
    size_t seededShift = firstFunctionShift(seeded);
    AOT_TEST_CHECK(seededShift);
    AOT_TEST_CHECK(!(seededShift % 4));
    AOT_TEST_CHECK(seededShift < 64);
    const Vector<uint32_t>& unseededStarts = unseeded.layout.bodyStarts;
    const Vector<uint32_t>& seededStarts = seeded.layout.bodyStarts;
    for (size_t position = 0; position < unseeded.functions.size(); ++position) {
        AOT_TEST_CHECK(seededStarts[position] - seededStarts[0] == unseededStarts[position] - unseededStarts[0]);
        AOT_TEST_CHECK(seeded.functions[position].function == unseeded.functions[position].function);
        AOT_TEST_CHECK(seeded.functions[position].body.bytes.size() == unseeded.functions[position].body.bytes.size());
    }
}

static void testRegExpAndWebAssembly()
{
    Steps steps(repeatedBodies(5));
    steps.regExpSection.bytes.fill(0, 100);
    steps.layOut(50, WasmSectionBuilder::codeAlignment);
    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.regExpCodeOffset >= layout.functionsEnd);
    AOT_TEST_CHECK(!(layout.regExpCodeOffset % RegExpSection::alignment));
    AOT_TEST_CHECK(layout.webAssemblyCodeOffset >= layout.regExpCodeOffset + 100);
    AOT_TEST_CHECK(!(layout.webAssemblyCodeOffset % WasmSectionBuilder::codeAlignment));
    AOT_TEST_CHECK(layout.constantsOffset == WTF::roundUpToMultipleOf<imageConstantsAlignment>(layout.webAssemblyCodeOffset + 50));
    AOT_TEST_CHECK(layout.codeSize == layout.constantsOffset);
}

static void testNeitherRegExpNorWebAssembly()
{
    Steps steps(repeatedBodies(5));
    steps.layOut();
    AOT_TEST_CHECK(steps.layout.regExpCodeOffset == steps.layout.functionsEnd);
    AOT_TEST_CHECK(steps.layout.webAssemblyCodeOffset == steps.layout.functionsEnd);
}

static void testOwnBodiesAfterCaller()
{
    CodeSketch caller = singleCallerSketch(1);
    caller.end();
    Steps steps(bodies(caller.take(), framed(2)));
    steps.layOut();
    AOT_TEST_CHECK(!steps.layout.ownBodyOffsets.isEmpty());
    uint64_t ownBody = steps.layout.ownBodyOffsets[0];
    AOT_TEST_CHECK(ownBody >= steps.layout.bodyStarts[0] + steps.functions[0].body.bytes.size());
    AOT_TEST_CHECK(ownBody < steps.layout.bodyStarts[1]);
}

static void testNoBody()
{
    Steps steps(Vector<CompiledCode> { });
    steps.layOut();
    AOT_TEST_CHECK(steps.layout.sharedCodeCopies.size() == 1);
}

static void testJumpToNext()
{
    Steps steps(bodies(tail(1, 2), framed(2)));
    steps.removeJumps();
    AOT_TEST_CHECK(steps.functions[0].body.relocations.isEmpty());
    AOT_TEST_CHECK(steps.functions[0].body.bytes.size() == 4);
    AOT_TEST_CHECK(steps.functions[0].body.paddingStart == 4);
    checkFallsThrough(steps, 0);
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::JumpsToNextFunctionRemoved) == 1);
}

#if CPU(X86_64)
static void testOddOffset()
{
    CodeSketch first(1);
    first.plain(1);
    uint32_t oddByte = first.pinnedBytes(1);
    first.jumpToFunction(2);
    Steps steps(bodies(first.take(), framed(2)));
    steps.removeJumps();
    checkFallsThrough(steps, 0);
    AOT_TEST_CHECK(steps.functions[0].body.bytes.size() == oddByte + 2);
    AOT_TEST_CHECK(!(steps.functions[0].body.bytes.size() % 2));
}
#endif

static void checkReturningCallBeforeJump(unsigned oddBytes)
{
    CodeSketch first(1);
    first.withFrame();
    first.plain(1);
    if (oddBytes)
        first.pinnedBytes(oddBytes);
    size_t returnOffset = first.callStub(writeBarrier()) + sizeOfNearCall;
    first.jumpToFunction(2);
    Steps steps(bodies(first.take(), framed(2)));
    if (!isX86_64()) {
        checkJumpRetained(steps);
        return;
    }
    steps.removeJumps();
    const CodeBody& body = steps.functions[0].body;
    checkFallsThrough(steps, 0);
    AOT_TEST_CHECK(body.relocations.size() == 1);
    AOT_TEST_CHECK(body.returnSites.size() == 1);
    AOT_TEST_CHECK(body.bytes.size() > returnOffset);
    AOT_TEST_CHECK(body.bytes.size() <= returnOffset + 2);
    AOT_TEST_CHECK(!(body.bytes.size() % 2));
}

static void testReturningCallBeforeJump()
{
    checkReturningCallBeforeJump(0);
    if (isX86_64())
        checkReturningCallBeforeJump(1);
}

static void testRunOfThree()
{
    Steps steps(bodies(tail(1, 2), tail(2, 3), framed(3)));
    steps.removeJumps();
    checkFallsThrough(steps, 0);
    checkFallsThrough(steps, 1);
}

static void testVeneer()
{
    Vector<CompiledCode> code;
    CodeSketch first(1);
    first.withFrame();
    first.plain(1);
    first.callFunction(numberOfFarBodies);
    first.plain(2);
    first.jumpToFunction(2);
    code.append(first.take());
    for (uint32_t function = 2; function <= numberOfFarBodies; ++function)
        code.append(big(function));
    Steps steps(WTF::move(code), reachOptions(LimitID::FunctionCallReach));
    checkJumpRetained(steps);
    AOT_TEST_CHECK(steps.finalJumps[0].jumpsToNextBody);
    AOT_TEST_CHECK(!steps.layout.veneersByBody[0].isEmpty());
}

static void testOwnOutlinedBody()
{
    CodeSketch caller = singleCallerSketch(1);
    caller.pinned(200);
    caller.jumpToFunction(2);
    Steps steps(bodies(caller.take(), framed(2)));
    steps.rewrite();
    AOT_TEST_CHECK(steps.outlinedCode.ownBodiesSize(0));
    checkJumpRetained(steps);
}

static void testJumpToStub()
{
    CodeSketch first(1);
    first.plain(1);
    first.jumpToStub(catchException());
    checkJumpRetained(bodies(first.take(), framed(2)));
}

static void testJumpToSecondEntry()
{
    CodeSketch first(1);
    first.plain(1);
    first.jumpToFunction(2, secondEntry);
    CodeSketch second = framedSketch(2);
    addSecondEntry(second);
    second.end();
    checkJumpRetained(bodies(first.take(), second.take()));
}

#if CPU(X86_64)
static void checkNullCheckBeforeJump(unsigned oddBytes)
{
    CodeSketch first(1);
    first.plain(1);
    if (oddBytes)
        first.pinnedBytes(oddBytes);
    uint32_t access = first.pinnedBytes(1);
    first.code().sites.faultingAccesses.append(FaultingAccess { access, { }, { } });
    first.jumpToFunction(2);
    checkJumpRetained(bodies(first.take(), framed(2)));
}
#endif

static void testLongRunIsBroken()
{
    constexpr uint32_t numberOfBodies = 300;
    Vector<CompiledCode> code;
    code.reserveInitialCapacity(numberOfBodies);
    for (uint32_t function = 1; function < numberOfBodies; ++function) {
        CodeSketch sketch(function);
        sketch.pinned(function, wordsInPinnedUnit);
        sketch.jumpToFunction(function + 1);
        code.append(sketch.take());
    }
    code.append(big(numberOfBodies));
    Steps steps(WTF::move(code));
    steps.removeJumps();
    bool hasClearBit = false;
    size_t sizeOfRun = 0;
    for (size_t position = 0; position + 1 < numberOfBodies; ++position) {
        if (!steps.layout.fallsIntoNext.get(position)) {
            hasClearBit = true;
            sizeOfRun = 0;
            continue;
        }
        sizeOfRun += steps.functions[position].body.bytes.size();
        AOT_TEST_CHECK(sizeOfRun <= maxRunSize);
    }
    AOT_TEST_CHECK(hasClearBit);
    AOT_TEST_CHECK(steps.layout.fallsIntoNext.bitCount());
}

std::span<const NativeTest> codeLayoutTests()
{
    static constexpr NativeTest tests[] = {
        { "image/stub-selection/called-keys"_s, testCalledKeys },
        { "image/stub-selection/key-of-outlined-jump"_s, testKeyOfOutlinedJump },
        { "image/stub-selection/equal-move-lists"_s, [] { checkSelectedMoveLists(1); } },
        { "image/stub-selection/different-move-lists"_s, [] { checkSelectedMoveLists(3); } },
        { "image/stub-selection/uncalled-key"_s, testUncalledKey },
        { "image/stub-selection/missing-stub"_s, [] { checkMissingStub(stubCaller(1, writeBarrier().withCount(1))); } },
        { "image/stub-selection/missing-stub-of-thunk"_s, [] { checkMissingStub(thunkCaller(1, stubArgumentMoves(writeBarrier().withCount(1), 1))); } },
        { "image/stub-selection/threads"_s, testSelectionThreads },
        { "image/layout/one-copy"_s, testOneCopy },
        { "image/layout/several-copies"_s, testSeveralCopies },
        { "image/layout/far-calls-get-veneers"_s, testFarCallsGetVeneers },
        { "image/layout/two-entries-two-veneers"_s, testTwoEntriesTwoVeneers },
        { "image/layout/veneer-pushes-call-out-of-reach"_s, testVeneerPushesCallOutOfReach },
        { "image/layout/far-regexp-entries"_s, testFarRegExpEntries },
        { "image/layout/seed"_s, testSeed },
        { "image/layout/regexp-and-wasm"_s, testRegExpAndWebAssembly },
        { "image/layout/neither"_s, testNeitherRegExpNorWebAssembly },
        { "image/layout/own-bodies-after-caller"_s, testOwnBodiesAfterCaller },
        { "image/layout/no-body"_s, testNoBody },
        { "image/fall-through/jump-to-next"_s, testJumpToNext },
        { "image/fall-through/returning-call-before-jump"_s, testReturningCallBeforeJump },
        { "image/fall-through/run-of-three"_s, testRunOfThree },
        { "image/fall-through/other-target"_s, [] { checkJumpRetained(bodies(tail(1, 3), framed(2), framed(3))); } },
        { "image/fall-through/next-has-prefix"_s, [] { checkJumpRetained(bodies(tail(1, 2), framedWithPrefix(2))); } },
        { "image/fall-through/veneer"_s, testVeneer },
        { "image/fall-through/own-outlined-body"_s, testOwnOutlinedBody },
        { "image/fall-through/jump-to-stub"_s, testJumpToStub },
        { "image/fall-through/jump-to-second-entry"_s, testJumpToSecondEntry },
        { "image/fall-through/last-body"_s, [] { checkJumpRetained(bodies(tail(1, 1))); } },
        { "image/fall-through/long-run-is-broken"_s, testLongRunIsBroken },
#if CPU(X86_64)
        { "image/fall-through/odd-offset"_s, testOddOffset },
        { "image/fall-through/null-check-before-jump"_s, [] { checkNullCheckBeforeJump(0); } },
        { "image/fall-through/null-check-at-odd-offset-before-jump"_s, [] { checkNullCheckBeforeJump(1); } },
#endif
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
