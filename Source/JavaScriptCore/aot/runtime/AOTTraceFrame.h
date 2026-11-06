/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"
#include "CallFrame.h"

namespace JSC::AOT {

class CompiledFrameIterator;
class Image;

// Whether a second image has been loaded into the realms of a first. Written while the images of the process are loaded, before any of their code runs.
JS_EXPORT_PRIVATE extern bool g_imagesShareRealms;

JS_EXPORT_PRIVATE unsigned positionOfRunningImageAmongSeveral();

ALWAYS_INLINE unsigned positionOfRunningImage()
{
    if (!g_imagesShareRealms) [[likely]]
        return 0;
    return positionOfRunningImageAmongSeveral();
}

struct TraceFrame {
    CompiledFunctionID code;
    FunctionID function;
    CallSiteIndex callSite;
    // One type for all of them: where the types differ, some compilers begin a new word.
    uint32_t isInlined : 1;
    uint32_t isConstructCall : 1;
    uint32_t isClassCalledWithoutNew : 1 { false };
    uint32_t isAsyncFrame : 1 { false };
    // What the site called was entered.
    uint32_t isCaller : 1 { false };
    // CompiledFrameIterator::recordedCallee() is null.
    uint32_t calleeLiesInFrame : 1 { false };
    // The callee of a tail call refused. JSC has popped the frame by then and lists it without its function object.
    uint32_t isAtFailedTailCall : 1 { false };
    // Image::position() of the image whose numbers these are: the one that runs when the frame is written down.
    // Every position takes addresses by the size of the structure heap (StructureRows), so there are no addresses for as many images as this counts.
    uint32_t imagePosition : 25 { positionOfRunningImage() };

    static TraceFrame of(CompiledFrameIterator&);
};
static_assert(sizeof(TraceFrame) == 16);

// While it lives, the image of the frame is the one that runs on this thread.
class ImageOfTraceFrameScope {
    WTF_MAKE_NONCOPYABLE(ImageOfTraceFrameScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit ImageOfTraceFrameScope(const TraceFrame& frame)
    {
        if (g_imagesShareRealms) [[unlikely]]
            runImageOf(frame);
    }
    ALWAYS_INLINE ~ImageOfTraceFrameScope()
    {
        if (m_previous) [[unlikely]]
            restore();
    }

private:
    JS_EXPORT_PRIVATE void runImageOf(const TraceFrame&);
    JS_EXPORT_PRIVATE void restore();

    const Image* m_previous { nullptr };
};

} // namespace JSC::AOT

#endif // ENABLE(AOT)
