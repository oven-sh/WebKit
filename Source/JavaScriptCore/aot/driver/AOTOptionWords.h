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

// The words of ir/schema/compile-call.def: "check=..", "testing=..", "threads=..". Bun hands each word of its developer options to the engine.
// No answer: the word was applied. Otherwise the sentence that says why not, and the options are as they were. A word that the schema lacks is refused, never swallowed.
JS_EXPORT_PRIVATE std::optional<String> applyOptionWord(Wire::CompileOptions&, StringView word);
JS_EXPORT_PRIVATE void listOptionWords(PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
