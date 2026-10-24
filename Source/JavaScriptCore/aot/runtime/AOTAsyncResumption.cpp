/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAsyncResumption.h"

#if ENABLE(AOT)

#include "AOTCompiledCall.h"
#include "AOTCoroutineTable.h"
#include "AOTInstance.h"
#include "AOTJobs.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "AOTStubsCoroutines.h"
#include "ExceptionHelpers.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"

namespace JSC { namespace AOT {

JSValue enterSavedFrame(VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue sent)
{
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2);
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Value) == 3 && static_cast<int32_t>(JSGenerator::Argument::ResumeMode) == 4);
    ASSERT(&instance == Instance::of(saved->realm()));
    CallData callData = CallData::forCompiledCode(instance.state->coroutines->savedFrameEntrypoint(), instance.globalObject);
    return enterCompiledCode(vm, callData, saved, jsUndefined(), saved, jsUndefined(), jsNumber(0), sent, jsNumber(static_cast<int32_t>(engineResumeModeOf(mode))));
}

void settlePromiseOfHandler(VM& vm, JSCell* cell, JSValue returned, JSValue error)
{
    auto* promise = uncheckedDowncast<JSPromise>(cell);
    if (error)
        promise->rejectPromise(vm, error);
    else
        promise->resolvePromise(promise->realm(), vm, returned);
}

namespace {

// What the adapter says about the coroutine or the handler that it has entered last, kept for the entry around this one.
class AdapterEntry {
public:
    AdapterEntry(Instance& instance, JSCell* frame, bool asyncContextIsPutBack)
        : m_coroutines(instance.coroutines)
        , m_outerFrame(std::exchange(m_coroutines.enteredFrame, frame))
        , m_outerAsyncGenerator(std::exchange(m_coroutines.enteredAsyncGenerator, nullptr))
        , m_outerPromiseOfHandler(std::exchange(m_coroutines.promiseOfEnteredHandler, nullptr))
        , m_outerAsyncContextIsPutBack(std::exchange(m_coroutines.asyncContextIsPutBack, asyncContextIsPutBack))
    {
    }

    // Only the restoring of a frame and a handler throw out of the adapter, so the exception is that of what was entered last, and nothing has run since the adapter named it.
    void finish(VM& vm, Instance& instance, TopExceptionScope& scope, JSValue returned)
    {
        m_coroutines.frameThatMayContinue = nullptr;
        auto* lastFrame = static_cast<SavedFrame*>(std::exchange(m_coroutines.enteredFrame, m_outerFrame));
        JSCell* lastAsyncGenerator = std::exchange(m_coroutines.enteredAsyncGenerator, m_outerAsyncGenerator);
        JSCell* promiseOfHandler = std::exchange(m_coroutines.promiseOfEnteredHandler, m_outerPromiseOfHandler);
        m_coroutines.asyncContextIsPutBack = m_outerAsyncContextIsPutBack;
        JSValue error;
        Exception* exception = scope.exception();
        if (exception) [[unlikely]] {
            error = exception->value();
            if (!scope.clearExceptionExceptTermination())
                return;
        }
        if (promiseOfHandler) {
            settlePromiseOfHandler(vm, promiseOfHandler, returned, error);
            return;
        }
        if (!lastFrame) {
            if (exception) [[unlikely]]
                reportErrorOfMicrotask(instance.globalObject, exception);
            return;
        }
        if (lastFrame->kind() == CoroutineKind::AsyncGenerator) {
            finishAsyncGeneratorBodyCall(instance.globalObject, uncheckedDowncast<JSAsyncGenerator>(lastAsyncGenerator), returned, error, &vm.syncResumeCallCache());
            RETURN_IF_EXCEPTION(scope, void());
            return;
        }
        if (error) [[unlikely]] {
            EnsureStillAliveScope keepsLastFrame(lastFrame);
            lastFrame->reject(instance.globalObject, vm, error);
        }
    }

private:
    CoroutineWords& m_coroutines;
    JSCell* m_outerFrame;
    JSCell* m_outerAsyncGenerator;
    JSCell* m_outerPromiseOfHandler;
    uint8_t m_outerAsyncContextIsPutBack;
};

} // anonymous namespace

static bool tryEnterFunctionOfJob(JSGlobalObject* globalObject, VM& vm, JSValue promise, JSValue function, JSValue argument, bool endsJob, bool asyncContextIsPutBack)
{
    if (!function.isCell() || !isCompiledCalleeType(function.asCell()->type()) || function.asCell()->structure()->realm() != globalObject)
        return false;
    Instance* instance = Instance::of(globalObject);
    if (!instance || !vm.isSafeToRecurseSoft()) [[unlikely]]
        return false;

    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2 && static_cast<int32_t>(JSGenerator::Argument::Value) == 3);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AdapterEntry entry(*instance, nullptr, asyncContextIsPutBack);
    instance->coroutines.nextEntryEndsJob = endsJob;
    CallData callData = CallData::forCompiledCode(instance->state->coroutines->handlerOfJobEntrypoint(), globalObject);
    JSValue returned = enterCompiledCode(vm, callData, asObject(function), jsUndefined(), function.asCell(), promise, function, argument, jsNumber(0));
    entry.finish(vm, *instance, scope, returned);
    if (instance->coroutines.waiterToResume)
        resumeRemainingWaiters(*instance);
    return true;
}

bool tryRunHandlerOfJob(JSGlobalObject* globalObject, VM& vm, JSValue promiseOrCapability, JSValue handler, JSValue argument, bool endsJob)
{
    if (!promiseOrCapability.isCell() || promiseOrCapability.asCell()->structureID() != globalObject->promiseStructure()->id())
        return false;
    return tryEnterFunctionOfJob(globalObject, vm, promiseOrCapability, handler, argument, endsJob, endsJob && vm.isAsyncContextTrackingEnabled());
}

bool tryRunFunctionOfJob(JSGlobalObject* globalObject, VM& vm, JSValue function, bool endsJob)
{
    return tryEnterFunctionOfJob(globalObject, vm, jsUndefined(), function, jsUndefined(), endsJob, false);
}

void resumeSavedAsyncFunction(JSGlobalObject* globalObject, VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue resolution)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        saved->willBeRestoredToStack();
        saved->reject(globalObject, vm, createStackOverflowError(globalObject));
        return;
    }
    // The adapter goes on with the waiters of functions that finish and with the jobs that follow.
    AdapterEntry entry(instance, saved, instance.coroutines.nextEntryEndsJob && vm.isAsyncContextTrackingEnabled());
    JSValue returned = enterSavedFrame(vm, instance, saved, mode, resolution);
    entry.finish(vm, instance, scope, returned);
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    auto* saved = uncheckedDowncast<SavedFrame>(context);
    Instance& instance = *Instance::of(saved->realm());
    // The loop over the jobs goes back to its caller between jobs of two realms, which the adapter doesn't.
    isEnteredByJobLoop &= globalObject == saved->realm();
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    instance.coroutines.nextEntryEndsJob = isEnteredByJobLoop;
    resumeSavedAsyncFunction(globalObject, vm, instance, saved, isRejected ? ResumeMode::Throw : ResumeMode::Next, resolution);
    if (instance.coroutines.waiterToResume)
        resumeRemainingWaiters(instance);
}

void resumeDriver(VM& vm, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    RELEASE_ASSERT(driver->classInfo() == SavedFrame::info());
    resumeAsyncFunction(driver->realm(), vm, driver, resolution, isRejected, isEnteredByJobLoop);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
