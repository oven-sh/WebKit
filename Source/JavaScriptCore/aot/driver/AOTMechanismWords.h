/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMechanisms.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE bool isMechanismsWord(StringView word);
JS_EXPORT_PRIVATE std::optional<String> applyMechanismsWord(Typed::Mechanisms&, StringView word);
JS_EXPORT_PRIVATE ASCIILiteral nameOfMechanism(Typed::Mechanism);
JS_EXPORT_PRIVATE void listMechanismsWord(PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
