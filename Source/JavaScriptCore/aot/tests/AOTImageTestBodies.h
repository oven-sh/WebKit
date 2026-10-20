/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include <initializer_list>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC::AOT::ImageTests {

inline constexpr unsigned wordsInPinnedUnit = 1024;
inline constexpr unsigned numberOfMixedBodyKinds = 7;
inline constexpr unsigned numberOfMixedBodies = 86 * numberOfMixedBodyKinds;

inline CodeSketch framedSketch(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(0x1000 + function);
    return sketch;
}

inline CompiledCode plainBody(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.end();
    return sketch.take();
}

inline CompiledCode takeWithHandler(CodeSketch& sketch, std::initializer_list<uint32_t> frameMapWords)
{
    uint32_t site = sketch.code().sites.positions.size() - 1;
    uint32_t handler = sketch.end();
    sketch.code().sites.handlers.append(SiteHandler { site, handler, HandlerType::Catch });
    sketch.code().sites.frameMapWords = frameMapWords;
    return sketch.take();
}

inline CompiledCode framedCallsBody(uint32_t function, uint32_t lastFunction)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.callFunction(1, sketch.addSite());
    sketch.callFunction(lastFunction, sketch.addSite());
    return takeWithHandler(sketch, { 1, 2 });
}

inline CompiledCode mixedBody(uint32_t function, uint32_t lastFunction)
{
    CodeSketch sketch(function);
    switch (function % numberOfMixedBodyKinds) {
    case 0:
        return plainBody(function);
    case 1:
        sketch.pinned(7);
        sketch.jumpToStub(catchException());
        break;
    case 2:
        sketch.jumpToFunction(function + 4 <= lastFunction ? function + 4 : 1);
        break;
    case 3:
        sketch.withFrame();
        sketch.plain(0x1000 + function);
        for (uint32_t word = 1; word <= 4; ++word)
            sketch.plain(word);
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.end();
        break;
    case 4:
        return framedCallsBody(function, lastFunction);
    case 5:
        sketch.pinned(function);
        sketch.jumpToFunction(function + 1 <= lastFunction ? function + 1 : 1);
        break;
    case 6:
        sketch.withFrame();
        sketch.pinned(function);
        sketch.jumpToFunction(function + 1 <= lastFunction ? function + 1 : 1);
        break;
    }
    return sketch.take();
}

inline Vector<CompiledCode> mixedBodies(unsigned count)
{
    Vector<CompiledCode> result;
    for (uint32_t function = 1; function <= count; ++function)
        result.append(mixedBody(function, count));
    return result;
}

inline TestImage builtImage(TestBuild& build, Vector<CompiledCode>&& code)
{
    TestImage image = build.build(WTF::move(code));
    AOT_TEST_CHECK(image.image);
    return image;
}

inline TestOptions reachOptions(LimitID limit = LimitID::DirectCallReach) { return { .loweredLimit = limit, .loweredLimitValue = loweredReach, .heat = Wire::HeatOverride::EverythingIsCold }; }
inline TestOptions sharedCodeReachOptions() { return { .loweredLimit = LimitID::DirectCallReach, .loweredLimitValue = loweredSharedCodeReach, .heat = Wire::HeatOverride::EverythingIsCold }; }

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
