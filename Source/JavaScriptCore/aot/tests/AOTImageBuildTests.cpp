/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTCodeLayout.h"
#include "AOTCodeRole.h"
#include "AOTConvention.h"
#include "AOTDriver.h"
#include "AOTFFIStubs.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageBuildSteps.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestBodies.h"
#include "AOTImageTestSupport.h"
#include "AOTLinkedFunctions.h"
#include "AOTPlan.h"
#include "AOTSectionWriter.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "AOTVector.h"
#include "FFIStubs.h"
#include "RegisterAtOffsetList.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

using Typed::CodeRole;
using TrustedImm32 = CCallHelpers::TrustedImm32;

static constexpr size_t largeBodySize = 200 * KB;
static constexpr unsigned wordsAppendedAtOnce = 1024;
static constexpr unsigned reachOfNearCallOnARM64 = 128 * MB;

static void shuffle(Vector<CodeUnit>& units, WeakRandom& random)
{
    for (size_t count = units.size(); count > 1; --count)
        std::swap(units[count - 1], units[random.getUint32(count)]);
}

static CompiledCode callerBody(uint32_t function, uint32_t callee)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callFunction(callee, sketch.addSite());
    sketch.end();
    return sketch.take();
}

static CodeSketch sketchOfSize(uint32_t function, size_t size)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    while (sketch.size() < size)
        sketch.plain(0x1000 + function, wordsAppendedAtOnce);
    return sketch;
}

static CompiledCode largeBody(uint32_t function)
{
    CodeSketch sketch = sketchOfSize(function, largeBodySize);
    sketch.end();
    return sketch.take();
}

static CompiledCode bodyLargerThanNearCallReach(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.noOperationBytes(reachOfNearCallOnARM64);
    sketch.end();
    return sketch.take();
}

static CompiledCode handlerBody(uint32_t function, uint32_t line, std::initializer_list<uint32_t> frameMapWords = { })
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite(0, line));
    return takeWithHandler(sketch, frameMapWords);
}

static CompiledCode returnsConstant(uint32_t function, const StubCatalog& catalog, int32_t value)
{
    return assemble(function, catalog, [&](Assembly& assembly) {
        assembly.jit.move(TrustedImm32(value), GPRInfo::returnValueGPR);
        assembly.jit.ret();
    }, [](LinkBuffer&, CompiledCode&) { });
}

static CompiledCode callsInFrame(uint32_t function, const StubCatalog& catalog, uint32_t callee)
{
    CCallHelpers::Label frameBegin;
    CCallHelpers::Label frameEnd;
    return assemble(function, catalog, [&](Assembly& assembly) {
        assembly.jit.emitFunctionPrologue();
        frameBegin = assembly.jit.label();
        assembly.calls.callFunction(assembly.jit, bodyUnitOf(callee), primaryEntry, { });
        frameEnd = assembly.jit.label();
        assembly.jit.emitFunctionEpilogue();
        assembly.jit.ret();
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        code.frame.frameRanges.append(CodeRange { linkBuffer.offsetOf(frameBegin), linkBuffer.offsetOf(frameEnd) });
        code.frame.frameSizeInBytes = 0;
        code.frame.stackUsedAtCalls = 16;
    });
}

static CompiledCode jumpsToFunction(uint32_t function, const StubCatalog& catalog, uint32_t callee)
{
    return assemble(function, catalog, [&](Assembly& assembly) {
        assembly.jit.move(TrustedImm32(1), GPRInfo::regT1);
        assembly.calls.jumpToFunction(assembly.jit, bodyUnitOf(callee), primaryEntry);
    }, [](LinkBuffer&, CompiledCode&) { });
}

static size_t codeOffsetOf(const TestImage& image, uint32_t function) { return image.address(function, 0) - image.image->code().data(); }
static int64_t run(const TestImage& image, uint32_t function) { return ExecutableCopy(image.image->code()).call<int64_t>(codeOffsetOf(image, function)); }

static uint32_t positionOfBody(const TestImage& image, uint32_t function)
{
    std::optional<uint32_t> position = CodeIndex(image.built.functions).indexOf(FunctionID(function));
    AOT_TEST_CHECK(position);
    return *position;
}

static const InternalError& errorOfKind(const Diagnostics& diagnostics, InternalErrorKind kind)
{
    const Vector<InternalError>& errors = diagnostics.internalErrors();
    size_t index = errors.findIf([&](const InternalError& error) {
        return error.kind == kind;
    });
    AOT_TEST_CHECK(index != notFound);
    return errors[index];
}

namespace {

struct AppendedStreams {
    Vector<uint32_t> offsets;
    Vector<uint8_t> bytes;
};

} // anonymous namespace

static constexpr std::array rolesInCodeOrder {
    CodeRole::GenericConstructEntry, CodeRole::AllocatingConstructEntry, CodeRole::GenericCallEntry, CodeRole::ModuleStart,
    CodeRole::ParameterGuard, CodeRole::Body, CodeRole::CoroutineBody, CodeRole::SlowBody,
};
static_assert(rolesInCodeOrder.size() == Typed::numberOfCodeRolesOfOneFunction);

static constexpr unsigned numberOfFunctionsWithEveryRole = 3;
static constexpr std::array boundFunctionsInPlanOrder {
    CodeUnit { FunctionID(1u), CodeRole::BoundFunction, 1 },
    CodeUnit { FunctionID(1u), CodeRole::BoundFunction, 2 },
    CodeUnit { FunctionID(2u), CodeRole::BoundFunction, 1 },
};

static CodeUnit unitWithRole(uint32_t function, CodeRole role) { return { FunctionID(function), role }; }

static Typed::CallingConventions createConventionsForUnitNumbers(unsigned numberOfFunctions, std::span<const CodeUnit> boundFunctions = { })
{
    Typed::CallingConventionParts parts;
    parts.conventions.fill(Typed::FunctionConvention { }, numberOfFunctions);
    for (CodeUnit bound : boundFunctions) {
        Typed::BoundFunctionPlan plan;
        plan.target = bound.function;
        plan.numberOfBoundArguments = bound.numberOfBoundArguments;
        parts.boundFunctions.append(WTF::move(plan));
    }
    return Typed::CallingConventions(WTF::move(parts));
}

static Typed::PlanParts createPlanPartsForUnitNumbers(unsigned numberOfFunctions)
{
    Typed::PlanParts parts;
    parts.conventions = createConventionsForUnitNumbers(numberOfFunctions);
    return parts;
}

static Vector<CodeUnit> allUnitsInCodeOrder()
{
    Vector<CodeUnit> units;
    for (uint32_t function = 0; function < numberOfFunctionsWithEveryRole; ++function) {
        for (CodeRole role : rolesInCodeOrder)
            units.append(unitWithRole(function, role));
    }
    for (CodeUnit bound : boundFunctionsInPlanOrder) {
        units.append(CodeUnit { bound.function, CodeRole::GenericEntryOfBoundFunction, bound.numberOfBoundArguments });
        units.append(bound);
    }
    return units;
}

static LinkedFunctions linkUnitsWithoutCode(std::span<const CodeUnit> units, const Typed::CallingConventions& conventions)
{
    BuildContext context(wireOptions());
    Vector<CompiledCode> code;
    for (CodeUnit unit : units) {
        CompiledCode codeOfUnit;
        codeOfUnit.unit = unit;
        codeOfUnit.unitIndex = conventions.indexOf(unit);
        codeOfUnit.placeInCode = placeInCodeOf(unit, codeOfUnit.unitIndex);
        code.append(WTF::move(codeOfUnit));
    }
    return createLinkedFunctions(WTF::move(code), context);
}

static Vector<CodeUnit> unitsOfLinkedFunctions(const LinkedFunctions& functions)
{
    Vector<CodeUnit> units;
    for (const CompiledCode& code : functions)
        units.append(code.unit);
    return units;
}

static void testOrderRoles()
{
    Typed::CallingConventions conventions = createConventionsForUnitNumbers(numberOfFunctionsWithEveryRole, boundFunctionsInPlanOrder);
    Vector<CodeUnit> expected = allUnitsInCodeOrder();
    AOT_TEST_CHECK(expected.size() == conventions.numberOfUnits());

    Vector<CodeUnit> units = expected;
    units.reverse();
    LinkedFunctions functions = linkUnitsWithoutCode(units.span(), conventions);
    AOT_TEST_CHECK(unitsOfLinkedFunctions(functions) == expected);
    for (uint32_t position = 0; position < functions.size(); ++position) {
        AOT_TEST_CHECK(functions[position].placeInCode == position);
        AOT_TEST_CHECK(functions.positionOfUnit(expected[position], conventions.indexOf(expected[position])) == position);
    }
}

// What leads into a body lies in front of it.
static void testEntranceLiesInFrontOfItsBody()
{
    Typed::CallingConventions conventions = createConventionsForUnitNumbers(4);
    Vector<CodeUnit> units {
        bodyUnitOf(3), bodyUnitOf(2), bodyUnitOf(1), unitWithRole(2, CodeRole::GenericCallEntry),
    };
    Vector<CodeUnit> expected {
        bodyUnitOf(1), unitWithRole(2, CodeRole::GenericCallEntry), bodyUnitOf(2), bodyUnitOf(3),
    };
    LinkedFunctions functions = linkUnitsWithoutCode(units.span(), conventions);
    AOT_TEST_CHECK(unitsOfLinkedFunctions(functions) == expected);

    CodeUnit withoutCode = unitWithRole(1, CodeRole::GenericCallEntry);
    AOT_TEST_CHECK(!functions.positionOfUnit(withoutCode, conventions.indexOf(withoutCode)));
}

static void testAnyInputOrder()
{
    Typed::CallingConventions conventions = createConventionsForUnitNumbers(numberOfFunctionsWithEveryRole, boundFunctionsInPlanOrder);
    Vector<CodeUnit> expected = allUnitsInCodeOrder();
    Vector<CodeUnit> units = expected;
    WeakRandom random(1);
    for (unsigned round = 0; round < 10; ++round) {
        shuffle(units, random);
        AOT_TEST_CHECK(unitsOfLinkedFunctions(linkUnitsWithoutCode(units.span(), conventions)) == expected);
    }
}

namespace {

struct ResolvedBodies {
    static constexpr unsigned numberOfFunctionsInPlan = 10;

    explicit ResolvedBodies(Vector<CompiledCode>&& code)
        : functions(createLinkedFunctions(WTF::move(code), context))
        , callees(resolveCallees(functions, plan, context))
    {
    }

    BuildContext context { wireOptions() };
    Typed::Plan plan { createPlanPartsForUnitNumbers(numberOfFunctionsInPlan) };
    LinkedFunctions functions;
    ResolvedCallees callees;
};

} // anonymous namespace

static void testCallWithoutCode()
{
    ResolvedBodies resolved(bodies(callerBody(1, 9)));
    AOT_TEST_CHECK(count(resolved.context, InternalErrorKind::CallToFunctionWithoutCode) == 1);
    const InternalError& error = errorOfKind(resolved.context.diagnostics(), InternalErrorKind::CallToFunctionWithoutCode);
    AOT_TEST_CHECK(error.function == FunctionID(1u));
    AOT_TEST_CHECK(error.count == 9);
}

static CompiledCode bodyNaming(uint32_t function, SymbolSpace space, RelocationKind kind, uint32_t callee)
{
    constexpr unsigned wordsOfSite = 2;
    AOT_TEST_CHECK(MacroAssembler::relocationSize(kind) <= wordsOfSite * sizeof(uint32_t));
    CodeSketch sketch(function);
    sketch.withFrame();
    uint32_t offset = sketch.plain(0x1000 + function, wordsOfSite);
    sketch.code().body.relocations.append(Relocation { offset, kind, space, primaryEntry, sketch.calleeIndex(callee) });
    sketch.end();
    return sketch.take();
}

static void testEveryRelocationNeedsCode()
{
    ResolvedBodies resolved(bodies(
        bodyNaming(1, SymbolSpace::Function, RelocationKind::AddressFar, 9),
        bodyNaming(2, SymbolSpace::Function, RelocationKind::AddressNear, 9),
        bodyNaming(3, SymbolSpace::Function, RelocationKind::Distance32, 9),
        bodyNaming(4, SymbolSpace::Function, RelocationKind::Jump, 9),
        bodyNaming(5, SymbolSpace::Constant, RelocationKind::AddressFar, 9),
        bodyNaming(6, SymbolSpace::Function, RelocationKind::AddressFar, 1)));
    AOT_TEST_CHECK(count(resolved.context, InternalErrorKind::CallToFunctionWithoutCode) == 4);
    for (const InternalError& error : resolved.context.diagnostics().internalErrors()) {
        AOT_TEST_CHECK(error.function.index() >= 1 && error.function.index() <= 4);
        AOT_TEST_CHECK(error.count == 9);
    }
}

static void testInlineFrameNeedsNone()
{
    CodeSketch sketch(1);
    sketch.withFrame();
    sketch.addSite(1);
    sketch.code().sites.numberOfSitesWithoutCode = 1;
    sketch.code().sites.inlineFrames = { InlineFrame { }, InlineFrame { 0, 1, sketch.calleeIndex(9), false, false } };
    sketch.plain(0x1001);
    sketch.end();

    ResolvedBodies resolved(bodies(sketch.take()));
    AOT_TEST_CHECK(!count(resolved.context, InternalErrorKind::CallToFunctionWithoutCode));
    AOT_TEST_CHECK(!resolved.callees.at(0, 0));
}

static CodeSketch sketchCallingUnit(uint32_t function, CodeUnit callee)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callFunction(callee, sketch.addSite());
    return sketch;
}

static void testUnitsOfOneFunctionAreToldApart()
{
    CodeUnit callEntry = unitWithRole(3, CodeRole::GenericCallEntry);

    CodeSketch caller = sketchCallingUnit(1, callEntry);
    caller.callFunction(3, caller.addSite());
    caller.end();
    CodeSketch entry(callEntry);
    entry.jumpToFunction(3);
    ResolvedBodies resolved(bodies(caller.take(), plainBody(3), entry.take()));
    AOT_TEST_CHECK(!count(resolved.context, InternalErrorKind::CallToFunctionWithoutCode));
    AOT_TEST_CHECK(resolved.functions[0].unit == bodyUnitOf(1));
    AOT_TEST_CHECK(resolved.functions[*resolved.callees.at(0, 0)].unit == callEntry);
    AOT_TEST_CHECK(resolved.functions[*resolved.callees.at(0, 1)].unit == bodyUnitOf(3));

    CodeSketch callerOfMissingEntry = sketchCallingUnit(1, callEntry);
    callerOfMissingEntry.end();
    ResolvedBodies lacking(bodies(callerOfMissingEntry.take(), plainBody(3)));
    AOT_TEST_CHECK(count(lacking.context, InternalErrorKind::CallToFunctionWithoutCode) == 1);
    AOT_TEST_CHECK(!lacking.callees.at(0, 0));
}

static void testEqualStreamsShare()
{
    BuildContext context(wireOptions());
    LinkedFunctions functions = createLinkedFunctions(bodies(handlerBody(1, 5), handlerBody(2, 5), handlerBody(3, 6), handlerBody(4, 5)), context);
    Vector<Vector<uint8_t>> callSites { { 8 }, { 8 }, { 8 }, { 16 } };
    SideTables tables = encodeSideTables(functions, callSites.span(), context);
    AOT_TEST_CHECK(tables.offsets[0] == tables.offsets[1]);
    // The third differs from the first in a line alone, and the positions lie apart.
    AOT_TEST_CHECK(tables.offsets[0] == tables.offsets[2]);
    AOT_TEST_CHECK(tables.offsets[0] != tables.offsets[3]);
}

static void testEqualFrameMapsShare()
{
    BuildContext context(wireOptions());
    LinkedFunctions functions = createLinkedFunctions(bodies(handlerBody(1, 5, { 1, 2 }), handlerBody(2, 5, { 1, 2 }), handlerBody(3, 5, { 1, 3 })), context);
    Vector<Vector<uint8_t>> callSites { { 8 }, { 8 }, { 8 } };
    SideTables tables = encodeSideTables(functions, callSites.span(), context);
    AOT_TEST_CHECK(tables.offsets[0] == tables.offsets[1]);
    AOT_TEST_CHECK(tables.offsets[0] != tables.offsets[2]);
    AOT_TEST_CHECK(tables.frameMapWords == Vector<uint32_t>({ 1, 2, 1, 3 }));
}

static void testAppendAligns()
{
    BuildContext context(wireOptions());
    SectionWriter writer(context);
    std::array<uint8_t, 3> first { 1, 2, 3 };
    std::array<uint8_t, 4> second { 4, 5, 6, 7 };
    AOT_TEST_CHECK(!writer.append(std::span<const uint8_t>(first), 1));
    AOT_TEST_CHECK(writer.append(std::span<const uint8_t>(second), 8) == 8);
    Vector<uint8_t> bytes = writer.takeBytes();
    AOT_TEST_CHECK(bytes.size() == 12);
    for (size_t index = 3; index < 8; ++index)
        AOT_TEST_CHECK(!bytes[index]);
}

static Vector<Vector<uint8_t>> randomStreams()
{
    WeakRandom random(1);
    Vector<Vector<uint8_t>> streams;
    streams.reserveInitialCapacity(100000);
    for (unsigned index = 0; index < 100000; ++index) {
        Vector<uint8_t> stream;
        for (unsigned size = random.getUint32(7); size; --size)
            stream.append(random.getUint32(4));
        streams.append(WTF::move(stream));
    }
    return streams;
}

static uint64_t keyOfStream(const Vector<uint8_t>& stream)
{
    uint64_t key = 1;
    for (uint8_t byte : stream)
        key = key << 2 | byte;
    return key;
}

static AppendedStreams appendSerially(std::span<const Vector<uint8_t>> streams)
{
    AppendedStreams result;
    UncheckedKeyHashMap<uint64_t, uint32_t> offsets;
    for (const Vector<uint8_t>& stream : streams) {
        if (stream.isEmpty()) {
            result.offsets.append(0);
            continue;
        }
        auto added = offsets.add(keyOfStream(stream), result.bytes.size());
        if (added.isNewEntry)
            result.bytes.appendVector(stream);
        result.offsets.append(added.iterator->value);
    }
    return result;
}

static AppendedStreams appendAllOnThreads(std::span<const Vector<uint8_t>> streams, unsigned threadCount)
{
    BuildContext context(wireOptions({ .threadCount = threadCount }));
    AOT_TEST_CHECK(context.threadCount() == threadCount);
    SectionWriter writer(context);
    AppendedStreams result;
    result.offsets = writer.appendAll(streams);
    result.bytes = writer.takeBytes();
    return result;
}

static void testAppendAll()
{
    Vector<Vector<uint8_t>> streams = randomStreams();
    AppendedStreams expected = appendSerially(streams.span());
    AppendedStreams appended = appendAllOnThreads(streams.span(), 1);
    AOT_TEST_CHECK(appended.offsets == expected.offsets);
    AOT_TEST_CHECK(appended.bytes == expected.bytes);
}

static void testAppendAllThreads()
{
    Vector<Vector<uint8_t>> streams = randomStreams();
    AppendedStreams one = appendAllOnThreads(streams.span(), 1);
    AppendedStreams eight = appendAllOnThreads(streams.span(), 8);
    AOT_TEST_CHECK(one.offsets == eight.offsets);
    AOT_TEST_CHECK(one.bytes == eight.bytes);
}

static void testVectorOverflow()
{
    constexpr size_t vectorBytes = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());
    static_assert(vectorBytes < defaultLimitValue(LimitID::TableBytes, hostArchitecture()), "A writer tests VectorBytes first, so no writer reports TableBytes.");

    BuildContext context(wireOptions());
    SectionWriter writer(context);
    Vector<uint8_t> bytes(FillWith { }, 100, 1);
    AOT_TEST_CHECK(!writer.append(bytes.span(), 1));
    AOT_TEST_CHECK(!writer.didOverflow());

    auto neverRead = MallocSpan<uint8_t>::malloc(vectorBytes + 1 - bytes.size());
    AOT_TEST_CHECK(!writer.append(neverRead.span(), 1));
    AOT_TEST_CHECK(writer.didOverflow());
    AOT_TEST_CHECK(!writer.append(bytes.span(), 1));
    Vector<Vector<uint8_t>> streams { Vector<uint8_t> { 1 }, Vector<uint8_t> { 2, 3 } };
    AOT_TEST_CHECK(writer.appendAll(streams.span()) == Vector<uint32_t>({ 0, 0 }));
    AOT_TEST_CHECK(writer.takeBytes().isEmpty());
    AOT_TEST_CHECK(count(context, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().size() == 1);
    AOT_TEST_CHECK(context.diagnostics().internalErrors().isEmpty());
}

static Vector<CompiledCode> bodiesCallingAcross(CompiledCode&& between) { return bodies(callerBody(1, 3), WTF::move(between), plainBody(3)); }

static void testLimitFunctionCallReach()
{
    // On x86-64 a near call reaches across all the code that an image can hold.
    if (!isARM64())
        return;

    TestBuild build(4);
    TestImage image = builtImage(build, bodiesCallingAcross(bodyLargerThanNearCallReach(2)));
    AOT_TEST_CHECK(build.statistic(StatisticID::Veneers) == 1);
    AOT_TEST_CHECK(build.statistic(StatisticID::VeneerBytesEmitted) == testTemplates().sizeOfVeneer());
    const auto& veneers = image.built.layout.veneersByBody[positionOfBody(image, 1)];
    VeneerTarget callee { SymbolSpace::Function, primaryEntry, positionOfBody(image, 3) };
    AOT_TEST_CHECK(veneers.size() == 1);
    AOT_TEST_CHECK(veneers[0] == callee);
    AOT_TEST_CHECK(image.built.layout.veneersByBody[positionOfBody(image, 2)].isEmpty());
    AOT_TEST_CHECK(image.built.layout.veneersByBody[positionOfBody(image, 3)].isEmpty());
    AOT_TEST_CHECK(build.statistic(StatisticID::StubCopiesEmitted) == image.image->sharedCodeCopies().size());
}

static Vector<CompiledCode> bodiesCallingStub(uint32_t numberOfBodies, size_t bodySize)
{
    Vector<CompiledCode> code;
    code.reserveInitialCapacity(numberOfBodies);
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchOfSize(function, bodySize);
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.end();
        code.append(sketch.take());
    }
    return code;
}

static void checkOneCopyAndNoVeneer(unsigned numberOfFunctions, Vector<CompiledCode>&& code)
{
    TestBuild build(numberOfFunctions);
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(!build.statistic(StatisticID::Veneers));
    AOT_TEST_CHECK(!build.statistic(StatisticID::VeneerBytesEmitted));
    AOT_TEST_CHECK(image.image->sharedCodeCopies().size() == 1);
    AOT_TEST_CHECK(build.statistic(StatisticID::StubCopiesEmitted) == 1);
}

static void testCallsWithinReach()
{
    constexpr uint32_t numberOfBodiesCallingStub = 48;
    constexpr size_t sizeOfBodyCallingStub = 64 * KB;
    checkOneCopyAndNoVeneer(4, bodiesCallingAcross(largeBody(2)));
    checkOneCopyAndNoVeneer(numberOfBodiesCallingStub + 1, bodiesCallingStub(numberOfBodiesCallingStub, sizeOfBodyCallingStub));
}

// A thunk moves into a register at most once, so the size of the shared code comes from the number of thunks.
static constexpr unsigned numberOfMovesInLargeThunk = numberOfDirectArgumentGPRs;

static ArgumentMoves largeThunkMoves(uint32_t thunk)
{
    constexpr uint64_t constantWithoutShortForm = 0x1111222233330000;
    constexpr unsigned argumentOfFirstMove = 1;
    static_assert(directArgumentGPR(argumentOfFirstMove) == GPRInfo::argumentGPR1, "stubArgumentMoves() moves into this register.");
    ArgumentMoves moves = stubArgumentMoves(writeBarrier(), constantWithoutShortForm + (static_cast<uint64_t>(thunk) << 8) + 1);
    for (unsigned argument = 0; argument < numberOfMovesInLargeThunk; ++argument) {
        if (argument != argumentOfFirstMove)
            appendArgumentMove(moves, directArgumentGPR(argument), ArgumentSource::Constant, constantWithoutShortForm + argument + 1);
    }
    return moves;
}

static void testLargeSharedCode()
{
    constexpr size_t leastSharedCodeSize = 24 * MB;
    constexpr size_t leastSizeOfMove = 10;
    constexpr uint32_t numberOfBodies = 27;
    constexpr uint32_t thunksCalledByBody = leastSharedCodeSize / (numberOfBodies * numberOfMovesInLargeThunk * leastSizeOfMove) + 1;

    TestBuild build(numberOfBodies + 1);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = framedSketch(function);
        for (uint32_t thunk = 0; thunk < thunksCalledByBody; ++thunk)
            sketch.callThunk(largeThunkMoves(function * thunksCalledByBody + thunk), sketch.addSite());
        sketch.end();
        code.append(sketch.take());
    }
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(image.built.sharedCode.argumentMoves.size() == numberOfBodies * thunksCalledByBody);
    AOT_TEST_CHECK(image.built.layout.sharedCodeSize > leastSharedCodeSize);
    AOT_TEST_CHECK(image.image->header().code.sharedCodeSize == image.built.layout.sharedCodeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::ArgumentThunkBytes) == image.built.sharedCode.argumentThunks.code.bytes.size());
    AOT_TEST_CHECK(build.statistic(StatisticID::ArgumentThunkBytes) > leastSharedCodeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::StubBytes) == image.built.sharedCode.blob.code.bytes.size());
}

static void testUnknownSavedRegister()
{
    CompiledCode code = plainBody(1);
    code.frame.calleeSaveRegisters = RegisterAtOffsetList(RegisterSet { GPRInfo::regT1 });
    TestBuild build(2);
    AOT_TEST_CHECK(!build.build(bodies(WTF::move(code))).image);
    AOT_TEST_CHECK(build.count(InternalErrorKind::FrameSavesUnknownRegister) == 1);
    const InternalError& error = errorOfKind(build.context().diagnostics(), InternalErrorKind::FrameSavesUnknownRegister);
    AOT_TEST_CHECK(error.function == FunctionID(1u));
    static_assert(Reg(GPRInfo::regT1).index(), "0 is also the count of an error that sets none");
    AOT_TEST_CHECK(error.count == Reg(GPRInfo::regT1).index());

    CompiledCode known = plainBody(1);
    Reg first = *RegisterSet::vmCalleeSaveRegisters().begin();
    known.frame.calleeSaveRegisters = RegisterAtOffsetList(RegisterSet { first });
    TestBuild accepted(2);
    AOT_TEST_CHECK(accepted.build(bodies(WTF::move(known))).image);
}

static void testStubNotGenerated()
{
    static_assert(!exists(writeBarrier().withCount(1)));
    CodeSketch sketch = framedSketch(1);
    sketch.callStub(writeBarrier().withCount(1), sketch.addSite());
    sketch.end();
    TestBuild build(2);
    AOT_TEST_CHECK(!build.build(bodies(sketch.take())).image);
    AOT_TEST_CHECK(build.count(InternalErrorKind::CallToStubNotGenerated) >= 1);
    AOT_TEST_CHECK(errorOfKind(build.context().diagnostics(), InternalErrorKind::CallToStubNotGenerated).function == FunctionID(1u));
}

static void testRelocationPastBody()
{
    CompiledCode code = plainBody(1);
    uint32_t key = static_cast<uint32_t>(code.body.stubKeys.size());
    code.body.stubKeys.append(catchException());
    code.body.relocations.append(Relocation { static_cast<uint32_t>(code.body.bytes.size()) + 64, RelocationKind::Jump, SymbolSpace::Stub, primaryEntry, key });
    TestBuild build(2);
    AOT_TEST_CHECK(!build.build(bodies(WTF::move(code))).image);
    AOT_TEST_CHECK(build.count(InternalErrorKind::RelocationNotApplied) >= 1);
}

// The bytes behind the end of the body are never read: the build stops before it copies any code.
static CompiledCode bodyOfMegabytes(uint32_t function, size_t megabytes)
{
    CompiledCode code = plainBody(function);
    code.body.bytes.grow(megabytes * MB);
    return code;
}

// An image is one Vector, which holds less than two gigabytes, and on x86-64 a near call reaches no further. Either way the build says which limit it met.
static void testCodeOverTwoGigabytes()
{
    TestBuild build(4);
    AOT_TEST_CHECK(!build.build(bodies(bodyOfMegabytes(1, 700), bodyOfMegabytes(2, 700), bodyOfMegabytes(3, 700))).image);
    AOT_TEST_CHECK(build.count(UserDiagnosticKind::LimitExceeded, isX86_64() ? LimitID::CodeBytes : LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.context().diagnostics().internalErrors().isEmpty());
}

// The lists of keys, of places and of runs of text that the stage makes for itself are measured first: no program that a test can build has as many.
static void testListOverVectorBytes()
{
    constexpr size_t sizeOfElement = 16;
    constexpr uint64_t fewestThatDoNotFit = 1ull << 27;
    static_assert((fewestThatDoNotFit - 1) * sizeOfElement <= defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) && fewestThatDoNotFit * sizeOfElement > defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));
    BuildContext context(wireOptions());
    AOT_TEST_CHECK(checkListSize(0, sizeOfElement, context));
    AOT_TEST_CHECK(checkListSize(fewestThatDoNotFit - 1, sizeOfElement, context));
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    AOT_TEST_CHECK(!checkListSize(fewestThatDoNotFit, sizeOfElement, context));
    AOT_TEST_CHECK(count(context, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(context.diagnostics().internalErrors().isEmpty());
}

static void testCallAndJump()
{
    TestBuild build(4);
    const StubCatalog& catalog = build.stubCatalog();
    TestImage image = builtImage(build, bodies(callsInFrame(1, catalog, 2), jumpsToFunction(2, catalog, 3), returnsConstant(3, catalog, 42)));
    AOT_TEST_CHECK(run(image, 1) == 42);
}

static void testRelocation()
{
    TestBuild build(3);
    const StubCatalog& catalog = build.stubCatalog();
    CompiledCode first = assemble(1, catalog, [](Assembly& assembly) {
        assembly.relocations.moveAddress({ SymbolSpace::Function, 0 }, GPRInfo::returnValueGPR, PCRelativeReach::Far);
        assembly.jit.ret();
    }, [](LinkBuffer&, CompiledCode& code) {
        code.body.callees.append(bodyUnitOf(2));
    });
    TestImage image = builtImage(build, bodies(WTF::move(first), returnsConstant(2, catalog, 1)));

    ExecutableCopy copy(image.image->code());
    int64_t address = copy.call<int64_t>(codeOffsetOf(image, 1));
    AOT_TEST_CHECK(address == reinterpret_cast<intptr_t>(copy.start() + codeOffsetOf(image, 2)));
}

static void testAccepted()
{
    TestBuild build(4);
    TestImage image = builtImage(build, bodies(plainBody(1), plainBody(2), plainBody(3)));
    const ImageHeader& header = image.image->header();
    AOT_TEST_CHECK(header.counts.functions == 3);
    AOT_TEST_CHECK(header.identity == testIdentity);
    AOT_TEST_CHECK(image.image->text().numberOfIdentifiers() == image.built.numbers->identifiers().count());
    AOT_TEST_CHECK(build.statistic(StatisticID::CodeBytesEmitted) == header.codeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::FunctionBytesEmitted) && build.statistic(StatisticID::StubBytes));
    AOT_TEST_CHECK(build.statistic(StatisticID::FunctionBytesEmitted) + build.statistic(StatisticID::StubBytes) <= header.code.functionsEnd);
    AOT_TEST_CHECK(!build.statistic(StatisticID::ArgumentThunkBytes) && !build.statistic(StatisticID::ConstantBytesOfJumpTables));
    AOT_TEST_CHECK(build.statistic(StatisticID::ImageFunctionsEmitted) == header.counts.functions);

    bool hasEntry = false;
    for (size_t entry = 0; entry < numberOfEntries; ++entry) {
        bool isUsed = header.entriesUsed[entry / 32] >> (entry % 32) & 1;
        AOT_TEST_CHECK(isUsed == image.built.entriesUsed.get(entry));
        hasEntry |= isUsed;
    }
    AOT_TEST_CHECK(hasEntry);
}

// The run time maps memory of its own over the callback context, so the image holds whole pages of zeros there.
static void testCallbackContextOfGeneratedStubsIsReserved()
{
    BuildContext context(wireOptions());
    FFIStubs stubs = generateFFIStubs(context);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    AOT_TEST_CHECK(stubs.code.isEmpty() == !FFI_STUBS_SUPPORTED);
    AOT_TEST_CHECK(stubs.callbackContextSize == (FFI_STUBS_SUPPORTED ? imagePageSize : 0));
    AOT_TEST_CHECK(stubs.callbackContextOffset + stubs.callbackContextSize == stubs.code.size());
    AOT_TEST_CHECK(!(stubs.callbackContextOffset % imagePageSize));
    AOT_TEST_CHECK(stubs.code.isEmpty() || !(stubs.codeAlignment % imagePageSize));
    AOT_TEST_CHECK(std::ranges::all_of(stubs.code.span().subspan(stubs.callbackContextOffset), [](uint8_t byte) {
        return !byte;
    }));
}

static Vector<uint8_t> randomBytes()
{
    WeakRandom random(1);
    Vector<uint8_t> bytes;
    for (unsigned index = 0; index < 4 * KB; ++index)
        bytes.append(random.getUint32(256));
    return bytes;
}

static void checkProgramRejected(std::span<const uint8_t> program)
{
    std::unique_ptr<CompileOutput> output = compileProgram(program, wireOptions());
    AOT_TEST_CHECK(output);
    AOT_TEST_CHECK(!output->hasImage());
    AOT_TEST_CHECK(!output->section(Wire::ResultSection::InternalErrors).empty());
    AOT_TEST_CHECK(output->section(Wire::ResultSection::UserDiagnostics).empty());
    AOT_TEST_CHECK(output->section(Wire::ResultSection::Symbols).empty());
}

static void testCompileRandomBytes()
{
    Vector<uint8_t> bytes = randomBytes();
    checkProgramRejected(bytes.span());
    checkProgramRejected({ });
}

static void checkOptionsRejected(const Wire::CompileOptions& options)
{
    Vector<uint8_t> bytes = randomBytes();
    std::unique_ptr<CompileOutput> output = compileProgram(bytes.span(), options);
    AOT_TEST_CHECK(output);
    AOT_TEST_CHECK(!output->hasImage());
    AOT_TEST_CHECK(output->section(Wire::ResultSection::UserDiagnostics).empty());
    std::span<const Wire::InternalErrorRecord> errors = output->records<Wire::InternalErrorRecord>(Wire::ResultSection::InternalErrors);
    AOT_TEST_CHECK(errors.size() == 1);
    AOT_TEST_CHECK(errors[0].kind == InternalErrorKind::InvalidCompileOptions);
}

static void testCompileBadOptionsSize()
{
    Wire::CompileOptions options = wireOptions();
    options.size = sizeof(Wire::CompileOptions) - 4;
    checkOptionsRejected(options);
}

static void testCompileAlignment() { AOT_TEST_CHECK(imageAlignment() == imagePageSize); }

static void testCompileTwoCallsAtOnce()
{
    Vector<uint8_t> bytes = randomBytes();
    Ref<Thread> first = Thread::create("aot-compile-test"_s, [&] {
        checkProgramRejected(bytes.span());
    });
    Ref<Thread> second = Thread::create("aot-compile-test"_s, [&] {
        checkProgramRejected(bytes.span());
    });
    first->waitForCompletion();
    second->waitForCompletion();
}

std::span<const NativeTest> imageBuildTests()
{
    static constexpr NativeTest tests[] = {
        { "image/order/roles"_s, testOrderRoles },
        { "image/order/entrance-lies-in-front-of-its-body"_s, testEntranceLiesInFrontOfItsBody },
        { "image/order/any-input-order"_s, testAnyInputOrder },
        { "image/callees/call-without-code"_s, testCallWithoutCode },
        { "image/callees/every-relocation-needs-code"_s, testEveryRelocationNeedsCode },
        { "image/callees/inline-frame-needs-none"_s, testInlineFrameNeedsNone },
        { "image/callees/units-of-one-function-are-told-apart"_s, testUnitsOfOneFunctionAreToldApart },
        { "image/side-tables/equal-streams-share"_s, testEqualStreamsShare },
        { "image/side-tables/equal-frame-maps-share"_s, testEqualFrameMapsShare },
        { "image/byte-streams/append-aligns"_s, testAppendAligns },
        { "image/byte-streams/append-all"_s, testAppendAll },
        { "image/byte-streams/append-all-threads"_s, testAppendAllThreads },
        { "image/byte-streams/vector-overflow"_s, testVectorOverflow },
        { "image/limits/function-call-reach"_s, testLimitFunctionCallReach },
        { "image/limits/calls-within-reach"_s, testCallsWithinReach },
        { "image/limits/large-shared-code"_s, testLargeSharedCode },
        { "image/limits/unknown-saved-register"_s, testUnknownSavedRegister },
        { "image/limits/stub-not-generated"_s, testStubNotGenerated },
        { "image/limits/relocation-past-body"_s, testRelocationPastBody },
        { "image/limits/code-over-two-gigabytes"_s, testCodeOverTwoGigabytes },
        { "image/limits/list-over-vector-bytes"_s, testListOverVectorBytes },
        { "image/writer/call-and-jump"_s, whenExecutable<testCallAndJump> },
        { "image/writer/relocation"_s, whenExecutable<testRelocation> },
        { "image/writer/accepted"_s, testAccepted },
        { "image/writer/callback-context-of-generated-stubs-is-reserved"_s, testCallbackContextOfGeneratedStubsIsReserved },
        { "image/compile/random-bytes"_s, testCompileRandomBytes },
        { "image/compile/bad-options-size"_s, testCompileBadOptionsSize },
        { "image/compile/alignment"_s, testCompileAlignment },
        { "image/compile/two-calls-at-once"_s, testCompileTwoCallsAtOnce },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
