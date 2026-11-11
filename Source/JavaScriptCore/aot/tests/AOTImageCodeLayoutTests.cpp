/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTImageBuildSteps.h"
#include "AOTImageTestBodies.h"
#include "AOTImageTestSupport.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "CPU.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT::ImageTests {

static constexpr unsigned wordsInBigBody = 1024;
static constexpr unsigned thunkCallsInBody = 100;
static constexpr size_t maxRunSize = 1 * MB;
static constexpr uint16_t secondEntry = 1;

#if CPU(ARM64)
static constexpr uint64_t reachOfNearCall = MacroAssembler::relocationReach(RelocationKind::Call);
static constexpr size_t sizeOfLongFiller = reachOfNearCall / 8 * 5;
static constexpr uint32_t numberOfFarBodies = 5;
static constexpr uint32_t firstLongFarBody = 3;
static constexpr uint32_t lastLongFarBody = numberOfFarBodies - 1;
static constexpr uint32_t numberOfLargeBodies = 4;
static_assert((lastLongFarBody - firstLongFarBody + 1) * sizeOfLongFiller > reachOfNearCall);
static_assert(numberOfLargeBodies * sizeOfLongFiller > 2 * reachOfNearCall);
#endif

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
    sketch.plain(function, wordsInBigBody);
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

static Vector<CompiledCode> framedBodies(unsigned count)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(count);
    for (uint32_t function = 1; function <= count; ++function)
        result.append(framed(function));
    return result;
}

static CompiledCode framedWithPrefix(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.plain(0x0105);
    sketch.code().body.entries[0].offset = sketch.size();
    sketch.plain(0x1000 + function);
    sketch.end();
    return sketch.take();
}

static void addSecondEntry(CodeSketch& sketch)
{
    sketch.code().body.entries.append(CodeEntry { sketch.plain(9), secondEntry });
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
        sketch.callThunk(lists[function % lists.size()]);
        for (unsigned call = 1; call < thunkCallsInBody; ++call)
            sketch.callThunkAgain(0);
        sketch.end();
        result.append(sketch.take());
    }
    return result;
}

#if CPU(ARM64)
static void appendLongFiller(CodeSketch& sketch)
{
    constexpr size_t roomForWhatFollows = 1 * KB;
    auto& bytes = sketch.code().body.bytes;
    size_t start = bytes.size();
    bytes.reserveCapacity(start + sizeOfLongFiller + roomForWhatFollows);
    bytes.grow(start + sizeOfLongFiller);
    zeroSpan(bytes.mutableSpan().subspan(start));
}

static CodeSketch farSketch(uint32_t function)
{
    CodeSketch sketch = bigSketch(function);
    if (function >= firstLongFarBody && function <= lastLongFarBody)
        appendLongFiller(sketch);
    return sketch;
}

template<typename AddCalls>
static Vector<CompiledCode> farBodies(const AddCalls& addCalls)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(numberOfFarBodies);
    for (uint32_t function = 1; function <= numberOfFarBodies; ++function) {
        CodeSketch sketch = farSketch(function);
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
        appendLongFiller(sketch);
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
#endif // CPU(ARM64)

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

#if CPU(ARM64)
static void checkStubCallsInReach(const Steps& steps)
{
    for (size_t position = 0; position < steps.functions.size(); ++position) {
        uint64_t copy = steps.layout.sharedCodeCopyByBody[position];
        AOT_TEST_CHECK(steps.layout.sharedCodeCopies.contains(copy));
        for (const Relocation& relocation : steps.functions[position].body.relocations) {
            if (relocation.space != SymbolSpace::Stub)
                continue;
            uint64_t from = static_cast<uint64_t>(steps.layout.bodyStarts[position]) + relocation.offset;
            AOT_TEST_CHECK(distanceBetween(from, copy) <= reachOfNearCall);
            AOT_TEST_CHECK(distanceBetween(from, copy + steps.layout.sharedCodeSize) <= reachOfNearCall);
        }
    }
}

static void checkFunctionCallsInReach(const Steps& steps)
{
    const CodeLayout& layout = steps.layout;
    for (uint32_t position = 0; position < steps.functions.size(); ++position) {
        const CodeBody& body = steps.functions[position].body;
        for (const Relocation& relocation : body.relocations) {
            if (relocation.space != SymbolSpace::Function)
                continue;
            uint32_t callee = *steps.callees.at(position, relocation.symbol);
            size_t veneer = layout.veneersByBody[position].find(VeneerTarget { SymbolSpace::Function, relocation.entry, callee });
            uint64_t target = veneer == notFound
                ? static_cast<uint64_t>(layout.bodyStarts[callee]) + *steps.functions[callee].body.offsetOfEntry(relocation.entry)
                : static_cast<uint64_t>(layout.firstVeneerOffset(position, body.bytes.size()) + veneer * testTemplates().sizeOfVeneer());
            uint64_t from = static_cast<uint64_t>(layout.bodyStarts[position]) + relocation.offset;
            AOT_TEST_CHECK(distanceBetween(from, target) < reachOfNearCall);
        }
    }
}
#endif // CPU(ARM64)

static void checkMoveLists(const Steps& steps, std::span<const ArgumentMoves> lists)
{
    const SharedCode& sharedCode = steps.sharedCode;
    AOT_TEST_CHECK(sharedCode.argumentMoves.size() == lists.size());
    for (size_t index = 0; index < lists.size(); ++index) {
        AOT_TEST_CHECK(equalSpans(sharedCode.argumentMoves[index].span(), lists[index].span()));
        AOT_TEST_CHECK(sharedCode.thunkOffset(lists[index].span()) == sharedCode.argumentThunks.offsets[index]);
    }
    AOT_TEST_CHECK(sharedCode.blob.offsetOf(writeBarrier()));
}

static void checkJumpRetained(Steps& steps)
{
    Vector<uint8_t> bytes(steps.functions[0].body.bytes.span());
    size_t numberOfRelocations = steps.functions[0].body.relocations.size();
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.layout.fallsIntoNext.bitCount());
    AOT_TEST_CHECK(steps.functions[0].body.bytes == bytes);
    AOT_TEST_CHECK(steps.functions[0].body.relocations.size() == numberOfRelocations);
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
    Steps steps(framedBodies(5));
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
    AOT_TEST_CHECK(layout.executableSize == WTF::roundUpToMultipleOf<imagePageSize>(layout.functionsEnd));
    AOT_TEST_CHECK(layout.constantsOffset == layout.executableSize);
    AOT_TEST_CHECK(layout.codeSize == layout.constantsOffset);
}

#if CPU(ARM64)
static void testSeveralCopies()
{
    Steps steps(largeStubCallers());
    steps.layOut();
    AOT_TEST_CHECK(steps.layout.sharedCodeSize < reachOfNearCall / 4);
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
    }));
    steps.layOut();
    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.veneersByBody[0].size() == 1);
    AOT_TEST_CHECK(layout.veneersByBody[0][0] == (VeneerTarget { SymbolSpace::Function, primaryEntry, numberOfFarBodies - 1 }));
    AOT_TEST_CHECK(layout.veneersByBody[1].isEmpty());
    AOT_TEST_CHECK(layout.sharedCodeVeneers.isEmpty());
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::Veneers) == 1);
    size_t firstVeneer = layout.firstVeneerOffset(0, steps.functions[0].body.bytes.size());
    AOT_TEST_CHECK(firstVeneer >= layout.bodyStarts[0] + steps.functions[0].body.bytes.size());
    AOT_TEST_CHECK(layout.bodyStarts[1] >= firstVeneer + testTemplates().sizeOfVeneer());
    checkFunctionCallsInReach(steps);
    checkStubCallsInReach(steps);
    checkCodeIsDisjoint(steps);
}

static void testOrderOfVeneers()
{
    Steps steps(farBodies([](uint32_t function, CodeSketch& sketch) {
        if (function >= lastLongFarBody)
            addSecondEntry(sketch);
        if (function != 1)
            return;
        sketch.callFunction(numberOfFarBodies, { }, secondEntry);
        sketch.callFunction(numberOfFarBodies);
        sketch.callFunction(lastLongFarBody, { }, secondEntry);
    }));
    steps.layOut();
    const auto& veneers = steps.layout.veneersByBody[0];
    AOT_TEST_CHECK(veneers.size() == 3);
    AOT_TEST_CHECK(veneers[0] == (VeneerTarget { SymbolSpace::Function, secondEntry, lastLongFarBody - 1 }));
    AOT_TEST_CHECK(veneers[1] == (VeneerTarget { SymbolSpace::Function, primaryEntry, numberOfFarBodies - 1 }));
    AOT_TEST_CHECK(veneers[2] == (VeneerTarget { SymbolSpace::Function, secondEntry, numberOfFarBodies - 1 }));
    checkFunctionCallsInReach(steps);
}

static void testFarRegExpEntries()
{
    Steps steps(largeStubCallers());
    steps.regExpSection.bytes.fill(0, 64);
    steps.regExpSection.entries.append(std::array<uint32_t, 2> { 0, 16 });
    steps.select();
    Vector<Relocation>& relocations = steps.sharedCode.blob.code.relocations;
    AOT_TEST_CHECK(relocations.isEmpty());
    relocations.append(Relocation { 0, RelocationKind::Call, SymbolSpace::RegExp, 1, 0 });
    relocations.append(Relocation { static_cast<uint32_t>(sizeOfNearCall), RelocationKind::Call, SymbolSpace::RegExp, 0, 0 });
    steps.layOut();

    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(distanceBetween(layout.sharedCodeCopies[0], layout.regExpCodeOffset) > reachOfNearCall);
    for (const auto& veneers : layout.veneersByBody)
        AOT_TEST_CHECK(veneers.isEmpty());
    AOT_TEST_CHECK(layout.sharedCodeVeneers.size() == 2);
    AOT_TEST_CHECK(layout.sharedCodeVeneers[0] == (VeneerTarget { SymbolSpace::RegExp, 0, 0 }));
    AOT_TEST_CHECK(layout.sharedCodeVeneers[1] == (VeneerTarget { SymbolSpace::RegExp, 1, 0 }));
    AOT_TEST_CHECK(layout.sharedCodeSize == steps.sharedCode.veneersOffset + 2 * testTemplates().sizeOfVeneer());
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::Veneers) == 2 * layout.sharedCodeCopies.size());
    checkCodeIsDisjoint(steps);
}
#endif // CPU(ARM64)

static void testRegExpAndWebAssembly()
{
    constexpr size_t webAssemblyCodeAlignment = 4 * KB;
    Steps steps(framedBodies(5));
    steps.regExpSection.bytes.fill(0, RegExpSection::alignment + 100);
    steps.regExpSection.dataOffset = RegExpSection::alignment;
    steps.layOut(50, webAssemblyCodeAlignment);
    const CodeLayout& layout = steps.layout;
    AOT_TEST_CHECK(layout.webAssemblyCodeOffset >= layout.functionsEnd);
    AOT_TEST_CHECK(!(layout.webAssemblyCodeOffset % webAssemblyCodeAlignment));
    AOT_TEST_CHECK(layout.regExpCodeOffset >= layout.webAssemblyCodeOffset + 50);
    AOT_TEST_CHECK(!(layout.regExpCodeOffset % RegExpSection::alignment));
    AOT_TEST_CHECK(layout.executableSize == layout.regExpCodeOffset + RegExpSection::alignment);
    AOT_TEST_CHECK(!(layout.executableSize % imagePageSize) && layout.executableSize - layout.webAssemblyCodeOffset - 50 - RegExpSection::alignment < imagePageSize);
    AOT_TEST_CHECK(layout.constantsOffset == WTF::roundUpToMultipleOf<imageConstantsAlignment>(layout.executableSize + 100));
    AOT_TEST_CHECK(layout.codeSize == layout.constantsOffset);
}

static void testConstantsInLastExecutablePage()
{
    for (bool constantsFit : { true, false }) {
        Steps steps(framedBodies(5));
        steps.findJumps();
        steps.constants.bytes.fill(1, constantsFit ? 24 : 2 * imagePageSize);
        steps.layOut();
        const CodeLayout& layout = steps.layout;
        size_t sizeOfConstants = WTF::roundUpToMultipleOf<imageConstantsAlignment>(steps.constants.bytes.size());
        AOT_TEST_CHECK(constantsFit == (sizeOfConstants <= layout.executableSize - layout.functionsEnd));
        AOT_TEST_CHECK(layout.constantsOffset == (constantsFit ? layout.executableSize - sizeOfConstants : layout.executableSize));
        AOT_TEST_CHECK(layout.codeSize == (constantsFit ? layout.executableSize : layout.executableSize + steps.constants.bytes.size()));
    }
}

static void testNeitherRegExpNorWebAssembly()
{
    Steps steps(framedBodies(5));
    steps.layOut();
    AOT_TEST_CHECK(steps.layout.webAssemblyCodeOffset == steps.layout.functionsEnd);
    AOT_TEST_CHECK(steps.layout.ffiCodeOffset == steps.layout.functionsEnd);
    AOT_TEST_CHECK(steps.layout.regExpCodeOffset == steps.layout.executableSize);
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
    checkFallsThrough(steps, 0);
}

#if CPU(X86_64)
static void testOddOffset()
{
    CodeSketch first(1);
    first.plain(1);
    uint32_t oddByte = first.noOperationBytes(1);
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
        first.noOperationBytes(oddBytes);
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

#if CPU(ARM64)
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
    for (uint32_t function = 2; function <= numberOfFarBodies; ++function) {
        CodeSketch sketch = farSketch(function);
        sketch.end();
        code.append(sketch.take());
    }
    Steps steps(WTF::move(code));
    checkJumpRetained(steps);
    AOT_TEST_CHECK(steps.finalJumps[0].jumpsToNextBody);
    AOT_TEST_CHECK(!steps.layout.veneersByBody[0].isEmpty());
}
#endif

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
        first.noOperationBytes(oddBytes);
    uint32_t access = first.noOperationBytes(1);
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
        sketch.plain(function, wordsInBigBody);
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
        { "image/stub-selection/equal-move-lists"_s, [] { checkSelectedMoveLists(1); } },
        { "image/stub-selection/different-move-lists"_s, [] { checkSelectedMoveLists(3); } },
        { "image/stub-selection/uncalled-key"_s, testUncalledKey },
        { "image/stub-selection/missing-stub"_s, [] { checkMissingStub(stubCaller(1, writeBarrier().withCount(1))); } },
        { "image/stub-selection/missing-stub-of-thunk"_s, [] { checkMissingStub(thunkCaller(1, stubArgumentMoves(writeBarrier().withCount(1), 1))); } },
        { "image/stub-selection/threads"_s, testSelectionThreads },
        { "image/layout/one-copy"_s, testOneCopy },
#if CPU(ARM64)
        { "image/layout/several-copies"_s, testSeveralCopies },
        { "image/layout/far-calls-get-veneers"_s, testFarCallsGetVeneers },
        { "image/layout/order-of-veneers"_s, testOrderOfVeneers },
        { "image/layout/far-regexp-entries"_s, testFarRegExpEntries },
#endif
        { "image/layout/regexp-and-wasm"_s, testRegExpAndWebAssembly },
        { "image/layout/constants-in-last-executable-page"_s, testConstantsInLastExecutablePage },
        { "image/layout/neither"_s, testNeitherRegExpNorWebAssembly },
        { "image/layout/no-body"_s, testNoBody },
        { "image/fall-through/jump-to-next"_s, testJumpToNext },
        { "image/fall-through/returning-call-before-jump"_s, testReturningCallBeforeJump },
        { "image/fall-through/run-of-three"_s, testRunOfThree },
        { "image/fall-through/other-target"_s, [] { checkJumpRetained(bodies(tail(1, 3), framed(2), framed(3))); } },
        { "image/fall-through/next-has-prefix"_s, [] { checkJumpRetained(bodies(tail(1, 2), framedWithPrefix(2))); } },
#if CPU(ARM64)
        { "image/fall-through/veneer"_s, testVeneer },
#endif
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
