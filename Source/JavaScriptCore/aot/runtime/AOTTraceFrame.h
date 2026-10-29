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

    static TraceFrame of(CompiledFrameIterator&);
};
static_assert(sizeof(TraceFrame) == 16);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
