/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageSiteTestBodies.h"
#include "AOTStubsExceptions.h"
#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace ImageTests {

using FaultWords = Vector<uint32_t, FaultSite::maxThrowSiteWords>;

namespace {

struct ExpectedFault {
    uint32_t offset;
    FaultSiteKind kind;
    bool hasFrame;
    FaultWords words;
};

struct FaultBody {
    CompiledCode code;
    Vector<ExpectedFault> faults;
};

} // anonymous namespace

static constexpr unsigned wordsInLoopPoll = (sizeOfLoopPoll + sizeof(uint32_t) - 1) / sizeof(uint32_t) + 1;

static void appendFault(CodeSketch& sketch, Vector<ExpectedFault>& faults, uint32_t word, bool hasFrame, FaultWords&& words)
{
    SiteDescription& sites = sketch.code().sites;
    if (words.isEmpty()) {
        uint32_t offset = sketch.pinned(word, wordsInLoopPoll);
        sites.pollSites.append(PollSite { offset, !hasFrame });
        faults.append(ExpectedFault { offset, FaultSiteKind::LoopPoll, hasFrame, { } });
        return;
    }
    uint32_t offset = sketch.pinned(word);
    FaultingAccess access { offset, { }, { } };
    access.throwSite.appendVector(words);
    sites.faultingAccesses.append(WTF::move(access));
    faults.append(ExpectedFault { offset, FaultSiteKind::NullCheck, hasFrame, WTF::move(words) });
}

template<typename WordsOf>
static FaultBody bodyWithFaults(uint32_t function, bool hasFrame, unsigned count, const WordsOf& wordsOf)
{
    CodeSketch sketch(function);
    if (hasFrame)
        sketch.withFrame();
    sketch.plain(0x1000 + function);
    Vector<ExpectedFault> faults;
    for (unsigned i = 0; i < count; ++i) {
        appendFault(sketch, faults, i, hasFrame, wordsOf(i));
        sketch.plain(0);
    }
    sketch.pinned(0, 2);
    if (hasFrame)
        sketch.end();
    else
        sketch.jumpToStub(catchException());
    return { sketch.take(), WTF::move(faults) };
}

static void checkFaults(const TestImage& image, uint32_t function, const Vector<ExpectedFault>& faults)
{
    const uint8_t* code = image.address(function, 0);
    for (const ExpectedFault& fault : faults) {
        const uint8_t* address = code + fault.offset;
        std::optional<FaultSite> site = image.image->faultSiteAt(address);
        AOT_TEST_CHECK(site);
        AOT_TEST_CHECK(site->kind == fault.kind);
        AOT_TEST_CHECK(site->hasFrame == fault.hasFrame);
        for (size_t index = 0; index < fault.words.size(); ++index)
            AOT_TEST_CHECK(site->throwSite[index] == fault.words[index]);
        if (fault.words.isEmpty())
            AOT_TEST_CHECK(!site->throwSite[0] && !site->throwSite[1]);
        if (fault.words.size() < FaultSite::maxThrowSiteWords)
            AOT_TEST_CHECK(!site->throwSite[FaultSite::maxThrowSiteWords - 1]);
        AOT_TEST_CHECK(!image.image->faultSiteAt(address - 1));
        AOT_TEST_CHECK(!image.image->faultSiteAt(address + 1));
    }
}

static FaultWords mixedWords(unsigned i)
{
    if (i % 3 == 2)
        return { };
    return { i % 11 };
}

static FaultWords wordsOfEveryCount(unsigned i)
{
    switch (i % 4) {
    case 0:
        return { i };
    case 1:
        return { i, i % 8 == 1 ? 9u : 5u };
    case 2:
        return { i, 5, 1000 + i };
    default:
        return { };
    }
}

template<typename WordsOf>
static void checkFramedBodyFaults(unsigned count, const WordsOf& wordsOf)
{
    FaultBody body = bodyWithFaults(1, true, count, wordsOf);
    BodiesImage built(2, bodies(WTF::move(body.code)));
    checkFaults(built.image, 1, body.faults);
}

template<typename WordsOf>
static void checkFramedBodiesFaults(uint32_t numberOfBodies, unsigned faultsPerBody, const WordsOf& wordsOf)
{
    Vector<CompiledCode> codes;
    Vector<Vector<ExpectedFault>> expectedFaultsByBody;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        FaultBody body = bodyWithFaults(function, true, faultsPerBody, [&](unsigned i) {
            return wordsOf(function, i);
        });
        codes.append(WTF::move(body.code));
        expectedFaultsByBody.append(WTF::move(body.faults));
    }
    BodiesImage built(numberOfBodies + 1, WTF::move(codes));
    for (uint32_t function = 1; function <= numberOfBodies; ++function)
        checkFaults(built.image, function, expectedFaultsByBody[function - 1]);
}

static void testFaultSitesNone()
{
    SiteBody body = bodyWithCalls(1, 3);
    uint32_t size = static_cast<uint32_t>(body.code.body.bytes.size());
    BodiesImage built(2, bodies(WTF::move(body.code)));
    AOT_TEST_CHECK(built.table().section<uint8_t>(ImageSection::FaultSites).empty());
    const uint8_t* code = built.image.address(1, 0);
    for (uint32_t offset = 0; offset < size; ++offset)
        AOT_TEST_CHECK(!built.table().faultSiteAt(code + offset));
}

static void testFaultSitesFrames()
{
    FaultBody framed = bodyWithFaults(1, true, 10, mixedWords);
    FaultBody frameless = bodyWithFaults(2, false, 10, mixedWords);
    BodiesImage built(3, bodies(WTF::move(framed.code), WTF::move(frameless.code)));
    checkFaults(built.image, 1, framed.faults);
    checkFaults(built.image, 2, frameless.faults);
}

static void testFaultSitesFrameInTheMiddle()
{
    CodeSketch sketch(1);
    Vector<ExpectedFault> faults;
    sketch.plain(0x1001);
    for (uint32_t word : { 1u, 2u })
        appendFault(sketch, faults, word, false, word == 1 ? FaultWords { 7 } : FaultWords { });
    uint32_t begin = sketch.size();
    for (uint32_t word : { 3u, 4u, 5u })
        appendFault(sketch, faults, word, true, word == 4 ? FaultWords { } : FaultWords { 7 + word, 5 });
    uint32_t end = sketch.size();
    appendFault(sketch, faults, 6, false, { 20 });
    sketch.pinned(0, 2);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.append(CodeRange { begin, end });

    BodiesImage built(2, bodies(sketch.take()));
    checkFaults(built.image, 1, faults);
}

static void testFaultSitesSecondWords()
{
    checkFramedBodyFaults(13, [](unsigned i) -> FaultWords {
        return { i, 5 };
    });
    checkFramedBodyFaults(13, [](unsigned i) -> FaultWords {
        return { i, i % 4 == 1 ? 9u : 5u };
    });
    checkFramedBodyFaults(5, [](unsigned i) -> FaultWords {
        return { i };
    });
    checkFramedBodyFaults(12, [](unsigned i) -> FaultWords {
        if (i % 2)
            return { i };
        return { i, 5 };
    });
}

static void testFaultSitesThirdWords()
{
    checkFramedBodyFaults(9, [](unsigned i) -> FaultWords {
        return { i, 5, 40 + i };
    });
    for (unsigned count : { 64u, 65u, 300u })
        checkFramedBodyFaults(count, wordsOfEveryCount);
}

static void testFaultSitesOutside()
{
    BodiesImage built(2, bodies(bodyWithFaults(1, true, 65, mixedWords).code));
    const Image& table = built.table();
    const uint8_t* code = table.code().data();
    AOT_TEST_CHECK(!table.faultSiteAt(code));
    AOT_TEST_CHECK(!table.faultSiteAt(code + table.header().code.functionsEnd));
    AOT_TEST_CHECK(!table.faultSiteAt(code + table.header().codeSize));
    AOT_TEST_CHECK(!table.faultSiteAt(code - 1));
    AOT_TEST_CHECK(!table.faultSiteAt(nullptr));
    AOT_TEST_CHECK(!table.faultSiteAt(table.sharedCodeCopy(0)));
}

static Vector<SiteHandler> handlersAt(std::initializer_list<std::array<uint32_t, 2>> sitesAndTargets, HandlerType type = HandlerType::Catch)
{
    Vector<SiteHandler> handlers;
    for (const std::array<uint32_t, 2>& siteAndTarget : sitesAndTargets)
        handlers.append(SiteHandler { siteAndTarget[0], siteAndTarget[1], type });
    return handlers;
}

static Vector<uint8_t> encodeHandlerList(const Vector<SiteHandler>& handlers)
{
    Vector<uint8_t> bytes;
    encodeHandlers(bytes, handlers.span());
    return bytes;
}

static std::optional<HandlerRecord> handlerIn(const Vector<uint8_t>& bytes, uint32_t site)
{
    return findHandler(bytes.span(), CallSiteIndex(site));
}

static HandlerRecord handlerRecord(uint32_t codeOffset, HandlerType type = HandlerType::Catch)
{
    return { codeOffset, type };
}

static Vector<uint8_t> checkEncodedHandlers(const Vector<SiteHandler>& handlers)
{
    Vector<uint8_t> bytes = encodeHandlerList(handlers);
    for (const SiteHandler& handler : handlers)
        AOT_TEST_CHECK(handlerIn(bytes, handler.site) == handlerRecord(handler.codeOffset, handler.type));
    return bytes;
}

static void testHandlersOne()
{
    Vector<uint8_t> bytes = checkEncodedHandlers(handlersAt({ { 5, 40 } }));
    for (uint32_t site : { 4u, 6u, 0u, std::numeric_limits<uint32_t>::max() })
        AOT_TEST_CHECK(!handlerIn(bytes, site));
}

static void testHandlersMerging()
{
    Vector<SiteHandler> twoTypes = handlersAt({ { 5, 40 }, { 6, 40 } });
    twoTypes.append(SiteHandler { 7, 40, HandlerType::Finally });

    size_t threeSites = checkEncodedHandlers(handlersAt({ { 5, 40 }, { 6, 40 }, { 7, 40 } })).size();
    size_t oneSite = checkEncodedHandlers(handlersAt({ { 5, 40 } })).size();
    size_t twoTargetsSize = checkEncodedHandlers(handlersAt({ { 5, 40 }, { 6, 40 }, { 7, 44 } })).size();
    size_t twoTypesSize = checkEncodedHandlers(twoTypes).size();
    Vector<uint8_t> gap = checkEncodedHandlers(handlersAt({ { 5, 40 }, { 7, 40 } }));

    AOT_TEST_CHECK(threeSites == oneSite);
    AOT_TEST_CHECK(threeSites < twoTargetsSize);
    AOT_TEST_CHECK(threeSites < twoTypesSize);
    AOT_TEST_CHECK(threeSites < gap.size());
    AOT_TEST_CHECK(!handlerIn(gap, 6));
}

static void testHandlersWidths()
{
    constexpr std::array<std::array<uint32_t, 3>, 6> cases { { { 254, 255, 4 }, { 255, 1, 7 }, { 1, 256, 7 }, { 65534, 65535, 7 }, { 65535, 1, 13 }, { 1, 65536, 13 } } };
    for (auto [site, target, size] : cases)
        AOT_TEST_CHECK(checkEncodedHandlers(handlersAt({ { site, target } })).size() == size);
}

static size_t checkManyHandlers(uint32_t count)
{
    Vector<SiteHandler> handlers;
    for (uint32_t k = 0; k < count; ++k)
        handlers.append(SiteHandler { 3 * k + 1, 8 * k, HandlerType::Catch });
    Vector<uint8_t> bytes = checkEncodedHandlers(handlers);
    for (uint32_t k = 0; k < count; ++k) {
        AOT_TEST_CHECK(!handlerIn(bytes, 3 * k));
        AOT_TEST_CHECK(!handlerIn(bytes, 3 * k + 2));
    }
    return bytes.size();
}

static void testHandlersMany()
{
    AOT_TEST_CHECK(checkManyHandlers(31) == 94);
    AOT_TEST_CHECK(checkManyHandlers(32) == 98);
    checkManyHandlers(1000);
}

static void testHandlersTypes()
{
    constexpr std::array<HandlerType, 3> types { HandlerType::Catch, HandlerType::Finally, HandlerType::SynthesizedCatch };
    Vector<SiteHandler> three;
    for (uint32_t i = 0; i < types.size(); ++i)
        three.append(SiteHandler { 2 * i + 1, 40, types[i] });
    checkEncodedHandlers(three);

    Vector<SiteHandler> cycle;
    for (uint32_t k = 0; k < 40; ++k)
        cycle.append(SiteHandler { 2 * k + 1, 100 + k, types[k % types.size()] });
    checkEncodedHandlers(cycle);
}

static void testHandlersThroughImage()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    Vector<ExpectedSite> sites;
    for (unsigned i = 0; i < 3; ++i)
        appendCall(sketch, sites, i);
    uint32_t landingPad = sketch.end();
    sketch.code().sites.handlers.append(SiteHandler { 2, landingPad, HandlerType::Catch });

    BodiesImage built(3, bodies(sketch.take(), bodyWithCalls(2, 3).code));
    const Image& table = built.table();
    uint32_t first = built.image.indexOf(1);
    AOT_TEST_CHECK(table.handlerAt(first, CallSiteIndex(2)) == handlerRecord(landingPad));
    AOT_TEST_CHECK(!table.handlerAt(first, CallSiteIndex(1)));
    AOT_TEST_CHECK(!table.handlerAt(first, CallSiteIndex(3)));
    AOT_TEST_CHECK(!table.handlerAt(built.image.indexOf(2), CallSiteIndex(1)));
}

std::span<const NativeTest> faultTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/fault-sites/none"_s, testFaultSitesNone },
        { "image/fault-sites/counts"_s, [] { for (unsigned count : { 1u, 64u, 65u, 1000u }) checkFramedBodyFaults(count, mixedWords); } },
        { "image/fault-sites/frames"_s, testFaultSitesFrames },
        { "image/fault-sites/frame-in-the-middle"_s, testFaultSitesFrameInTheMiddle },
        { "image/fault-sites/several-bodies"_s, [] { checkFramedBodiesFaults(20, 7, [](uint32_t function, unsigned i) -> FaultWords { return { 7 * function + i }; }); } },
        { "image/fault-sites/wide-word-numbers"_s, [] { checkFramedBodiesFaults(70, 1000, [](uint32_t function, unsigned i) -> FaultWords { return { 1000 * (function - 1) + i }; }); } },
        { "image/fault-sites/second-words"_s, testFaultSitesSecondWords },
        { "image/fault-sites/third-words"_s, testFaultSitesThirdWords },
        { "image/fault-sites/outside"_s, testFaultSitesOutside },
        { "image/handlers/one"_s, testHandlersOne },
        { "image/handlers/merging"_s, testHandlersMerging },
        { "image/handlers/widths"_s, testHandlersWidths },
        { "image/handlers/many"_s, testHandlersMany },
        { "image/handlers/types"_s, testHandlersTypes },
        { "image/handlers/empty"_s, [] { AOT_TEST_CHECK(encodeHandlerList({ }).isEmpty()); AOT_TEST_CHECK(!findHandler({ }, CallSiteIndex(1))); } },
        { "image/handlers/through-image"_s, testHandlersThroughImage },
    };
    return tests;
}

} } } // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
