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

JS_EXPORT_PRIVATE unsigned positionOfRunningImage();

struct TraceFrame {
    CompiledFunctionID code;
    FunctionID function;
    CallSiteIndex callSite;
    bool isInlined : 1;
    bool isConstructCall : 1;
    bool isClassCalledWithoutNew : 1 { false };
    bool isAsyncFrame : 1 { false };
    // What the site called was entered.
    bool isCaller : 1 { false };
    // CompiledFrameIterator::recordedCallee() is null.
    bool calleeLiesInFrame : 1 { false };
    // The callee of a tail call refused. JSC has popped the frame by then and lists it without its function object.
    bool isAtFailedTailCall : 1 { false };
    // Image::position() of the image whose numbers these are: the one that runs when the frame is written down.
    uint8_t imagePosition : 3 { static_cast<uint8_t>(positionOfRunningImage()) };

    static TraceFrame of(CompiledFrameIterator&);
};
static_assert(sizeof(TraceFrame) == 16);

// While it lives, the image of the frame is the one that runs on this thread.
class ImageOfTraceFrameScope {
    WTF_MAKE_NONCOPYABLE(ImageOfTraceFrameScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    JS_EXPORT_PRIVATE explicit ImageOfTraceFrameScope(const TraceFrame&);
    JS_EXPORT_PRIVATE ~ImageOfTraceFrameScope();

private:
    const Image* m_previous { nullptr };
};

} // namespace JSC::AOT

#endif // ENABLE(AOT)
