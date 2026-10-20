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
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTStackTrace.h"
#include "JSCJSValue.h"

namespace JSC {

class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

JS_EXPORT_PRIVATE uint64_t operationCallCount(VM&, Entry);
JS_EXPORT_PRIVATE void writePerfMap();
JS_EXPORT_PRIVATE void setMismatchLog(intptr_t descriptor, bool isJSON);
JS_EXPORT_PRIVATE void setMismatchLogFormat(bool isJSON);
JS_EXPORT_PRIVATE JSObject* withAllPropertiesInline(JSGlobalObject*, JSObject*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
