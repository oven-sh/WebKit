/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageBuildSteps.h"
#include "AOTStubsBarriers.h"
#include <wtf/Vector.h>

namespace JSC::AOT::ImageTests {

inline constexpr uint32_t callSize = static_cast<uint32_t>(sizeOfNearCall);
inline constexpr uint32_t sequenceOffset = 4;
inline constexpr uint32_t sequenceLength = 16;
inline constexpr uint32_t sequenceEnd = sequenceOffset + sequenceLength;
inline constexpr uint32_t removedBytes = sequenceLength - callSize;
inline constexpr unsigned numberOfBodies = 40;
inline constexpr uint32_t aliasFunctionBase = 100;
inline constexpr uint32_t handlerSite = 1;
inline constexpr uint32_t returnSize = isARM64() ? sizeof(uint32_t) : 1;
inline constexpr uint32_t calledFunction = 50;

inline CodeSketch sketchWithFrame(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    return sketch;
}

template<typename Finish>
Vector<CompiledCode> sequenceBodies(const Finish& finish, CompiledCode (CodeSketch::*take)() = &CodeSketch::take)
{
    Vector<CompiledCode> bodies;
    for (uint32_t function = 1; function <= numberOfBodies; ++function) {
        CodeSketch sketch = sketchWithFrame(function);
        sketch.plain(0x1000 + function);
        appendSequence(sketch);
        finish(sketch, function);
        bodies.append((sketch.*take)());
    }
    return bodies;
}

inline void appendPinnedSequence(CodeSketch& sketch)
{
    for (uint32_t word = 1; word <= 4; ++word)
        sketch.pinned(word);
}

inline CompiledCode pinnedBody(uint32_t function)
{
    CodeSketch sketch = sketchWithFrame(function);
    sketch.pinned(0x1000 + function);
    appendPinnedSequence(sketch);
    sketch.end();
    return sketch.take();
}

inline void finishWithCall(CodeSketch& sketch, StubKey key)
{
    sketch.callStub(key, sketch.addSite());
    sketch.end();
}

inline void finishWithMoves(CodeSketch& sketch, uint64_t constant)
{
    sketch.callThunk(stubArgumentMoves(writeBarrier(), constant), sketch.addSite());
    sketch.end();
}

inline Vector<CompiledCode> bodiesEndingInCall()
{
    return sequenceBodies([](CodeSketch& sketch, uint32_t) {
        finishWithCall(sketch, writeBarrier());
    });
}

inline Vector<CompiledCode> bodiesJumpingTo(uint32_t target)
{
    return sequenceBodies([&](CodeSketch& sketch, uint32_t) {
        sketch.jumpTo(target);
        sketch.end();
    });
}

inline Vector<CompiledCode> bodiesCalling(uint32_t evenCallee, uint32_t oddCallee)
{
    Vector<CompiledCode> bodies = sequenceBodies([&](CodeSketch& sketch, uint32_t function) {
        sketch.callFunction(function % 2 ? oddCallee : evenCallee);
        sketch.end();
    });
    bodies.append(pinnedBody(calledFunction));
    return bodies;
}

inline CompiledCode singleCallerBody(uint32_t function, uint32_t firstWord)
{
    CodeSketch sketch = singleCallerSketch(function, firstWord);
    sketch.end();
    return sketch.take();
}

inline EntryAlias createAlias(uint32_t function, uint32_t body, AliasPlacement placement = AliasPlacement::AtPrimaryEntry, uint32_t start = 0, uint32_t prefixSize = 0)
{
    return { FunctionID(function), FunctionID(body), { }, placement, start, prefixSize };
}

inline Vector<EntryAlias> createBodyAliases(AliasPlacement placement, uint32_t start, uint32_t prefixSize = 0)
{
    Vector<EntryAlias> aliases;
    for (uint32_t function = 1; function <= numberOfBodies; ++function)
        aliases.append(createAlias(aliasFunctionBase + function, function, placement, start, prefixSize));
    return aliases;
}

inline void checkSharedSequence(Steps& steps, uint32_t offset = sequenceOffset)
{
    const Outlining& outlining = steps.outlining;
    AOT_TEST_CHECK(outlining.bodies.size() == 1);
    const OutlinedBody& body = outlining.bodies[0];
    AOT_TEST_CHECK(body.length == sequenceLength && body.numberOfSites == numberOfBodies);
    AOT_TEST_CHECK(!body.singleCaller && !body.endsInCall);
    for (const Vector<OutlinedSite>& sites : outlining.sites) {
        AOT_TEST_CHECK(sites.size() == 1);
        AOT_TEST_CHECK(sites[0].offset == offset && sites[0].length == sequenceLength && !sites[0].body);
    }
    AOT_TEST_CHECK(steps.build.statistic(StatisticID::BodiesOutlined) == 1);
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
