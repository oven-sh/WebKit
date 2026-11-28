/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <span>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace AOT {

enum class JSONTextMemory : bool { Constant, MayChange };

JSValue tryParseJSON(JSGlobalObject*, StringView, JSONTextMemory = JSONTextMemory::Constant);
[[noreturn]] void crashOnJSONOutputMismatch(const String& written, const String& genericOutput);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
