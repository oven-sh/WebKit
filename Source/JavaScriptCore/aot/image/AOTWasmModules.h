/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTDiagnostics.h"
#include "AOTImageBuilder.h"
#include "AOTWasmInterface.h"

namespace JSC {

class VM;

namespace AOT {

class BuildContext;

namespace Typed {
class Program;
}

JS_EXPORT_PRIVATE WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program&, const CodeTemplates&, BuildContext&);

#if ENABLE(WEBASSEMBLY)
JS_EXPORT_PRIVATE UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
