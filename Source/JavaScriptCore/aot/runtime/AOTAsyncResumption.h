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
JS_EXPORT_PRIVATE void runAsyncFunctionResumeJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t status, JSValue, JSValue resolution, JSValue context, JSValue asyncContext, MicrotaskCallCache*);
JS_EXPORT_PRIVATE void runHandlerJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t, JSValue promise, JSValue handler, JSValue argument, JSValue asyncContext, MicrotaskCallCache*);
JS_EXPORT_PRIVATE void runFunctionJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t, JSValue function, JSValue, JSValue, JSValue, MicrotaskCallCache*);
JS_EXPORT_PRIVATE void runAsyncGeneratorBodyJob(JSGlobalObject*, VM&, InternalMicrotask, uint8_t status, JSValue, JSValue resolution, JSValue generator, JSValue asyncContext, MicrotaskCallCache*);
JS_EXPORT_PRIVATE bool hasCompiledBody(JSAsyncGenerator*);
JS_EXPORT_PRIVATE void resumeDriver(VM&, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);

JS_EXPORT_PRIVATE JSValue tryAwaitInCurrentStep(JSGlobalObject*, JSValue settlementTarget, JSValue awaited);
JS_EXPORT_PRIVATE bool tryFinishCurrentStep(JSValue settlementTarget, JSValue value, bool isDone);
struct AdapterEntry {
    JSCell* outerFrame { nullptr };
    JSCell* outerAsyncGenerator { nullptr };
    JSCell* outerPromiseOfHandler { nullptr };
    uintptr_t outerAsyncContextIsPutBack { false };
};
static_assert(sizeof(AdapterEntry) == 4 * sizeof(void*));
struct AsyncGeneratorBodyEntry {
    Instance* instance { nullptr };
    AdapterEntry adapterEntry;
};
JS_EXPORT_PRIVATE AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator*, bool isEnteredByJobLoop);
JS_EXPORT_PRIVATE JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry&, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated);
ALWAYS_INLINE bool canEnterJobFunction(JSGlobalObject* globalObject, VM& vm, JSValue function)
{
    return function.isCell() && isCompiledCalleeType(function.asCell()->type()) && function.asCell()->structure()->realm() == globalObject && globalObject->aotInstance() && vm.isSafeToRecurseSoft();
}
ALWAYS_INLINE bool canRunJobHandler(JSGlobalObject* globalObject, VM& vm, JSValue promiseOrCapability, JSValue handler)
{
    return promiseOrCapability.isCell() && promiseOrCapability.asCell()->structureID() == globalObject->promiseStructure()->id() && canEnterJobFunction(globalObject, vm, handler);
}
JS_EXPORT_PRIVATE JSValue deliverStepToDriver(JSObject* driver, JSValue resolution, bool isRejected);
JS_EXPORT_PRIVATE bool acceptsBareSteps(JSValue settlementTarget);
JS_EXPORT_PRIVATE JSValue driverResumeContext(JSValue settlementTarget);
JS_EXPORT_PRIVATE void deliverBareStepToDriver(JSObject* driver, bool isDone);

JSValue enterSavedFrame(VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue sent);
void resumeSavedAsyncFunction(JSGlobalObject*, VM&, Instance&, SavedFrame*, Typed::ResumeMode, JSValue resolution);
void settlePromiseOfHandler(VM&, JSCell* promise, JSValue returned, JSValue error);
void takeAwayRightToContinue(Instance&);

JS_EXPORT_PRIVATE bool isSuspendedAsyncFunction(JSCell*);
JS_EXPORT_PRIVATE bool isSuspendedModuleBody(VM&, JSValue coroutine);
JS_EXPORT_PRIVATE JSPromise* asyncFunctionPromise(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, WTF::Vector<StackFrame>&, JSCell* coroutine);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, WTF::Vector<StackFrame>&, JSCell* coroutine, size_t maxStackSize);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
