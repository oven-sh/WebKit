/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJobs.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTSavedFrame.h"
#include "AsyncContextSwapScope.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSPromise.h"
#include "JSSentinel.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"

namespace JSC { namespace AOT {

static ALWAYS_INLINE bool isSavedFrameOf(const Instance& instance, JSValue value)
{
    return value && value.isCell() && value.asCell()->structureID().bits() == static_cast<uint32_t>(instance.coroutines.savedFrameHeader);
}

void resumeWhenSettled(JSGlobalObject* globalObject, VM& vm, JSValue awaited, SavedFrame* waiter)
{
    constexpr InternalMicrotask task = InternalMicrotask::CompiledAsyncFunctionResume;
    if (!awaited.isObject()) {
        // The generator that was stepped resumes the waiter.
        if (awaited == vm.fastAsyncGeneratorSentinel()) [[unlikely]]
            return;
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), awaited, waiter, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    if (awaited.asCell()->structureID() == globalObject->promiseStructure()->id()) {
        uncheckedDowncast<JSPromise>(awaited)->performPromiseThenWithInternalMicrotask(vm, task, nullptr, waiter, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    if (awaited.asCell()->type() != JSPromiseType && isDefinitelyNonThenable(asObject(awaited), globalObject)) {
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), awaited, waiter, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, awaited, task, waiter);
}

void resumeWhenCallSettles(Instance& instance, JSValue awaited, SavedFrame* waiter)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    JSValue asyncContext = AsyncContextSwapScope::current(vm, globalObject);
    if (awaited.isCell() && awaited.asCell()->structureID() == globalObject->promiseStructure()->id()) {
        uncheckedDowncast<JSPromise>(awaited)->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, nullptr, waiter, asyncContext);
        return;
    }
    if (isSavedFrameOf(instance, awaited)) {
        auto* callee = uncheckedDowncast<SavedFrame>(awaited);
        if (JSPromise* promise = callee->promise())
            promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, nullptr, waiter, asyncContext);
        else
            callee->setWaiter(vm, waiter, asyncContext);
        return;
    }
    globalObject->queueMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), awaited, waiter, asyncContext);
}

bool tryScheduleImmediateResume(Instance& instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    CoroutineWords& coroutines = instance.coroutines;
    if (coroutines.waiterToResume || instance.awaitMustSuspend)
        return false;
    // resumeAsyncFunction resumes the waiter. Whoever enters the body of an async generator, which may have the flag too, does not.
    SavedFrame* running = SavedFrame::forFrame(instance, frame);
    if (!running || running->kind() != CoroutineKind::AsyncFunction || !running->isEnteredByJobLoop())
        return false;
    if (resolution.isObject() && !isDefinitelyNonThenable(asObject(resolution), instance.globalObject))
        return false;
    if (promise->inlineReactionKind() != JSPromise::InlineReactionKind::InternalMicrotask)
        return false;
    JSValue waiter = promise->inlineReactionContext();
    if (!isSavedFrameOf(instance, waiter))
        return false;
    auto job = promise->tryFulfillTakingInlineJob(*instance.vm, resolution);
    if (!job)
        return false;
    RELEASE_ASSERT(job->task == InternalMicrotask::CompiledAsyncFunctionResume);
    coroutines.waiterToResume = waiter.asCell();
    coroutines.waiterResolution = JSValue::encode(resolution);
    coroutines.waiterAsyncContext = JSValue::encode(job->asyncContext);
    return true;
}

NEVER_INLINE void resumeRemainingWaiters(Instance& instance)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    CoroutineWords& coroutines = instance.coroutines;
    while (JSCell* waiter = std::exchange(coroutines.waiterToResume, nullptr)) {
        if (vm.hasPendingTerminationException()) [[unlikely]]
            break;
        auto* saved = uncheckedDowncast<SavedFrame>(waiter);
        AsyncContextSwapScope asyncContextScope(vm, globalObject, JSValue::decode(coroutines.waiterAsyncContext));
        saved->setIsEnteredByJobLoop(true);
        resumeSavedAsyncFunction(globalObject, vm, instance, saved, ResumeMode::Next, JSValue::decode(coroutines.waiterResolution));
    }
    coroutines.waiterResolution = JSValue::encode(JSValue());
    coroutines.waiterAsyncContext = JSValue::encode(JSValue());
}

AsyncGeneratorStepInProgress::AsyncGeneratorStepInProgress(Instance& instance, JSObject* driver, JSAsyncGenerator* generator)
    : m_instance(instance)
    , m_enclosingStep(instance.coroutines.stepInProgress)
    , m_driver(driver)
    , m_generator(generator)
{
    instance.coroutines.stepInProgress = this;
}

AsyncGeneratorStepInProgress::~AsyncGeneratorStepInProgress()
{
    m_instance.coroutines.stepInProgress = m_enclosingStep;
}

void AsyncGeneratorStepInProgress::finish(JSValue value, bool isDone)
{
    ASSERT(!isFinished() && value);
    m_value = value;
    m_isDone = isDone;
}

static AsyncGeneratorStepInProgress* findUnfinishedStep(JSValue settlementTarget)
{
    if (!settlementTarget.isObject())
        return nullptr;
    Instance* instance = Instance::of(asObject(settlementTarget)->realm());
    AsyncGeneratorStepInProgress* step = instance ? instance->coroutines.stepInProgress : nullptr;
    if (!step || step->driver() != settlementTarget.asCell() || step->isFinished() || !nextJobRunsImmediately(*instance))
        return nullptr;
    return step;
}

JSValue tryAwaitForStepInProgress(JSGlobalObject* globalObject, JSValue settlementTarget, JSValue awaited)
{
    if (!findUnfinishedStep(settlementTarget))
        return { };
    if (!awaited.isObject())
        return awaited;
    JSObject* object = asObject(awaited);
    if (object->structureID() == globalObject->promiseStructure()->id()) {
        auto* promise = uncheckedDowncast<JSPromise>(object);
        if (promise->status() != JSPromise::Status::Fulfilled)
            return { };
        promise->markAsHandled();
        return promise->settlementValue();
    }
    if (object->type() == JSPromiseType || !isDefinitelyNonThenable(object, globalObject))
        return { };
    return awaited;
}

bool tryFinishStepInProgress(JSValue settlementTarget, JSValue value, bool isDone)
{
    AsyncGeneratorStepInProgress* step = findUnfinishedStep(settlementTarget);
    if (!step)
        return false;
    step->finish(value, isDone);
    return true;
}

void willEnterAsyncGeneratorBody(JSAsyncGenerator* generator, bool isEnteredByJobLoop)
{
    JSValue frame = generator->frame();
    if (!frame.isCell() || frame.asCell()->classInfo() != SavedFrame::info())
        return;
    auto* saved = uncheckedDowncast<SavedFrame>(frame.asCell());
    if (!isEnteredByJobLoop) {
        // The consumer of an unfinished step suspends as soon as the generator does.
        AsyncGeneratorStepInProgress* step = Instance::of(saved->realm())->coroutines.stepInProgress;
        isEnteredByJobLoop = step && step->generator() == generator && !step->isFinished();
    }
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
}

static JSObject* driverOf(SavedFrame* coroutine, const uint64_t* frame)
{
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1);
    switch (coroutine->kind()) {
    case CoroutineKind::AsyncFunction:
        return coroutine;
    case CoroutineKind::AsyncGenerator:
        return asObject(std::bit_cast<CallFrame*>(frame[0])->uncheckedArgument(0));
    case CoroutineKind::Generator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AsyncGeneratorStep stepAsyncGenerator(Instance& instance, const uint64_t* frame, JSAsyncGenerator* generator, JSValue sent)
{
    JSGlobalObject* globalObject = instance.globalObject;
    MicrotaskCallCache* callCache = &instance.vm->syncResumeCallCache();
    // A coroutine that has not been suspended yet has nothing that the generator could resume.
    SavedFrame* coroutine = SavedFrame::forFrame(instance, frame);
    if (!coroutine)
        return { asyncGeneratorNext(globalObject, generator, sent, callCache), AsyncGeneratorStepStatus::Awaited };

    JSObject* driver = driverOf(coroutine, frame);
    if (!coroutine->isEnteredByJobLoop())
        return { asyncIteratorNextWithDriver(globalObject, generator, driver, sent, callCache), AsyncGeneratorStepStatus::Awaited };

    AsyncGeneratorStepInProgress step(instance, driver, generator);
    JSValue awaited = asyncIteratorNextWithDriver(globalObject, generator, driver, sent, callCache);
    if (!step.isFinished())
        return { awaited, AsyncGeneratorStepStatus::Awaited };
    return { step.value(), step.isDone() ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
