/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTJobs.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSavedFrame.h"
#include "InternalFunction.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "LinkTimeConstant.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewPromise, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewPromise);
    RELEASE_AND_RETURN(scope, JSPromise::create(vm, globalObject->promiseStructure()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreatePromise, JSCell*, (Instance* instance, EncodedJSValue constructor))
{
    constexpr Entry entry = Entry::operationAOTCreatePromise;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* newTarget = objectArgument(constructor, entry);
    JSObject* promiseConstructor = globalObject->promiseConstructor();
    ASSERT(promiseConstructor == globalObject->linkTimeConstant(LinkTimeConstant::Promise));
    instance->realm.linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Promise)] = JSValue::encode(promiseConstructor);
    Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, promiseStructure, newTarget, promiseConstructor);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, JSPromise::create(vm, structure));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRejectPromise, void, (Instance* instance, JSCell* promise, EncodedJSValue reason))
{
    constexpr Entry entry = Entry::operationAOTRejectPromise;
    AOT_OPERATION_BEGIN(instance, entry);
    cellArgument<JSPromise>(promise, entry)->reject(vm, JSValue::decode(reason));
}

static bool tryResumeAwaiterImmediately(Instance* instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    return nextJobRunsImmediately(*instance) && tryScheduleImmediateResume(*instance, frame, promise, resolution);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolvePromise, void, (Instance* instance, JSCell* promise, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTResolvePromise;
    AOT_OPERATION_BEGIN(instance, entry);
    cellArgument<JSPromise>(promise, entry)->resolve(globalObject, vm, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolvePromiseOrReturnResult, EncodedJSValue, (Instance* instance, JSCell* promise, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTResolvePromiseOrReturnResult;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue resolution = JSValue::decode(value);
    auto* frame = std::bit_cast<const uint64_t*>(callFrame->callerFrame());
    if (promise) {
        JSPromise* resolved = cellArgument<JSPromise>(promise, entry);
        if (tryResumeAwaiterImmediately(instance, frame, resolved, resolution))
            OPERATION_RETURN(scope, JSValue::encode(promise));
        resolved->resolve(globalObject, vm, resolution);
        OPERATION_RETURN(scope, JSValue::encode(promise));
    }
    bool isNotThenable = !resolution.isObject() || isDefinitelyNonThenable(asObject(resolution), globalObject);
    SavedFrame* saved = SavedFrame::forFrame(*instance, frame);
    if (isNotThenable) {
        if (saved)
            saved->fulfill(globalObject, vm, resolution);
        OPERATION_RETURN(scope, value);
    }
    JSPromise* created = saved ? saved->ensurePromise(globalObject, vm) : JSPromise::create(vm, globalObject->promiseStructure());
    created->resolve(globalObject, vm, resolution);
    OPERATION_RETURN(scope, JSValue::encode(created));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSaveFrameAndAwait, JSCell*, (Instance* instance, EncodedJSValue awaited, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* suspensionWords))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSaveFrameAndAwait);
    SuspendedMachineFrame machineFrame { frame, bottom, resumeAddress, reinterpret_cast<const SuspensionWords*>(suspensionWords) };
    SavedFrame* saved = SavedFrame::saveAsyncFunction(vm, *instance, machineFrame);
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), saved);
    if (scope.exception()) [[unlikely]]
        saved->willBeRestoredToStack();
    OPERATION_RETURN(scope, saved->promiseOrWaiter());
}

JSC_DEFINE_JIT_OPERATION(operationAOTSaveFrameAndAwaitCall, JSCell*, (Instance* instance, EncodedJSValue awaited, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* suspensionWords))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSaveFrameAndAwaitCall);
    SuspendedMachineFrame machineFrame { frame, bottom, resumeAddress, reinterpret_cast<const SuspensionWords*>(suspensionWords) };
    SavedFrame* saved = SavedFrame::saveAsyncFunction(vm, *instance, machineFrame);
    resumeWhenCallSettles(*instance, JSValue::decode(awaited), saved);
    OPERATION_RETURN(scope, saved->promiseOrWaiter());
}

JSC_DEFINE_JIT_OPERATION(operationAOTAwaitWithSavedFrame, JSCell*, (Instance* instance, EncodedJSValue awaited, JSCell* savedFrame))
{
    constexpr Entry entry = Entry::operationAOTAwaitWithSavedFrame;
    AOT_OPERATION_BEGIN(instance, entry);
    SavedFrame* saved = cellArgument<SavedFrame>(savedFrame, entry);
    vm.writeBarrier(saved);
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), saved);
    if (scope.exception()) [[unlikely]]
        saved->willBeRestoredToStack();
    OPERATION_RETURN(scope, saved->promiseOrWaiter());
}

JSC_DEFINE_JIT_OPERATION(operationAOTAwaitCallWithSavedFrame, JSCell*, (Instance* instance, EncodedJSValue awaited, JSCell* savedFrame))
{
    constexpr Entry entry = Entry::operationAOTAwaitCallWithSavedFrame;
    AOT_OPERATION_BEGIN(instance, entry);
    SavedFrame* saved = cellArgument<SavedFrame>(savedFrame, entry);
    vm.writeBarrier(saved);
    resumeWhenCallSettles(*instance, JSValue::decode(awaited), saved);
    OPERATION_RETURN(scope, saved->promiseOrWaiter());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStepAsyncGenerator, void, (Instance* instance, JSCell* generator, EncodedJSValue sent, EncodedJSValue* results))
{
    constexpr Entry entry = Entry::operationAOTStepAsyncGenerator;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* frame = std::bit_cast<const uint64_t*>(callFrame->callerFrame());
    AsyncGeneratorStep step = stepAsyncGenerator(*instance, frame, cellArgument<JSAsyncGenerator>(generator, entry), JSValue::decode(sent));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    auto words = unsafeMakeSpan(results, numberOfEnumerators<StepGeneratorResult>);
    words[static_cast<size_t>(StepGeneratorResult::Value)] = JSValue::encode(step.value);
    words[static_cast<size_t>(StepGeneratorResult::Status)] = JSValue::encode(jsNumber(static_cast<int32_t>(step.status)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRejectWithException, JSCell*, (Instance* instance, uint64_t frame))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRejectWithException);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = catchScope.exception();
    JSValue value;
    if (exception) {
        value = exception->value();
        catchScope.clearException();
    } else
        value = JSValue::decode(std::exchange(instance->runtime.exceptions.thrownValue, 0));
    RELEASE_ASSERT(value);
    SavedFrame* saved = SavedFrame::forFrame(*instance, std::bit_cast<const uint64_t*>(frame));
    if (!saved) {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->reject(vm, value);
        return promise;
    }
    switch (saved->kind()) {
    case CoroutineKind::AsyncFunction:
        saved->reject(globalObject, vm, value);
        return saved->promiseOrWaiter();
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator:
        if (exception)
            throwException(globalObject, scope, exception);
        else
            throwException(globalObject, scope, value);
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseThen, void, (Instance* instance, JSCell* promise, EncodedJSValue onFulfilled, EncodedJSValue onRejected, JSCell* derived))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseThen);
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, JSValue::decode(onFulfilled), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseCatch, JSCell*, (Instance* instance, JSCell* promise, EncodedJSValue onRejected))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseCatch);
    JSPromise* derived = JSPromise::create(vm, globalObject->promiseStructure());
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, jsUndefined(), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope, derived);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseAll, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* elements))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseAll);
    OPERATION_RETURN(scope, promiseAllOfElements(globalObject, std::span { elements, count }));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
