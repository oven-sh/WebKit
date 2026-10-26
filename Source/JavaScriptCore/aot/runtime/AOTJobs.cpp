/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTCompiledCall.h"
#include "AOTCoroutineTable.h"
#include "AOTCoroutineWords.h"
#include "AOTGenerator.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSavedFrame.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTStubFrames.h"
#include "AOTStubsCoroutines.h"
#include "AOTThreshold.h"
#include "AOTUnwinding.h"
#include "AsyncContextSwapScope.h"
#include "DeferTermination.h"
#include "EnsureStillAliveHere.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSSentinel.h"
#include "LinkTimeConstant.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#include <array>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct AsyncGeneratorStep {
    JSValue value;
    AsyncGeneratorStepStatus status;
};

static ALWAYS_INLINE bool nextJobRunsImmediately(const Instance& instance)
{
    return !instance.awaitMustSuspend && instance.realm.microtaskQueue->isEmpty();
}

static ALWAYS_INLINE bool isSavedFrameOf(const Instance& instance, JSValue value)
{
    return value && value.isCell() && value.asCell()->structureID().bits() == static_cast<uint32_t>(instance.coroutines.savedFrameHeader);
}

// Of any realm.
static ALWAYS_INLINE SavedFrame* savedFrameIn(JSValue value)
{
    return value && value.isCell() && value.asCell()->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(value.asCell()) : nullptr;
}

// What the adapter says about the coroutine or the handler that it has entered last is kept for the entry around this one.
static AsyncGeneratorBodyEntry willEnterAdapter(Instance& instance, JSCell* frame, JSCell* asyncGenerator, bool asyncContextIsPutBack)
{
    CoroutineWords& coroutines = instance.coroutines;
    return { &instance, std::exchange(coroutines.enteredFrame, frame), std::exchange(coroutines.enteredAsyncGenerator, asyncGenerator), std::exchange(coroutines.promiseOfEnteredHandler, nullptr), !!std::exchange(coroutines.asyncContextIsPutBack, asyncContextIsPutBack) };
}

struct LastEnteredByAdapter {
    SavedFrame* frame;
    JSCell* asyncGenerator;
    JSCell* promiseOfHandler;
};

static LastEnteredByAdapter didLeaveAdapter(const AsyncGeneratorBodyEntry& entry)
{
    CoroutineWords& coroutines = entry.instance->coroutines;
    coroutines.asyncContextIsPutBack = entry.outerAsyncContextIsPutBack;
    return { static_cast<SavedFrame*>(std::exchange(coroutines.enteredFrame, entry.outerFrame)), std::exchange(coroutines.enteredAsyncGenerator, entry.outerAsyncGenerator), std::exchange(coroutines.promiseOfEnteredHandler, entry.outerPromiseOfHandler) };
}

// The adapter enters the function of a job only where no async context is in place, and an error is reported under what was in place in front of the job.
static void reportErrorOfFunctionOfJob(Instance& instance, JSValue exceptionOrValue)
{
    VM& vm = *instance.vm;
    if (vm.isAsyncContextTrackingEnabled())
        instance.globalObject->m_asyncContextData->putInternalField(vm, 0, jsUndefined());
    reportErrorOfMicrotask(instance.globalObject, exceptionOrValue);
}

static void resumeWhenSettled(JSGlobalObject* globalObject, VM& vm, JSValue awaited, SavedFrame* waiter, InternalMicrotask task = InternalMicrotask::CompiledAsyncFunctionResume)
{
    if (!awaited.isObject()) {
        // The iterator that was stepped resumes the waiter.
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

static void resumeWhenCallSettles(Instance& instance, JSValue awaited, SavedFrame* waiter)
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
    waiter->queueResumeJob(globalObject, vm, awaited, false, asyncContext);
}

// The frame is that of an async function that returns next: the job is taken out of the queue, so no code of the program may run before the frame has returned to the adapter.
static bool tryScheduleImmediateResume(Instance& instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    constexpr uint16_t reactionMask = JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask;
    constexpr uint16_t resumeReaction = static_cast<uint16_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
        | static_cast<uint16_t>(InternalMicrotask::CompiledAsyncFunctionResume) << JSPromise::inlineReactionMicrotaskShift;
    CoroutineWords& coroutines = instance.coroutines;
    if (coroutines.waiterToResume || instance.awaitMustSuspend || coroutines.frameThatMayContinue != frame)
        return false;
    // Whoever resumed an async function resumes the waiter. Whoever enters the body of an async generator, which may go on too, does not.
    SavedFrame* running = SavedFrame::forFrame(instance, frame);
    if (!running || running->kind() != CoroutineKind::AsyncFunction)
        return false;
    if (resolution.isObject() && !isDefinitelyNonThenable(asObject(resolution), instance.globalObject))
        return false;
    if ((promise->flags() & reactionMask) != resumeReaction)
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

static NEVER_INLINE void resumeRemainingWaiters(Instance& instance)
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
        coroutines.nextEntryEndsJob = true;
        resumeSavedAsyncFunction(globalObject, vm, instance, saved, ResumeMode::Next, JSValue::decode(coroutines.waiterResolution));
    }
    coroutines.waiterResolution = JSValue::encode(JSValue());
    coroutines.waiterAsyncContext = JSValue::encode(JSValue());
}

// A step whose consumer is on the stack below the producer, was entered as the last thing of a job, and suspends next unless the step finishes first.
class AsyncGeneratorStepInProgress {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(AsyncGeneratorStepInProgress);
public:
    AsyncGeneratorStepInProgress(Instance& instance, JSObject* driver)
        : driver(driver)
        , m_innermostStep(instance.coroutines.stepInProgress)
        , m_enclosingStep(std::exchange(m_innermostStep, this))
    {
    }

    ~AsyncGeneratorStepInProgress() { m_innermostStep = m_enclosingStep; }

    AsyncGeneratorStep resultOr(JSValue awaited) const
    {
        if (!value)
            return { awaited, AsyncGeneratorStepStatus::Result };
        return { value, isDone ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded };
    }

    JSObject* const driver;
    // Empty until the step is finished.
    JSValue value;
    bool isDone { false };

private:
    AsyncGeneratorStepInProgress*& m_innermostStep;
    AsyncGeneratorStepInProgress* m_enclosingStep;
};

static AsyncGeneratorStepInProgress* findUnfinishedStep(JSValue settlementTarget)
{
    if (!settlementTarget.isObject())
        return nullptr;
    Instance* instance = Instance::of(asObject(settlementTarget)->realm());
    AsyncGeneratorStepInProgress* step = instance ? instance->coroutines.stepInProgress : nullptr;
    if (!step || step->driver != settlementTarget.asCell() || step->value || !nextJobRunsImmediately(*instance))
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
    ASSERT(value);
    step->value = value;
    step->isDone = isDone;
    return true;
}

// Room for the frames from the function that enters a body down to the frame of the body.
AOT_THRESHOLD(size_t, bytesOfStackClearedBelowCaller, 1024);

SUPPRESS_ASAN NEVER_INLINE void clearStackBelowCaller()
{
    std::array<uintptr_t, bytesOfStackClearedBelowCaller / sizeof(uintptr_t)> words;
    words.fill(0);
    ensureStillAliveHere(words.data());
}

AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator* generator, bool isEnteredByJobLoop)
{
    SavedFrame* saved = savedFrameIn(generator->frame());
    if (!saved)
        return { };
    Instance* instance = Instance::of(saved->realm());
    instance->coroutines.nextEntryEndsJob = isEnteredByJobLoop;
    // Only a body that RunAsyncGeneratorStep has entered goes on at an await above its consumer: the frame of that stub tells a stack trace that the consumer waits.
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    return willEnterAdapter(*instance, saved, generator, instance->coroutines.nextEntryEndsJob && instance->vm->isAsyncContextTrackingEnabled());
}

JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry& entry, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated)
{
    if (!entry.instance)
        return entered;
    Instance& instance = *entry.instance;
    auto [lastFrame, lastAsyncGenerator, promiseOfHandler] = didLeaveAdapter(entry);
    if (lastFrame && lastFrame->kind() == CoroutineKind::AsyncGenerator)
        return uncheckedDowncast<JSAsyncGenerator>(lastAsyncGenerator);
    instance.coroutines.frameThatMayContinue = nullptr;
    if (isTerminated)
        return nullptr;
    if (!lastFrame) {
        if (promiseOfHandler)
            settlePromiseOfHandler(*instance.vm, promiseOfHandler, returned, error);
        else if (error) [[unlikely]]
            reportErrorOfFunctionOfJob(instance, error);
    } else if (error) [[unlikely]] {
        EnsureStillAliveScope keepsLastFrame(lastFrame);
        lastFrame->reject(instance.globalObject, *instance.vm, error);
    }
    if (instance.coroutines.waiterToResume)
        resumeRemainingWaiters(instance);
    return nullptr;
}

static SavedFrame* savedFrameOfDriver(JSValue driver)
{
    if (auto* generator = dynamicDowncast<JSAsyncGenerator>(driver))
        return savedFrameIn(generator->frame());
    return savedFrameIn(driver);
}

bool takesBareSteps(JSValue settlementTarget)
{
    return savedFrameOfDriver(settlementTarget);
}

void deliverBareStepToDriver(JSObject* driver, bool isDone)
{
    savedFrameOfDriver(driver)->setStepStatus(isDone ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded);
}

JSValue deliverStepToDriver(JSObject* driver, JSValue resolution, bool isRejected)
{
    SavedFrame* saved = savedFrameOfDriver(driver);
    if (!saved || isRejected || !resolution.isObject())
        return resolution;
    // A "then" on Object.prototype can put any object in the place of the engine's. One of this Structure has both members as plain data.
    JSObject* result = asObject(resolution);
    if (result->structure() != result->realm()->iteratorResultObjectStructureConcurrently())
        return resolution;
    JSValue done = result->getDirect(iteratorResultObjectDonePropertyOffset);
    if (!done.isBoolean())
        return resolution;
    saved->setStepStatus(done.asBoolean() ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded);
    JSValue value = result->getDirect(iteratorResultObjectValuePropertyOffset);
    // While nobody has put a "then" where a result object finds it, the object is the one that the producer keeps for its next step (settleDriverWithIteratorResult in JSMicrotask.cpp), and nobody else has seen it.
    if (result->realm()->promiseThenWatchpointSet().isStillValid()) [[likely]]
        result->putDirectOffset(result->vm(), iteratorResultObjectValuePropertyOffset, jsUndefined());
    return value;
}

// The generator, if the frame is that of a body that the stub StepAsyncGenerator entered.
static JSAsyncGenerator* asyncGeneratorEnteredByStub(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.stubs.returnAddress(StubFrameKind::AsyncGeneratorBody))
        return nullptr;
    return std::bit_cast<JSAsyncGenerator* const*>(frame[0])[asyncGeneratorSlotInStubFrame];
}

// The SavedFrame that the frame was restored from, whoever restored it. A coroutine that has not been suspended yet has none.
static SavedFrame* coroutineOfFrame(const Instance& instance, const uint64_t* frame)
{
    if (SavedFrame* saved = SavedFrame::forFrame(instance, frame))
        return saved;
    JSAsyncGenerator* generator = asyncGeneratorEnteredByStub(instance, frame);
    return generator ? savedFrameOfGenerator(generator) : nullptr;
}

static JSObject* driverOf(const Instance& instance, SavedFrame* coroutine, const uint64_t* frame)
{
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1);
    switch (coroutine->kind()) {
    case CoroutineKind::AsyncFunction:
        return coroutine;
    case CoroutineKind::AsyncGenerator:
        if (JSAsyncGenerator* generator = asyncGeneratorEnteredByStub(instance, frame))
            return generator;
        return asObject(std::bit_cast<CallFrame*>(frame[0])->uncheckedArgument(0));
    case CoroutineKind::Generator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// With the status Result, the value is what the consumer has to await: the sentinel if the producer resumes it, else the promise of an iterator result.
enum class StepAnswer : uint8_t { ResumesDriver, IsPromise };

// The consumer is the only one that sees the promise, and it awaits the promise next. The job that a step of "for await" ends with stands where the job of that await would stand, and hands over the value and the status in the place of the iterator result.
static AsyncGeneratorStep awaitPromiseOfStep(Instance& instance, JSValue promise, JSObject* driver)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    // An await reads "constructor" of the promise where a program could tell.
    if (!driver || promise.asCell()->structureID() != globalObject->promiseStructure()->id() || globalObject->promiseSpeciesWatchpointSet().state() != IsWatched)
        return { promise, AsyncGeneratorStepStatus::Result };
    uncheckedDowncast<JSPromise>(promise)->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncGeneratorDriverResume, nullptr, driver, AsyncContextSwapScope::current(vm, globalObject));
    return { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
}

static AsyncGeneratorStep stepAsyncIterator(Instance& instance, const uint64_t* frame, SavedFrame* newSavedFrame, StepAnswer answer, JSObject* iterator, JSValue sent)
{
    JSGlobalObject* globalObject = instance.globalObject;
    MicrotaskCallCache* callCache = &instance.vm->syncResumeCallCache();
    // A coroutine that has not been suspended yet and got no SavedFrame for this step has nothing that the iterator could resume.
    SavedFrame* coroutine = newSavedFrame ? newSavedFrame : coroutineOfFrame(instance, frame);
    if (!coroutine || answer == StepAnswer::IsPromise) {
        auto scope = DECLARE_THROW_SCOPE(*instance.vm);
        auto* generator = dynamicDowncast<JSAsyncGenerator>(iterator);
        JSValue promise = generator ? asyncGeneratorNext(globalObject, generator, sent ? sent : jsUndefined(), callCache) : asyncFromSyncIteratorNext(globalObject, uncheckedDowncast<JSAsyncFromSyncIterator>(iterator), sent);
        RETURN_IF_EXCEPTION(scope, (AsyncGeneratorStep { JSValue(), AsyncGeneratorStepStatus::Result }));
        return awaitPromiseOfStep(instance, promise, coroutine ? driverOf(instance, coroutine, frame) : nullptr);
    }

    JSObject* driver = driverOf(instance, coroutine, frame);
    if (instance.coroutines.frameThatMayContinue != frame)
        return { asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache), AsyncGeneratorStepStatus::Result };

    AsyncGeneratorStepInProgress step(instance, driver);
    return step.resultOr(asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache));
}

static bool endOfBodyAnswersOtherRequestsFirst(const uint64_t* slotsOfStub, JSAsyncGenerator* generator)
{
    return slotsOfStub[consumerSlotInStubFrame] & consumerOfStepAwaitsCall && !generator->queue().isNull();
}

static JSPromise* tryPutPromiseInPlaceOfDriver(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator)
{
    if (generator->isQueueEmpty() || generator->resumePromise() != JSValue(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame])))
        return nullptr;
    JSPromise* promise = JSPromise::create(*instance.vm, instance.globalObject->promiseStructure());
    generator->setResumePromise(*instance.vm, promise);
    return promise;
}

static void rejectAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue error)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    JSPromise* promise = endOfBodyAnswersOtherRequestsFirst(slotsOfStub, generator) ? tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator) : nullptr;
    finishAsyncGeneratorBodyCall(globalObject, generator, JSValue(), error, &vm.syncResumeCallCache());
    if (!promise)
        return;
    auto* driver = std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame]);
    if (SavedFrame* saved = savedFrameIn(driver))
        resumeWhenSettled(globalObject, vm, promise, saved);
    else
        JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, promise, InternalMicrotask::AsyncGeneratorBodyCallNormal, driver);
}

static AsyncGeneratorStep finishAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue returned, bool consumerMayContinue)
{
    VM& vm = *instance.vm;
    bool isSeenTo = returned == JSValue(vm.fastAsyncGeneratorSentinel());
    if (slotsOfStub[consumerSlotInStubFrame] & consumerOfStepAwaitsPromise || (!isSeenTo && endOfBodyAnswersOtherRequestsFirst(slotsOfStub, generator))) {
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSPromise* promise = tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator);
        if (!isSeenTo)
            finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
        if (!promise || scope.exception())
            return { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
        return awaitPromiseOfStep(instance, promise, asObject(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame])));
    }
    ASSERT(!isSeenTo);
    // The request at the head of the queue is the one that the stub wrote, so it names the consumer.
    std::optional<AsyncGeneratorStepInProgress> step;
    if (consumerMayContinue)
        step.emplace(instance, asObject(generator->resumePromise()));
    finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
    return step ? step->resultOr(vm.fastAsyncGeneratorSentinel()) : AsyncGeneratorStep { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
}

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

// Only the restoring of a frame and a handler throw out of the adapter, so the exception is that of what was entered last, and nothing has run since the adapter named it.
static void finishAdapterEntry(VM& vm, const AsyncGeneratorBodyEntry& entry, TopExceptionScope& scope, JSValue returned)
{
    Instance& instance = *entry.instance;
    instance.coroutines.frameThatMayContinue = nullptr;
    auto [lastFrame, lastAsyncGenerator, promiseOfHandler] = didLeaveAdapter(entry);
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
            reportErrorOfFunctionOfJob(instance, exception);
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

static bool tryEnterFunctionOfJob(JSGlobalObject* globalObject, VM& vm, JSValue promise, JSValue function, JSValue argument, bool endsJob, bool asyncContextIsPutBack)
{
    if (!function.isCell() || !isCompiledCalleeType(function.asCell()->type()) || function.asCell()->structure()->realm() != globalObject)
        return false;
    Instance* instance = Instance::of(globalObject);
    if (!instance || !vm.isSafeToRecurseSoft()) [[unlikely]]
        return false;

    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2 && static_cast<int32_t>(JSGenerator::Argument::Value) == 3);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AsyncGeneratorBodyEntry entry = willEnterAdapter(*instance, nullptr, nullptr, asyncContextIsPutBack);
    instance->coroutines.nextEntryEndsJob = endsJob;
    CallData callData = CallData::forCompiledCode(instance->state->coroutines->handlerOfJobEntrypoint(), globalObject);
    JSValue returned = enterCompiledCode(vm, callData, asObject(function), jsUndefined(), function.asCell(), promise, function, argument, jsNumber(0));
    finishAdapterEntry(vm, entry, scope, returned);
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
    AsyncGeneratorBodyEntry entry = willEnterAdapter(instance, saved, nullptr, instance.coroutines.nextEntryEndsJob && vm.isAsyncContextTrackingEnabled());
    JSValue returned = enterSavedFrame(vm, instance, saved, mode, resolution);
    finishAdapterEntry(vm, entry, scope, returned);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewPromise, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewPromise);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
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
        if (!nextJobRunsImmediately(*instance) || !tryScheduleImmediateResume(*instance, frame, resolved, resolution))
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

enum class AwaitedKind : uint8_t { Value, ValueAfterTurn, ValueInModuleBody, ResultOfCall };

static constexpr InternalMicrotask resumingTaskOf(AwaitedKind kind)
{
    switch (kind) {
    case AwaitedKind::Value:
    case AwaitedKind::ResultOfCall:
        return InternalMicrotask::CompiledAsyncFunctionResume;
    case AwaitedKind::ValueAfterTurn:
        return InternalMicrotask::CompiledAsyncFunctionResumeAfterTurn;
    case AwaitedKind::ValueInModuleBody:
        return InternalMicrotask::CompiledModuleBodyResume;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<AwaitedKind kind>
static ALWAYS_INLINE JSCell* suspendUntilSettled(Instance* instance, VM& vm, ThrowScope& scope, EncodedJSValue awaited, SavedFrame* saved)
{
    if constexpr (kind == AwaitedKind::ResultOfCall)
        resumeWhenCallSettles(*instance, JSValue::decode(awaited), saved);
    else {
        resumeWhenSettled(instance->globalObject, vm, JSValue::decode(awaited), saved, resumingTaskOf(kind));
        if (scope.exception()) [[unlikely]]
            saved->willBeRestoredToStack();
    }
    return saved->promiseOrWaiter();
}

#define AOT_DEFINE_AWAIT_OPERATIONS(saveFrameAndAwait, awaitWithFrame, kind) \
    JSC_DEFINE_JIT_OPERATION(saveFrameAndAwait, JSCell*, (Instance* instance, EncodedJSValue awaited, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* suspensionWords)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::saveFrameAndAwait); \
        SavedFrame* saved = SavedFrame::saveAsyncFunction(vm, *instance, { frame, bottom, resumeAddress, reinterpret_cast<const SuspensionWords*>(suspensionWords) }); \
        OPERATION_RETURN(scope, suspendUntilSettled<kind>(instance, vm, scope, awaited, saved)); \
    } \
    JSC_DEFINE_JIT_OPERATION(awaitWithFrame, JSCell*, (Instance* instance, EncodedJSValue awaited, JSCell* savedFrame)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::awaitWithFrame); \
        SavedFrame* saved = cellArgument<SavedFrame>(savedFrame, Entry::awaitWithFrame); \
        vm.writeBarrier(saved); \
        OPERATION_RETURN(scope, suspendUntilSettled<kind>(instance, vm, scope, awaited, saved)); \
    }

AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwait, operationAOTAwaitWithSavedFrame, AwaitedKind::Value)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitAfterTurn, operationAOTAwaitWithSavedFrameAfterTurn, AwaitedKind::ValueAfterTurn)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitInModuleBody, operationAOTAwaitWithSavedFrameInModuleBody, AwaitedKind::ValueInModuleBody)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitCall, operationAOTAwaitCallWithSavedFrame, AwaitedKind::ResultOfCall)
#undef AOT_DEFINE_AWAIT_OPERATIONS

static JSObject* asyncIteratorArgument(JSCell* iterator, Entry entry)
{
    if (!iterator || (!iterator->inherits<JSAsyncGenerator>() && !iterator->inherits<JSAsyncFromSyncIterator>())) [[unlikely]]
        crashOnWrongOperand(entry);
    return asObject(iterator);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAsyncIteratorStep, EncodedJSValue, (Instance* instance, JSCell* iterator, EncodedJSValue sent))
{
    constexpr Entry entry = Entry::operationAOTAsyncIteratorStep;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* frame = std::bit_cast<const uint64_t*>(callFrame->callerFrame());
    auto* slotsOfStub = std::bit_cast<uint64_t*>(callFrame);
    uint64_t& consumer = slotsOfStub[consumerSlotInStubFrame];
    if (consumer & consumerOfStepNeedsSavedFrame) {
        // The stack of the consumer ends where the frame record of the stub begins.
        SuspensionWords words { slotsOfStub[siteWordSlotInStubFrame], slotsOfStub[driverSlotInStubFrame] };
        SavedFrame* made = SavedFrame::createForAsyncFunction(vm, *instance, { const_cast<uint64_t*>(frame), slotsOfStub + 2, nullptr, &words });
        slotsOfStub[driverSlotInStubFrame] = std::bit_cast<uint64_t>(made);
        consumer = (consumer & ~static_cast<uint64_t>(consumerOfStepNeedsSavedFrame)) | consumerOfStepHasNewSavedFrame;
    }
    SavedFrame* newSavedFrame = consumer & consumerOfStepHasNewSavedFrame ? std::bit_cast<SavedFrame*>(slotsOfStub[driverSlotInStubFrame]) : nullptr;
    constexpr uint64_t awaitsWhatNextReturns = consumerOfStepAwaitsPromise | consumerOfStepAwaitsCall;
    StepAnswer answer = consumer & awaitsWhatNextReturns ? StepAnswer::IsPromise : StepAnswer::ResumesDriver;
    AsyncGeneratorStep step = stepAsyncIterator(*instance, frame, newSavedFrame, answer, asyncIteratorArgument(iterator, entry), JSValue::decode(sent));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    instance->coroutines.stepStatus = static_cast<uint32_t>(step.status);
    OPERATION_RETURN(scope, JSValue::encode(step.value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFinishAsyncIteratorStep, EncodedJSValue, (Instance* instance, JSCell* generator, EncodedJSValue returned, uint32_t consumerMayContinue))
{
    constexpr Entry entry = Entry::operationAOTFinishAsyncIteratorStep;
    AOT_OPERATION_BEGIN(instance, entry);
    AsyncGeneratorStep step = finishAsyncGeneratorStep(*instance, std::bit_cast<const uint64_t*>(callFrame), cellArgument<JSAsyncGenerator>(generator, entry), JSValue::decode(returned), consumerMayContinue);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    instance->coroutines.stepStatus = static_cast<uint32_t>(step.status);
    OPERATION_RETURN(scope, JSValue::encode(step.value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRejectWithException, JSCell*, (Instance* instance, uint64_t frameBits))
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
    auto* frame = std::bit_cast<const uint64_t*>(frameBits);
    if (JSAsyncGenerator* generator = asyncGeneratorEnteredByStub(*instance, frame)) {
        rejectAsyncGeneratorStep(*instance, std::bit_cast<const uint64_t*>(frame[0]), generator, value);
        OPERATION_RETURN(scope, vm.fastAsyncGeneratorSentinel());
    }
    SavedFrame* saved = SavedFrame::forFrame(*instance, frame);
    if (!saved) {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->reject(vm, value);
        OPERATION_RETURN(scope, promise);
    }
    switch (saved->kind()) {
    case CoroutineKind::AsyncFunction:
        saved->reject(globalObject, vm, value);
        OPERATION_RETURN(scope, saved->promiseOrWaiter());
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator:
        if (exception)
            throwException(globalObject, scope, exception);
        else
            throwException(globalObject, scope, value);
        OPERATION_RETURN(scope, nullptr);
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

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseAll, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* elements, uint32_t directCalls))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseAll);
    OPERATION_RETURN(scope, promiseAllOfElements(globalObject, std::span { elements, count }, directCalls));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewGenerator, JSCell*, (Instance* instance, EncodedJSValue closure, uint32_t isAsync))
{
    constexpr Entry entry = Entry::operationAOTNewGenerator;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* generatorFunction = objectArgument(closure, entry);
    JSObject* generator = createGenerator(globalObject, *instance, generatorFunction, isAsync ? CoroutineKind::AsyncGenerator : CoroutineKind::Generator);
    // The prototype property of a generator function is a data property that cannot be reconfigured, so reading it runs no code.
    scope.releaseAssertNoException();
    return generator;
}

static CoroutineKind generatorKind(JSObject* generator, Entry entry)
{
    if (generator->inherits<Generator>())
        return CoroutineKind::Generator;
    if (!generator->inherits<JSAsyncGenerator>()) [[unlikely]]
        crashOnWrongOperand(entry);
    return CoroutineKind::AsyncGenerator;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTakeGeneratorPrototype, void, (Instance* instance, JSCell* generator, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTTakeGeneratorPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* object = cellArgument<JSObject>(generator, entry);
    Structure* structure = structureOfGenerator(globalObject, *instance, objectArgument(closure, entry), generatorKind(object, entry));
    scope.releaseAssertNoException();
    if (object->structure() != structure)
        object->setStructure(vm, structure);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSaveFrameAndYield, JSCell*, (Instance* instance, JSCell* generator, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* suspensionWords))
{
    constexpr Entry entry = Entry::operationAOTSaveFrameAndYield;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = cellArgument<JSObject>(generator, entry);
    CoroutineKind kind = generatorKind(object, entry);
    RELEASE_ASSERT(!savedFrameOfGenerator(object));
    SuspendedMachineFrame machineFrame { frame, bottom, resumeAddress, reinterpret_cast<const SuspensionWords*>(suspensionWords) };
    SavedFrame* saved = SavedFrame::create(vm, *instance, kind, machineFrame);
    saved->save(vm, machineFrame, SuspensionKind::Yield);
    setSavedFrameOfGenerator(vm, object, saved);
    OPERATION_RETURN(scope, saved);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
