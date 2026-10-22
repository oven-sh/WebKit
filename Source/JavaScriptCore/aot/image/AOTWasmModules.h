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
#include <wtf/BitVector.h>

namespace JSC {

class VM;

namespace AOT {

class BuildContext;

namespace Typed {
class Program;
}

// The modules that the program imports, and the ones whose bytes stand in a string constant as base64 text, alone or behind the head of a data: URL.
JS_EXPORT_PRIVATE WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program&, const CodeTemplates&, BuildContext&, const BitVector* stringsInImage = nullptr);
// False where compileWebAssemblyModules() finds no module, whichever strings the image holds.
JS_EXPORT_PRIVATE bool mayHaveWebAssemblyModules(const Typed::Program&);

#if ENABLE(WEBASSEMBLY)
JS_EXPORT_PRIVATE UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
