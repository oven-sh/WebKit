/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include "JSGlobalObject.h"
#include "StackFrame.h"
#include "VMInlines.h"

namespace JSC {

class JSAsyncGenerator;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class MicrotaskCallCache;
class VM;
enum class InternalMicrotask : uint8_t;

namespace AOT {

class SavedFrame;
struct Instance;

JS_EXPORT_PRIVATE void resumeAsyncFunction(JSGlobalObject*, VM&, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
// These have the signature of runInternalMicrotask(), which tail-calls them.
JS_EXPORT_PRIVATE void runAsyncFunctionResumeJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t status, JSValue, JSValue resolution, JSValue context, JSValue asyncContext, MicrotaskCallCache*);
// A PromiseReactionJob whose handler gets the argument alone. Only if canRunHandlerOfJob().
JS_EXPORT_PRIVATE void runHandlerJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t, JSValue promise, JSValue handler, JSValue argument, JSValue asyncContext, MicrotaskCallCache*);
// A BunPerformMicrotaskJob whose function gets no argument, where no async context is in place. Only if canEnterFunctionOfJob(). What it throws is reported before this returns.
JS_EXPORT_PRIVATE void runFunctionJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t, JSValue function, JSValue, JSValue, JSValue, MicrotaskCallCache*);
// An AsyncGeneratorBodyCallNormal or AsyncGeneratorBodyCallReturn job. Only if hasCompiledBody().
JS_EXPORT_PRIVATE void runAsyncGeneratorBodyJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t status, JSValue, JSValue resolution, JSValue generator, JSValue asyncContext, MicrotaskCallCache*);
JS_EXPORT_PRIVATE bool hasCompiledBody(JSAsyncGenerator*);
JS_EXPORT_PRIVATE void resumeDriver(VM&, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);

// For the request of an async generator, or of the wrapper of a sync iterator, that settles the target.
JS_EXPORT_PRIVATE JSValue tryAwaitForStepInProgress(JSGlobalObject*, JSValue settlementTarget, JSValue awaited);
JS_EXPORT_PRIVATE bool tryFinishStepInProgress(JSValue settlementTarget, JSValue value, bool isDone);
struct AsyncGeneratorBodyEntry {
    Instance* instance { nullptr };
    JSCell* outerFrame { nullptr };
    JSCell* outerAsyncGenerator { nullptr };
    JSCell* outerPromiseOfHandler { nullptr };
    // A whole word, so that a frame that holds an entry writes all of it.
    uintptr_t outerAsyncContextIsPutBack { false };
};
static_assert(sizeof(AsyncGeneratorBodyEntry) == 5 * sizeof(void*));
// The job loop enters a body as the last thing it does in a job.
JS_EXPORT_PRIVATE AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator*, bool isEnteredByJobLoop);
// The adapter may have gone on to the consumers of the body and to the jobs that follow. Returns the generator whose body returned last, or null if something else did: that is finished here.
JS_EXPORT_PRIVATE JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry&, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated);
ALWAYS_INLINE bool canEnterFunctionOfJob(JSGlobalObject* globalObject, VM& vm, JSValue function)
{
    return function.isCell() && isCompiledCalleeType(function.asCell()->type()) && function.asCell()->structure()->realm() == globalObject && globalObject->aotInstance() && vm.isSafeToRecurseSoft();
}
ALWAYS_INLINE bool canRunHandlerOfJob(JSGlobalObject* globalObject, VM& vm, JSValue promiseOrCapability, JSValue handler)
{
    return promiseOrCapability.isCell() && promiseOrCapability.asCell()->structureID() == globalObject->promiseStructure()->id() && canEnterFunctionOfJob(globalObject, vm, handler);
}
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
JS_EXPORT_PRIVATE bool isSuspendedModuleBody(VM&, JSValue coroutine);
JS_EXPORT_PRIVATE JSPromise* asyncFunctionPromise(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, WTF::Vector<StackFrame>&, JSCell* coroutine);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, WTF::Vector<StackFrame>&, JSCell* coroutine, size_t maxStackSize);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
