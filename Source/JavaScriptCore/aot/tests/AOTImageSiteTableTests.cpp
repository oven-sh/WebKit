/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageSiteTestBodies.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPolls.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <wtf/WeakRandom.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

namespace {

struct PrologueBody {
    CompiledCode code;
    uint32_t callOffset;
    uint32_t resumeOffset;
    std::optional<uint32_t> ordinaryReturnOffset;
};

} // anonymous namespace

static void appendCallWithTwoReads(CodeSketch& sketch, Vector<ExpectedSite>& sites)
{
    sketch.plain(0);
    OptionalIndex firstRead = sketch.addSite();
    OptionalIndex secondRead = sketch.addSite();
    uint32_t call = sketch.callStub(writeBarrier(), secondRead);
    sketch.code().body.firstReadSites.append(FirstReadSite { returnOffsetOf(call), *firstRead });
    ExpectedSite expected { returnOffsetOf(call), *secondRead };
    expected.firstReadSite = *firstRead;
    sites.append(WTF::move(expected));
}

static void appendCallWithReadWithoutSite(CodeSketch& sketch, Vector<ExpectedSite>& sites, uint32_t columnDelta)
{
    sketch.pinned(columnDelta);
    OptionalIndex site = sketch.addSite();
    uint32_t call = sketch.callStub(writeBarrier(), site);
    sketch.code().body.firstReadSites.append(FirstReadSite { returnOffsetOf(call), 0, columnDelta });
    ExpectedSite expected { returnOffsetOf(call), *site };
    if (columnDelta)
        expected.firstReadSite = firstReadSiteInFrontOf(CallSiteIndex(*site), columnDelta).bits();
    sites.append(WTF::move(expected));
}

static uint32_t appendPrologueCheck(CodeSketch& sketch, uint32_t function, uint32_t resumeOffset)
{
    uint32_t call = sketch.callStub(pollAtPrologue(true));
    changeUnits(sketch.code().body, [](Vector<CodeUnit>& units) {
        units.last().kind = CodeUnit::Kind::Pinned;
    });
    sketch.code().frame.prologueChecks.append(PrologueCheck { function, call, resumeOffset });
    return call;
}

static CodeSketch sketchWithPrologueCheck(uint32_t function, bool hasOrdinaryCall, PrologueBody& body)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    body.resumeOffset = sketch.plain(1);
    if (hasOrdinaryCall)
        body.ordinaryReturnOffset = returnOffsetOf(sketch.callStub(writeBarrier(), sketch.addSite()));
    sketch.end();
    body.callOffset = appendPrologueCheck(sketch, function, body.resumeOffset);
    return sketch;
}

static PrologueBody bodyWithPrologueCheck(uint32_t function, bool hasOrdinaryCall)
{
    PrologueBody body { };
    body.code = sketchWithPrologueCheck(function, hasOrdinaryCall, body).take();
    return body;
}

static void checkCallSites(const TestImage& image, uint32_t function, const Vector<ExpectedSite>& sites)
{
    const Image& table = *image.image;
    uint32_t index = image.indexOf(function);
    uint32_t unit = static_cast<uint32_t>(codeOffsetUnit);
    auto isExpectedAt = [&](uint32_t offset) {
        return std::ranges::any_of(sites, [&](const ExpectedSite& site) {
            return site.returnOffset == offset;
        });
    };
    // An address without a site of its own has the site in front of it, unless that one says more than where it is.
    auto checkSiteInFrontOf = [&](uint32_t offset) {
        const ExpectedSite* inFront = nullptr;
        for (const ExpectedSite& site : sites) {
            if (site.returnOffset < offset && (!inFront || site.returnOffset > inFront->returnOffset))
                inFront = &site;
        }
        std::optional<CallSiteRecord> record = table.callSiteAt(index, offset);
        bool rowHoldsFirstRead = inFront && inFront->firstReadSite && *inFront->firstReadSite >> siteNumberBits;
        if (!inFront || inFront->isConstruct || inFront->isTailCall || inFront->calledBuiltin || rowHoldsFirstRead) {
            AOT_TEST_CHECK(!record);
            return;
        }
        AOT_TEST_CHECK(record && record->callSite.bits() == inFront->number && record->inlineFrame == inFront->inlineFrame);
        AOT_TEST_CHECK(record->kind == CallSiteKind::Plain && !record->calledBuiltin && !record->firstReadSite);
    };
    for (const ExpectedSite& site : sites) {
        std::optional<CallSiteRecord> record = table.callSiteAt(index, site.returnOffset);
        AOT_TEST_CHECK(record);
        AOT_TEST_CHECK(record->callSite.bits() == site.number);
        AOT_TEST_CHECK(record->inlineFrame == site.inlineFrame);
        AOT_TEST_CHECK(record->isConstruct() == site.isConstruct);
        AOT_TEST_CHECK(record->isTailCall() == site.isTailCall);
        AOT_TEST_CHECK(record->isInsideCallee() == site.isInsideCallee);
        AOT_TEST_CHECK(record->calledBuiltin == site.calledBuiltin);
        std::optional<uint32_t> firstReadSite;
        if (record->firstReadSite)
            firstReadSite = record->firstReadSite->bits();
        AOT_TEST_CHECK(firstReadSite == site.firstReadSite);
        for (uint32_t neighbor : { site.returnOffset - unit, site.returnOffset + unit }) {
            if (!isExpectedAt(neighbor))
                checkSiteInFrontOf(neighbor);
        }
    }
    AOT_TEST_CHECK(!table.callSiteAt(index, 0));
    checkSiteInFrontOf(static_cast<uint32_t>(table.sizeOfCodeOf(index)));
}

// Calls in a row that say the same have one site, which stands at the first of them.
static void testCallSitesReturnAddressesShareASite()
{
    constexpr unsigned numberOfSites = 200;
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    Vector<ExpectedSite> sites;
    Vector<std::pair<uint32_t, uint32_t>> sharing;
    for (unsigned i = 0; i < numberOfSites; ++i) {
        appendCall(sketch, sites, i);
        for (unsigned follower = 0; follower < i % 3; ++follower) {
            sketch.pinned(0x4000 + 4 * i + follower);
            sharing.append({ returnOffsetOf(sketch.callStub(writeBarrier(), OptionalIndex { sites.last().number })), sites.last().number });
        }
    }
    SiteBody body = finishBody(sketch, WTF::move(sites));
    size_t numberOfPositions = body.code.sites.positions.size();
    BodiesImage built(2, bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(numberOfPositions == numberOfSites + 1);
    checkCallSites(built.image, 1, body.sites);
    uint32_t index = built.image.indexOf(1);
    for (auto [returnOffset, number] : sharing) {
        std::optional<CallSiteRecord> record = built.table().callSiteAt(index, returnOffset);
        AOT_TEST_CHECK(record && record->callSite.bits() == number);
    }
}

static void checkBodyCallSites(SiteBody&& body)
{
    BodiesImage built(2, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
}

static const uint8_t* callSitesOf(const TestImage& image, uint32_t function)
{
    const ImageFunction& record = image.image->function(image.indexOf(function));
    if (!record.has(ImageFunctionFlag::HasCallSites))
        return nullptr;
    return SideTableParts::read(record, image.image->section<uint8_t>(ImageSection::SideTables).data()).callSites;
}

static void testCallSitesCounts()
{
    constexpr std::array<unsigned, 7> counts { 1, 63, 64, 65, 128, 129, 1000 };
    Vector<CompiledCode> codes;
    Vector<Vector<ExpectedSite>> expectedSitesByBody;
    for (uint32_t i = 0; i < counts.size(); ++i) {
        SiteBody body = bodyWithCalls(i + 1, counts[i]);
        codes.append(WTF::move(body.code));
        expectedSitesByBody.append(WTF::move(body.sites));
    }
    BodiesImage built(static_cast<unsigned>(counts.size()) + 1, WTF::move(codes));
    for (uint32_t i = 0; i < counts.size(); ++i)
        checkCallSites(built.image, i + 1, expectedSitesByBody[i]);
}

static void testCallSitesNoStream()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    sketch.end();
    uint32_t size = sketch.size();
    BodiesImage built(2, bodies(sketch.take()));
    uint32_t index = built.image.indexOf(1);
    AOT_TEST_CHECK(!built.table().function(index).has(ImageFunctionFlag::HasCallSites));
    for (uint32_t offset = 0; offset <= size; ++offset)
        AOT_TEST_CHECK(!built.table().callSiteAt(index, offset));
}

static void testCallSitesFlags()
{
    SiteBody body = bodyWithCalls(1, 8);
    Vector<FlaggedSite>& flaggedSites = body.code.sites.flaggedSites;
    flaggedSites.append(FlaggedSite { 2, CallSiteKind::Construct });
    flaggedSites.append(FlaggedSite { 3, CallSiteKind::TailCall });
    flaggedSites.append(FlaggedSite { 4, CallSiteKind::TailConstruct });
    flaggedSites.append(FlaggedSite { 5, CallSiteKind::InsideCallee });
    flaggedSites.append(FlaggedSite { 6, CallSiteKind::InsideBuiltin, BuiltinID(0) });
    flaggedSites.append(FlaggedSite { 7, CallSiteKind::InsideBuiltinCallee, BuiltinID(300) });
    body.sites[1].isConstruct = true;
    body.sites[2].isTailCall = true;
    body.sites[3].isConstruct = true;
    body.sites[3].isTailCall = true;
    body.sites[4].isTailCall = true;
    body.sites[4].isInsideCallee = true;
    body.sites[5].calledBuiltin = BuiltinID(0);
    body.sites[6].isTailCall = true;
    body.sites[6].isInsideCallee = true;
    body.sites[6].calledBuiltin = BuiltinID(300);
    checkBodyCallSites(WTF::move(body));
}

static void testCallSitesFlagWithoutCode()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    sketch.addSite();
    sketch.code().sites.numberOfSitesWithoutCode = 1;
    sketch.code().sites.flaggedSites.append(FlaggedSite { 1, CallSiteKind::Construct });
    Vector<ExpectedSite> expected;
    appendCall(sketch, expected, 0);
    AOT_TEST_CHECK(expected[0].number == 2);
    checkBodyCallSites(finishBody(sketch, WTF::move(expected)));
}

static void testCallSitesTwoReads()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    Vector<ExpectedSite> expected;
    appendCallWithTwoReads(sketch, expected);
    AOT_TEST_CHECK(expected[0].number == 2);
    AOT_TEST_CHECK(expected[0].firstReadSite == 1u);
    checkBodyCallSites(finishBody(sketch, WTF::move(expected)));
}

static void testCallSitesTwoReadsAcrossCheckpoint()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    Vector<ExpectedSite> expected;
    for (unsigned i = 0; i < 63; ++i)
        appendCall(sketch, expected, i);
    appendCallWithTwoReads(sketch, expected);
    AOT_TEST_CHECK(expected[63].number == 65);
    AOT_TEST_CHECK(expected[63].firstReadSite == 64u);
    for (unsigned i = 0; i < 10; ++i)
        appendCall(sketch, expected, i);
    checkBodyCallSites(finishBody(sketch, WTF::move(expected)));
}

static void testCallSitesReadsWithoutSites()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    Vector<ExpectedSite> expected;
    for (uint32_t columnDelta : { 1u, 0u, 7u, 8u, 63u, 64u, maxFirstReadColumnDelta }) {
        appendCallWithReadWithoutSite(sketch, expected, columnDelta);
        appendCall(sketch, expected, columnDelta);
        appendCallWithTwoReads(sketch, expected);
    }
    for (unsigned i = 0; i < 130; ++i)
        appendCallWithReadWithoutSite(sketch, expected, 1 + i % 20);
    checkBodyCallSites(finishBody(sketch, WTF::move(expected)));
}

static void checkPositionsOf(const BodiesImage& built, uint32_t function, const Vector<SitePosition>& positions)
{
    uint32_t index = built.image.indexOf(function);
    uint32_t count = static_cast<uint32_t>(positions.size());
    for (uint32_t k = 0; k < count; ++k) {
        AOT_TEST_CHECK(built.table().positionOf(index, CallSiteIndex(k)) == sourcePositionOf(positions[k]));
        SourcePosition oneColumnInFront { { positions[k].line, positions[k].column - 1 }, positions[k].source };
        AOT_TEST_CHECK(built.table().positionOf(index, firstReadSiteInFrontOf(CallSiteIndex(k), 1)) == oneColumnInFront);
    }
    AOT_TEST_CHECK(!built.table().positionOf(index, CallSiteIndex(count)));
    AOT_TEST_CHECK(!built.table().positionOf(index, firstReadSiteInFrontOf(CallSiteIndex(count), 1)));
}

static void checkInlineFrame(InlineFrameReader& reader, const BodiesImage& built, unsigned frame, const InlineFrameShape& shape, uint32_t callSite, uint32_t function = 1)
{
    const InlineFrameRecord& record = reader.at(built.table(), built.image.indexOf(function), frame);
    AOT_TEST_CHECK(record.parent == shape.parent);
    AOT_TEST_CHECK(record.callSite == CallSiteIndex(callSite));
    AOT_TEST_CHECK(record.function == (shape.calledBuiltin ? functionInFramesOf(shape.calledBuiltin, built.table().programTables().numberOfFunctions()) : FunctionID(shape.callee)));
    AOT_TEST_CHECK(record.isConstruct == shape.isConstruct);
    AOT_TEST_CHECK(record.isTailCall == shape.isTailCall);
}

static void testCallSitesInlineFrames()
{
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall);
    BodiesImage built(5, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
    AOT_TEST_CHECK(built.table().function(built.image.indexOf(1)).has(ImageFunctionFlag::HasInlineFrames));
    InlineFrameReader reader;
    for (unsigned frame : { 3u, 1u, 2u })
        checkInlineFrame(reader, built, frame, chainedFrames[frame - 1], 1 + frame);
}

static void testCallSitesInlineFramesOfTwoFunctionsInTurn()
{
    const std::array<unsigned, 3> siblingFramesByCall { 0, 1, 2 };
    BodiesImage built(5, bodies(bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall).code, bodyWithInlineFrames(4, 0, siblingFrames, siblingFramesByCall).code));
    auto readBothThrice = [&](InlineFrameReader& reader) {
        for (unsigned turn = 0; turn < 3; ++turn) {
            for (unsigned frame : { 3u, 1u, 2u })
                checkInlineFrame(reader, built, frame, chainedFrames[frame - 1], 1 + frame);
            for (unsigned frame : { 2u, 1u })
                checkInlineFrame(reader, built, frame, siblingFrames[frame - 1], frame, 4);
        }
    };
    InlineFrameReader remembersOne;
    readBothThrice(remembersOne);

    DecodedInlineFrames decoded;
    InlineFrameReader reader(decoded);
    readBothThrice(reader);
    AOT_TEST_CHECK(decoded.numberOfFramesDecoded() == 5);

    DecodedInlineFrames decodedOutwards;
    InlineFrameReader readsOutwards(decodedOutwards);
    for (unsigned turn = 0; turn < 2; ++turn) {
        for (unsigned frame : { 1u, 2u, 3u }) {
            checkInlineFrame(readsOutwards, built, frame, chainedFrames[frame - 1], 1 + frame);
            checkInlineFrame(readsOutwards, built, 1, siblingFrames[0], 1, 4);
        }
    }
    AOT_TEST_CHECK(decodedOutwards.numberOfFramesDecoded() == 4);
}

static void testCallSitesInlineFrameOfBuiltin()
{
    const std::array<InlineFrameShape, 3> frames { { { 0, 2, false, false }, { 1, 2, true, false, BuiltinID(300) }, { 2, 3, false, false } } };
    const std::array<unsigned, 4> framesByCall { 0, 1, 3, 1 };
    SiteBody body = bodyWithInlineFrames(1, 0, frames, framesByCall);
    Vector<SitePosition> positions = body.code.sites.positions;
    BodiesImage built(5, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
    checkPositionsOf(built, 1, positions);
    InlineFrameReader reader;
    for (unsigned frame : { 3u, 2u, 1u })
        checkInlineFrame(reader, built, frame, frames[frame - 1], frame);
}

static void testCallSitesManyInlineFrames()
{
    constexpr unsigned numberOfFrames = 300;
    constexpr unsigned numberOfSitesInDeepFrame = 70;
    Vector<InlineFrameShape> frames;
    for (unsigned frame = 1; frame <= numberOfFrames; ++frame)
        frames.append(InlineFrameShape { frame - 1, 2 + frame % 3, false, !(frame % 7) });
    Vector<unsigned> framesByCall(FillWith { }, numberOfSitesInDeepFrame, numberOfFrames - 1);
    framesByCall.appendList({ numberOfFrames, 0u, 256u, numberOfFrames - 1 });

    SiteBody body = bodyWithInlineFrames(1, 0, frames.span(), framesByCall.span());
    Vector<SitePosition> positions = body.code.sites.positions;
    BodiesImage built(5, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
    checkPositionsOf(built, 1, positions);
    InlineFrameReader reader;
    for (unsigned frame : { numberOfFrames, 1u, 255u, 256u, numberOfFrames - 1 })
        checkInlineFrame(reader, built, frame, frames[frame - 1], frame);
    AOT_TEST_CHECK(built.build.statistic(StatisticID::LargestInlineFrameCountInFunction) == numberOfFrames);

    InlineFrameReader readsFromTheFirst;
    DecodedInlineFrames decoded;
    InlineFrameReader keepsFromTheFirst(decoded);
    for (unsigned frame = 1; frame <= numberOfFrames; ++frame) {
        checkInlineFrame(readsFromTheFirst, built, frame, frames[frame - 1], frame);
        checkInlineFrame(keepsFromTheFirst, built, frame, frames[frame - 1], frame);
        checkInlineFrame(keepsFromTheFirst, built, 1 + frame / 2, frames[frame / 2], 1 + frame / 2);
        AOT_TEST_CHECK(decoded.numberOfFramesDecoded() == frame);
    }
}

static void testCallSitesSiteFrames()
{
    BodiesImage built(5, bodies(bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall).code));
    uint32_t index = built.image.indexOf(1);
    Vector<unsigned> frames { 0, 0, 1, 2 };
    frames.append(std::span<const unsigned> { chainedFramesByCall });
    frames.append(0);

    SiteFrames reader(built.table(), index);
    for (unsigned frame : frames)
        AOT_TEST_CHECK(reader.next() == frame);

    for (unsigned skipped : { 0u, 1u, 5u, 16u }) {
        SiteFrames skipping(built.table(), index);
        skipping.skip(skipped);
        for (size_t i = skipped; i < frames.size(); ++i)
            AOT_TEST_CHECK(skipping.next() == frames[i]);
    }
}

static void testCallSitesSiteFramesWithoutCode()
{
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, { });
    SitePosition third = body.code.sites.positions[3];
    BodiesImage built(5, bodies(WTF::move(body.code)));
    uint32_t index = built.image.indexOf(1);
    AOT_TEST_CHECK(!built.table().function(index).has(ImageFunctionFlag::HasInlineFrames));
    SiteFrames reader(built.table(), index);
    for (unsigned i = 0; i < 10; ++i)
        AOT_TEST_CHECK(!reader.next());
    AOT_TEST_CHECK(built.table().positionOf(index, CallSiteIndex(3)) == sourcePositionOf(third));
}

static void testCallSitesFaultAndPollSites()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    SiteDescription& sites = sketch.code().sites;
    Vector<ExpectedSite> expected;

    uint32_t access = sketch.pinned(1, 4);
    OptionalIndex accessSite = sketch.addSite();
    sites.faultingAccesses.append(FaultingAccess { access, accessSite, { 8 } });
    expected.append(ExpectedSite { returnOffsetOf(access), *accessSite });

    appendCall(sketch, expected, 0);

    if constexpr (hasFaultingLoopPolls) {
        uint32_t poll = sketch.pinned(2, 4);
        OptionalIndex pollSite = sketch.addSite();
        sites.pollSites.append(PollSite { poll, false, pollSite });
        expected.append(ExpectedSite { poll + static_cast<uint32_t>(sizeOfLoopPoll), *pollSite });
    }
    checkBodyCallSites(finishBody(sketch, WTF::move(expected)));
}

static void testCallSitesOutlinedCall()
{
    constexpr uint32_t numberOfBodies = 40;
    Vector<CompiledCode> codes;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch(function);
        sketch.withFrame();
        sketch.plain(0x1000 + function);
        for (uint32_t word = 1; word <= 4; ++word)
            sketch.plain(word);
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.end();
        codes.append(sketch.take());
    }
    BodiesImage built(numberOfBodies + 1, WTF::move(codes));

    AOT_TEST_CHECK(built.image.built.outlining.bodies.size() == 1);
    AOT_TEST_CHECK(built.image.built.outlining.bodies[0].endsInCall);
    const LinkedFunctions& functions = built.image.built.functions;
    AOT_TEST_CHECK(functions.size() == numberOfBodies);
    for (uint32_t position = 0; position < functions.size(); ++position) {
        const CodeBody& body = functions[position].body;
        AOT_TEST_CHECK(body.relocations.size() == 1);
        AOT_TEST_CHECK(body.relocations[0].space == SymbolSpace::OutlinedBody);
        AOT_TEST_CHECK(body.returnSites.size() == 1);
        AOT_TEST_CHECK(body.returnSites[0].offset == returnOffsetOf(body.relocations[0].offset));
        std::optional<CallSiteRecord> record = built.table().callSiteAt(position, body.returnSites[0].offset);
        AOT_TEST_CHECK(record);
        AOT_TEST_CHECK(record->callSite.bits() == 1);
    }
}

static void testCallSitesEqualStreamsShare()
{
    BodiesImage built(4, bodies(bodyWithCalls(1, 20).code, bodyWithCalls(2, 20).code, bodyWithCalls(3, 21).code));
    AOT_TEST_CHECK(callSitesOf(built.image, 1));
    AOT_TEST_CHECK(callSitesOf(built.image, 1) == callSitesOf(built.image, 2));
    AOT_TEST_CHECK(callSitesOf(built.image, 3));
    AOT_TEST_CHECK(callSitesOf(built.image, 3) != callSitesOf(built.image, 1));
}

static void checkPrologueCheck(bool hasOrdinaryCall)
{
    PrologueBody body = bodyWithPrologueCheck(1, hasOrdinaryCall);
    BodiesImage built(3, bodies(WTF::move(body.code), bodyWithCalls(2, 1).code));
    uint32_t index = built.image.indexOf(1);
    PrologueCheck expected { index, body.callOffset, body.resumeOffset };
    uint32_t returnOffset = returnOffsetOf(body.callOffset);
    AOT_TEST_CHECK(callSitesOf(built.image, 1));
    AOT_TEST_CHECK(built.table().prologueCheckAt(built.image.address(1, returnOffset)) == expected);
    AOT_TEST_CHECK(!built.table().callSiteAt(index, returnOffset));
}

static void testCallSitesTwoPrologueChecks()
{
    CodeSketch sketch(1);
    sketch.withFrame();
    uint32_t firstResume = sketch.plain(1);
    uint32_t secondResume = sketch.plain(2);
    uint32_t ordinaryReturnOffset = returnOffsetOf(sketch.callStub(writeBarrier(), sketch.addSite()));
    sketch.end();
    uint32_t firstCall = appendPrologueCheck(sketch, 1, firstResume);
    uint32_t secondCall = appendPrologueCheck(sketch, 1, secondResume);

    BodiesImage built(2, bodies(sketch.take()));
    const Image& table = built.table();
    uint32_t index = built.image.indexOf(1);
    AOT_TEST_CHECK(table.prologueCheckAt(built.image.address(1, returnOffsetOf(firstCall))) == PrologueCheck({ index, firstCall, firstResume }));
    AOT_TEST_CHECK(table.prologueCheckAt(built.image.address(1, returnOffsetOf(secondCall))) == PrologueCheck({ index, secondCall, secondResume }));
    AOT_TEST_CHECK(!table.prologueCheckAt(built.image.address(1, ordinaryReturnOffset)));
    AOT_TEST_CHECK(table.callSiteAt(index, ordinaryReturnOffset));
    AOT_TEST_CHECK(!table.callSiteAt(index, returnOffsetOf(firstCall)));
    AOT_TEST_CHECK(!table.callSiteAt(index, returnOffsetOf(secondCall)));
}

static void testCallSitesPrologueCheckElsewhere()
{
    PrologueBody body = bodyWithPrologueCheck(1, true);
    SiteBody other = bodyWithCalls(2, 1);
    BodiesImage built(3, bodies(WTF::move(body.code), WTF::move(other.code)));
    const Image& table = built.table();
    AOT_TEST_CHECK(!table.prologueCheckAt(built.image.address(1, *body.ordinaryReturnOffset)));
    AOT_TEST_CHECK(!table.prologueCheckAt(built.image.address(2, other.sites[0].returnOffset)));
    const void* stub = table.stubAddress(catchException());
    AOT_TEST_CHECK(stub);
    AOT_TEST_CHECK(!table.prologueCheckAt(stub));
    AOT_TEST_CHECK(!table.prologueCheckAt(table.code().data()));
}

static void testEndOfLastReturnSite()
{
    PrologueBody withOrdinaryCall = bodyWithPrologueCheck(1, true);
    AOT_TEST_CHECK(withOrdinaryCall.code.body.returnSites.last().offset == returnOffsetOf(withOrdinaryCall.callOffset));
    AOT_TEST_CHECK(endOfLastReturnSite(withOrdinaryCall.code) == withOrdinaryCall.ordinaryReturnOffset);
    AOT_TEST_CHECK(!endOfLastReturnSite(bodyWithPrologueCheck(1, false).code));

    SiteBody calls = bodyWithCalls(1, 3);
    AOT_TEST_CHECK(endOfLastReturnSite(calls.code) == calls.sites.last().returnOffset);
}

static CompiledCode trimmed(CompiledCode&& code)
{
    TestBuild build(2);
    LinkedFunctions functions = createLinkedFunctions(bodies(WTF::move(code)), build.context());
    Vector<EntryAlias> aliases;
    removeJumpOnlyFunctions(functions, aliases, testTemplates(), build.context());
    AOT_TEST_CHECK(functions.size() == 1);
    AOT_TEST_CHECK(aliases.isEmpty());
    return WTF::move(functions[0]);
}

static void testTrimToPrologueCheck()
{
    PrologueBody body { };
    CodeSketch sketch = sketchWithPrologueCheck(1, true, body);
    sketch.padding(4 * sizeof(uint32_t));
    CompiledCode code = trimmed(sketch.take());
    AOT_TEST_CHECK(code.body.bytes.size() == returnOffsetOf(body.callOffset));
    AOT_TEST_CHECK(code.body.paddingStart == code.body.bytes.size());
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}

static void testTrimKeepsBreakpointAfterCall()
{
    const Vector<uint8_t>& breakpoint = testTemplates().breakpoint;
    CodeSketch withoutPadding(1);
    startFramedBody(withoutPadding, 1);
    uint32_t returnOffset = returnOffsetOf(withoutPadding.callStub(writeBarrier(), withoutPadding.addSite()));
    CompiledCode code = trimmed(withoutPadding.takeUnchecked());
    AOT_TEST_CHECK(code.body.bytes.size() == returnOffset + breakpoint.size());
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.span().subspan(returnOffset), breakpoint.span()));
    AOT_TEST_CHECK(code.body.paddingStart == code.body.bytes.size());
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);

    CodeSketch withPadding(1);
    startFramedBody(withPadding, 1);
    withPadding.callStub(writeBarrier(), withPadding.addSite());
    withPadding.padding(4 * sizeof(uint32_t));
    AOT_TEST_CHECK(trimmed(withPadding.take()).body.bytes.size() == returnOffset + codeOffsetUnit);
}

static void checkPositions(const Image& image, uint32_t imageFunction, const uint8_t* stream, OwnPosition ofFunctionInFront, const Vector<SitePosition>& positions)
{
    uint32_t count = static_cast<uint32_t>(positions.size());
    for (uint32_t k = 0; k < count; ++k)
        AOT_TEST_CHECK(decodePosition(image, imageFunction, stream, ofFunctionInFront, CallSiteIndex(k)) == sourcePositionOf(positions[k]));
    AOT_TEST_CHECK(!decodePosition(image, imageFunction, stream, ofFunctionInFront, CallSiteIndex(count)));
    AOT_TEST_CHECK(!decodePosition(image, imageFunction, stream, ofFunctionInFront, CallSiteIndex(std::numeric_limits<uint32_t>::max())));
}

static void checkEncodedPositions(std::initializer_list<Vector<SitePosition>> tables)
{
    BodiesImage any(2, bodies(bodyWithCalls(1, 1).code));
    for (const Vector<SitePosition>& positions : tables) {
        SiteDescription sites;
        sites.positions = positions;
        for (OwnPosition ofFunctionInFront : { OwnPosition { }, OwnPosition::of(sites), OwnPosition { 0xffffffff, OwnPosition::of(sites).source }, OwnPosition { 7, 0xffffffff }, OwnPosition { 0, 1 } }) {
            Vector<uint8_t> stream;
            encodePositions(stream, sites, nullptr, ofFunctionInFront);
            checkPositions(any.table(), 0, stream.span().data(), ofFunctionInFront, positions);
        }
    }
}

static void checkBodyPositions(SiteBody&& body, unsigned numberOfFunctions)
{
    Vector<SitePosition> positions = body.code.sites.positions;
    BodiesImage built(numberOfFunctions, bodies(WTF::move(body.code)));
    checkPositionsOf(built, 1, positions);
}

static Vector<SitePosition> countedPositions(uint32_t count)
{
    Vector<SitePosition> positions;
    for (uint32_t k = 0; k < count; ++k)
        positions.append(positionAt(10 + k / 3, 1 + k % 40));
    return positions;
}

static Vector<SitePosition> positionsWithCyclingModules(uint32_t remainderWithoutModule)
{
    Vector<SitePosition> positions;
    for (uint32_t k = 0; k < 30; ++k)
        positions.append(positionAt(1 + k, 2 + k, k % 7 == remainderWithoutModule ? ModuleID() : ModuleID(k % 3)));
    return positions;
}

static Vector<SitePosition> randomPositions(uint32_t count)
{
    WeakRandom random(1);
    Vector<SitePosition> positions;
    uint32_t line = 1;
    uint32_t column = 1;
    for (uint32_t k = 0; k < count; ++k) {
        if (!random.getUint32(4)) {
            line = random.getUint32();
            column = random.getUint32();
        } else {
            line += random.getUint32(16) - 8;
            column += random.getUint32(16) - 8;
        }
        uint32_t module = random.getUint32(5);
        positions.append(positionAt(line, column, module < 4 ? ModuleID(module) : ModuleID()));
    }
    return positions;
}

static void testPositionsCounts()
{
    checkEncodedPositions({ countedPositions(2), countedPositions(511), countedPositions(512), countedPositions(513), countedPositions(1024), countedPositions(1025), countedPositions(1500) });
}

static void testPositionsEntrySizes()
{
    auto sizeOfEntry = [](Vector<SitePosition>&& positions, OwnPosition ofFunctionInFront) {
        SiteDescription sites;
        sites.positions = WTF::move(positions);
        Vector<uint8_t> stream;
        encodePositions(stream, sites, nullptr, ofFunctionInFront);
        return stream.size();
    };
    AOT_TEST_CHECK(sizeOfEntry({ positionAt(1000, 5, ModuleID(20000u)) }, { 990, 20001 }) == 3);
    AOT_TEST_CHECK(sizeOfEntry({ positionAt(1000, 5, ModuleID(20000u)) }, { 1010, 20001 }) == 3);
    AOT_TEST_CHECK(sizeOfEntry({ positionAt(1000, 5, ModuleID(20000u)) }, { 990, 20000 }) == 5);
    AOT_TEST_CHECK(sizeOfEntry({ positionAt(1000, 5, ModuleID(20000u)) }, { }) == 7);
    AOT_TEST_CHECK(sizeOfEntry({ positionAt(1000, 5, ModuleID(20000u)), positionAt(1000, 9, ModuleID(20000u)), positionAt(1001, 7, ModuleID(20000u)) }, { 990, 20001 }) == 6);
}

static void testPositionsAlternatingFrames()
{
    Vector<unsigned> frames;
    for (unsigned i = 0; i < 1200; ++i)
        frames.append(1 + i % 2);
    checkBodyPositions(bodyWithInlineFrames(1, 0, siblingFrames, frames.span()), 4);
}

static void testPositionsSuspensionPositions()
{
    constexpr std::array<unsigned, 4> frames { 1, 2, 0, 2 };
    checkBodyPositions(bodyWithInlineFrames(1, 5, siblingFrames, frames), 4);
}

static void testPositionsOwnPosition()
{
    SiteBody body = bodyWithCalls(1, 1);
    SitePosition own = body.code.sites.positions[0];
    BodiesImage built(2, bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(!built.table().isObfuscated());
    AOT_TEST_CHECK(built.table().positionOf(built.image.indexOf(1), CallSiteIndex(0)) == sourcePositionOf(own));
}

// The place of a function's positions is written down for every 32nd function. The others are found from there.
static void testPositionsOfManyFunctions()
{
    constexpr uint32_t numberOfBodies = 3 * PositionIndex::functionsBetween + 5;
    Vector<CompiledCode> codes;
    Vector<Vector<SitePosition>> positions;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        SiteBody body = bodyWithCalls(function, function % 7);
        positions.append(body.code.sites.positions);
        codes.append(WTF::move(body.code));
    }
    BodiesImage built(numberOfBodies + 1, WTF::move(codes));
    for (uint32_t function = 1; function <= numberOfBodies; ++function)
        checkPositionsOf(built, function, positions[function - 1]);
}

static void testPositionsObfuscated()
{
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall);
    TestBuild build(5, TestOptions { .output = Wire::OutputOptions::Obfuscate });
    TestImage image = build.build(bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(image.image->isObfuscated());
    uint32_t index = image.indexOf(1);
    AOT_TEST_CHECK(!image.image->function(index).has(ImageFunctionFlag::HasPositions));
    for (uint32_t site : { 0u, 1u, 5u })
        AOT_TEST_CHECK(!image.image->positionOf(index, CallSiteIndex(site)));
    AOT_TEST_CHECK(image.image->function(index).has(ImageFunctionFlag::HasInlineFrames));
    checkCallSites(image, 1, body.sites);
}

std::span<const NativeTest> siteTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/call-sites/counts"_s, testCallSitesCounts },
        { "image/call-sites/no-stream"_s, testCallSitesNoStream },
        { "image/call-sites/return-addresses-share-a-site"_s, testCallSitesReturnAddressesShareASite },
        { "image/call-sites/flags"_s, testCallSitesFlags },
        { "image/call-sites/flag-without-code"_s, testCallSitesFlagWithoutCode },
        { "image/call-sites/two-reads"_s, testCallSitesTwoReads },
        { "image/call-sites/two-reads-across-checkpoint"_s, testCallSitesTwoReadsAcrossCheckpoint },
        { "image/call-sites/reads-without-sites"_s, testCallSitesReadsWithoutSites },
        { "image/call-sites/inline-frames"_s, testCallSitesInlineFrames },
        { "image/call-sites/inline-frame-of-a-built-in"_s, testCallSitesInlineFrameOfBuiltin },
        { "image/call-sites/many-inline-frames"_s, testCallSitesManyInlineFrames },
        { "image/call-sites/inline-frames-of-two-functions-in-turn"_s, testCallSitesInlineFramesOfTwoFunctionsInTurn },
        { "image/call-sites/site-frames"_s, testCallSitesSiteFrames },
        { "image/call-sites/site-frames-without-code"_s, testCallSitesSiteFramesWithoutCode },
        { "image/call-sites/fault-and-poll-sites"_s, testCallSitesFaultAndPollSites },
        { "image/call-sites/outlined-call"_s, testCallSitesOutlinedCall },
        { "image/call-sites/equal-streams-share"_s, testCallSitesEqualStreamsShare },
        { "image/call-sites/prologue-check"_s, [] { checkPrologueCheck(true); } },
        { "image/call-sites/only-prologue-check"_s, [] { checkPrologueCheck(false); } },
        { "image/call-sites/two-prologue-checks"_s, testCallSitesTwoPrologueChecks },
        { "image/call-sites/prologue-check-elsewhere"_s, testCallSitesPrologueCheckElsewhere },
        { "image/call-sites/end-of-last-return-site"_s, testEndOfLastReturnSite },
        { "image/call-sites/trim-to-prologue-check"_s, testTrimToPrologueCheck },
        { "image/call-sites/trim-keeps-breakpoint-after-call"_s, testTrimKeepsBreakpointAfterCall },
        { "image/positions/one"_s, [] { checkEncodedPositions({ countedPositions(1) }); } },
        { "image/positions/counts"_s, testPositionsCounts },
        { "image/positions/line-goes-down"_s, [] { checkEncodedPositions({ { positionAt(100, 5), positionAt(90, 6), positionAt(95, 7), positionAt(1, 8) } }); } },
        { "image/positions/large-numbers"_s, [] { checkEncodedPositions({ { positionAt(0xfffffffe, 0x80000001), positionAt(1, 1), positionAt(2, 3), positionAt(0xfffffffe, 0x80000001), positionAt(3, 7) } }); } },
        { "image/positions/modules"_s, [] { checkEncodedPositions({ positionsWithCyclingModules(6), positionsWithCyclingModules(0) }); } },
        { "image/positions/random"_s, [] { checkEncodedPositions({ randomPositions(2000) }); } },
        { "image/positions/entry-sizes"_s, testPositionsEntrySizes },
        { "image/positions/with-inline-frames"_s, [] { checkBodyPositions(bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall), 5); } },
        { "image/positions/alternating-frames"_s, testPositionsAlternatingFrames },
        { "image/positions/suspension-positions"_s, testPositionsSuspensionPositions },
        { "image/positions/own-position"_s, testPositionsOwnPosition },
        { "image/positions/many-functions"_s, testPositionsOfManyFunctions },
        { "image/positions/obfuscated"_s, testPositionsObfuscated },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
