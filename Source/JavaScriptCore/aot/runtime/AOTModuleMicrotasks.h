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

// asyncContext: what the job was queued with, which the job that it queues next is queued with too.
void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder, JSValue asyncContext);
void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status);
void runCompiledModuleImportStep(JSValue importPromise, JSValue module, JSValue error, uint8_t stepsLeft, JSValue asyncContext);
// Behind the last step that the loader takes for a graph.
void didSettleLoad(JSPromise* settled);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
