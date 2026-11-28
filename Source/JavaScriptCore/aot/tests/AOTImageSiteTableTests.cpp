/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageSiteTestBodies.h"
#include "AOTPositionEncoder.h"
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

namespace {

class CodedPositions {
public:
    CodedPositions(const SiteDescription& sites, OwnPosition ofPreviousFunction)
        : m_ofPreviousFunction(ofPreviousFunction)
    {
        PositionEncoder encoder;
        encoder.encode(sites, ofPreviousFunction);
        PositionCodes codes(encoder.counts());
        m_section = codes.bytes();
        m_place = m_section.size();
        encoder.appendNextCoded(m_section, codes);
    }

    std::optional<SourcePosition> at(const Image& image, uint32_t function, uint32_t site) const
    {
        const uint8_t* section = m_section.span().data();
        return decodePosition(image, function, section, section + m_place, m_ofPreviousFunction, CallSiteIndex(site));
    }

private:
    OwnPosition m_ofPreviousFunction;
    Vector<uint8_t> m_section;
    size_t m_place;
};

} // anonymous namespace

static void checkPositions(const Image& image, uint32_t imageFunction, const CodedPositions& coded, const Vector<SitePosition>& positions)
{
    uint32_t count = static_cast<uint32_t>(positions.size());
    for (uint32_t k = 0; k < count; ++k)
        AOT_TEST_CHECK(coded.at(image, imageFunction, k) == sourcePositionOf(positions[k]));
    AOT_TEST_CHECK(!coded.at(image, imageFunction, count));
    AOT_TEST_CHECK(!coded.at(image, imageFunction, std::numeric_limits<uint32_t>::max()));
}

static void checkEncodedPositions(std::initializer_list<Vector<SitePosition>> tables)
{
    BodiesImage any(2, bodies(bodyWithCalls(1, 1).code));
    for (const Vector<SitePosition>& positions : tables) {
        SiteDescription sites;
        sites.positions = positions;
        for (OwnPosition ofPreviousFunction : { OwnPosition { }, OwnPosition::of(sites), OwnPosition { 0xffffffff, OwnPosition::of(sites).source }, OwnPosition { 7, 0xffffffff }, OwnPosition { 0, 1 } }) {
            checkPositions(any.table(), 0, CodedPositions(sites, ofPreviousFunction), positions);
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

static void testPositionsWithConstructionStarts()
{
    BodiesImage any(2, bodies(bodyWithCalls(1, 1).code));
    SiteDescription sites;
    sites.positions = { positionAt(1, 1), positionAt(10, 20), positionAt(10, 30), positionAt(12, 5), positionAt(12, 9), positionAt(13, 2), positionAt(13, 8) };
    sites.rareLists.ensure().constructionStarts = {
        SiteConstructionStart { 1, 10, 4 },
        SiteConstructionStart { 3, 11, 7 },
        SiteConstructionStart { 4, 12, 9, true },
        SiteConstructionStart { 5, 14, 3, false, true },
    };
    CodedPositions coded(sites, OwnPosition { });
    auto decoded = [&](uint32_t site) {
        return coded.at(any.table(), 0, site);
    };
    ModuleID module(0u);
    AOT_TEST_CHECK(decoded(0) == SourcePosition({ { 1, 1 }, module }));
    AOT_TEST_CHECK(decoded(1) == SourcePosition({ { 10, 20 }, module, { 10, 4 } }));
    AOT_TEST_CHECK(decoded(2) == SourcePosition({ { 10, 30 }, module }));
    AOT_TEST_CHECK(decoded(3) == SourcePosition({ { 12, 5 }, module, { 11, 7 } }));
    AOT_TEST_CHECK(decoded(4) == SourcePosition({ { 12, 9 }, module, { 12, 9 }, true }));
    AOT_TEST_CHECK(decoded(5) == SourcePosition({ { 13, 2 }, module, { 14, 3 }, false, true }));
    AOT_TEST_CHECK(decoded(6) == SourcePosition({ { 13, 8 }, module }));
    AOT_TEST_CHECK(!decoded(7));
}

static Vector<SitePosition> positionsOnOneLine(uint32_t count)
{
    Vector<SitePosition> positions;
    for (uint32_t k = 0; k < count; ++k)
        positions.append(positionAt(3, 5 + k));
    return positions;
}

static void checkPrefixCode(const PrefixCode::Counts& counts, unsigned maxShortCodeBits)
{
    PrefixCode code(counts);
    Vector<uint8_t> bytesOfCode;
    unsigned shortCodeBits = code.shortCodeBits(maxShortCodeBits);
    code.append(bytesOfCode, shortCodeBits);
    size_t numberOfBytes = std::ranges::count_if(counts, [](uint32_t count) { return count; });
    AOT_TEST_CHECK(shortCodeBits && shortCodeBits <= std::min<size_t>(maxShortCodeBits, std::max<size_t>(numberOfBytes, 1)));
    AOT_TEST_CHECK(bytesOfCode.size() == PrefixCode::sizeOfShortWords(shortCodeBits) + sizeof(PrefixCode::LongWords) + PrefixCode::maxLength + numberOfBytes);
    PrefixCodeView view(bytesOfCode.span().data(), 64 - shortCodeBits);
    Vector<uint8_t> written;
    WeakRandom random(1);
    for (unsigned byte = 0; byte < counts.size(); ++byte) {
        for (unsigned k = 0; counts[byte] && k < 3; ++k)
            written.insert(random.getUint32(written.size() + 1), static_cast<uint8_t>(byte));
    }
    for (size_t size = 0; size <= written.size(); ++size) {
        Vector<uint8_t> stream(FillWith { }, 2 * size + 1, 0);
        PrefixCodeWriter writer(stream.mutableSpan().data());
        for (size_t k = 0; k < size; ++k)
            writer.append(code.word(written[k]));
        PrefixCodeReader reader(stream.span().data(), writer.finish());
        for (size_t k = 0; k < size; ++k)
            AOT_TEST_CHECK(reader.read(view) == written[k]);
        AOT_TEST_CHECK(reader.read(view) == PrefixCode::endOfStream);
    }
}

static void checkPrefixCode(const PrefixCode::Counts& counts)
{
    for (unsigned maxShortCodeBits : { 1u, PrefixCode::bitsOfShortWord, 10u })
        checkPrefixCode(counts, maxShortCodeBits);
}

static void testPrefixCodes()
{
    PrefixCode::Counts counts { };
    checkPrefixCode(counts);
    counts[7] = 1;
    checkPrefixCode(counts);
    counts[200] = std::numeric_limits<uint32_t>::max();
    checkPrefixCode(counts);
    counts.fill(1);
    checkPrefixCode(counts);
    uint32_t last = 1;
    uint32_t next = 1;
    for (unsigned byte = 0; byte < 40; ++byte) {
        counts[byte * 5] = next;
        next += std::exchange(last, next);
    }
    checkPrefixCode(counts);
}

static void checkVarintBytes(std::span<const uint8_t> bytes, size_t maxSize)
{
    Vector<uint8_t> coded = encodeVarintBytes(bytes);
    AOT_TEST_CHECK(coded.size() <= maxSize);
    AOT_TEST_CHECK(equalSpans(std::span<const uint8_t>(decodeVarintBytes(coded.span()).span()), bytes));
}

static void testVarintBytes()
{
    checkVarintBytes({ }, 0);
    checkVarintBytes(std::array<uint8_t, 3> { 1, 2, 3 }, 7);
    Vector<uint8_t> bytes;
    WeakRandom random(1);
    for (unsigned count = 0; count < 100000; ++count)
        appendVarint(bytes, random.getUint32(random.getUint32(8) ? 20 : 100000));
    size_t size = bytes.size();
    checkVarintBytes(bytes.span(), size);
    AOT_TEST_CHECK(encodeVarintBytes(bytes.span()).size() < size);
}

static void testPositionsObfuscated()
{
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall);
    TestBuild build(5, TestOptions { .output = Wire::OutputOptions::Obfuscate });
    TestImage image = build.build(bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(image.image->isObfuscated());
    AOT_TEST_CHECK(image.image->programTables().functionNames().empty());
    uint32_t index = image.indexOf(1);
    AOT_TEST_CHECK(!image.image->function(index).has(ImageFunctionFlag::HasPositions));
    for (uint32_t site : { 0u, 1u, 5u })
        AOT_TEST_CHECK(!image.image->positionOf(index, CallSiteIndex(site)));
    AOT_TEST_CHECK(image.image->function(index).has(ImageFunctionFlag::HasInlineFrames));
    checkCallSites(image, 1, body.sites);
}

static void testFunctionNamesKeptWithoutPositions()
{
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall);
    constexpr auto output = static_cast<Wire::OutputOptions>(static_cast<uint32_t>(Wire::OutputOptions::Obfuscate) | static_cast<uint32_t>(Wire::OutputOptions::KeepsFunctionNames));
    TestBuild build(5, TestOptions { .output = output });
    TestImage image = build.build(bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(image.image->isObfuscated());
    AOT_TEST_CHECK(!image.image->programTables().functionNames().empty());
    AOT_TEST_CHECK(!image.image->function(image.indexOf(1)).has(ImageFunctionFlag::HasPositions));
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
        { "image/positions/of-one-kind"_s, [] { checkEncodedPositions({ positionsOnOneLine(1), positionsOnOneLine(2), positionsOnOneLine(9), positionsOnOneLine(1025) }); } },
        { "image/positions/prefix-codes"_s, testPrefixCodes },
        { "image/positions/varint-bytes"_s, testVarintBytes },
        { "image/positions/construction-starts"_s, testPositionsWithConstructionStarts },
        { "image/positions/obfuscated"_s, testPositionsObfuscated },
        { "image/positions/function-names-kept-without-positions"_s, testFunctionNamesKeptWithoutPositions },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
