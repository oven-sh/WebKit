/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "JSCJSValue.h"
#include "StackFrame.h"
#include <wtf/Vector.h>

namespace JSC {

class JSAsyncGenerator;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class VM;

namespace AOT {

class SavedFrame;
struct Instance;

JS_EXPORT_PRIVATE void resumeAsyncFunction(JSGlobalObject*, VM&, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
JS_EXPORT_PRIVATE void resumeDriver(VM&, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);

// For the request of a generator that settles the target. Each answers only while compiled code waits on the stack for the end of that step and the job that the engine
// would queue is the next one to run. The first returns what the await of a yielded value gives, or the empty value.
JS_EXPORT_PRIVATE JSValue tryAwaitForStepInProgress(JSGlobalObject*, JSValue settlementTarget, JSValue awaited);
JS_EXPORT_PRIVATE bool tryFinishStepInProgress(JSValue settlementTarget, JSValue value, bool isDone);
// The job loop enters a body as the last thing it does in a job.
JS_EXPORT_PRIVATE void willEnterAsyncGeneratorBody(JSAsyncGenerator*, bool isEnteredByJobLoop);

JSValue enterSavedFrame(VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue sent);
void resumeSavedAsyncFunction(JSGlobalObject*, VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue resolution);

JS_EXPORT_PRIVATE bool isSuspendedAsyncFunction(JSCell*);
JS_EXPORT_PRIVATE JSPromise* asyncFunctionPromise(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* coroutine);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* coroutine, size_t maxStackSize);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
