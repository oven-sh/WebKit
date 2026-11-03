/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageOutliningTestBodies.h"
#include "AOTStubsPolls.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include <wtf/BitSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace ImageTests {

using Label = CCallHelpers::Label;
using Kind = CodeUnit::Kind;

namespace {

struct SiteEmitter {
    bool emitsCalls { false };
    WTF::Vector<Label> starts;
    WTF::Vector<Label> ends;
};

struct Rewritten {
    const CompiledCode& after() const { return steps->functions[0]; }

    CompiledCode before;
    CompiledCode expected;
    std::unique_ptr<Steps> steps;
};

struct VisitedOffset {
    friend bool operator==(const VisitedOffset&, const VisitedOffset&) = default;

    uint32_t offset;
    CodeOffsetRole role;
    unsigned size;
};

struct MovedOffset {
    uint32_t before;
    uint32_t after;
    bool liesAfterSite;
};

struct RecordLabels {
    Label frameBegin;
    Label resume;
    Label fault;
    Label poll;
    Label landingPad;
    Label tableTarget;
    Label frameEnd;
    Label prologueCall;
};

struct TableLabels {
    Label head;
    Label between;
    Label end;
};

} // anonymous namespace

static const StubCatalog& catalog()
{
    static NeverDestroyed<StubCatalog> catalog(StubOptions { }, StubPolicy { });
    return catalog;
}

static Relocation outlinedCall(uint32_t offset, uint32_t body)
{
    return { offset, RelocationKind::Call, SymbolSpace::OutlinedBody, primaryEntry, body };
}

static Relocation finalJump(uint32_t offset, SymbolSpace space)
{
    return { offset, RelocationKind::Jump, space, primaryEntry, 0 };
}

static Vector<Relocation> outlinedCallsOf(const CompiledCode& code)
{
    Vector<Relocation> calls;
    for (const Relocation& relocation : code.body.relocations) {
        if (relocation.space == SymbolSpace::OutlinedBody)
            calls.append(relocation);
    }
    return calls;
}

static void emitPlain(Assembly& assembly, unsigned bytes)
{
    Label begin = assembly.jit.label();
    assembly.jit.emitNops(bytes);
    assembly.units.addPlain(begin, assembly.jit.label(), RegisterSet { });
}

static void emitEnd(Assembly& assembly)
{
    Label begin = assembly.jit.label();
    assembly.jit.breakpoint();
    assembly.units.addEnd(begin, assembly.jit.label());
}

static void emitRange(Assembly& assembly, SiteEmitter& sites)
{
    uint32_t number = sites.starts.size();
    sites.starts.append(assembly.jit.label());
    if (sites.emitsCalls)
        assembly.relocations.call({ SymbolSpace::OutlinedBody, number });
    else {
        for (unsigned i = 0; i < 4; ++i)
            emitPlain(assembly, 4);
    }
    sites.ends.append(assembly.jit.label());
}

static void linkNothing(LinkBuffer&, CompiledCode&) { }

template<typename Emit, typename AfterLink>
static Rewritten rewriteOne(const Emit& emit, const AfterLink& afterLink)
{
    auto linkFramed = [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        code.frame.frameRanges = { CodeRange { 0, code.body.paddingStart } };
        afterLink(linkBuffer, code);
    };
    Outlining outlining;
    outlining.sites.grow(1);
    Rewritten result;
    SiteEmitter sequences;
    result.before = assemble(1, catalog(), [&](Assembly& assembly) {
        emit(assembly, sequences);
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        linkFramed(linkBuffer, code);
        for (uint32_t i = 0; i < sequences.starts.size(); ++i) {
            uint32_t offset = linkBuffer.offsetOf(sequences.starts[i]);
            uint32_t length = linkBuffer.offsetOf(sequences.ends[i]) - offset;
            outlining.sites[0].append(OutlinedSite { offset, length, i });
            outlining.bodies.append(OutlinedBody { 0, offset, length, 1, OptionalIndex { 0 }, false });
        }
    });
    SiteEmitter calls;
    calls.emitsCalls = true;
    result.expected = assemble(1, catalog(), [&](Assembly& assembly) {
        emit(assembly, calls);
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        linkFramed(linkBuffer, code);
        for (Label end : calls.ends)
            code.body.returnSites.append(ReturnSite { linkBuffer.offsetOf(end), { } });
        std::ranges::sort(code.body.returnSites, { }, &ReturnSite::offset);
    });

    Vector<CompiledCode> functions;
    functions.append(result.before);
    result.steps = makeUnique<Steps>(WTF::move(functions));
    result.steps->useOutlining(WTF::move(outlining));
    result.steps->rewrite();
    return result;
}

static Vector<VisitedOffset> visitedOffsetsOf(const CompiledCode& code)
{
    Vector<VisitedOffset> offsets;
    forEachCodeOffset(code, [&](uint32_t offset, CodeOffsetRole role, unsigned size) {
        offsets.append(VisitedOffset { offset, role, size });
    });
    return offsets;
}

static CompiledCode withoutLocalSites(CompiledCode code)
{
    code.body.localSites.clear();
    return code;
}

static void checkRewritten(const Rewritten& rewritten)
{
    const CompiledCode& after = rewritten.after();
    const CompiledCode& expected = rewritten.expected;
    AOT_TEST_CHECK(after.body.bytes == expected.body.bytes && after.body.paddingStart == after.body.bytes.size());
    AOT_TEST_CHECK(after.body.relocations == expected.body.relocations);
    AOT_TEST_CHECK(std::ranges::equal(after.body.returnSites, expected.body.returnSites, [](const ReturnSite& a, const ReturnSite& b) {
        return a.offset == b.offset && a.site == b.site;
    }));
    AOT_TEST_CHECK(visitedOffsetsOf(after) == visitedOffsetsOf(withoutLocalSites(expected)));

    const Vector<OutlinedSite>& sites = rewritten.steps->outlining.sites[0];
    Vector<Relocation> calls = outlinedCallsOf(after);
    AOT_TEST_CHECK(calls.size() == sites.size());
    for (uint32_t i = 0; i < sites.size(); ++i) {
        AOT_TEST_CHECK(sites[i].length == sequenceLength);
        AOT_TEST_CHECK(calls[i] == outlinedCall(sites[i].offset - i * removedBytes, i));
    }
}

static const LinkedSite& findBranch(const CompiledCode& code)
{
    AOT_TEST_CHECK(code.body.localSites.size() == 1);
    return code.body.localSites[0];
}

static void checkBranchMoved(const Rewritten& rewritten)
{
    checkRewritten(rewritten);
    const LinkedSite& before = findBranch(rewritten.before);
    const LinkedSite& expected = findBranch(rewritten.expected);
    AOT_TEST_CHECK(before.kind == expected.kind);
    unsigned size = MacroAssembler::relocationSize(before.kind);
    AOT_TEST_CHECK(memcmp(rewritten.before.body.bytes.span().data() + before.offset, rewritten.after().body.bytes.span().data() + expected.offset, size));
}

template<typename EmitBranch, typename EmitSkipped>
static Rewritten rewriteBranchOver(const EmitBranch& emitBranch, const EmitSkipped& emitSkipped)
{
    Rewritten rewritten = rewriteOne([&](Assembly& assembly, SiteEmitter& sites) {
        CCallHelpers::Jump branch = emitBranch(assembly.jit);
        emitSkipped(assembly, sites);
        branch.link(&assembly.jit);
        emitEnd(assembly);
    }, linkNothing);
    checkBranchMoved(rewritten);
    return rewritten;
}

static CCallHelpers::Jump emitJump(CCallHelpers& jit)
{
    return jit.jump();
}

static void testBranchForward()
{
    rewriteBranchOver(emitJump, emitRange);
}

static void testConditionalBranch()
{
    rewriteBranchOver([](CCallHelpers& jit) {
        return jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
    }, emitRange);
}

static void testBranchBackward()
{
    Rewritten rewritten = rewriteOne([](Assembly& assembly, SiteEmitter& sites) {
        Label top = assembly.jit.label();
        emitRange(assembly, sites);
        assembly.jit.jump().linkTo(top, &assembly.jit);
        emitEnd(assembly);
    }, linkNothing);
    checkBranchMoved(rewritten);
}

#if CPU(X86_64)
static void testShortBranchStaysShort()
{
    Rewritten rewritten = rewriteBranchOver(emitJump, [](Assembly& assembly, SiteEmitter& sites) {
        emitRange(assembly, sites);
        emitPlain(assembly, 40);
    });
    AOT_TEST_CHECK(findBranch(rewritten.before).kind == RelocationKind::ShortJump);
    AOT_TEST_CHECK(MacroAssembler::relocationSize(findBranch(rewritten.expected).kind) == 2);
}
#endif

static void testTwoSites()
{
    rewriteBranchOver(emitJump, [](Assembly& assembly, SiteEmitter& sites) {
        emitRange(assembly, sites);
        emitPlain(assembly, 16);
        emitRange(assembly, sites);
    });
}

static void testJumpTableTargets()
{
    TableLabels labels;
    Rewritten rewritten = rewriteOne([&](Assembly& assembly, SiteEmitter& sites) {
        labels.head = assembly.jit.label();
        emitRange(assembly, sites);
        emitPlain(assembly, 8);
        labels.between = assembly.jit.label();
        emitPlain(assembly, 8);
        labels.end = assembly.jit.label();
        emitEnd(assembly);
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        code.body.jumpTables.append(JumpTable { { linkBuffer.offsetOf(labels.between), linkBuffer.offsetOf(labels.end), linkBuffer.offsetOf(labels.head) } });
        code.body.jumpTables.append(JumpTable { { linkBuffer.offsetOf(labels.head), linkBuffer.offsetOf(labels.between) } });
    });
    checkRewritten(rewritten);
    const Vector<JumpTable>& before = rewritten.before.body.jumpTables;
    const Vector<JumpTable>& after = rewritten.after().body.jumpTables;
    AOT_TEST_CHECK(after == rewritten.expected.body.jumpTables);
    uint32_t head = outlinedCallsOf(rewritten.after())[0].offset;
    AOT_TEST_CHECK(after[0].targets[0] == before[0].targets[0] - removedBytes && after[0].targets[1] == before[0].targets[1] - removedBytes);
    AOT_TEST_CHECK(after[0].targets[2] == head && after[1].targets[0] == head);
    AOT_TEST_CHECK(after[1].targets[1] == before[1].targets[1] - removedBytes);
}

static void emitRelocationOfEachSpace(Assembly& assembly)
{
    CCallHelpers& jit = assembly.jit;
    ArgumentMoves moves;
    appendArgumentMove(moves, GPRInfo::argumentGPR1, ArgumentSource::Constant, 7);
    assembly.calls.callFunction(jit, FunctionID(1u), primaryEntry, OptionalIndex { 1 });
    assembly.calls.call(jit, writeBarrier(), OptionalIndex { 2 });
    assembly.calls.call(jit, writeBarrier(), std::nullopt, OptionalIndex { 3 }, moves);
    assembly.calls.callForTwoReads(jit, toPropertyKey(), OptionalIndex { 4 }, OptionalIndex { 5 });
    for (SymbolSpace space : { SymbolSpace::Constant, SymbolSpace::JumpTable, SymbolSpace::RegExp, SymbolSpace::WebAssembly, SymbolSpace::FFI })
        assembly.relocations.moveAddress({ space, 0 }, GPRInfo::regT0, PCRelativeReach::Far);
    assembly.relocations.moveInstanceAddress({ SymbolSpace::SiteCache, 0 }, GPRInfo::regT1, GPRInfo::regT0);
}

static void emitRecordProgram(Assembly& assembly, SiteEmitter& sites, RecordLabels& labels)
{
    CCallHelpers& jit = assembly.jit;
    emitPlain(assembly, 4);
    labels.frameBegin = jit.label();
    emitPlain(assembly, 4);
    labels.resume = jit.label();
    emitPlain(assembly, 4);
    CCallHelpers::Jump branch = jit.jump();
    emitRange(assembly, sites);
    emitRelocationOfEachSpace(assembly);
    labels.fault = jit.label();
    jit.emitNops(4);
    labels.poll = jit.label();
    jit.emitNops(8);
    branch.link(&jit);
    labels.landingPad = jit.label();
    jit.emitNops(4);
    labels.tableTarget = jit.label();
    emitPlain(assembly, 4);
    labels.frameEnd = jit.label();
    emitEnd(assembly);
    labels.prologueCall = jit.label();
    assembly.calls.call(jit, pollAtPrologue(true), OptionalIndex { });
    assembly.units.pin(labels.prologueCall, jit.label());
}

static void linkRecordProgram(LinkBuffer& linkBuffer, CompiledCode& code, const RecordLabels& labels)
{
    code.frame.frameRanges = { CodeRange { linkBuffer.offsetOf(labels.frameBegin), linkBuffer.offsetOf(labels.frameEnd) } };
    code.frame.prologueChecks.append(PrologueCheck { 1, linkBuffer.offsetOf(labels.prologueCall), linkBuffer.offsetOf(labels.resume) });
    code.body.jumpTables.append(JumpTable { { linkBuffer.offsetOf(labels.tableTarget), linkBuffer.offsetOf(labels.resume) } });
    code.sites.faultingAccesses.append(FaultingAccess { linkBuffer.offsetOf(labels.fault), { }, { 0 } });
    code.sites.pollSites.append(PollSite { linkBuffer.offsetOf(labels.poll), false });
    code.sites.handlers.append(SiteHandler { handlerSite, linkBuffer.offsetOf(labels.landingPad), HandlerType::Catch });
}

static void checkCallsAndRelocationsMoved(const CodeBody& before, const CodeBody& after, uint32_t siteOffset)
{
    AOT_TEST_CHECK(after.relocations.size() == before.relocations.size() + 1 && after.relocations[0] == outlinedCall(siteOffset, 0));
    WTF::BitSet<numberOfSymbolSpaces> spaces;
    spaces.set(static_cast<size_t>(after.relocations[0].space));
    for (size_t i = 0; i < before.relocations.size(); ++i) {
        Relocation moved = before.relocations[i];
        moved.offset -= removedBytes;
        AOT_TEST_CHECK(after.relocations[i + 1] == moved);
        spaces.set(static_cast<size_t>(moved.space));
    }
    AOT_TEST_CHECK(spaces.count() == numberOfSymbolSpaces);

    AOT_TEST_CHECK(after.returnSites.size() == before.returnSites.size() + 1);
    AOT_TEST_CHECK(after.returnSites[0].offset == siteOffset + callSize && !after.returnSites[0].site);
    for (size_t i = 0; i < before.returnSites.size(); ++i) {
        AOT_TEST_CHECK(after.returnSites[i + 1].offset == before.returnSites[i].offset - removedBytes);
        AOT_TEST_CHECK(after.returnSites[i + 1].site == before.returnSites[i].site);
    }
}

static void testRecordsMove()
{
    RecordLabels labels;
    Rewritten rewritten = rewriteOne([&](Assembly& assembly, SiteEmitter& sites) {
        emitRecordProgram(assembly, sites, labels);
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        linkRecordProgram(linkBuffer, code, labels);
    });
    checkBranchMoved(rewritten);
    const CompiledCode& before = rewritten.before;
    const CompiledCode& after = rewritten.after();
    checkCallsAndRelocationsMoved(before.body, after.body, rewritten.steps->outlining.sites[0][0].offset);
    AOT_TEST_CHECK(before.body.firstReadSites.size() == 1 && after.body.firstReadSites.size() == 1 && after.body.firstReadSites[0].site == 4);
    const MovedOffset offsets[] = {
        { before.body.entries[0].offset, after.body.entries[0].offset, false },
        { before.body.firstReadSites[0].returnOffset, after.body.firstReadSites[0].returnOffset, true },
        { before.body.jumpTables[0].targets[0], after.body.jumpTables[0].targets[0], true },
        { before.body.jumpTables[0].targets[1], after.body.jumpTables[0].targets[1], false },
        { before.body.paddingStart, after.body.paddingStart, true },
        { before.sites.handlers[0].codeOffset, after.sites.handlers[0].codeOffset, true },
        { before.sites.faultingAccesses[0].codeOffset, after.sites.faultingAccesses[0].codeOffset, true },
        { before.sites.pollSites[0].offset, after.sites.pollSites[0].offset, true },
        { before.frame.frameRanges[0].begin, after.frame.frameRanges[0].begin, false },
        { before.frame.frameRanges[0].end, after.frame.frameRanges[0].end, true },
        { before.frame.prologueChecks[0].callOffset, after.frame.prologueChecks[0].callOffset, true },
        { before.frame.prologueChecks[0].resumeOffset, after.frame.prologueChecks[0].resumeOffset, false },
    };
    for (const MovedOffset& offset : offsets)
        AOT_TEST_CHECK(offset.after == offset.before - (offset.liesAfterSite ? removedBytes : 0));
}

static void testHandlerAtHead()
{
    Label head;
    Rewritten rewritten = rewriteOne([&](Assembly& assembly, SiteEmitter& sites) {
        emitRange(assembly, sites);
        emitPlain(assembly, 8);
        head = assembly.jit.label();
        emitRange(assembly, sites);
        emitEnd(assembly);
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        code.sites.handlers.append(SiteHandler { handlerSite, linkBuffer.offsetOf(head), HandlerType::Catch });
    });
    checkRewritten(rewritten);
    AOT_TEST_CHECK(rewritten.after().sites.handlers[0].codeOffset == outlinedCallsOf(rewritten.after())[1].offset);
}

static void testSiteEndsInCall()
{
    Steps steps(bodiesEndingInCall());
    steps.rewrite();
    const OutlinedBodies& shared = steps.outlinedCode.sharedBodies;
    AOT_TEST_CHECK(shared.bytes.size() == sequenceLength + sizeOfJump);
    AOT_TEST_CHECK(shared.relocations.size() == 1 && shared.relocations[0] == finalJump(sequenceLength, SymbolSpace::Stub));
    AOT_TEST_CHECK(equalSpans(shared.bytes.span().last(sizeOfJump), testTemplates().nearJump.span()));
    AOT_TEST_CHECK(steps.outlinedCode.stubKeys.size() == 1 && steps.outlinedCode.stubKeys[0] == writeBarrier());
    for (const CompiledCode& code : steps.functions) {
        const CodeBody& body = code.body;
        AOT_TEST_CHECK(body.relocations.size() == 1 && body.relocations[0] == outlinedCall(sequenceOffset, 0));
        AOT_TEST_CHECK(body.returnSites.size() == 1 && body.returnSites[0].offset == sequenceOffset + callSize);
        AOT_TEST_CHECK(body.returnSites[0].site == OptionalIndex { 1 });
    }
}

static void testCallForTwoReads()
{
    Steps steps(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        OptionalIndex firstRead = sketch.addSite();
        uint32_t call = sketch.callStub(toPropertyKey(), sketch.addSite());
        sketch.code().body.firstReadSites.append(FirstReadSite { call + callSize, *firstRead });
        sketch.end();
    }));
    steps.rewrite();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && steps.outlining.bodies[0].endsInCall == !isARM64());
    uint32_t returnOffset = sequenceOffset + callSize + (isARM64() ? callSize : 0);
    for (const CompiledCode& code : steps.functions) {
        const CodeBody& body = code.body;
        AOT_TEST_CHECK(body.firstReadSites.size() == 1 && body.firstReadSites[0].returnOffset == returnOffset && body.firstReadSites[0].site == 1);
        AOT_TEST_CHECK(body.returnSites.last().offset == returnOffset && body.returnSites.last().site == OptionalIndex { 2 });
    }
}

static void testReturns()
{
    Steps steps(repeatedBodies());
    steps.rewrite();
    const OutlinedBodies& shared = steps.outlinedCode.sharedBodies;
    AOT_TEST_CHECK(shared.offsets.size() == 1 && shared.relocations.isEmpty());
    Vector<uint8_t> expected;
    for (uint32_t word = 1; word <= 4; ++word)
        expected.append(asByteSpan(word));
#if CPU(ARM64)
    constexpr uint32_t ret = 0xd65f03c0;
    expected.append(asByteSpan(ret));
#else
    expected.append(0xc3);
#endif
    AOT_TEST_CHECK(shared.bytes == expected);
    for (const CompiledCode& code : steps.functions) {
        AOT_TEST_CHECK(code.body.returnSites.size() == 1 && code.body.returnSites[0].offset == sequenceOffset + callSize);
        AOT_TEST_CHECK(!code.body.returnSites[0].site);
    }
}

static void testOwnAgainstShared()
{
    Vector<CompiledCode> bodies = repeatedBodies();
    bodies.append(singleCallerBody(numberOfBodies + 1, 5));
    Steps steps(WTF::move(bodies));
    steps.rewrite();
    const OutlinedCode& outlined = steps.outlinedCode;
    AOT_TEST_CHECK(outlined.indexWithinKind.size() == 2 && !outlined.indexWithinKind[0] && !outlined.indexWithinKind[1]);
    AOT_TEST_CHECK(outlined.ownBodiesByCaller[numberOfBodies].size() == 1);
    AOT_TEST_CHECK(outlined.ownBodiesSize(numberOfBodies) == sequenceLength + returnSize);
    for (uint32_t position = 0; position < numberOfBodies; ++position)
        AOT_TEST_CHECK(outlined.ownBodiesByCaller[position].isEmpty() && !outlined.ownBodiesSize(position));
}

#if CPU(X86_64)
static void testSiteEndsInFunctionCall()
{
    Steps steps(bodiesCalling(calledFunction, calledFunction));
    steps.rewrite();
    const OutlinedCode& outlined = steps.outlinedCode;
    AOT_TEST_CHECK(outlined.sharedBodies.relocations.size() == 1 && outlined.sharedBodies.relocations[0] == finalJump(sequenceLength, SymbolSpace::Function));
    AOT_TEST_CHECK(outlined.callees.size() == 1 && outlined.callees[0].body == OptionalIndex { numberOfBodies } && !outlined.callees[0].alias);
}

static void testAliasStaysAligned()
{
    static constexpr uint32_t aliasBodyWord = 0x11223344;
    uint32_t aliasStart = 0;
    Vector<CompiledCode> bodies = sequenceBodies([&](CodeSketch& sketch, uint32_t) {
        sketch.end();
        aliasStart = sketch.pinned(aliasBodyWord, 2);
    }, &CodeSketch::takeUnchecked);
    AOT_TEST_CHECK(!(aliasStart % 2));
    Steps steps(WTF::move(bodies), { }, createBodyAliases(AliasPlacement::AfterBody, aliasStart));
    steps.rewrite();
    for (uint32_t position = 0; position < numberOfBodies; ++position) {
        uint32_t movedStart = steps.aliases[position].start;
        const Vector<uint8_t>& bytes = steps.functions[position].body.bytes;
        AOT_TEST_CHECK(!(movedStart % 2));
        const auto& padding = steps.outlinedCode.paddingBeforeAliases[position];
        AOT_TEST_CHECK(padding.size() == 1 && padding[0].first == aliasStart && padding[0].second == 1);
        AOT_TEST_CHECK(bytes[movedStart - 1] == 0xf4);
        AOT_TEST_CHECK(bytes.size() == movedStart + 2 * sizeof(aliasBodyWord));
        for (unsigned i = 0; i < 2; ++i)
            AOT_TEST_CHECK(!memcmp(bytes.span().data() + movedStart + i * sizeof(aliasBodyWord), &aliasBodyWord, sizeof(aliasBodyWord)));
    }
}
#endif

static void testAliasBeforeBodyStays()
{
    Vector<CompiledCode> bodies = repeatedBodies();
    for (CompiledCode& code : bodies) {
        changeUnits(code.body, [](Vector<CodeUnit>& units) {
            units[0].kind = Kind::Pinned;
        });
        code.body.entries[0].offset = sequenceOffset;
    }
    Steps steps(WTF::move(bodies), { }, createBodyAliases(AliasPlacement::BeforeBody, 0));
    steps.rewrite();
    checkSharedSequence(steps);
    for (uint32_t position = 0; position < numberOfBodies; ++position) {
        AOT_TEST_CHECK(!steps.aliases[position].start && steps.outlinedCode.paddingBeforeAliases[position].isEmpty());
        AOT_TEST_CHECK(steps.functions[position].body.primaryEntryOffset() == sequenceOffset);
    }
}

static void testMovesAreCopied()
{
    Steps steps(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        finishWithMoves(sketch, 7);
    }));
    steps.rewrite();
    const OutlinedCode& outlined = steps.outlinedCode;
    AOT_TEST_CHECK(outlined.argumentMoves.size() == 1 && outlined.argumentMoves[0] == stubArgumentMoves(writeBarrier(), 7));
    AOT_TEST_CHECK(outlined.sharedBodies.relocations.size() == 1 && outlined.sharedBodies.relocations[0] == finalJump(sequenceLength, SymbolSpace::ArgumentThunk));
}

static Vector<CompiledCode> repeatedBodiesAndPinnedBody()
{
    Vector<CompiledCode> bodies = repeatedBodies();
    bodies.append(pinnedBody(99));
    return bodies;
}

static void testUntouchedBody()
{
    Steps steps(repeatedBodiesAndPinnedBody());
    steps.rewrite();
    AOT_TEST_CHECK(outlinedCallsOf(steps.functions[0]).size() == 1 && outlinedCallsOf(steps.functions[numberOfBodies]).isEmpty());
    AOT_TEST_CHECK(steps.functions[numberOfBodies].body.bytes == pinnedBody(99).body.bytes);
}

static void testUnitsAreGone()
{
    Vector<CompiledCode> bodies = bodiesJumpingTo(sequenceOffset);
    bodies.append(pinnedBody(99));
    AOT_TEST_CHECK(!bodies[0].body.localSites.isEmpty());
    Steps steps(WTF::move(bodies));
    steps.rewrite();
    for (const CompiledCode& code : steps.functions)
        AOT_TEST_CHECK(code.body.units.isEmpty() && code.body.localSites.isEmpty());
}

static void testNothingChosen()
{
    constexpr uint32_t numberOfSites = 2;
    Steps steps(repeatedBodies(numberOfSites));
    steps.rewrite();
    const OutlinedCode& outlined = steps.outlinedCode;
    AOT_TEST_CHECK(outlined.ownBodiesByCaller.size() == numberOfSites && outlined.paddingBeforeAliases.size() == numberOfSites);
    for (uint32_t position = 0; position < numberOfSites; ++position) {
        AOT_TEST_CHECK(outlined.ownBodiesByCaller[position].isEmpty() && outlined.paddingBeforeAliases[position].isEmpty());
        AOT_TEST_CHECK(steps.functions[position].body.relocations.isEmpty() && steps.functions[position].body.returnSites.isEmpty());
        AOT_TEST_CHECK(steps.functions[position].body.bytes == repeated(position + 1).body.bytes);
    }
    AOT_TEST_CHECK(outlined.sharedBodies.bytes.isEmpty() && outlined.ownBodies.bytes.isEmpty());
}

static CompiledCode assembleSum(uint32_t function)
{
    Label frameBegin;
    Label frameEnd;
    return assemble(function, catalog(), [&](Assembly& assembly) {
        CCallHelpers& jit = assembly.jit;
        jit.emitFunctionPrologue();
        frameBegin = jit.label();
        jit.move(CCallHelpers::TrustedImm32(function), GPRInfo::returnValueGPR);
        for (int32_t addend = 1; addend <= 4; ++addend) {
            Label begin = jit.label();
            jit.add32(CCallHelpers::TrustedImm32(addend), GPRInfo::returnValueGPR);
            assembly.units.addPlain(begin, jit.label(), RegisterSet { GPRInfo::returnValueGPR });
        }
        frameEnd = jit.label();
        jit.emitFunctionEpilogue();
        jit.ret();
    }, [&](LinkBuffer& linkBuffer, CompiledCode& code) {
        code.frame.frameRanges = { CodeRange { linkBuffer.offsetOf(frameBegin), linkBuffer.offsetOf(frameEnd) } };
        code.frame.frameSizeInBytes = 0;
    });
}

static void testOutlinedCodeRuns()
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function)
        bodies.append(assembleSum(function));
    TestBuild build(numberOfBodies + 1);
    TestImage built = build.build(WTF::move(bodies));
    AOT_TEST_CHECK(built.image);
    AOT_TEST_CHECK(build.statistic(StatisticID::BodiesOutlined) >= 1);
    ExecutableCopy copy(built.image->code());
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        size_t offset = built.address(function, 0) - built.image->code().data();
        AOT_TEST_CHECK(copy.call<int32_t>(offset) == static_cast<int32_t>(function + 10));
    }
}

std::span<const NativeTest> outliningRewriteTests()
{
    static constexpr NativeTest tests[] = {
        { "image/outlining-rewrite/branch-forward"_s, testBranchForward },
        { "image/outlining-rewrite/branch-backward"_s, testBranchBackward },
        { "image/outlining-rewrite/conditional-branch"_s, testConditionalBranch },
        { "image/outlining-rewrite/two-sites"_s, testTwoSites },
        { "image/outlining-rewrite/jump-table-targets"_s, testJumpTableTargets },
        { "image/outlining-rewrite/records-move"_s, testRecordsMove },
        { "image/outlining-rewrite/handler-at-head"_s, testHandlerAtHead },
        { "image/outlining-rewrite/site-ends-in-call"_s, testSiteEndsInCall },
        { "image/outlining-rewrite/call-for-two-reads"_s, testCallForTwoReads },
        { "image/outlining-rewrite/returns"_s, testReturns },
        { "image/outlining-rewrite/own-against-shared"_s, testOwnAgainstShared },
        { "image/outlining-rewrite/alias-before-body-stays"_s, testAliasBeforeBodyStays },
        { "image/outlining-rewrite/moves-are-copied"_s, testMovesAreCopied },
        { "image/outlining-rewrite/untouched-body"_s, testUntouchedBody },
        { "image/outlining-rewrite/units-are-gone"_s, testUnitsAreGone },
        { "image/outlining-rewrite/nothing-chosen"_s, testNothingChosen },
        { "image/outlining-rewrite/outlined-code-runs"_s, whenExecutable<testOutlinedCodeRuns> },
#if CPU(X86_64)
        { "image/outlining-rewrite/short-branch-stays-short"_s, testShortBranchStaysShort },
        { "image/outlining-rewrite/site-ends-in-function-call"_s, testSiteEndsInFunctionCall },
        { "image/outlining-rewrite/alias-stays-aligned"_s, testAliasStaysAligned },
#endif
    };
    return tests;
}

} } } // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
