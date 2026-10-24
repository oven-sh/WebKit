/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageBuildSteps.h"
#include "AOTVector.h"

#include "AOTCodeLinker.h"
#include "AOTInstanceLayout.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsPropertyKeys.h"
#include "GPRInfo.h"
#include <span>
#include <wtf/BitSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace ImageTests {

namespace {

struct BrokenRelocation {
    SymbolSpace space;
    void (*damage)(Relocation&);
};

} // anonymous namespace

static constexpr InstanceSizes linkedInstanceSizes { 0, 0, 4 };
static constexpr uint32_t linkedSiteCache = 3;

template<typename Emit>
static uint32_t appendRelocated(CodeSketch& sketch, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, testAssemblerPolicy());
    emit(recorder);
    auto unit = recorder.finalize(LinkBuffer::Profile::FTL, nullptr);
    AOT_TEST_CHECK(unit);
    AOT_TEST_CHECK(unit->relocations.size() == 1);
    CodeBody& body = sketch.code().body;
    uint32_t offset = sketch.pinnedBytes(static_cast<unsigned>(unit->bytes.size()));
    memcpySpan(body.bytes.mutableSpan().subspan(offset), unit->bytes.span());
    Relocation relocation = unit->relocations[0];
    relocation.offset += offset;
    body.relocations.append(relocation);
    return offset;
}

static ConstantData constantOf(uint64_t word)
{
    return { Vector<uint8_t>(asByteSpan(word)), sizeof(uint64_t) };
}

static CompiledCode bodyWithManySpaces(uint32_t function, uint32_t callee)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(0x2000 + function);
    appendSequence(sketch);
    uint32_t afterSequence = sketch.callFunction(callee, sketch.addSite());
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.callThunk(stubArgumentMoves(writeBarrier(), 1), sketch.addSite());
    uint32_t regExpCall = appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.call({ SymbolSpace::RegExp, 0, 1 });
    });
    sketch.code().body.returnSites.append(ReturnSite { regExpCall + static_cast<uint32_t>(sizeOfNearCall), sketch.addSite() });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveAddress({ SymbolSpace::Constant, 0 }, GPRInfo::regT0, PCRelativeReach::Far);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.load64({ SymbolSpace::Constant, 1 }, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveAddress({ SymbolSpace::JumpTable, 0 }, GPRInfo::regT0, PCRelativeReach::Far);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveInstanceAddress({ SymbolSpace::SiteCache, linkedSiteCache }, instanceGPR, GPRInfo::regT0);
    });
    uint32_t end = sketch.end();
    sketch.code().body.constants = { constantOf(0x1122334455667788), constantOf(0x99aabbccddeeff00) };
    sketch.code().body.jumpTables = { JumpTable { { afterSequence, end } } };
    return sketch.take();
}

static Vector<CompiledCode> bodiesWithEverySpace(uint32_t& function)
{
    Vector<CompiledCode> code = repeatedBodies();
    function = static_cast<uint32_t>(code.size()) + 1;
    code.append(bodyWithManySpaces(function, 1));
    return code;
}

static RegExpSection createRegExpSection()
{
    RegExpSection section;
    while (section.bytes.size() < 64)
        section.bytes.appendVector(testTemplates().breakpoint);
    section.entries.append({ 0, 32 });
    return section;
}

static void prepareToLink(Steps& steps)
{
    steps.regExpSection = createRegExpSection();
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
}

static Vector<uint8_t> link(Steps& steps)
{
    Vector<uint8_t> code(FillWith { }, static_cast<size_t>(steps.layout.codeSize), static_cast<uint8_t>(0));
    CodeLayoutInputs layoutInputs = steps.layoutInputs();
    linkCode({ layoutInputs, steps.layout, steps.outlining, steps.constants, linkedInstanceSizes }, code.mutableSpan(), steps.build.context());
    return code;
}

static size_t expectedTarget(const Steps& steps, uint32_t position, const Relocation& relocation)
{
    const CodeBody& body = steps.functions[position].body;
    const CodeLayout& layout = steps.layout;
    const SharedCode& sharedCode = steps.sharedCode;
    size_t copy = layout.sharedCodeCopyByBody[position];
    uint32_t symbol = relocation.symbol;
    switch (relocation.space) {
    case SymbolSpace::Function: {
        uint32_t callee = *steps.callees.at(position, symbol).body;
        return layout.bodyStarts[callee] + *steps.functions[callee].body.offsetOfEntry(relocation.entry);
    }
    case SymbolSpace::Stub:
        return copy + *sharedCode.blob.offsetOf(body.stubKeys[symbol]);
    case SymbolSpace::ArgumentThunk:
        return copy + sharedCode.argumentThunksOffset + sharedCode.thunkOffset(body.argumentMoves[symbol]);
    case SymbolSpace::OutlinedBody: {
        uint32_t number = steps.outlinedCode.indexWithinKind[symbol];
        if (steps.outlining.bodies[symbol].singleCaller)
            return layout.ownBodyOffsets[number];
        return copy + sharedCode.sharedBodiesOffset + steps.outlinedCode.sharedBodies.offsets[number];
    }
    case SymbolSpace::Constant:
        return layout.constantsOffset + steps.constants.constantOffsetsByBody[position][symbol];
    case SymbolSpace::JumpTable:
        return layout.constantsOffset + steps.constants.jumpTableOffsetsByBody[position][symbol];
    case SymbolSpace::SiteCache:
        return InstanceLayout::offsetOfSiteCache(linkedInstanceSizes, symbol);
    case SymbolSpace::RegExp:
        return layout.regExpCodeOffset + steps.regExpSection.entries[symbol][relocation.entry];
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        break;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static bool liesInside(const CodeBody& body, const Relocation& relocation)
{
    return relocation.offset + MacroAssembler::relocationSize(relocation.kind) <= body.bytes.size();
}

static void checkLinkedBody(const Steps& steps, uint32_t position, std::span<const uint8_t> code)
{
    const CodeBody& body = steps.functions[position].body;
    size_t start = steps.layout.bodyStarts[position];
    AOT_TEST_CHECK(steps.layout.veneersByBody[position].isEmpty());
    Vector<uint8_t> expected(FillWith { }, code.size(), static_cast<uint8_t>(0));
    memcpySpan(expected.mutableSpan().subspan(start), body.bytes.span());
    for (const Relocation& relocation : body.relocations) {
        if (liesInside(body, relocation))
            AOT_TEST_CHECK(applyRelocation(expected.mutableSpan(), start, relocation, expectedTarget(steps, position, relocation)));
    }
    AOT_TEST_CHECK(equalSpans(code.subspan(start, body.bytes.size()), expected.span().subspan(start, body.bytes.size())));
}

static void checkLinkedData(const Steps& steps, uint32_t position, std::span<const uint8_t> code)
{
    const CodeBody& body = steps.functions[position].body;
    size_t constantsOffset = steps.layout.constantsOffset;
    for (size_t index = 0; index < body.constants.size(); ++index) {
        size_t offset = constantsOffset + steps.constants.constantOffsetsByBody[position][index];
        AOT_TEST_CHECK(!(offset % body.constants[index].alignment));
        AOT_TEST_CHECK(equalSpans(code.subspan(offset, body.constants[index].bytes.size()), body.constants[index].bytes.span()));
    }
    for (size_t index = 0; index < body.jumpTables.size(); ++index) {
        size_t table = constantsOffset + steps.constants.jumpTableOffsetsByBody[position][index];
        const Vector<uint32_t>& targets = body.jumpTables[index].targets;
        for (size_t word = 0; word < targets.size(); ++word) {
            int64_t distance = static_cast<int64_t>(steps.layout.bodyStarts[position]) + targets[word] - static_cast<int64_t>(table);
            AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(code.subspan(table + word * sizeof(int32_t)).data()) == distance);
        }
    }
}

static void checkLinkedBodies(const Steps& steps, std::span<const uint8_t> code)
{
    for (uint32_t position = 0; position < steps.functions.size(); ++position) {
        checkLinkedBody(steps, position, code);
        checkLinkedData(steps, position, code);
    }
}

static WTF::BitSet<numberOfSymbolSpaces> spacesNamedBy(const CodeBody& body)
{
    WTF::BitSet<numberOfSymbolSpaces> spaces;
    for (const Relocation& relocation : body.relocations)
        spaces.set(static_cast<size_t>(relocation.space));
    return spaces;
}

static void testEverySpaceLinks()
{
    uint32_t function = 0;
    Steps steps(bodiesWithEverySpace(function));
    prepareToLink(steps);
    uint32_t position = *steps.index->indexOf(FunctionID(function));
    WTF::BitSet<numberOfSymbolSpaces> spaces = spacesNamedBy(steps.functions[position].body);
    AOT_TEST_CHECK(spaces.count() == numberOfSymbolSpaces - 2);
    AOT_TEST_CHECK(!spaces.get(static_cast<size_t>(SymbolSpace::WebAssembly)));
    AOT_TEST_CHECK(!spaces.get(static_cast<size_t>(SymbolSpace::FFI)));

    Vector<uint8_t> code = link(steps);
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    checkLinkedBodies(steps, code.span());
}

static void testOwnOutlinedBodyLinks()
{
    CodeSketch sketch = singleCallerSketch(1);
    sketch.end();
    Steps steps(bodies(sketch.take()));
    prepareToLink(steps);
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(steps.outlining.bodies[0].singleCaller == OptionalIndex { 0 });
    AOT_TEST_CHECK(spacesNamedBy(steps.functions[0].body).get(static_cast<size_t>(SymbolSpace::OutlinedBody)));

    Vector<uint8_t> code = link(steps);
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    checkLinkedBodies(steps, code.span());
    const OutlinedBodies& ownBodies = steps.outlinedCode.ownBodies;
    AOT_TEST_CHECK(equalSpans(code.span().subspan(steps.layout.ownBodyOffsets[0], ownBodies.sizeOf(0)), ownBodies.bytes.span().first(ownBodies.sizeOf(0))));
}

static Relocation& relocationIn(Steps& steps, uint32_t position, SymbolSpace space)
{
    for (Relocation& relocation : steps.functions[position].body.relocations) {
        if (relocation.space == space)
            return relocation;
    }
    AOT_TEST_CHECK(false);
    return steps.functions[position].body.relocations[0];
}

static const InternalError& onlyError(Steps& steps, InternalErrorKind kind)
{
    AOT_TEST_CHECK(steps.build.count(kind) == 1);
    const Vector<InternalError>& errors = steps.build.context().diagnostics().internalErrors();
    AOT_TEST_CHECK(errors.size() == 1);
    return errors[0];
}

static void testMissingStub()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), repeated(2)));
    prepareToLink(steps);
    AOT_TEST_CHECK(exists(toPropertyKey()));
    AOT_TEST_CHECK(!steps.sharedCode.blob.offsetOf(toPropertyKey()));
    steps.functions[0].body.stubKeys[relocationIn(steps, 0, SymbolSpace::Stub).symbol] = toPropertyKey();
    link(steps);
    AOT_TEST_CHECK(onlyError(steps, InternalErrorKind::CallToStubNotGenerated).function == FunctionID(1u));
}

static void testRelocationPastEnd()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), repeated(2)));
    prepareToLink(steps);
    CodeBody& body = steps.functions[0].body;
    body.relocations.last().offset = static_cast<uint32_t>(body.bytes.size()) - codeOffsetUnit;
    AOT_TEST_CHECK(!liesInside(body, body.relocations.last()));
    Vector<uint8_t> code = link(steps);
    const InternalError& error = onlyError(steps, InternalErrorKind::RelocationNotApplied);
    AOT_TEST_CHECK(error.count == 1);
    AOT_TEST_CHECK(!error.function);
    checkLinkedBodies(steps, code.span());
}

static void nameSymbolPastEnd(Relocation& relocation)
{
    relocation.symbol = 1000;
}

static void testRelocationsThatNameNothing()
{
    static constexpr BrokenRelocation cases[] = {
        { SymbolSpace::Stub, [](Relocation& relocation) { relocation.space = SymbolSpace::WebAssembly; } },
        { SymbolSpace::Stub, [](Relocation& relocation) { relocation.space = SymbolSpace::FFI; } },
        { SymbolSpace::Stub, [](Relocation& relocation) { relocation.space = SymbolSpace::SiteCache; } },
        { SymbolSpace::RegExp, [](Relocation& relocation) { relocation.entry = 2; } },
        { SymbolSpace::Function, nameSymbolPastEnd },
        { SymbolSpace::Stub, nameSymbolPastEnd },
        { SymbolSpace::ArgumentThunk, nameSymbolPastEnd },
        { SymbolSpace::Constant, nameSymbolPastEnd },
        { SymbolSpace::JumpTable, nameSymbolPastEnd },
        { SymbolSpace::SiteCache, [](Relocation& relocation) { relocation.symbol = linkedInstanceSizes.numberOfSiteCaches; } },
        { SymbolSpace::RegExp, nameSymbolPastEnd },
    };
    for (const BrokenRelocation& broken : cases) {
        Steps steps(bodies(bodyWithManySpaces(1, 2), repeated(2)));
        prepareToLink(steps);
        Relocation& relocation = relocationIn(steps, 0, broken.space);
        broken.damage(relocation);
        link(steps);
        const InternalError& error = onlyError(steps, InternalErrorKind::RelocationNotApplied);
        AOT_TEST_CHECK(error.function == FunctionID(1u));
        AOT_TEST_CHECK(error.count == relocation.offset);
    }
}

std::span<const NativeTest> codeLinkerTests()
{
    static constexpr NativeTest tests[] = {
        { "image/linker/every-space-links"_s, testEverySpaceLinks },
        { "image/linker/own-outlined-body-links"_s, testOwnOutlinedBodyLinks },
        { "image/linker/missing-stub"_s, testMissingStub },
        { "image/linker/relocation-past-end"_s, testRelocationPastEnd },
        { "image/linker/relocations-that-name-nothing"_s, testRelocationsThatNameNothing },
    };
    return tests;
}

} } } // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
