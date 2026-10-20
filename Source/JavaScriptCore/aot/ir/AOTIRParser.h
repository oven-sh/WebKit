/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVerifier.h"
#include "JSExportMacros.h"
#include <memory>
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

JS_EXPORT_PRIVATE std::unique_ptr<Program> parseProgram(StringView text, IRForm, BuildContext&, RawWordCheck = RawWordCheck::Run);
JS_EXPORT_PRIVATE std::unique_ptr<Program> parseProgram(StringView text, IRForm, BuildContext&, const VerifierInputs&);

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
