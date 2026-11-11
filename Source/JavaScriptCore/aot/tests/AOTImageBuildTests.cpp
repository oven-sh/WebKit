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

} // anonymous namespace

static constexpr std::array rolesInCodeOrder {
    CodeRole::GenericConstructEntry, CodeRole::AllocatingConstructEntry, CodeRole::GenericCallEntry, CodeRole::ModuleStart,
    CodeRole::ParameterGuard, CodeRole::Body, CodeRole::CoroutineBody, CodeRole::SlowBody,
};
static_assert(rolesInCodeOrder.size() == Typed::numberOfCodeRolesOfOneFunction);

static constexpr unsigned numberOfFunctionsWithEveryRole = 3;

static CodeUnit unitWithRole(uint32_t function, CodeRole role) { return { FunctionID(function), role }; }

static Typed::CallingConventions createConventionsForUnitNumbers(unsigned numberOfFunctions)
{
    Typed::CallingConventionParts parts;
    parts.conventions.fill(Typed::FunctionConvention { }, numberOfFunctions);
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
    Typed::CallingConventions conventions = createConventionsForUnitNumbers(numberOfFunctionsWithEveryRole);
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
    Vector<Vector<uint16_t>> streams { Vector<uint16_t> { 1 }, Vector<uint16_t> { 2, 3 }, Vector<uint16_t> { 1 } };
    AOT_TEST_CHECK(findFirstEqualStreams(streams.span(), context.parallelLoops()) == Vector<uint32_t>({ 0, 1, 0 }));
    AOT_TEST_CHECK(writer.takeBytes().isEmpty());
    AOT_TEST_CHECK(count(context, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().size() == 1);
    AOT_TEST_CHECK(context.diagnostics().internalErrors().isEmpty());
}

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

static CompiledCode bodyOfMegabytes(uint32_t function, size_t megabytes)
{
    CompiledCode code = plainBody(function);
    code.body.bytes.grow(megabytes * MB);
    return code;
}

static void testCodeOverTwoGigabytes()
{
    TestBuild build(4);
    AOT_TEST_CHECK(!build.build(bodies(bodyOfMegabytes(1, 700), bodyOfMegabytes(2, 700), bodyOfMegabytes(3, 700))).image);
    AOT_TEST_CHECK(build.count(UserDiagnosticKind::LimitExceeded, isX86_64() ? LimitID::CodeBytes : LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.context().diagnostics().internalErrors().isEmpty());
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

static void testCompileTestingSwitchPastLastMember()
{
    static_assert(numberOfEnumerators<Wire::TestingSwitch> > 32 && numberOfEnumerators<Wire::TestingSwitch> < 64);
    Wire::CompileOptions options = wireOptions();
    options.testing = static_cast<Wire::TestingSwitch>(1ull << numberOfEnumerators<Wire::TestingSwitch>);
    checkOptionsRejected(options);
}

std::span<const NativeTest> imageBuildTests()
{
    static constexpr NativeTest tests[] = {
        { "image/order/roles"_s, testOrderRoles },
        { "image/callees/inline-frame-needs-none"_s, testInlineFrameNeedsNone },
        { "image/callees/units-of-one-function-are-told-apart"_s, testUnitsOfOneFunctionAreToldApart },
        { "image/byte-streams/vector-overflow"_s, testVectorOverflow },
        { "image/limits/large-shared-code"_s, testLargeSharedCode },
        { "image/limits/unknown-saved-register"_s, testUnknownSavedRegister },
        { "image/limits/relocation-past-body"_s, testRelocationPastBody },
        { "image/limits/code-over-two-gigabytes"_s, testCodeOverTwoGigabytes },
        { "image/compile/random-bytes"_s, testCompileRandomBytes },
        { "image/compile/bad-options-size"_s, testCompileBadOptionsSize },
        { "image/compile/testing-switch-past-the-last-member"_s, testCompileTestingSwitchPastLastMember },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
