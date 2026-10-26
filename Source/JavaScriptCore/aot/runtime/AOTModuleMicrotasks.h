/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include "JSPromise.h"

namespace JSC {

class JSGlobalObject;

namespace AOT {

void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder);
void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status);
void runCompiledModuleImportStep(JSValue importPromise, JSValue module, uint8_t stepsLeft);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
