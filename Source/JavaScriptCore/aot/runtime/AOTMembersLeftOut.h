/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSValue;
class VM;

namespace AOT {

// A NAME NEVER THROWS.

// The error is about to be thrown because code of the library that is not in the image was to run.
void noteErrorOfCodeLeftOut(JSGlobalObject*, JSObject* error);
// Where the host was making or getting a member and an exception is pending.
JS_EXPORT_PRIVATE JSValue takeStandInForMemberLeftOut(JSGlobalObject*, const String& nameOfMember, JSObject* answeredBy = nullptr);
// The same for a getter of the host that needs code of the library to make what it answers (`require.main`).
JSValue takeStandInForGetterLeftOut(JSGlobalObject*, JSObject* getter);
// The error says that what was read from `base` is no function.
void explainErrorBehindMemberLeftOut(JSGlobalObject*, JSObject* error, JSValue base);
void forgetErrorOfCodeLeftOut(VM&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
