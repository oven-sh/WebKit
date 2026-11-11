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
    uint32_t isInlined : 1;
    uint32_t isConstructCall : 1;
    uint32_t isClassCalledWithoutNew : 1 { false };
    uint32_t isAsyncFrame : 1 { false };
    uint32_t isCaller : 1 { false };
    uint32_t isAtFailedTailCall : 1 { false };

    static TraceFrame of(CompiledFrameIterator&);
};
static_assert(sizeof(TraceFrame) == 16);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
