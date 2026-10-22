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

// For the request of an async generator, or of the wrapper of a sync iterator, that settles the target.
JS_EXPORT_PRIVATE JSValue tryAwaitForStepInProgress(JSGlobalObject*, JSValue settlementTarget, JSValue awaited);
JS_EXPORT_PRIVATE bool tryFinishStepInProgress(JSValue settlementTarget, JSValue value, bool isDone);
struct AsyncGeneratorBodyEntry {
    Instance* instance { nullptr };
    JSCell* outerFrame { nullptr };
    JSCell* outerAsyncGenerator { nullptr };
    JSCell* outerPromiseOfHandler { nullptr };
    bool outerAsyncContextIsPutBack { false };
};
// The job loop enters a body as the last thing it does in a job.
JS_EXPORT_PRIVATE AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator*, bool isEnteredByJobLoop);
// The adapter may have gone on to the consumers of the body and to the jobs that follow. Returns the generator whose body returned last, or null if something else did: that is finished here.
JS_EXPORT_PRIVATE JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry&, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated);
// A PromiseReactionJob whose handler is compiled code and gets the argument alone, under the async context that is in place. False: nothing is done.
JS_EXPORT_PRIVATE bool tryRunHandlerOfJob(JSGlobalObject*, VM&, JSValue promiseOrCapability, JSValue handler, JSValue argument, bool endsJob);
// A BunPerformMicrotaskJob whose function is compiled code and gets no argument. What it throws is reported before this returns. The caller has no AsyncContextSwapScope yet.
JS_EXPORT_PRIVATE bool tryRunFunctionOfJob(JSGlobalObject*, VM&, JSValue function, bool endsJob);
// For the job that resumes a driver.
JS_EXPORT_PRIVATE JSValue deliverStepToDriver(JSObject* driver, JSValue resolution, bool isRejected);
// A driver of compiled code needs no iterator result: its job has the value as it is, and in the first argument, which is undefined in every other job of the kind, whether the producer is done.
JS_EXPORT_PRIVATE bool takesBareSteps(JSValue settlementTarget);
JS_EXPORT_PRIVATE void deliverBareStepToDriver(JSObject* driver, bool isDone);

JSValue enterSavedFrame(VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue sent);
void resumeSavedAsyncFunction(JSGlobalObject*, VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue resolution);
// For whoever entered the adapter, if the handler of a job is what the adapter entered last and the handler's promise is not settled yet.
void settlePromiseOfHandler(VM&, JSCell* promise, JSValue returned, JSValue error);

JS_EXPORT_PRIVATE bool isSuspendedAsyncFunction(JSCell*);
JS_EXPORT_PRIVATE JSPromise* asyncFunctionPromise(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* coroutine);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* coroutine, size_t maxStackSize);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
