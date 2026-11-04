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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
