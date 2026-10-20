/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC {

namespace B3 {
class RelocationLowering;
}

namespace AOT {

class BuildContext;

enum class BackEndConvention : uint8_t { Function, StubBody };

struct FunctionBackEndTraits {
    Typed::FunctionID function;
    BackEndConvention convention { BackEndConvention::Function };
    bool savesFrameOnSuspend { false };
    bool readsCallerFrame { false };
    bool isAsync { false };
    bool isCold { false };
    bool returnsAreUnlikely { false };
};

JS_EXPORT_PRIVATE B3::Policy b3PolicyFor(const BuildContext&, const FunctionBackEndTraits&, B3::RelocationLowering&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
