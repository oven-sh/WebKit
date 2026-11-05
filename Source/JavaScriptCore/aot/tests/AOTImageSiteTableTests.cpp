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
    sketch.plain(columnDelta);
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
    sketch.code().frame.prologueChecks.append(PrologueCheck { function, call, resumeOffset });
    return call;
}

static PrologueBody bodyWithPrologueCheck(uint32_t function, bool hasOrdinaryCall)
{
    PrologueBody body { };
    CodeSketch sketch(function);
    sketch.withFrame();
    body.resumeOffset = sketch.plain(1);
    if (hasOrdinaryCall)
        body.ordinaryReturnOffset = returnOffsetOf(sketch.callStub(writeBarrier(), sketch.addSite()));
    sketch.end();
    body.callOffset = appendPrologueCheck(sketch, function, body.resumeOffset);
    body.code = sketch.take();
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

static void checkBodyCallSites(SiteBody&& body)
{
    BodiesImage built(2, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
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

static void testCallSitesInlineFrameOfBuiltin()
{
    const std::array<InlineFrameShape, 3> frames { { { 0, 2, false, false }, { 1, 2, true, false, BuiltinID(300) }, { 2, 3, false, false } } };
    const std::array<unsigned, 4> framesByCall { 0, 1, 3, 1 };
    SiteBody body = bodyWithInlineFrames(1, 0, frames, framesByCall);
    Vector<SitePosition> positions(body.code.sites.positions.span());
    BodiesImage built(5, bodies(WTF::move(body.code)));
    checkCallSites(built.image, 1, body.sites);
    checkPositionsOf(built, 1, positions);
    InlineFrameReader reader;
    for (unsigned frame : { 3u, 2u, 1u })
        checkInlineFrame(reader, built, frame, frames[frame - 1], frame);
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
        { "image/call-sites/reads-without-sites"_s, testCallSitesReadsWithoutSites },
        { "image/call-sites/inline-frames"_s, testCallSitesInlineFrames },
        { "image/call-sites/inline-frame-of-a-built-in"_s, testCallSitesInlineFrameOfBuiltin },
        { "image/call-sites/two-prologue-checks"_s, testCallSitesTwoPrologueChecks },
        { "image/call-sites/prologue-check-elsewhere"_s, testCallSitesPrologueCheckElsewhere },
        { "image/positions/random"_s, [] { checkEncodedPositions({ randomPositions(2000) }); } },
        { "image/positions/obfuscated"_s, testPositionsObfuscated },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
