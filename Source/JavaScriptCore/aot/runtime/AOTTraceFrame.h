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
    bool isInlined;
    bool isConstructCall;

    static TraceFrame of(CompiledFrameIterator&);
};
static_assert(sizeof(TraceFrame) == 16);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
