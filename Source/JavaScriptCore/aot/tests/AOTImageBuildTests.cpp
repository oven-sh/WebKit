/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompile.h"
#include "AOTDerivedFunctions.h"
#include "AOTHeat.h"
#include "AOTIRParser.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageBuildSteps.h"
#include "AOTImageTestBodies.h"
#include "AOTImageTestSupport.h"
#include "AOTSectionWriter.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "FFIStubs.h"
#include "RegisterAtOffsetList.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

using Typed::CreatorRecord;
using Typed::FunctionRole;
using TrustedImm32 = CCallHelpers::TrustedImm32;

static constexpr size_t largeBodySize = 200 * KB;
static_assert(largeBodySize > loweredReach);

static Vector<CompiledCode> plainBodies(std::span<const uint32_t> functions)
{
    Vector<CompiledCode> result;
    for (uint32_t function : functions)
        result.append(plainBody(function));
    return result;
}

static Vector<uint32_t> idsOf(const LinkedFunctions& functions)
{
    Vector<uint32_t> ids;
    for (const CompiledCode& code : functions)
        ids.append(code.function.index());
    return ids;
}

static void shuffle(Vector<uint32_t>& values, WeakRandom& random)
{
    for (size_t count = values.size(); count > 1; --count)
        std::swap(values[count - 1], values[random.getUint32(count)]);
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
        sketch.pinned(0x1000 + function, wordsInPinnedUnit);
    return sketch;
}

static CompiledCode largeBody(uint32_t function)
{
    CodeSketch sketch = sketchOfSize(function, largeBodySize);
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
        assembly.calls.callFunction(assembly.jit, FunctionID(callee), primaryEntry, { });
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
        assembly.calls.jumpToFunction(assembly.jit, FunctionID(callee), primaryEntry);
    }, [](LinkBuffer&, CompiledCode&) { });
}

static size_t codeOffsetOf(const TestImage& image, uint32_t function) { return image.address(function, 0) - image.image->code().data(); }
static int64_t run(const TestImage& image, uint32_t function) { return ExecutableCopy(image.image->code()).call<int64_t>(codeOffsetOf(image, function)); }

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

struct ProgramWithUnknownHeat {
    ProgramWithUnknownHeat(const String& text, Typed::IRForm);

    BuildContext context { wireOptions() };
    Typed::PipelineOutput pipeline;
};

struct AppendedStreams {
    Vector<uint32_t> offsets;
    Vector<uint8_t> bytes;
};

} // anonymous namespace

static constexpr ASCIILiteral roleProgramText =
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"a\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f2 name=\"c\" results=[{type=anything}]\n"
    "function f3 role=GenericEntrypoint captures=[{type=function}] results=[{type=anything}] derivation={source=f1} creator=Entrypoint{target=f1}\n"
    "function f4 name=\"d\" results=[{type=anything}]\n"
    "function f5 name=\"b\" results=[{type=anything}]\n"_s;

// f1 is the entrance of "a": f5 is the typed body behind its tests, f6 the generic version. f3 is entered by value and leads to the entrance. f7 is an entrance with fewer tests.
static constexpr ASCIILiteral entranceProgramText =
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"a\" role=EntryChecks parameters=[{type=number flags=[IsExpected]}] results=[{type=anything}] creator=EntryChecks{body=f5 fallback=f6}\n"
    "function f2 name=\"b\" results=[{type=anything}]\n"
    "function f3 role=GenericEntrypoint captures=[{type=function}] results=[{type=anything}] derivation={source=f1} creator=Entrypoint{target=f1}\n"
    "function f4 name=\"c\" results=[{type=anything}]\n"
    "function f5 name=\"a\" parameters=[{type=number}] results=[{type=anything}] derivation={source=f1}\n"
    "function f6 name=\"a\" role=GenericVersion parameters=[{type=number flags=[IsExpected]}] results=[{type=anything}] derivation={source=f1} creator=Version{provenVersion=f5}\n"
    "function f7 name=\"a\" role=CallSiteVersion parameters=[{type=number}] results=[{type=anything}] derivation={source=f1} creator=Version{provenVersion=f1}\n"
    "function f8 name=\"d\" results=[{type=anything}]\n"_s;

ProgramWithUnknownHeat::ProgramWithUnknownHeat(const String& text, Typed::IRForm form)
{
    pipeline.program = Typed::parseProgram(text, form, context);
    AOT_TEST_CHECK(pipeline.program);
    pipeline.heat = makeUnique<Typed::Heat>();
}

static Vector<uint32_t> orderedIDs(Vector<CompiledCode>&& code, const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    LinkedFunctions functions = createLinkedFunctions(WTF::move(code), context);
    orderFunctions(functions, pipeline, context);
    return idsOf(functions);
}

static void testOrderRoles()
{
    ProgramWithUnknownHeat roles(roleProgramText, Typed::IRForm::Prepared);
    Vector<uint32_t> ids { 5, 4, 3, 2, 1, 0 };
    AOT_TEST_CHECK(orderedIDs(plainBodies(ids.span()), roles.pipeline, roles.context) == Vector<uint32_t>({ 3, 1, 2, 4, 5, 0 }));
}

// What leads into a body lies in front of it, the nearer the fewer steps it takes. The generic version lies with the cold code.
static void testEntranceLiesInFrontOfItsBody()
{
    ProgramWithUnknownHeat program(entranceProgramText, Typed::IRForm::Prepared);
    Vector<uint32_t> ids { 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    AOT_TEST_CHECK(orderedIDs(plainBodies(ids.span()), program.pipeline, program.context) == Vector<uint32_t>({ 2, 4, 3, 7, 1, 5, 8, 0, 6 }));
}

static void testUnknownHeatIsIDOrder()
{
    ProgramWithUnknownHeat program(programText(9), Typed::IRForm::AsRead);
    Vector<uint32_t> ids { 8, 7, 6, 5, 4, 3, 2, 1 };
    AOT_TEST_CHECK(orderedIDs(plainBodies(ids.span()), program.pipeline, program.context) == Vector<uint32_t>({ 1, 2, 3, 4, 5, 6, 7, 8 }));
}

static void testAnyInputOrder()
{
    ProgramWithUnknownHeat roles(roleProgramText, Typed::IRForm::Prepared);
    WeakRandom random(1);
    Vector<uint32_t> ids { 0, 1, 2, 3, 4, 5 };
    for (unsigned round = 0; round < 10; ++round) {
        shuffle(ids, random);
        AOT_TEST_CHECK(orderedIDs(plainBodies(ids.span()), roles.pipeline, roles.context) == Vector<uint32_t>({ 3, 1, 2, 4, 5, 0 }));
    }
}

static uint32_t bandOf(Typed::HeatClass heatClass)
{
    switch (heatClass) {
    case Typed::HeatClass::Hot:
        return 0;
    case Typed::HeatClass::Warm:
    case Typed::HeatClass::Unknown:
        return 1;
    case Typed::HeatClass::RunsOnce:
        return 2;
    case Typed::HeatClass::Cold:
        return 3;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::array<uint32_t, 4> orderKey(const Typed::Program& program, const Typed::Heat& heat, FunctionID function)
{
    uint32_t side = 1;
    FunctionID leader = function;
    if (program.at(function).role == FunctionRole::GenericEntrypoint) {
        side = 0;
        leader = program.at(function).creator.as<CreatorRecord::Entrypoint>().target;
    }
    uint32_t band = program.at(leader).isModuleBody() ? bandOf(Typed::HeatClass::RunsOnce) : bandOf(heat.of(program, leader));
    return { band, leader.index(), side, function.index() };
}

static void testFollowsKey()
{
    TestBuild build(9);
    const Typed::Program& program = build.program();
    const Typed::Heat& heat = *build.pipeline().heat;
    Vector<uint32_t> ids;
    unsigned numberOfEntrypoints = 0;
    for (uint32_t function = 0; function < 9; ++function) {
        ids.append(function);
        if (FunctionID entrypoint = build.pipeline().derivedFunctions->find(Typed::derivedFunctionKey(FunctionRole::GenericEntrypoint, FunctionID(function), 0))) {
            ids.append(entrypoint.index());
            ++numberOfEntrypoints;
        }
    }
    AOT_TEST_CHECK(numberOfEntrypoints);
    WeakRandom random(1);
    shuffle(ids, random);

    Vector<uint32_t> ordered = orderedIDs(plainBodies(ids.span()), build.pipeline(), build.context());
    AOT_TEST_CHECK(ordered.size() == ids.size());
    for (size_t index = 1; index < ordered.size(); ++index)
        AOT_TEST_CHECK(orderKey(program, heat, FunctionID(ordered[index - 1])) < orderKey(program, heat, FunctionID(ordered[index])));
}

namespace {

struct ResolvedBodies {
    ResolvedBodies(Vector<CompiledCode>&& code, std::span<const EntryAlias> aliases = { })
        : functions(createLinkedFunctions(WTF::move(code), context))
        , index(functions.span())
        , callees(resolveCallees(functions, index, aliases, context))
    {
    }

    BuildContext context { wireOptions() };
    LinkedFunctions functions;
    CodeIndex index;
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
    uint32_t offset = sketch.pinned(0x1000 + function, wordsOfSite);
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
    AOT_TEST_CHECK(!resolved.callees.at(0, 0).body);
}

static void testAliasResolves()
{
    std::array aliases { EntryAlias { .function = FunctionID(3u), .body = FunctionID(4u), .entry = { } } };
    ResolvedBodies resolved(bodies(callerBody(1, 3), plainBody(4)), aliases);
    uint32_t caller = *resolved.index.indexOf(FunctionID(1u));
    ResolvedCallee callee = resolved.callees.at(caller, 0);
    AOT_TEST_CHECK(*callee.body == *resolved.index.indexOf(FunctionID(4u)));
    AOT_TEST_CHECK(!*callee.alias);
    AOT_TEST_CHECK(resolved.functions[caller].body.callees[0] == FunctionID(3u));
    AOT_TEST_CHECK(!count(resolved.context, InternalErrorKind::CallToFunctionWithoutCode));
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

static void testFrameMapsDivide()
{
    BuildContext context(wireOptions());
    LinkedFunctions functions = createLinkedFunctions(bodies(handlerBody(1, 5, { 1, 2 }), handlerBody(2, 5, { 1, 2 })), context);
    Vector<Vector<uint8_t>> callSites { { 8 }, { 8 } };
    SideTables tables = encodeSideTables(functions, callSites.span(), context);
    AOT_TEST_CHECK(tables.offsets[0] != tables.offsets[1]);
    AOT_TEST_CHECK(tables.frameMapWords == Vector<uint32_t>({ 1, 2, 1, 2 }));
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

static std::optional<Wire::LossRecord> lossOf(const Vector<Wire::LossRecord>& losses, LimitID limit)
{
    size_t index = losses.findIf([&](const Wire::LossRecord& loss) {
        return loss.limit == limit;
    });
    return index == notFound ? std::nullopt : std::optional(losses[index]);
}

static Vector<CompiledCode> farCallBodies() { return bodies(callerBody(1, 3), largeBody(2), plainBody(3)); }

static void testLimitFunctionCallReach()
{
    TestBuild lowered(4, reachOptions(LimitID::FunctionCallReach));
    TestImage image = builtImage(lowered, farCallBodies());
    AOT_TEST_CHECK(lowered.statistic(StatisticID::Veneers) == 1);
    AOT_TEST_CHECK(image.built.layout.veneersByBody[image.indexOf(1)].size() == 1);
    AOT_TEST_CHECK(image.image->sharedCodeCopies().size() == 1);
    Vector<Wire::LossRecord> losses = lowered.context().takeLosses(&lowered.program());
    std::optional<Wire::LossRecord> loss = lossOf(losses, LimitID::FunctionCallReach);
    AOT_TEST_CHECK(loss && loss->count == 1 && loss->witness.function == 1);
    AOT_TEST_CHECK(!lossOf(losses, LimitID::DirectCallReach));
    AOT_TEST_CHECK(!lowered.count(UserDiagnosticKind::LimitExceeded, LimitID::FunctionCallReach));
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

static constexpr uint32_t numberOfBodiesPastReach = 48;
static constexpr size_t sizeOfBodyPastReach = 64 * KB;
static_assert(numberOfBodiesPastReach * sizeOfBodyPastReach >= 3 * loweredSharedCodeReach);

static void testLimitDirectCallReach()
{
    TestBuild lowered(numberOfBodiesPastReach + 1, sharedCodeReachOptions());
    TestImage image = builtImage(lowered, bodiesCallingStub(numberOfBodiesPastReach, sizeOfBodyPastReach));
    size_t numberOfCopies = image.image->sharedCodeCopies().size();
    AOT_TEST_CHECK(image.built.layout.sharedCodeSize < loweredSharedCodeReach);
    AOT_TEST_CHECK(numberOfCopies > 1 && numberOfCopies < numberOfBodiesPastReach);
    AOT_TEST_CHECK(!lowered.statistic(StatisticID::Veneers));
    Vector<Wire::LossRecord> losses = lowered.context().takeLosses(&lowered.program());
    std::optional<Wire::LossRecord> loss = lossOf(losses, LimitID::DirectCallReach);
    AOT_TEST_CHECK(loss && loss->count == numberOfCopies - 1);
    AOT_TEST_CHECK(!lossOf(losses, LimitID::FunctionCallReach));
    AOT_TEST_CHECK(!lowered.count(UserDiagnosticKind::LimitExceeded, LimitID::DirectCallReach));
}

static void checkReachNotLowered(unsigned numberOfFunctions, Vector<CompiledCode>&& code)
{
    TestBuild unchanged(numberOfFunctions, { .heat = Wire::HeatOverride::EverythingIsCold });
    TestImage image = builtImage(unchanged, WTF::move(code));
    AOT_TEST_CHECK(!unchanged.statistic(StatisticID::Veneers));
    AOT_TEST_CHECK(image.image->sharedCodeCopies().size() == 1);
    Vector<Wire::LossRecord> losses = unchanged.context().takeLosses(&unchanged.program());
    AOT_TEST_CHECK(!lossOf(losses, LimitID::FunctionCallReach) && !lossOf(losses, LimitID::DirectCallReach));
}

static void testReachNotLowered()
{
    checkReachNotLowered(4, farCallBodies());
    checkReachNotLowered(numberOfBodiesPastReach + 1, bodiesCallingStub(numberOfBodiesPastReach, sizeOfBodyPastReach));
}

static void testManySharedCodeCopies()
{
    constexpr size_t leastNumberOfCopies = 49;
    constexpr size_t bodySize = 256 * KB;
    constexpr uint32_t numberOfBodies = 400;
    static_assert(bodySize < loweredSharedCodeReach);
    static_assert(numberOfBodies * bodySize >= leastNumberOfCopies * 2 * loweredSharedCodeReach, "One copy serves less than the reach on either side.");

    TestBuild build(numberOfBodies + 1, sharedCodeReachOptions());
    TestImage image = builtImage(build, bodiesCallingStub(numberOfBodies, bodySize));
    size_t numberOfCopies = image.image->sharedCodeCopies().size();
    AOT_TEST_CHECK(image.built.functions.size() == numberOfBodies);
    AOT_TEST_CHECK(image.built.layout.sharedCodeSize < loweredSharedCodeReach);
    AOT_TEST_CHECK(numberOfCopies >= leastNumberOfCopies && numberOfCopies < numberOfBodies);
    AOT_TEST_CHECK(build.statistic(StatisticID::StubCopiesEmitted) == numberOfCopies);
}

static ArgumentMoves largeThunkMoves(uint32_t thunk, unsigned numberOfMoves)
{
    constexpr uint64_t constantWithoutShortForm = 0x1111222233330000;
    ArgumentMoves moves = stubArgumentMoves(writeBarrier(), constantWithoutShortForm + (static_cast<uint64_t>(thunk) << 8) + 1);
    for (unsigned move = 1; move < numberOfMoves; ++move)
        appendArgumentMove(moves, GPRInfo::argumentGPR1, ArgumentSource::Constant, constantWithoutShortForm + move);
    return moves;
}

static void testLargeSharedCode()
{
    constexpr size_t leastSharedCodeSize = 24 * MB;
    constexpr size_t leastSizeOfMove = 10;
    constexpr unsigned movesInThunk = 100;
    constexpr uint32_t thunksCalledByBody = 1000;
    constexpr uint32_t numberOfBodies = 27;
    static_assert(static_cast<size_t>(numberOfBodies) * thunksCalledByBody * movesInThunk * leastSizeOfMove > leastSharedCodeSize);

    TestBuild build(numberOfBodies + 1);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = framedSketch(function);
        for (uint32_t thunk = 0; thunk < thunksCalledByBody; ++thunk)
            sketch.callThunk(largeThunkMoves(function * thunksCalledByBody + thunk, movesInThunk), sketch.addSite());
        sketch.end();
        code.append(sketch.take());
    }
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(image.built.sharedCode.argumentMoves.size() == numberOfBodies * thunksCalledByBody);
    AOT_TEST_CHECK(image.built.layout.sharedCodeSize > leastSharedCodeSize);
    AOT_TEST_CHECK(image.image->header().code.sharedCodeSize == image.built.layout.sharedCodeSize);
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

static void testCallAndJump()
{
    TestBuild build(4);
    const StubCatalog& catalog = build.stubCatalog();
    TestImage image = builtImage(build, bodies(callsInFrame(1, catalog, 2), jumpsToFunction(2, catalog, 3), returnsConstant(3, catalog, 42)));
    AOT_TEST_CHECK(run(image, 1) == 42);
}

static void testThroughVeneer()
{
    TestBuild build(5, reachOptions(LimitID::FunctionCallReach));
    const StubCatalog& catalog = build.stubCatalog();
    TestImage image = builtImage(build, bodies(callsInFrame(1, catalog, 3), largeBody(2), jumpsToFunction(3, catalog, 4), returnsConstant(4, catalog, 42)));
    AOT_TEST_CHECK(build.statistic(StatisticID::Veneers) >= 1);
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
        code.body.callees.append(FunctionID(2u));
    });
    TestImage image = builtImage(build, bodies(WTF::move(first), returnsConstant(2, catalog, 1)));

    ExecutableCopy copy(image.image->code());
    int64_t address = copy.call<int64_t>(codeOffsetOf(image, 1));
    AOT_TEST_CHECK(address == reinterpret_cast<intptr_t>(copy.start() + codeOffsetOf(image, 2)));
}

static void testBytesOutsideColdBlocks()
{
    TestBuild build(4);
    CompiledCode withHotBlocks = plainBody(1);
    CompiledCode allCold = plainBody(2);
    uint64_t sizeOfFirst = withHotBlocks.body.bytes.size();
    uint64_t sizeOfBoth = sizeOfFirst + allCold.body.bytes.size();
    withHotBlocks.body.sizeOfHotBlocks = safeCast<uint32_t>(sizeOfFirst);
    AOT_TEST_CHECK(!allCold.body.sizeOfHotBlocks);
    builtImage(build, bodies(WTF::move(withHotBlocks), WTF::move(allCold)));
    AOT_TEST_CHECK(build.statistic(StatisticID::BytesGeneratedInHotAndWarmBands) == sizeOfBoth);
    AOT_TEST_CHECK(build.statistic(StatisticID::BytesGeneratedOutsideColdBlocksInHotAndWarmBands) == sizeOfFirst);
}

static void testAccepted()
{
    TestBuild build(4);
    TestImage image = builtImage(build, bodies(plainBody(1), plainBody(2), plainBody(3)));
    const ImageHeader& header = image.image->header();
    AOT_TEST_CHECK(header.counts.functions == 3);
    AOT_TEST_CHECK(header.identity == testIdentity);
    AOT_TEST_CHECK(image.image->text().numberOfIdentifiers() == build.context().identifierNumbering().count());
    AOT_TEST_CHECK(build.statistic(StatisticID::CodeBytesEmitted) == header.codeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::ImageFunctionsEmitted) == header.counts.functions);

    bool hasEntry = false;
    for (size_t entry = 0; entry < numberOfEntries; ++entry) {
        bool isUsed = header.entriesUsed[entry / 32] >> (entry % 32) & 1;
        AOT_TEST_CHECK(isUsed == image.built.entriesUsed.get(entry));
        hasEntry |= isUsed;
    }
    AOT_TEST_CHECK(hasEntry);
}

// One page of code, then one page of zeros.
static FFIStubs stubsWithCallbackContext(size_t contextOffset, size_t contextSize)
{
    FFIStubs stubs { Vector<uint8_t>(FillWith { }, 2 * imagePageSize, 0), imagePageSize, contextOffset, contextSize };
    std::ranges::fill(stubs.code.mutableSpan().first(imagePageSize), 0xcc);
    return stubs;
}

static void testReservedRangeOfEmptyPages()
{
    TestBuild build(2);
    TestImage image = build.build(bodies(plainBody(1)), { }, { }, stubsWithCallbackContext(imagePageSize, imagePageSize));
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(!build.count(InternalErrorKind::WritableSectionInImage));
}

static void testCallbackContextOfGeneratedStubsIsReserved()
{
    TestBuild build(2);
    FFIStubs stubs = generateFFIStubs(build.context());
    AOT_TEST_CHECK(stubs.code.isEmpty() == !FFI_STUBS_SUPPORTED);
    AOT_TEST_CHECK(stubs.callbackContextSize == (FFI_STUBS_SUPPORTED ? imagePageSize : 0));
    AOT_TEST_CHECK(stubs.callbackContextOffset + stubs.callbackContextSize == stubs.code.size());
    AOT_TEST_CHECK(build.build(bodies(plainBody(1)), { }, { }, WTF::move(stubs)).image);
    AOT_TEST_CHECK(!build.count(InternalErrorKind::WritableSectionInImage));
}

static void checkReservedRangeFailsBuild(size_t contextOffset, size_t contextSize)
{
    TestBuild build(2);
    AOT_TEST_CHECK(!build.build(bodies(plainBody(1)), { }, { }, stubsWithCallbackContext(contextOffset, contextSize)).image);
    AOT_TEST_CHECK(build.count(InternalErrorKind::WritableSectionInImage) == 1);
}

static void testReservedRangeHoldsBytes()
{
    checkReservedRangeFailsBuild(0, imagePageSize);
    checkReservedRangeFailsBuild(0, 2 * imagePageSize);
}

static void testReservedRangeSharesPage()
{
    checkReservedRangeFailsBuild(imagePageSize + 64, imagePageSize - 64);
    checkReservedRangeFailsBuild(imagePageSize, 64);
}

static void testReservedRangeEndsPastCode()
{
    checkReservedRangeFailsBuild(imagePageSize, 2 * imagePageSize);
    checkReservedRangeFailsBuild(2 * imagePageSize, imagePageSize);
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
    AOT_TEST_CHECK(output->image.isEmpty());
    AOT_TEST_CHECK(output->diagnostics.hasErrors());
    AOT_TEST_CHECK(!output->diagnostics.internalErrors().isEmpty());
    AOT_TEST_CHECK(output->diagnostics.userDiagnostics().isEmpty());
    AOT_TEST_CHECK(output->symbols.isEmpty());
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
    AOT_TEST_CHECK(output->image.isEmpty());
    AOT_TEST_CHECK(output->diagnostics.userDiagnostics().isEmpty());
    AOT_TEST_CHECK(output->diagnostics.internalErrors().size() == 1);
    AOT_TEST_CHECK(output->diagnostics.internalErrors()[0].kind == InternalErrorKind::InvalidCompileOptions);
}

static void testCompileBadOptionsSize()
{
    Wire::CompileOptions options = wireOptions();
    options.size = sizeof(Wire::CompileOptions) - 4;
    checkOptionsRejected(options);
}

static void testCompileLimitValues()
{
    std::unique_ptr<CompileOutput> output = compileProgram({ }, wireOptions({ .loweredLimit = LimitID::DirectCallReach, .loweredLimitValue = loweredReach }));
    AOT_TEST_CHECK(output);
    AOT_TEST_CHECK(output->limitValues.size() == numberOfEnumerators<LimitID>);
    AOT_TEST_CHECK(!output->limitValues[static_cast<size_t>(LimitID::None)]);
    AOT_TEST_CHECK(output->limitValues[static_cast<size_t>(LimitID::DirectCallReach)] == loweredReach);
    AOT_TEST_CHECK(output->limitValues[static_cast<size_t>(LimitID::CodeBytes)] == defaultLimitValue(LimitID::CodeBytes, hostArchitecture()));
}

static void testCompileLimitThatFailsBuild()
{
    for (LimitID limit : { LimitID::TableBytes, LimitID::VectorBytes, LimitID::CodeBytes, LimitID::ImageFunctions }) {
        AOT_TEST_CHECK(!canBeLowered(limit));
        checkOptionsRejected(wireOptions({ .loweredLimit = limit, .loweredLimitValue = 64 }));
    }
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
        { "image/order/unknown-heat-is-id-order"_s, testUnknownHeatIsIDOrder },
        { "image/order/any-input-order"_s, testAnyInputOrder },
        { "image/order/follows-key"_s, testFollowsKey },
        { "image/callees/call-without-code"_s, testCallWithoutCode },
        { "image/callees/every-relocation-needs-code"_s, testEveryRelocationNeedsCode },
        { "image/callees/inline-frame-needs-none"_s, testInlineFrameNeedsNone },
        { "image/callees/alias"_s, testAliasResolves },
        { "image/side-tables/equal-streams-share"_s, testEqualStreamsShare },
        { "image/side-tables/frame-maps-divide"_s, testFrameMapsDivide },
        { "image/byte-streams/append-aligns"_s, testAppendAligns },
        { "image/byte-streams/append-all"_s, testAppendAll },
        { "image/byte-streams/append-all-threads"_s, testAppendAllThreads },
        { "image/byte-streams/vector-overflow"_s, testVectorOverflow },
        { "image/limits/function-call-reach"_s, testLimitFunctionCallReach },
        { "image/limits/direct-call-reach"_s, testLimitDirectCallReach },
        { "image/limits/reach-not-lowered"_s, testReachNotLowered },
        { "image/limits/many-shared-code-copies"_s, testManySharedCodeCopies },
        { "image/limits/large-shared-code"_s, testLargeSharedCode },
        { "image/limits/unknown-saved-register"_s, testUnknownSavedRegister },
        { "image/limits/stub-not-generated"_s, testStubNotGenerated },
        { "image/limits/relocation-past-body"_s, testRelocationPastBody },
        { "image/writer/call-and-jump"_s, whenExecutable<testCallAndJump> },
        { "image/writer/through-veneer"_s, whenExecutable<testThroughVeneer> },
        { "image/writer/relocation"_s, whenExecutable<testRelocation> },
        { "image/writer/accepted"_s, testAccepted },
        { "image/writer/bytes-outside-cold-blocks"_s, testBytesOutsideColdBlocks },
        { "image/writer/reserved-range-of-empty-pages"_s, testReservedRangeOfEmptyPages },
        { "image/writer/callback-context-of-generated-stubs-is-reserved"_s, testCallbackContextOfGeneratedStubsIsReserved },
        { "image/writer/reserved-range-holds-bytes"_s, testReservedRangeHoldsBytes },
        { "image/writer/reserved-range-shares-page"_s, testReservedRangeSharesPage },
        { "image/writer/reserved-range-ends-past-code"_s, testReservedRangeEndsPastCode },
        { "image/compile/random-bytes"_s, testCompileRandomBytes },
        { "image/compile/bad-options-size"_s, testCompileBadOptionsSize },
        { "image/compile/limit-values"_s, testCompileLimitValues },
        { "image/compile/limit-that-fails-the-build"_s, testCompileLimitThatFailsBuild },
        { "image/compile/alignment"_s, testCompileAlignment },
        { "image/compile/two-calls-at-once"_s, testCompileTwoCallsAtOnce },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
