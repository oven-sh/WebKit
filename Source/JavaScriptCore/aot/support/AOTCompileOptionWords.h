/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWire.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE Wire::CompileOptions makeDefaultCompileOptions();
JS_EXPORT_PRIVATE std::optional<String> applyOptionWord(Wire::CompileOptions&, StringView word);
JS_EXPORT_PRIVATE void listOptionWords(PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
