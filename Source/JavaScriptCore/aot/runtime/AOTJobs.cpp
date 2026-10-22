/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJobs.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTGenerator.h"
#include "AOTSavedFrame.h"
#include "AOTStubFrames.h"
#include "AsyncContextSwapScope.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSPromise.h"
#include "JSSentinel.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static ALWAYS_INLINE bool isSavedFrameOf(const Instance& instance, JSValue value)
{
    return value && value.isCell() && value.asCell()->structureID().bits() == static_cast<uint32_t>(instance.coroutines.savedFrameHeader);
}

// Of any realm.
static ALWAYS_INLINE SavedFrame* savedFrameIn(JSValue value)
{
    return value && value.isCell() && value.asCell()->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(value.asCell()) : nullptr;
}

void resumeWhenSettled(JSGlobalObject* globalObject, VM& vm, JSValue awaited, SavedFrame* waiter, InternalMicrotask task)
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
        coroutines.nextEntryEndsJob = true;
        resumeSavedAsyncFunction(globalObject, vm, instance, saved, ResumeMode::Next, JSValue::decode(coroutines.waiterResolution));
    }
    coroutines.waiterResolution = JSValue::encode(JSValue());
    coroutines.waiterAsyncContext = JSValue::encode(JSValue());
}

AsyncGeneratorStepInProgress::AsyncGeneratorStepInProgress(Instance& instance, JSObject* driver, JSObject* producer)
    : m_instance(instance)
    , m_enclosingStep(instance.coroutines.stepInProgress)
    , m_driver(driver)
    , m_producer(producer)
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

AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator* generator, bool isEnteredByJobLoop)
{
    SavedFrame* saved = savedFrameIn(generator->frame());
    if (!saved)
        return { };
    Instance* instance = Instance::of(saved->realm());
    instance->coroutines.nextEntryEndsJob = isEnteredByJobLoop;
    if (!isEnteredByJobLoop) {
        // The consumer of an unfinished step suspends as soon as the generator does.
        AsyncGeneratorStepInProgress* step = instance->coroutines.stepInProgress;
        isEnteredByJobLoop = step && step->producer() == generator && !step->isFinished();
    }
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    return { instance, std::exchange(instance->coroutines.enteredFrame, saved), std::exchange(instance->coroutines.enteredAsyncGenerator, generator), std::exchange(instance->coroutines.promiseOfEnteredHandler, nullptr),
        !!std::exchange(instance->coroutines.asyncContextIsPutBack, instance->coroutines.nextEntryEndsJob && instance->vm->isAsyncContextTrackingEnabled()) };
}

JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry& entry, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated)
{
    if (!entry.instance)
        return entered;
    Instance& instance = *entry.instance;
    auto* lastFrame = static_cast<SavedFrame*>(std::exchange(instance.coroutines.enteredFrame, entry.outerFrame));
    JSCell* lastAsyncGenerator = std::exchange(instance.coroutines.enteredAsyncGenerator, entry.outerAsyncGenerator);
    JSCell* promiseOfHandler = std::exchange(instance.coroutines.promiseOfEnteredHandler, entry.outerPromiseOfHandler);
    instance.coroutines.asyncContextIsPutBack = entry.outerAsyncContextIsPutBack;
    if (lastFrame && lastFrame->kind() == CoroutineKind::AsyncGenerator)
        return uncheckedDowncast<JSAsyncGenerator>(lastAsyncGenerator);
    instance.coroutines.frameThatMayContinue = nullptr;
    if (isTerminated)
        return nullptr;
    if (!lastFrame) {
        if (promiseOfHandler)
            settlePromiseOfHandler(*instance.vm, promiseOfHandler, returned, error);
        else if (error) [[unlikely]]
            reportErrorOfMicrotask(instance.globalObject, error);
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
    // While nobody has put a "then" where a result object finds it, the object is the one that the producer keeps for its next step (settleDriverWithIteratorResult in JSMicrotask.cpp),
    // and nobody else has seen it. It would keep the value alive until that step.
    if (result->realm()->promiseThenWatchpointSet().isStillValid()) [[likely]]
        result->putDirectOffset(result->vm(), iteratorResultObjectValuePropertyOffset, jsUndefined());
    return value;
}

JSAsyncGenerator* asyncGeneratorEnteredByStub(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.stubs.returnAddress(StubFrameKind::AsyncGeneratorBody))
        return nullptr;
    return std::bit_cast<JSAsyncGenerator* const*>(frame[0])[asyncGeneratorSlotInStubFrame];
}

SavedFrame* coroutineOfFrame(const Instance& instance, const uint64_t* frame)
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

AsyncGeneratorStep stepAsyncIterator(Instance& instance, const uint64_t* frame, SavedFrame* newSavedFrame, StepAnswer answer, JSObject* iterator, JSValue sent)
{
    JSGlobalObject* globalObject = instance.globalObject;
    MicrotaskCallCache* callCache = &instance.vm->syncResumeCallCache();
    // A coroutine that has not been suspended yet and got no SavedFrame for this step has nothing that the iterator could resume.
    SavedFrame* coroutine = newSavedFrame ? newSavedFrame : coroutineOfFrame(instance, frame);
    if (!coroutine || answer == StepAnswer::IsPromise) {
        if (auto* generator = dynamicDowncast<JSAsyncGenerator>(iterator))
            return { asyncGeneratorNext(globalObject, generator, sent ? sent : jsUndefined(), callCache), AsyncGeneratorStepStatus::Result };
        return { asyncFromSyncIteratorNext(globalObject, uncheckedDowncast<JSAsyncFromSyncIterator>(iterator), sent), AsyncGeneratorStepStatus::Result };
    }

    JSObject* driver = driverOf(instance, coroutine, frame);
    if (instance.coroutines.frameThatMayContinue != frame)
        return { asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache), AsyncGeneratorStepStatus::Result };

    AsyncGeneratorStepInProgress step(instance, driver, iterator);
    JSValue awaited = asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache);
    if (!step.isFinished())
        return { awaited, AsyncGeneratorStepStatus::Result };
    return { step.value(), step.isDone() ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded };
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

void rejectAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue error)
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

AsyncGeneratorStep finishAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue returned, bool consumerMayContinue)
{
    VM& vm = *instance.vm;
    bool isSeenTo = returned == JSValue(vm.fastAsyncGeneratorSentinel());
    if (slotsOfStub[consumerSlotInStubFrame] & consumerOfStepAwaitsPromise || (!isSeenTo && endOfBodyAnswersOtherRequestsFirst(slotsOfStub, generator))) {
        JSPromise* promise = tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator);
        if (!isSeenTo)
            finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
        return { promise ? JSValue(promise) : JSValue(vm.fastAsyncGeneratorSentinel()), AsyncGeneratorStepStatus::Result };
    }
    ASSERT(!isSeenTo);
    // The request at the head of the queue is the one that the stub wrote, so it names the consumer.
    std::optional<AsyncGeneratorStepInProgress> step;
    if (consumerMayContinue)
        step.emplace(instance, asObject(generator->resumePromise()), generator);
    finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
    if (!step || !step->isFinished())
        return { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
    return { step->value(), step->isDone() ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded };
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
