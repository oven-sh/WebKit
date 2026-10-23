/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>

namespace JSC::AOT::ImageTests {

struct ExpectedSite {
    uint32_t returnOffset;
    uint32_t number;
    unsigned inlineFrame { 0 };
    bool isConstruct { false };
    bool isTailCall { false };
    std::optional<uint32_t> firstReadSite { };
};

struct SiteBody {
    CompiledCode code;
    Vector<ExpectedSite> sites;
};

struct InlineFrameShape {
    uint32_t parent;
    uint32_t callee;
    bool isTailCall;
    bool isConstruct;
};

struct BodiesImage {
    BodiesImage(unsigned numberOfFunctions, Vector<CompiledCode>&& codes)
        : build(numberOfFunctions)
        , image(build.build(WTF::move(codes)))
    {
        AOT_TEST_CHECK(image.image);
    }

    const Image& table() const { return *image.image; }

    TestBuild build;
    TestImage image;
};

inline constexpr std::array<InlineFrameShape, 3> chainedFrames { { { 0, 2, false, false }, { 1, 3, false, true }, { 2, 4, true, false } } };
inline constexpr std::array<InlineFrameShape, 2> siblingFrames { { { 0, 2, false, false }, { 0, 3, false, false } } };
inline constexpr std::array<unsigned, 12> chainedFramesByCall { 0, 1, 2, 3, 3, 2, 1, 0, 2, 0, 3, 1 };

inline uint32_t returnOffsetOf(uint32_t call)
{
    return call + static_cast<uint32_t>(sizeOfNearCall);
}

inline void startFramedBody(CodeSketch& sketch, uint32_t function)
{
    sketch.withFrame();
    sketch.plain(0x1000 + function);
}

inline SiteBody finishBody(CodeSketch& sketch, Vector<ExpectedSite>&& sites)
{
    sketch.end();
    return { sketch.take(), WTF::move(sites) };
}

// Pinned: the offsets that these bodies expect are those of the sketch, so the outliner must find nothing to take.
inline void appendCall(CodeSketch& sketch, Vector<ExpectedSite>& sites, uint32_t word)
{
    sketch.pinned(word);
    OptionalIndex site = sketch.addSite(0, 10 + word, 1 + word % 7);
    uint32_t call = sketch.callStub(writeBarrier(), site);
    sites.append(ExpectedSite { returnOffsetOf(call), *site });
}

inline SiteBody bodyWithCalls(uint32_t function, unsigned numberOfCalls)
{
    CodeSketch sketch(function);
    startFramedBody(sketch, function);
    Vector<ExpectedSite> sites;
    for (unsigned i = 0; i < numberOfCalls; ++i)
        appendCall(sketch, sites, i);
    return finishBody(sketch, WTF::move(sites));
}

inline SiteBody bodyWithInlineFrames(uint32_t function, unsigned numberOfSuspensionSites, std::span<const InlineFrameShape> frames, std::span<const unsigned> framesByCall)
{
    CodeSketch sketch(function);
    startFramedBody(sketch, function);
    SiteDescription& sites = sketch.code().sites;
    auto addPosition = [&](unsigned frame) {
        uint32_t number = static_cast<uint32_t>(sites.positions.size());
        OptionalIndex site = sketch.addSite(frame, 100 * (frame + 1) + number, 1 + number % 5);
        sites.positions.last().source = ModuleID(frame);
        return site;
    };

    for (unsigned i = 0; i < numberOfSuspensionSites; ++i)
        addPosition(0);
    sites.inlineFrames.append(InlineFrame { 0, 0, 0, false, false });
    for (const InlineFrameShape& frame : frames) {
        OptionalIndex callSite = addPosition(frame.parent);
        sites.inlineFrames.append(InlineFrame { frame.parent, *callSite, sketch.calleeIndex(frame.callee), frame.isTailCall, frame.isConstruct });
    }
    sites.numberOfSitesWithoutCode = numberOfSuspensionSites + static_cast<unsigned>(frames.size());

    Vector<ExpectedSite> expected;
    uint32_t word = 0;
    for (unsigned frame : framesByCall) {
        sketch.plain(word++);
        OptionalIndex site = addPosition(frame);
        uint32_t call = sketch.callStub(writeBarrier(), site);
        expected.append(ExpectedSite { returnOffsetOf(call), *site, frame });
    }
    return finishBody(sketch, WTF::move(expected));
}

inline SiteBody bodyWithSpreadSites(uint32_t function, bool hasInlineFrame)
{
    CodeSketch sketch(function);
    startFramedBody(sketch, function);
    SiteDescription& sites = sketch.code().sites;
    if (hasInlineFrame) {
        OptionalIndex callSite = sketch.addSite(0, 20, 2);
        sites.inlineFrames.append(InlineFrame { 0, 0, 0, false, false });
        sites.inlineFrames.append(InlineFrame { 0, *callSite, sketch.calleeIndex(function + 1), false, false });
        sites.numberOfSitesWithoutCode = 1;
    }
    Vector<ExpectedSite> expected;
    appendCall(sketch, expected, 0);
    appendCall(sketch, expected, 1);
    OptionalIndex firstItem = sketch.addSite(hasInlineFrame ? 1 : 0, 70, 3);
    OptionalIndex secondItem = sketch.addSite(0, 40, 4);
    OptionalIndex thirdItem = sketch.addSite(0, 90, 5);
    sites.spreadSites.append(SpreadSite { expected[0].number, 0, *firstItem });
    sites.spreadSites.append(SpreadSite { expected[0].number, 1, *secondItem });
    sites.spreadSites.append(SpreadSite { expected[1].number, 0, *thirdItem });
    return finishBody(sketch, WTF::move(expected));
}

inline SitePosition positionAt(uint32_t line, uint32_t column, ModuleID module = ModuleID(0u))
{
    return { line, column, module, 0, 0 };
}

inline SourcePosition sourcePositionOf(const SitePosition& position)
{
    return { { position.line, position.column }, position.source };
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
