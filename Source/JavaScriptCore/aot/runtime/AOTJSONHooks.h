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

// A part in the middle of a text that compiled code writes: the text that JSON.stringify(value) gives, at the cursor, where no code of the program has to run for it.
// `levelsAround`: the objects and lists that are open around the value. They count as those inside it do.
// The cursor behind the text, with maxStringBuilderReservation bytes of room. Null: the value is left to the call as written, which starts from the beginning. An exception comes with null alone.
Latin1Character* tryAppendJSONText(JSGlobalObject*, StringBuilderStorage&, Latin1Character* cursor, JSValue, unsigned levelsAround);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
