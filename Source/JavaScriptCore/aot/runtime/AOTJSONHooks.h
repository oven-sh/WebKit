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

namespace JSC {

class JSGlobalObject;

namespace AOT {

class StringBuilderStorage;

JSValue tryParseJSON(JSGlobalObject*, StringView);

Latin1Character* tryAppendJSONText(JSGlobalObject*, StringBuilderStorage&, Latin1Character* cursor, JSValue, unsigned levelsAround);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
