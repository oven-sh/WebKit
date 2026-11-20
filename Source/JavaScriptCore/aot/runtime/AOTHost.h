/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinModules.h"
#include "AOTCompiledCallee.h"
#include "AOTGenerator.h"
#include "AOTModuleHost.h"
#include "AOTPropertyWalk.h"
#include "AOTRealmStartup.h"
#include "AOTStackTrace.h"
#include "JSCJSValue.h"

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace AOT {

JS_EXPORT_PRIVATE JSObject* withAllPropertiesInline(JSGlobalObject*, JSObject*);

// For an object above a global object with GetOwnPropertySlotIsImpureForPropertyAbsence and NewImpurePropertyFiresWatchpoints: it may have the name
// (null: any name) from now on. Where it cannot promise to say so, it taints the slot of the lookup.
JS_EXPORT_PRIVATE void didAnnounceNameAboveGlobalObject(VM&, UniquedStringImpl* nameOrNull);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
