/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTVector.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

struct FFIStubs {
    Vector<uint8_t> code;
    size_t codeAlignment { 1 };
    size_t callbackContextOffset { 0 };
    size_t callbackContextSize { 0 };
    Vector<uint8_t> directory;
};

bool mayUseFFI(const Typed::PipelineOutput&, const CompileOptions&);
JS_EXPORT_PRIVATE FFIStubs generateFFIStubs(BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
