/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageOutliningTestBodies.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPolls.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/WeakRandom.h>

namespace JSC::AOT::ImageTests {

using Kind = CodeUnit::Kind;

static constexpr uint32_t sequenceMiddle = 12;
static constexpr uint32_t pinnedUnitOffset = 8;
static constexpr int64_t smallestSavingChosen = 64;
static constexpr unsigned numberOfSitesWorthOneCopy = 8;
static constexpr unsigned numberOfSitesWorthNothing = 2;
static constexpr uint32_t aliasOfCalledFunction = calledFunction + 1;

static constexpr int64_t savingOf(int64_t numberOfSites, int64_t numberOfCopies)
{
    return numberOfSites * removedBytes - numberOfCopies * (sequenceLength + returnSize);
}
static_assert(savingOf(numberOfSitesWorthOneCopy, 1) >= smallestSavingChosen && savingOf(numberOfSitesWorthOneCopy, 2) < smallestSavingChosen);
static_assert(savingOf(numberOfSitesWorthNothing, 1) > 0 && savingOf(numberOfSitesWorthNothing, 1) < smallestSavingChosen);

static bool isInSomeSite(const Outlining& outlining, uint32_t position, uint32_t offset)
{
    return std::ranges::any_of(outlining.sites[position], [&](const OutlinedSite& site) {
        return site.offset <= offset && offset < site.offset + site.length;
    });
}

static void checkChosenAsSharedSequence(Vector<CompiledCode>&& bodies)
{
    Steps steps(WTF::move(bodies));
    steps.choose();
    checkSharedSequence(steps);
}

static void checkNothingChosen(Vector<CompiledCode>&& bodies, unsigned numberOfSharedCodeCopies = 1)
{
    Steps steps(WTF::move(bodies));
    AOT_TEST_CHECK(steps.numberOfSharedCodeCopies == 1);
    steps.numberOfSharedCodeCopies = numberOfSharedCodeCopies;
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.isEmpty());
    for (const Vector<OutlinedSite>& sites : steps.outlining.sites)
        AOT_TEST_CHECK(sites.isEmpty());
}

static void checkNoSiteCrosses(Vector<CompiledCode>&& bodies, uint32_t offset, Vector<EntryAlias>&& aliases = { })
{
    Steps steps(WTF::move(bodies), { }, WTF::move(aliases));
    steps.choose();
    AOT_TEST_CHECK(!steps.outlining.bodies.isEmpty());
    for (const Vector<OutlinedSite>& sites : steps.outlining.sites) {
        for (const OutlinedSite& site : sites)
            AOT_TEST_CHECK(!(site.offset < offset && offset < site.offset + site.length));
    }
}

static void checkOffsetInNoSite(Vector<CompiledCode>&& bodies, uint32_t offset, Vector<EntryAlias>&& aliases = { })
{
    Steps steps(WTF::move(bodies), { }, WTF::move(aliases));
    steps.choose();
    for (uint32_t position = 0; position < steps.outlining.sites.size(); ++position)
        AOT_TEST_CHECK(!isInSomeSite(steps.outlining, position, offset));
}

static const Relocation& callEndingAt(const CompiledCode& code, uint32_t end)
{
    auto* call = std::ranges::lower_bound(code.body.relocations, end - callSize, { }, &Relocation::offset);
    AOT_TEST_CHECK(call != code.body.relocations.end() && call->offset + callSize == end && call->kind == RelocationKind::Call);
    return *call;
}

template<typename IsSameCall>
static void checkSitesEndInSameCall(Vector<CompiledCode>&& bodies, const IsSameCall& isSameCall)
{
    Steps steps(WTF::move(bodies));
    steps.choose();
    AOT_TEST_CHECK(std::ranges::count_if(steps.outlining.bodies, &OutlinedBody::endsInCall) == 2);
    for (uint32_t position = 0; position < steps.outlining.sites.size(); ++position) {
        for (const OutlinedSite& site : steps.outlining.sites[position]) {
            const OutlinedBody& body = steps.outlining.bodies[site.body];
            if (!body.endsInCall)
                continue;
            const CompiledCode& source = steps.functions[body.function];
            const CompiledCode& code = steps.functions[position];
            AOT_TEST_CHECK(isSameCall(source.body, callEndingAt(source, body.offset + body.length), code.body, callEndingAt(code, site.offset + site.length)));
        }
    }
}

static void testSharedSequence()
{
    checkChosenAsSharedSequence(repeatedBodies());
}

static void testSingleCaller()
{
    Steps steps(bodies(singleCallerBody(1, 1)));
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && steps.outlining.bodies[0].singleCaller == OptionalIndex { 0 });
    AOT_TEST_CHECK(steps.outlining.sites[0].size() == numberOfSingleCallerSites);
}

static void testSingleCallerPaysForOneCopy()
{
    CodeSketch sketch = sketchWithFrame(1);
    for (uint32_t site = 0; site < numberOfSitesWorthOneCopy; ++site) {
        sketch.pinned(100 + site);
        appendSequence(sketch);
    }
    sketch.end();
    Steps steps(bodies(sketch.take()));
    steps.numberOfSharedCodeCopies = 2;
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && steps.outlining.bodies[0].singleCaller == OptionalIndex { 0 });
    AOT_TEST_CHECK(steps.outlining.bodies[0].numberOfSites == numberOfSitesWorthOneCopy);
}

static void testSharedBodyPaysForEveryCopy()
{
    Steps steps(repeatedBodies(numberOfSitesWorthOneCopy));
    AOT_TEST_CHECK(steps.numberOfSharedCodeCopies == 1);
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && !steps.outlining.bodies[0].singleCaller);
    checkNothingChosen(repeatedBodies(numberOfSitesWorthOneCopy), 2);
}

static void testEndsInCall()
{
    Steps steps(bodiesEndingInCall());
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && steps.outlining.bodies[0].endsInCall);
    AOT_TEST_CHECK(steps.outlining.bodies[0].length == sequenceLength + sizeOfNearCall);
}

static void testStartsAtTarget()
{
    checkChosenAsSharedSequence(bodiesJumpingTo(sequenceOffset));
}

static void testPlainPartOfBody()
{
    uint32_t plainSequence = 0;
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        sketch.pinned(0x1000 + function);
        appendPinnedSequence(sketch);
        plainSequence = sketch.size();
        appendSequence(sketch);
        sketch.end();
        bodies.append(sketch.take());
    }
    Steps steps(WTF::move(bodies));
    steps.choose();
    checkSharedSequence(steps, plainSequence);
}

static void testPinnedBytesRepeat()
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        bodies.append(pinnedBody(function));
        AOT_TEST_CHECK(bodies.last().body.bytes == repeated(function).body.bytes);
    }
    checkNothingChosen(WTF::move(bodies));
}

#if CPU(X86_64)
static void testCallsOfOneBodyMatch()
{
    Steps steps(bodiesCalling(calledFunction, aliasOfCalledFunction), { }, { createAlias(aliasOfCalledFunction, calledFunction) });
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1 && steps.outlining.bodies[0].endsInCall);
    AOT_TEST_CHECK(steps.outlining.bodies[0].numberOfSites == numberOfBodies);
}

static void testCallOfInnerEntry()
{
    checkOffsetInNoSite(bodiesCalling(aliasOfCalledFunction, aliasOfCalledFunction), sequenceEnd, { createAlias(aliasOfCalledFunction, calledFunction, AliasPlacement::AfterBody, 8) });
}
#endif

static void testNoSaving()
{
    checkNothingChosen(repeatedBodies(numberOfSitesWorthNothing));
}

static void testFewSites()
{
    constexpr unsigned numberOfSites = 4;
    static_assert(savingOf(numberOfSites, 1) >= 16 && savingOf(numberOfSites, 1) < smallestSavingChosen);
    checkNothingChosen(repeatedBodies(numberOfSites));
}

#if CPU(X86_64)
// Seven bytes that every body has, in two units. A call in their place saves two bytes a site.
static Vector<CompiledCode> bodiesRepeatingSevenBytes(bool mayRun)
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        sketch.plain(0x1000 + function);
        uint32_t first = sketch.plain(0x11111111);
        uint32_t second = sketch.plain(0x00222222 | function << 24);
        sketch.end();
        CompiledCode code = sketch.take();
        Vector<CodeUnit> units;
        for (CodeUnit unit : code.body.units) {
            unit.mayRun = mayRun && (unit.offset == first || unit.offset == second);
            if (unit.offset != second) {
                units.append(unit);
                continue;
            }
            AOT_TEST_CHECK(unit.length == 4 && unit.kind == Kind::Plain);
            units.append(CodeUnit { second, 3, Kind::Plain, unit.mayRun });
            units.append(CodeUnit { second + 3, 1, Kind::Pinned });
        }
        code.body.units = WTF::move(units);
        bodies.append(WTF::move(code));
    }
    return bodies;
}

static void testSmallPieceWhereCodeMayRun()
{
    Steps rarelyRun(bodiesRepeatingSevenBytes(false));
    rarelyRun.choose();
    AOT_TEST_CHECK(rarelyRun.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(rarelyRun.outlining.bodies[0].length == 7 && rarelyRun.outlining.bodies[0].numberOfSites == numberOfBodies);
    checkNothingChosen(bodiesRepeatingSevenBytes(true));

    constexpr unsigned manySites = 512;
    constexpr unsigned fewSites = manySites - 1;
    auto bodiesThatMayRun = [](unsigned numberOfSequences, unsigned numberOfSites) {
        Vector<CompiledCode> bodies;
        for (uint32_t function = 1; function <= numberOfSites; ++function) {
            CodeSketch sketch = sketchWithFrame(function);
            sketch.plain(0x1000 + function);
            for (unsigned i = 0; i < numberOfSequences; ++i)
                appendSequence(sketch, 1 + 4 * i);
            sketch.end();
            bodies.append(sketch.take());
        }
        for (CompiledCode& code : bodies) {
            changeUnits(code.body, [](Vector<CodeUnit>& units) {
                for (CodeUnit& unit : units)
                    unit.mayRun = unit.kind == Kind::Plain;
            });
        }
        return bodies;
    };
    checkNothingChosen(bodiesThatMayRun(1, fewSites));
    Steps longer(bodiesThatMayRun(2, fewSites));
    longer.choose();
    AOT_TEST_CHECK(longer.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(longer.outlining.bodies[0].length == 2 * sequenceLength && longer.outlining.bodies[0].numberOfSites == fewSites);

    Steps usedOften(bodiesThatMayRun(1, manySites));
    usedOften.choose();
    AOT_TEST_CHECK(usedOften.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(usedOften.outlining.bodies[0].length == sequenceLength && usedOften.outlining.bodies[0].numberOfSites == manySites);
}
#endif

static void testTargetInMiddle()
{
    checkNoSiteCrosses(bodiesJumpingTo(sequenceMiddle), sequenceMiddle);
}

static void testHandlerInMiddle()
{
    checkNoSiteCrosses(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        sketch.code().sites.handlers.append(SiteHandler { handlerSite, sequenceMiddle, HandlerType::Catch });
        sketch.end();
    }), sequenceMiddle);
}

static void testJumpTableTargetInMiddle()
{
    checkNoSiteCrosses(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        sketch.code().body.jumpTables.append(JumpTable { { sequenceMiddle } });
        sketch.end();
    }), sequenceMiddle);
}

static void testResumePointInMiddle()
{
    checkNoSiteCrosses(sequenceBodies([](CodeSketch& sketch, uint32_t function) {
        uint32_t callOffset = sketch.callStub(pollAtPrologue());
        changeUnits(sketch.code().body, [](Vector<CodeUnit>& units) {
            units.last().kind = Kind::Pinned;
        });
        sketch.end();
        sketch.code().frame.prologueChecks.append(PrologueCheck { function, callOffset, sequenceMiddle });
    }), sequenceMiddle);
}

static void testFrameRangeEndInMiddle()
{
    Vector<CompiledCode> bodies = repeatedBodies();
    for (CompiledCode& code : bodies)
        code.frame.frameRanges = { CodeRange { 0, sequenceMiddle }, CodeRange { sequenceMiddle, code.body.paddingStart } };
    checkNoSiteCrosses(WTF::move(bodies), sequenceMiddle);
}

static void testAliasStartInMiddle()
{
    checkNoSiteCrosses(repeatedBodies(), sequenceMiddle, createBodyAliases(AliasPlacement::AfterBody, sequenceMiddle));
}

static void testAliasEntryInMiddle()
{
    checkNoSiteCrosses(repeatedBodies(), sequenceMiddle, createBodyAliases(AliasPlacement::AfterBody, sequenceOffset, sequenceMiddle - sequenceOffset));
}

static void testPinnedInMiddle()
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        sketch.plain(1);
        sketch.plain(2);
        sketch.pinned(9);
        sketch.plain(3);
        sketch.plain(4);
        sketch.end();
        bodies.append(sketch.take());
    }
    checkOffsetInNoSite(WTF::move(bodies), pinnedUnitOffset);
}

template<typename AddRecord>
static void checkRecordPinsUnit(const AddRecord& addRecord)
{
    checkOffsetInNoSite(sequenceBodies([&](CodeSketch& sketch, uint32_t) {
        addRecord(sketch.code());
        sketch.end();
    }, &CodeSketch::takeUnchecked), pinnedUnitOffset);
}

static void testRecordsPinUnits()
{
    checkRecordPinsUnit([](CompiledCode& code) {
        code.sites.faultingAccesses.append(FaultingAccess { pinnedUnitOffset, { }, { 0 } });
    });
    checkRecordPinsUnit([](CompiledCode& code) {
        code.sites.pollSites.append(PollSite { pinnedUnitOffset, false });
    });
    checkRecordPinsUnit([](CompiledCode& code) {
        code.body.relocations.append(Relocation { pinnedUnitOffset, RelocationKind::AddressNear, SymbolSpace::Constant, primaryEntry, 0 });
    });
    checkRecordPinsUnit([](CompiledCode& code) {
        code.body.relocations.append(Relocation { pinnedUnitOffset, RelocationKind::InstanceOffset, SymbolSpace::SiteCache, primaryEntry, 0 });
    });
    checkRecordPinsUnit([](CompiledCode& code) {
        code.body.localSites.append(LinkedSite { pinnedUnitOffset, sequenceOffset, RelocationKind::Jump });
    });
    checkRecordPinsUnit([](CompiledCode& code) {
        code.frame.prologueChecks.append(PrologueCheck { code.function.index(), pinnedUnitOffset, sequenceOffset });
    });
}

static void testOtherKeysAtEnd()
{
    checkSitesEndInSameCall(sequenceBodies([](CodeSketch& sketch, uint32_t function) {
        finishWithCall(sketch, function % 2 ? toPropertyKey() : writeBarrier());
    }), [](const CodeBody& firstBody, const Relocation& first, const CodeBody& secondBody, const Relocation& second) {
        return first.space == SymbolSpace::Stub && second.space == SymbolSpace::Stub && firstBody.stubKeys[first.symbol] == secondBody.stubKeys[second.symbol];
    });
}

static void testOtherMovesAtEnd()
{
    checkSitesEndInSameCall(sequenceBodies([](CodeSketch& sketch, uint32_t function) {
        finishWithMoves(sketch, function % 2);
    }), [](const CodeBody& firstBody, const Relocation& first, const CodeBody& secondBody, const Relocation& second) {
        return first.space == SymbolSpace::ArgumentThunk && second.space == SymbolSpace::ArgumentThunk && firstBody.argumentMoves[first.symbol] == secondBody.argumentMoves[second.symbol];
    });
}

static void testPrologueCheckCall()
{
    checkOffsetInNoSite(sequenceBodies([](CodeSketch& sketch, uint32_t function) {
        uint32_t callOffset = sketch.callStub(pollAtPrologue());
        uint32_t resumeOffset = sketch.end();
        sketch.code().frame.prologueChecks.append(PrologueCheck { function, callOffset, resumeOffset });
    }, &CodeSketch::takeUnchecked), sequenceEnd);
}

static void testCallOfRegExp()
{
    checkOffsetInNoSite(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.lastRelocation().space = SymbolSpace::RegExp;
        sketch.end();
    }), sequenceEnd);
}

static void testTailCall()
{
    checkOffsetInNoSite(sequenceBodies([](CodeSketch& sketch, uint32_t) {
        sketch.jumpToStub(catchException());
    }), sequenceEnd);
}

static void testFrameless()
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch(function);
        sketch.plain(0x1000 + function);
        appendSequence(sketch);
        sketch.jumpToStub(catchException());
        bodies.append(sketch.take());
        AOT_TEST_CHECK(!bodies.last().frame.hasFrame());
    }
    if (isARM64())
        checkNothingChosen(WTF::move(bodies));
    else
        checkChosenAsSharedSequence(WTF::move(bodies));
}

static void testOutsideFrameRange()
{
    uint32_t sequenceInFrame = 0;
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        sketch.plain(0x1000 + function);
        appendSequence(sketch);
        uint32_t frameBegin = sketch.pinned(0x2000 + function);
        sequenceInFrame = sketch.size();
        appendSequence(sketch);
        sketch.end();
        bodies.append(sketch.take());
        bodies.last().frame.frameRanges = { CodeRange { frameBegin, bodies.last().body.paddingStart } };
    }
    Steps steps(WTF::move(bodies));
    steps.choose();
    AOT_TEST_CHECK(steps.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(steps.outlining.bodies[0].numberOfSites == (isARM64() ? numberOfBodies : 2 * numberOfBodies));
    for (uint32_t position = 0; position < numberOfBodies; ++position) {
        AOT_TEST_CHECK(isInSomeSite(steps.outlining, position, sequenceInFrame));
        AOT_TEST_CHECK(isInSomeSite(steps.outlining, position, sequenceOffset) == !isARM64());
    }
}

static Outlining chooseRandomBodies(unsigned threadCount)
{
    WeakRandom random(1);
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= 300; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        for (unsigned i = 0; i < 12; ++i)
            sketch.plain(1 + random.getUint32(6));
        sketch.end();
        bodies.append(sketch.take());
    }
    Steps steps(WTF::move(bodies), { .threadCount = threadCount });
    steps.choose();
    return WTF::move(steps.outlining);
}

static void testThreads()
{
    Outlining one = chooseRandomBodies(1);
    Outlining eight = chooseRandomBodies(8);
    AOT_TEST_CHECK(!one.bodies.isEmpty());
    AOT_TEST_CHECK(std::ranges::equal(one.bodies, eight.bodies, [](const OutlinedBody& a, const OutlinedBody& b) {
        return a.function == b.function && a.offset == b.offset && a.length == b.length && a.numberOfSites == b.numberOfSites && a.singleCaller == b.singleCaller && a.endsInCall == b.endsInCall;
    }));
    AOT_TEST_CHECK(std::ranges::equal(one.sites, eight.sites, [](const Vector<OutlinedSite>& first, const Vector<OutlinedSite>& second) {
        return std::ranges::equal(first, second, [](const OutlinedSite& a, const OutlinedSite& b) {
            return a.offset == b.offset && a.length == b.length && a.body == b.body;
        });
    }));
}

std::span<const NativeTest> outliningTests()
{
    static constexpr NativeTest tests[] = {
        { "image/outliner/shared-sequence"_s, testSharedSequence },
        { "image/outliner/single-caller"_s, testSingleCaller },
        { "image/outliner/single-caller-pays-for-one-copy"_s, testSingleCallerPaysForOneCopy },
        { "image/outliner/shared-body-pays-for-every-copy"_s, testSharedBodyPaysForEveryCopy },
        { "image/outliner/ends-in-call"_s, testEndsInCall },
        { "image/outliner/starts-at-target"_s, testStartsAtTarget },
        { "image/outliner/plain-part-of-body"_s, testPlainPartOfBody },
        { "image/outliner/pinned-bytes-repeat"_s, testPinnedBytesRepeat },
        { "image/outliner/no-saving"_s, testNoSaving },
        { "image/outliner/few-sites"_s, testFewSites },
        { "image/outliner/target-in-middle"_s, testTargetInMiddle },
        { "image/outliner/handler-in-middle"_s, testHandlerInMiddle },
        { "image/outliner/jump-table-target-in-middle"_s, testJumpTableTargetInMiddle },
        { "image/outliner/resume-point-in-middle"_s, testResumePointInMiddle },
        { "image/outliner/frame-range-end-in-middle"_s, testFrameRangeEndInMiddle },
        { "image/outliner/alias-start-in-middle"_s, testAliasStartInMiddle },
        { "image/outliner/alias-entry-in-middle"_s, testAliasEntryInMiddle },
        { "image/outliner/pinned-in-middle"_s, testPinnedInMiddle },
        { "image/outliner/records-pin-units"_s, testRecordsPinUnits },
        { "image/outliner/other-keys-at-end"_s, testOtherKeysAtEnd },
        { "image/outliner/other-moves-at-end"_s, testOtherMovesAtEnd },
        { "image/outliner/prologue-check-call"_s, testPrologueCheckCall },
        { "image/outliner/call-of-regexp"_s, testCallOfRegExp },
        { "image/outliner/tail-call"_s, testTailCall },
        { "image/outliner/frameless"_s, testFrameless },
        { "image/outliner/outside-frame-range"_s, testOutsideFrameRange },
        { "image/outliner/threads"_s, testThreads },
#if CPU(X86_64)
        { "image/outliner/calls-of-one-body-match"_s, testCallsOfOneBodyMatch },
        { "image/outliner/small-piece-where-code-may-run"_s, testSmallPieceWhereCodeMayRun },
        { "image/outliner/call-of-inner-entry"_s, testCallOfInnerEntry },
#endif
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
