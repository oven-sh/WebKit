/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTAsyncResumption.h"
#include "AOTCompiledCall.h"
#include "AOTCoroutineTable.h"
#include "AOTCoroutineWords.h"
#include "AOTGenerator.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTReactionChain.h"
#include "AOTRuntimeReporter.h"
#include "AOTSavedFrame.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTStubFrames.h"
#include "AOTStubsCoroutines.h"
#include "AOTUnwinding.h"
#include "AsyncContextSwapScope.h"
#include "DeferTermination.h"
#include "EnsureStillAliveHere.h"
#include "ExceptionHelpers.h"
#include "GeneratorPrototype.h"
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
#include "MicrotaskCall.h"
#include "MicrotaskQueueInlines.h"
#include "OwedOutOfMemoryError.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#include <array>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/StackPointer.h>

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

static ALWAYS_INLINE SavedFrame* savedFrameIn(JSValue value)
{
    return value && value.isCell() && value.asCell()->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(value.asCell()) : nullptr;
}

static void markStepsWhoseConsumerHasAwaited(const Instance&, const uint64_t* frame);
static JSAsyncGenerator* asyncGeneratorEnteredByStub(const Instance&, const uint64_t* frame);

static ALWAYS_INLINE bool mustSuspendAtNextAwait(const uint64_t* frame) { return std::bit_cast<uintptr_t>(frame) & mustSuspendAtNextAwaitBit; }
static ALWAYS_INLINE const uint64_t* withoutBit(const uint64_t* frame) { return std::bit_cast<const uint64_t*>(std::bit_cast<uintptr_t>(frame) & ~static_cast<uintptr_t>(mustSuspendAtNextAwaitBit)); }

static const uint64_t* resumableFrame(const Instance& instance)
{
    const uint64_t* frame = instance.coroutines.resumableFrame;
    return mustSuspendAtNextAwait(frame) ? nullptr : frame;
}

static const uint64_t* withoutResumptionRight(const Instance& instance, const uint64_t* frame)
{
    const uint64_t* bare = withoutBit(frame);
    if (!bare || asyncGeneratorEnteredByStub(instance, bare))
        return nullptr;
    return std::bit_cast<const uint64_t*>(std::bit_cast<uintptr_t>(bare) | mustSuspendAtNextAwaitBit);
}

const uint64_t* frameWithAwaitingCallers(const Instance& instance)
{
    const uint64_t* frame = withoutBit(instance.coroutines.resumableFrame);
    if (!frame) [[likely]]
        return nullptr;
    const void* returnAddress = std::bit_cast<const void*>(frame[1]);
    if (returnAddress == instance.stubs.returnAddress(StubFrameKind::SavedFrameAdapter) || returnAddress == instance.stubs.returnAddress(StubFrameKind::AsyncGeneratorBody))
        return nullptr;
    return frame;
}

static AdapterEntry willEnterAdapter(Instance& instance, JSCell* frame, JSCell* asyncGenerator, bool asyncContextIsPutBack)
{
    instance.vm->didEnterVM = true;
    if (!instance.vm->isAsyncContextTrackingEnabled())
        markStepsWhoseConsumerHasAwaited(instance, resumableFrame(instance));
    CoroutineWords& coroutines = instance.coroutines;
    return { std::exchange(coroutines.enteredFrame, frame), std::exchange(coroutines.enteredAsyncGenerator, asyncGenerator), std::exchange(coroutines.promiseOfEnteredHandler, nullptr), !!std::exchange(coroutines.asyncContextIsPutBack, asyncContextIsPutBack), withoutResumptionRight(instance, coroutines.resumableFrame) };
}

struct LastEnteredByAdapter {
    SavedFrame* frame;
    JSCell* asyncGenerator;
    JSCell* promiseOfHandler;
    bool handlerReturnsFulfillment;
};

static LastEnteredByAdapter didLeaveAdapter(Instance& instance, const AdapterEntry& entry)
{
    CoroutineWords& coroutines = instance.coroutines;
    coroutines.asyncContextIsPutBack = entry.outerAsyncContextIsPutBack;
    coroutines.resumableFrame = entry.resumableOuterFrame;
    return { static_cast<SavedFrame*>(std::exchange(coroutines.enteredFrame, entry.outerFrame)), std::exchange(coroutines.enteredAsyncGenerator, entry.outerAsyncGenerator), std::exchange(coroutines.promiseOfEnteredHandler, entry.outerPromiseOfHandler), !!std::exchange(coroutines.handlerReturnsFulfillment, 0) };
}

static JSValue promiseReturnedByHandler(Instance& instance, JSValue fulfillment)
{
    VM& vm = *instance.vm;
    JSPromise* promise = JSPromise::create(vm, instance.globalObject->promiseStructure());
    promise->fulfillPromise(vm, fulfillment);
    return promise;
}

static void reportErrorOfFunctionOfJob(Instance& instance, JSValue exceptionOrValue)
{
    VM& vm = *instance.vm;
    if (vm.isAsyncContextTrackingEnabled())
        instance.globalObject->m_asyncContextData->putInternalField(vm, 0, jsUndefined());
    reportErrorOfMicrotask(instance.globalObject, exceptionOrValue);
}

static void resumeWhenSettled(JSGlobalObject* globalObject, VM& vm, JSValue awaited, SavedFrame* waiter, InternalMicrotask task)
{
    if (!awaited.isObject()) {
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

static bool frameEndsJob(const Instance& instance, const uint64_t* frame)
{
    return nextJobRunsImmediately(instance) && resumableFrame(instance) == frame;
}

static bool isUnmodifiedPromise(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && value.asCell()->structureID() == globalObject->promiseStructure()->id() && globalObject->promiseThenWatchpointSet().isStillValid()
        && globalObject->promiseSpeciesWatchpointSet().state() == IsWatched;
}

static void resolveWithReturned(Instance& instance, VM& vm, JSPromise* promise, JSValue resolution)
{
    JSGlobalObject* globalObject = instance.globalObject;
    if (!isUnmodifiedPromise(globalObject, resolution)) {
        promise->resolve(globalObject, vm, resolution);
        return;
    }
    void*& lastResolved = instance.coroutines.lastPromiseResolvedWithSettled;
    if (nextJobRunsImmediately(instance) ? uncheckedDowncast<JSPromise>(resolution)->status() == JSPromise::Status::Fulfilled : lastResolved == resolution.asCell())
        lastResolved = promise;
    promise->resolve(globalObject, vm, resolution);
}

static void setResumeReaction(VM& vm, JSPromise* promise, SavedFrame* waiter)
{
    auto* bytes = std::bit_cast<uint8_t*>(promise);
    *std::bit_cast<EncodedJSValue*>(bytes + JSPromise::offsetOfSlot()) = JSValue::encode(waiter);
    *std::bit_cast<uint64_t*>(bytes + JSPromise::offsetOfPacked()) = resumeReactionBits;
    vm.writeBarrier(promise, waiter);
}

static JSValue settledValueOf(JSGlobalObject* globalObject, JSPromise* promise)
{
    if (promise->status() != JSPromise::Status::Fulfilled)
        return { };
    JSValue value = promise->settlementValue();
    return value.isObject() && !isDefinitelyNonThenable(asObject(value), globalObject) ? JSValue() : value;
}

static void resumeWhenResolutionSettles(Instance& instance, VM& vm, ThrowScope& scope, JSValue resolution, SavedFrame* waiter, bool isEndOfJob)
{
    JSGlobalObject* globalObject = instance.globalObject;
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    JSValue asyncContext = AsyncContextSwapScope::current(vm, globalObject);
    if (!isUnmodifiedPromise(globalObject, resolution)) [[unlikely]] {
        promise->resolve(globalObject, vm, resolution);
        if (scope.exception()) [[unlikely]] {
            waiter->willBeRestoredToStack();
            return;
        }
    } else if (isEndOfJob)
        uncheckedDowncast<JSPromise>(resolution)->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, promise, jsUndefined());
    else
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJobFast, 0, resolution, promise, asyncContext);
    if (promise->status() == JSPromise::Status::Pending && !vm.isAsyncContextTrackingEnabled()) [[likely]]
        setResumeReaction(vm, promise, waiter);
    else
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, nullptr, waiter, asyncContext);
}

static bool tryScheduleImmediateResume(Instance& instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    constexpr uint16_t reactionMask = JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask;
    constexpr uint16_t resumeReaction = static_cast<uint16_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
        | static_cast<uint16_t>(InternalMicrotask::CompiledAsyncFunctionResume) << JSPromise::inlineReactionMicrotaskShift;
    CoroutineWords& coroutines = instance.coroutines;
    if (coroutines.waiterToResume || instance.awaitMustSuspend || coroutines.resumableFrame != frame)
        return false;
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

JSValue tryAwaitInCurrentStep(JSGlobalObject* globalObject, JSValue settlementTarget, JSValue awaited)
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

bool tryFinishCurrentStep(JSValue settlementTarget, JSValue value, bool isDone)
{
    AsyncGeneratorStepInProgress* step = findUnfinishedStep(settlementTarget);
    if (!step)
        return false;
    ASSERT(value);
    step->value = value;
    step->isDone = isDone;
    return true;
}

AsyncGeneratorBodyEntry willEnterAsyncGeneratorBody(JSAsyncGenerator* generator, bool isEnteredByJobLoop)
{
    SavedFrame* saved = savedFrameIn(generator->frame());
    if (!saved)
        return { };
    Instance* instance = Instance::owning(saved);
    instance->coroutines.nextEntryEndsJob = isEnteredByJobLoop;
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    return { instance, willEnterAdapter(*instance, saved, generator, instance->coroutines.nextEntryEndsJob && instance->vm->isAsyncContextTrackingEnabled()) };
}

JSAsyncGenerator* didReturnFromAsyncGeneratorBody(const AsyncGeneratorBodyEntry& entry, JSAsyncGenerator* entered, JSValue returned, JSValue error, bool isTerminated)
{
    if (!entry.instance)
        return entered;
    Instance& instance = *entry.instance;
    auto [lastFrame, lastAsyncGenerator, promiseOfHandler, handlerReturnsFulfillment] = didLeaveAdapter(instance, entry.adapterEntry);
    if (lastFrame && lastFrame->kind() == CoroutineKind::AsyncGenerator)
        return uncheckedDowncast<JSAsyncGenerator>(lastAsyncGenerator);
    if (isTerminated)
        return nullptr;
    if (!lastFrame) {
        if (promiseOfHandler)
            settlePromiseOfHandler(*instance.vm, promiseOfHandler, handlerReturnsFulfillment ? promiseReturnedByHandler(instance, returned) : returned, error);
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

JSValue driverResumeContext(JSValue settlementTarget)
{
    SavedFrame* saved = savedFrameOfDriver(settlementTarget);
    return saved && saved->goesOnInContextOfItsAwait() ? saved->contextOfAwait() : JSValue();
}

bool acceptsBareSteps(JSValue settlementTarget)
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
    JSObject* result = asObject(resolution);
    if (result->structure() != result->realm()->iteratorResultObjectStructureConcurrently())
        return resolution;
    JSValue done = result->getDirect(iteratorResultObjectDonePropertyOffset);
    if (!done.isBoolean())
        return resolution;
    saved->setStepStatus(done.asBoolean() ? AsyncGeneratorStepStatus::Completed : AsyncGeneratorStepStatus::Yielded);
    JSValue value = result->getDirect(iteratorResultObjectValuePropertyOffset);
    if (result->realm()->promiseThenWatchpointSet().isStillValid()) [[likely]]
        result->putDirectOffset(result->vm(), iteratorResultObjectValuePropertyOffset, jsUndefined());
    return value;
}

static JSAsyncGenerator* asyncGeneratorEnteredByStub(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.stubs.returnAddress(StubFrameKind::AsyncGeneratorBody))
        return nullptr;
    return std::bit_cast<JSAsyncGenerator* const*>(frame[0])[asyncGeneratorSlotInStubFrame];
}

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

enum class StepAnswer : uint8_t { ResumesDriver, IsPromise };

static AsyncGeneratorStep awaitPromiseOfStep(Instance& instance, JSValue promise, JSObject* driver)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    if (!driver || promise.asCell()->structureID() != globalObject->promiseStructure()->id() || globalObject->promiseSpeciesWatchpointSet().state() != IsWatched)
        return { promise, AsyncGeneratorStepStatus::Result };
    uncheckedDowncast<JSPromise>(promise)->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncGeneratorDriverResume, nullptr, driver, AsyncContextSwapScope::current(vm, globalObject));
    return { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
}

static AsyncGeneratorStep stepAsyncIterator(Instance& instance, const uint64_t* frame, SavedFrame* newSavedFrame, StepAnswer answer, JSObject* iterator, JSValue sent)
{
    JSGlobalObject* globalObject = instance.globalObject;
    MicrotaskCallCache* callCache = &instance.vm->syncResumeCallCache();
    SavedFrame* coroutine = newSavedFrame ? newSavedFrame : coroutineOfFrame(instance, frame);
    if (!coroutine || answer == StepAnswer::IsPromise) {
        auto scope = DECLARE_THROW_SCOPE(*instance.vm);
        auto* generator = dynamicDowncast<JSAsyncGenerator>(iterator);
        JSValue promise = generator ? asyncGeneratorNext(globalObject, generator, sent ? sent : jsUndefined(), callCache) : asyncFromSyncIteratorNext(globalObject, uncheckedDowncast<JSAsyncFromSyncIterator>(iterator), sent);
        RETURN_IF_EXCEPTION(scope, (AsyncGeneratorStep { JSValue(), AsyncGeneratorStepStatus::Result }));
        return awaitPromiseOfStep(instance, promise, coroutine ? driverOf(instance, coroutine, frame) : nullptr);
    }

    JSObject* driver = driverOf(instance, coroutine, frame);
    if (instance.coroutines.resumableFrame != frame)
        return { asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache), AsyncGeneratorStepStatus::Result };

    AsyncGeneratorStepInProgress step(instance, driver);
    return step.resultOr(asyncIteratorNextWithDriver(globalObject, iterator, driver, sent, callCache));
}

static JSPromise* tryPutPromiseInPlaceOfDriver(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator)
{
    if (!(slotsOfStub[consumerSlotInStubFrame] & consumerOfStepAwaitsCall))
        return nullptr;
    if (generator->isQueueEmpty() || generator->resumePromise() != JSValue(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame])))
        return nullptr;
    JSPromise* promise = JSPromise::create(*instance.vm, instance.globalObject->promiseStructure());
    generator->setResumePromise(*instance.vm, promise);
    return promise;
}

static bool consumerHasAwaitedInFrontOfEnd(const uint64_t* slotsOfStub, JSAsyncGenerator* generator)
{
    if (slotsOfStub[consumerSlotInStubFrame] & consumerOfStepHasAwaited)
        return true;
    if (int32_t state = generator->state(); !JSAsyncGenerator::isSuspendedYieldState(state) && !JSAsyncGenerator::isExecutingState(state))
        return true;
    SavedFrame* body = savedFrameIn(generator->frame());
    return body && body->hasPassedAwaitOnStack();
}

static void noteThatConsumerHasPassedAwait(const uint64_t* slotsOfStub)
{
    auto* consumer = dynamicDowncast<JSAsyncGenerator>(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame]));
    if (SavedFrame* frame = consumer ? savedFrameIn(consumer->frame()) : nullptr)
        frame->notePassedAwaitOnStack();
}

static AsyncGeneratorStep awaitPromiseInPlaceOfDriver(Instance& instance, const uint64_t* slotsOfStub, JSPromise* promise)
{
    std::optional<AsyncContextSwapScope> contextAtAwait;
    if (slotsOfStub[consumerSlotInStubFrame] & consumerOfStepHasAwaited)
        contextAtAwait.emplace(*instance.vm, instance.globalObject, jsUndefined());
    return awaitPromiseOfStep(instance, promise, asObject(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame])));
}

static void markStepsWhoseConsumerHasAwaited(const Instance& instance, const uint64_t* frame)
{
    ASSERT(!frame || std::bit_cast<uintptr_t>(frame) > std::bit_cast<uintptr_t>(currentStackPointer()));
    bool hasAwaited = false;
    while (JSAsyncGenerator* generator = frame ? asyncGeneratorEnteredByStub(instance, frame) : nullptr) {
        SavedFrame* body = savedFrameIn(generator->frame());
        hasAwaited |= body && body->hasPassedAwaitOnStack();
        auto* slotsOfStub = std::bit_cast<uint64_t*>(frame[0]);
        if (hasAwaited)
            slotsOfStub[consumerSlotInStubFrame] |= consumerOfStepHasAwaited;
        frame = std::bit_cast<const uint64_t*>(slotsOfStub[0]);
    }
}

void takeAwayRightToContinue(Instance& instance)
{
    const uint64_t* frame = resumableFrame(instance);
    markStepsWhoseConsumerHasAwaited(instance, frame);
    if (frame)
        instance.coroutines.resumableFrame = withoutResumptionRight(instance, frame);
}

static void rejectAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue error)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    JSPromise* promise = tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator);
    auto awaitPromise = [&] {
        std::optional<AsyncContextSwapScope> contextAtAwait;
        if (slotsOfStub[consumerSlotInStubFrame] & consumerOfStepHasAwaited)
            contextAtAwait.emplace(vm, globalObject, jsUndefined());
        auto* driver = std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame]);
        if (SavedFrame* saved = savedFrameIn(driver))
            resumeWhenSettled(globalObject, vm, promise, saved, InternalMicrotask::CompiledAsyncFunctionResume);
        else
            JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, promise, InternalMicrotask::AsyncGeneratorBodyCallNormal, driver);
    };
    bool hasAwaitedInFront = promise && consumerHasAwaitedInFrontOfEnd(slotsOfStub, generator);
    if (hasAwaitedInFront) {
        noteThatConsumerHasPassedAwait(slotsOfStub);
        awaitPromise();
    }
    finishAsyncGeneratorBodyCall(globalObject, generator, JSValue(), error, &vm.syncResumeCallCache());
    if (promise && !hasAwaitedInFront)
        awaitPromise();
}

static bool mayWaitAsDriver(const Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator)
{
    return !instance.vm->isAsyncContextTrackingEnabled() && generator->queue().isNull() && generator->resumePromise() == JSValue(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame]))
        && generator->realm()->promiseThenWatchpointSet().isStillValid()
        && instance.globalObject->promiseSpeciesWatchpointSet().state() == IsWatched;
}

static AsyncGeneratorStep finishAsyncGeneratorStep(Instance& instance, const uint64_t* slotsOfStub, JSAsyncGenerator* generator, JSValue returned, bool consumerMayContinue)
{
    VM& vm = *instance.vm;
    bool isSeenTo = returned == JSValue(vm.fastAsyncGeneratorSentinel());
    const bool awaitsCall = slotsOfStub[consumerSlotInStubFrame] & consumerOfStepAwaitsCall;
    if (awaitsCall && (isSeenTo || !mayWaitAsDriver(instance, slotsOfStub, generator))) {
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSPromise* promise = tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator);
        std::optional<AsyncGeneratorStep> awaitedInFront;
        if (promise && !isSeenTo && consumerHasAwaitedInFrontOfEnd(slotsOfStub, generator)) {
            noteThatConsumerHasPassedAwait(slotsOfStub);
            awaitedInFront = awaitPromiseInPlaceOfDriver(instance, slotsOfStub, promise);
        }
        if (!isSeenTo)
            finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
        if (scope.exception() || !promise)
            return { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
        return awaitedInFront ? *awaitedInFront : awaitPromiseInPlaceOfDriver(instance, slotsOfStub, promise);
    }
    RELEASE_ASSERT(!isSeenTo);
    SavedFrame* consumer = awaitsCall ? savedFrameOfDriver(JSValue(std::bit_cast<JSCell*>(slotsOfStub[driverSlotInStubFrame]))) : nullptr;
    RELEASE_ASSERT(!awaitsCall || consumer);
    const bool hasAwaitedInFront = consumer && consumerHasAwaitedInFrontOfEnd(slotsOfStub, generator);
    if (hasAwaitedInFront)
        noteThatConsumerHasPassedAwait(slotsOfStub);
    if (consumer)
        consumer->setGoesOnInContextOfItsAwait(true);
    std::optional<AsyncGeneratorStepInProgress> step;
    if (consumerMayContinue)
        step.emplace(instance, asObject(generator->resumePromise()));
    finishAsyncGeneratorBodyCall(instance.globalObject, generator, returned, JSValue(), &vm.syncResumeCallCache());
    AsyncGeneratorStep answer = step ? step->resultOr(vm.fastAsyncGeneratorSentinel()) : AsyncGeneratorStep { vm.fastAsyncGeneratorSentinel(), AsyncGeneratorStepStatus::Result };
    if (!consumer)
        return answer;
    if (answer.status != AsyncGeneratorStepStatus::Result) {
        consumer->setGoesOnInContextOfItsAwait(false);
        return answer;
    }
    if (!hasAwaitedInFront && vm.isAsyncContextTrackingEnabled() && !vm.hasPendingTerminationException()) [[unlikely]] {
        if (JSPromise* promise = tryPutPromiseInPlaceOfDriver(instance, slotsOfStub, generator)) {
            consumer->setGoesOnInContextOfItsAwait(false);
            return awaitPromiseInPlaceOfDriver(instance, slotsOfStub, promise);
        }
    }
    return answer;
}

JSValue enterSavedFrame(VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue sent)
{
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2);
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Value) == 3 && static_cast<int32_t>(JSGenerator::Argument::ResumeMode) == 4);
    ASSERT(&instance == Instance::owning(saved));
    if (!instance.vm->isAsyncContextTrackingEnabled())
        markStepsWhoseConsumerHasAwaited(instance, resumableFrame(instance));
    const uint64_t* outerFrame = withoutResumptionRight(instance, instance.coroutines.resumableFrame);
    CallData callData = CallData::forCompiledCode(instance.state->coroutines->savedFrameEntrypoint(), instance.globalObject);
    JSValue returned = enterCompiledCode(vm, callData, saved, jsUndefined(), saved, jsUndefined(), jsNumber(0), sent, jsNumber(static_cast<int32_t>(engineResumeModeOf(mode))));
    instance.coroutines.resumableFrame = outerFrame;
    return returned;
}

void settlePromiseOfHandler(VM& vm, JSCell* cell, JSValue returned, JSValue error)
{
    if (auto* chain = dynamicDowncast<ReactionChain>(cell)) {
        chain->advance();
        chain->settle(vm, error ? error : returned, error ? JSPromise::Status::Rejected : JSPromise::Status::Fulfilled);
        return;
    }
    auto* promise = uncheckedDowncast<JSPromise>(cell);
    if (error)
        promise->rejectPromise(vm, error);
    else
        promise->resolvePromise(promise->realm(), vm, returned);
}

static void finishAdapterEntry(VM& vm, Instance& instance, const AdapterEntry& entry, TopExceptionScope& scope, JSValue returned)
{
    auto [lastFrame, lastAsyncGenerator, promiseOfHandler, handlerReturnsFulfillment] = didLeaveAdapter(instance, entry);
    JSValue error;
    Exception* exception = scope.exception();
    if (exception) [[unlikely]] {
        error = exception->value();
        if (!scope.clearExceptionExceptTermination())
            return;
    }
    if (promiseOfHandler) {
        settlePromiseOfHandler(vm, promiseOfHandler, handlerReturnsFulfillment ? promiseReturnedByHandler(instance, returned) : returned, error);
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

static ALWAYS_INLINE void enterFunctionOfJob(JSGlobalObject* globalObject, VM& vm, JSValue promise, JSValue function, JSValue argument, bool endsJob, bool asyncContextIsPutBack)
{
    ASSERT(canEnterJobFunction(globalObject, vm, function));
    Instance* instance = Instance::of(globalObject);

    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2 && static_cast<int32_t>(JSGenerator::Argument::Value) == 3);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AdapterEntry entry = willEnterAdapter(*instance, nullptr, nullptr, asyncContextIsPutBack);
    instance->coroutines.nextEntryEndsJob = endsJob;
    CallData callData = CallData::forCompiledCode(instance->state->coroutines->handlerOfJobEntrypoint(), globalObject);
    JSValue returned = enterCompiledCode(vm, callData, asObject(function), jsUndefined(), function.asCell(), promise, function, argument, jsNumber(0));
    finishAdapterEntry(vm, *instance, entry, scope, returned);
    if (instance->coroutines.waiterToResume)
        resumeRemainingWaiters(*instance);
}

static NEVER_INLINE void enterFunctionOfJobInAsyncContext(JSGlobalObject* globalObject, VM& vm, JSValue promise, JSValue function, JSValue argument, bool endsJob, bool asyncContextIsPutBack, JSValue asyncContext)
{
    AsyncContextSwapScope asyncContextScope(vm, globalObject, asyncContext);
    enterFunctionOfJob(globalObject, vm, promise, function, argument, endsJob, asyncContextIsPutBack);
}

NEVER_INLINE void runHandlerJob(JSGlobalObject* globalObject, VM& vm, InternalMicrotask, uint8_t, JSValue promise, JSValue handler, JSValue argument, JSValue asyncContext, MicrotaskCallCache* microtaskCallCache)
{
    bool endsJob = microtaskCallCache && microtaskCallCache->isOwnedByJobLoop();
    if (vm.isAsyncContextTrackingEnabled()) [[unlikely]]
        return enterFunctionOfJobInAsyncContext(globalObject, vm, promise, handler, argument, endsJob, endsJob, asyncContext);
    enterFunctionOfJob(globalObject, vm, promise, handler, argument, endsJob, false);
}

NEVER_INLINE void runFunctionJob(JSGlobalObject* globalObject, VM& vm, InternalMicrotask, uint8_t, JSValue function, JSValue, JSValue, JSValue, MicrotaskCallCache* microtaskCallCache)
{
    bool endsJob = microtaskCallCache && microtaskCallCache->isOwnedByJobLoop();
    if (vm.isAsyncContextTrackingEnabled()) [[unlikely]]
        return enterFunctionOfJobInAsyncContext(globalObject, vm, jsUndefined(), function, jsUndefined(), endsJob, false, jsUndefined());
    enterFunctionOfJob(globalObject, vm, jsUndefined(), function, jsUndefined(), endsJob, false);
}

void resumeSavedAsyncFunction(JSGlobalObject* globalObject, VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue resolution)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        saved->willBeRestoredToStack();
        saved->reject(globalObject, vm, createStackOverflowError(globalObject));
        return;
    }
    AdapterEntry entry = willEnterAdapter(instance, saved, nullptr, instance.coroutines.nextEntryEndsJob && vm.isAsyncContextTrackingEnabled());
    JSValue returned = enterSavedFrame(vm, instance, saved, mode, resolution);
    finishAdapterEntry(vm, instance, entry, scope, returned);
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    auto* saved = uncheckedDowncast<SavedFrame>(context);
    Instance& instance = *saved->realm()->aotInstance();
    countEvent(vm, RuntimeCounterID::AsyncFunctionsResumedByRuntime);
    isEnteredByJobLoop &= globalObject == saved->realm();
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    instance.coroutines.nextEntryEndsJob = isEnteredByJobLoop;
    resumeSavedAsyncFunction(globalObject, vm, instance, saved, isRejected ? ResumeMode::Throw : ResumeMode::Next, resolution);
    if (instance.coroutines.waiterToResume)
        resumeRemainingWaiters(instance);
}

static NEVER_INLINE void resumeAsyncFunctionInAsyncContext(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop, JSValue asyncContext)
{
    AsyncContextSwapScope asyncContextScope(vm, globalObject, asyncContext);
    resumeAsyncFunction(globalObject, vm, context, resolution, isRejected, isEnteredByJobLoop);
}

NEVER_INLINE void runAsyncFunctionResumeJob(JSGlobalObject* globalObject, VM& vm, InternalMicrotask, uint8_t status, JSValue, JSValue resolution, JSValue context, JSValue asyncContext, MicrotaskCallCache* microtaskCallCache)
{
    bool isRejected = static_cast<JSPromise::Status>(status) == JSPromise::Status::Rejected;
    bool isEnteredByJobLoop = microtaskCallCache && microtaskCallCache->isOwnedByJobLoop();
    if (vm.isAsyncContextTrackingEnabled()) [[unlikely]]
        return resumeAsyncFunctionInAsyncContext(globalObject, vm, context, resolution, isRejected, isEnteredByJobLoop, asyncContext);
    resumeAsyncFunction(globalObject, vm, context, resolution, isRejected, isEnteredByJobLoop);
}

bool hasCompiledBody(JSAsyncGenerator* generator)
{
    return savedFrameIn(generator->frame());
}

static NEVER_INLINE EncodedJSValue stackOverflowErrorOfAsyncGeneratorBody(JSGlobalObject* realm)
{
    return JSValue::encode(createStackOverflowError(realm));
}

static ALWAYS_INLINE void resumeAsyncGeneratorBody(VM& vm, JSAsyncGenerator* generator, JSValue resumeValue, JSGenerator::ResumeMode resumeMode, MicrotaskCallCache* microtaskCallCache)
{
    SavedFrame* saved = savedFrameIn(generator->frame());
    int32_t state = generator->state();
    generator->setState(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Executing));
    AsyncGeneratorBodyEntry entry = willEnterAsyncGeneratorBody(generator, microtaskCallCache && microtaskCallCache->isOwnedByJobLoop());

    JSValue value;
    JSValue error;
    bool isTerminated = false;
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        error = JSValue::decode(stackOverflowErrorOfAsyncGeneratorBody(saved->realm()));
    else {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        CallData callData = CallData::forCompiledCode(entry.instance->state->coroutines->savedFrameEntrypoint(), entry.instance->globalObject);
        value = enterCompiledCode(vm, callData, saved, generator->thisValue(), generator,
            generator, jsNumber(state >> JSAsyncGenerator::reasonShift), resumeValue, jsNumber(static_cast<int32_t>(resumeMode)));
        if (scope.exception()) [[unlikely]] {
            error = scope.exception()->value();
            isTerminated = !scope.clearExceptionExceptTermination();
        }
    }
    generator = didReturnFromAsyncGeneratorBody(entry, generator, value, error, isTerminated);
    if (!generator || isTerminated)
        return;
    finishAsyncGeneratorBodyCall(generator->realm(), generator, value, error, microtaskCallCache);
}

static NEVER_INLINE void resumeAsyncGeneratorBodyInAsyncContext(JSGlobalObject* globalObject, VM& vm, JSAsyncGenerator* generator, JSValue resumeValue, JSGenerator::ResumeMode resumeMode, MicrotaskCallCache* microtaskCallCache, JSValue asyncContext)
{
    AsyncContextSwapScope asyncContextScope(vm, globalObject, asyncContext);
    resumeAsyncGeneratorBody(vm, generator, resumeValue, resumeMode, microtaskCallCache);
}

NEVER_INLINE void runAsyncGeneratorBodyJob(JSGlobalObject* globalObject, VM& vm, InternalMicrotask task, uint8_t status, JSValue, JSValue resolution, JSValue context, JSValue asyncContext, MicrotaskCallCache* microtaskCallCache)
{
    auto* generator = uncheckedDowncast<JSAsyncGenerator>(context);
    JSGenerator::ResumeMode resumeMode = static_cast<JSPromise::Status>(status) == JSPromise::Status::Rejected ? JSGenerator::ResumeMode::ThrowMode
        : task == InternalMicrotask::AsyncGeneratorBodyCallReturn ? JSGenerator::ResumeMode::ReturnMode : JSGenerator::ResumeMode::NormalMode;
    if (vm.isAsyncContextTrackingEnabled()) [[unlikely]]
        return resumeAsyncGeneratorBodyInAsyncContext(globalObject, vm, generator, resolution, resumeMode, microtaskCallCache, asyncContext);
    resumeAsyncGeneratorBody(vm, generator, resolution, resumeMode, microtaskCallCache);
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
    RELEASE_AND_RETURN(scope, newCellForCompiledCode(vm, JSPromise::create(vm, globalObject->promiseStructure())));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSettledResult, JSCell*, (Instance* instance, EncodedJSValue value, uint32_t status))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSettledResult);
    DeferTerminationForAWhile deferTermination(vm);
    JSValue settled = JSValue::decode(value);
    bool isRejected = static_cast<JSPromise::Status>(status) == JSPromise::Status::Rejected;
    RELEASE_AND_RETURN(scope, newCellForCompiledCode(vm, isRejected ? createPromiseAllSettledRejectedResult(globalObject, settled) : createPromiseAllSettledFulfilledResult(globalObject, settled)));
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
    resolveWithReturned(*instance, vm, cellArgument<JSPromise>(promise, entry), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolvePromiseOrReturnResult, EncodedJSValue, (Instance* instance, JSCell* promise, EncodedJSValue value, const uint64_t* frame))
{
    constexpr Entry entry = Entry::operationAOTResolvePromiseOrReturnResult;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue resolution = JSValue::decode(value);
    if (isUnmodifiedPromise(globalObject, resolution) && frameEndsJob(*instance, frame)) {
        JSPromise* inner = uncheckedDowncast<JSPromise>(resolution);
        if (JSValue settled = settledValueOf(globalObject, inner)) {
            inner->markAsHandled();
            resolution = settled;
        } else {
            SavedFrame* saved = SavedFrame::forFrame(*instance, frame);
            JSPromise* resolved = promise ? cellArgument<JSPromise>(promise, entry) : saved ? saved->ensurePromise(globalObject, vm) : JSPromise::create(vm, globalObject->promiseStructure());
            if (resolved != inner) {
                inner->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, resolved, jsUndefined());
                OPERATION_RETURN(scope, JSValue::encode(resolved));
            }
        }
    }
    if (promise) {
        JSPromise* resolved = cellArgument<JSPromise>(promise, entry);
        if (!nextJobRunsImmediately(*instance) || !tryScheduleImmediateResume(*instance, frame, resolved, resolution))
            resolveWithReturned(*instance, vm, resolved, resolution);
        OPERATION_RETURN(scope, JSValue::encode(promise));
    }
    bool isNotThenable = !resolution.isObject() || isDefinitelyNonThenable(asObject(resolution), globalObject);
    SavedFrame* saved = SavedFrame::forFrame(*instance, frame);
    if (isNotThenable) {
        if (saved)
            saved->fulfill(globalObject, vm, resolution);
        OPERATION_RETURN(scope, JSValue::encode(resolution));
    }
    JSPromise* created = saved ? saved->ensurePromise(globalObject, vm) : JSPromise::create(vm, globalObject->promiseStructure());
    resolveWithReturned(*instance, vm, created, resolution);
    OPERATION_RETURN(scope, JSValue::encode(created));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSettleAwaitedPromise, size_t, (uint64_t instanceWord, JSCell* awaited))
{
    auto& instance = *std::bit_cast<Instance*>(instanceWord);
    JSGlobalObject* globalObject = instance.globalObject;
    instance.coroutines.lastPromiseResolvedWithSettled = nullptr;
    if (instance.awaitMustSuspend || !globalObject->promiseThenWatchpointSet().isStillValid() || globalObject->promiseSpeciesWatchpointSet().state() != IsWatched)
        return false;
    auto wordOfQueue = [&](ptrdiff_t offset) -> size_t& {
        return *std::bit_cast<size_t*>(std::bit_cast<uint8_t*>(instance.realm.microtaskQueue) + offset);
    };
    auto packedOf = [](JSCell* promise) -> uint64_t& {
        return *std::bit_cast<uint64_t*>(std::bit_cast<uint8_t*>(promise) + JSPromise::offsetOfPacked());
    };
    size_t& start = wordOfQueue(MicrotaskQueue::offsetOfStart());
    const size_t end = wordOfQueue(MicrotaskQueue::offsetOfEnd());
    const size_t mask = wordOfQueue(MicrotaskQueue::offsetOfCapacityMask());
    const auto* jobs = std::bit_cast<const QueuedTask*>(wordOfQueue(MicrotaskQueue::offsetOfBuffer()));

    const StructureID structureOfPromises = globalObject->promiseStructure()->id();
    JSCell* resolved = awaited;
    if (resolved->structureID() != structureOfPromises)
        return false;
    for (size_t index = end; index != start;) {
        index = (index - 1) & mask;
        const QueuedTask& job = jobs[index];
        if (job.job() != InternalMicrotask::PromiseResolveThenableJobFast || job.globalObject() != globalObject || job.arguments()[1] != JSValue(resolved) || packedOf(resolved) != resolvedPromiseWithoutReactionBits)
            return false;
        if (!job.arguments()[0].isCell())
            return false;
        resolved = job.arguments()[0].asCell();
        if (resolved->structureID() != structureOfPromises)
            return false;
    }
    auto* settled = uncheckedDowncast<JSPromise>(resolved);
    if (settled->status() != JSPromise::Status::Fulfilled)
        return false;
    JSValue value = settled->settlementValue();
    if (value.isObject()) {
        Structure* structure = value.asCell()->structure();
        if (structure->definitelyNonThenableState() != Structure::DefinitelyNonThenableState::NonThenable || structure->realm() != globalObject)
            return false;
    }

    for (size_t index = start; index != end; index = (index + 1) & mask) {
        JSCell* promise = jobs[index].arguments()[1].asCell();
        *std::bit_cast<EncodedJSValue*>(std::bit_cast<uint8_t*>(promise) + JSPromise::offsetOfSlot()) = JSValue::encode(value);
        packedOf(promise) = fulfilledPromiseBits | promiseIsHandledBits;
        instance.vm->writeBarrier(promise, value);
    }
    start = end;
    wordOfQueue(MicrotaskQueue::offsetOfMarkedBefore()) = 0;
    return true;
}

enum class AwaitedKind : uint8_t { Value, ValueInModuleBody, ResultOfCall, Resolution };

static constexpr InternalMicrotask resumingTaskOf(AwaitedKind kind)
{
    switch (kind) {
    case AwaitedKind::Value:
    case AwaitedKind::ResultOfCall:
    case AwaitedKind::Resolution:
        return InternalMicrotask::CompiledAsyncFunctionResume;
    case AwaitedKind::ValueInModuleBody:
        return InternalMicrotask::CompiledModuleBodyResume;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<AwaitedKind kind>
static ALWAYS_INLINE JSCell* suspendUntilSettled(Instance* instance, VM& vm, ThrowScope& scope, EncodedJSValue awaited, SavedFrame* saved, const uint64_t* frame)
{
    UNUSED_PARAM(frame);
    if constexpr (kind == AwaitedKind::ResultOfCall)
        resumeWhenCallSettles(*instance, JSValue::decode(awaited), saved);
    else if constexpr (kind == AwaitedKind::Resolution)
        resumeWhenResolutionSettles(*instance, vm, scope, JSValue::decode(awaited), saved, frameEndsJob(*instance, frame));
    else if (kind == AwaitedKind::Value && isSavedFrameOf(*instance, JSValue::decode(awaited)))
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
        OPERATION_RETURN(scope, suspendUntilSettled<kind>(instance, vm, scope, awaited, saved, frame)); \
    } \
    JSC_DEFINE_JIT_OPERATION(awaitWithFrame, JSCell*, (Instance* instance, EncodedJSValue awaited, JSCell* savedFrame)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::awaitWithFrame); \
        SavedFrame* saved = cellArgument<SavedFrame>(savedFrame, Entry::awaitWithFrame); \
        vm.writeBarrier(saved); \
        OPERATION_RETURN(scope, suspendUntilSettled<kind>(instance, vm, scope, awaited, saved, std::bit_cast<const uint64_t*>(callFrame->callerFrame()))); \
    }

AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwait, operationAOTAwaitWithSavedFrame, AwaitedKind::Value)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitInModuleBody, operationAOTAwaitWithSavedFrameInModuleBody, AwaitedKind::ValueInModuleBody)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitCall, operationAOTAwaitCallWithSavedFrame, AwaitedKind::ResultOfCall)
AOT_DEFINE_AWAIT_OPERATIONS(operationAOTSaveFrameAndAwaitResolution, operationAOTAwaitResolutionWithSavedFrame, AwaitedKind::Resolution)
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
        SuspensionWords words { slotsOfStub[siteWordSlotInStubFrame], slotsOfStub[driverSlotInStubFrame] };
        SavedFrame* made = SavedFrame::createForAsyncFunction(vm, *instance, { const_cast<uint64_t*>(frame), slotsOfStub + 2, nullptr, &words });
        slotsOfStub[driverSlotInStubFrame] = std::bit_cast<uint64_t>(made);
        consumer = (consumer & ~static_cast<uint64_t>(consumerOfStepNeedsSavedFrame)) | consumerOfStepHasNewSavedFrame;
    }
    SavedFrame* newSavedFrame = consumer & consumerOfStepHasNewSavedFrame ? std::bit_cast<SavedFrame*>(slotsOfStub[driverSlotInStubFrame]) : nullptr;
    StepAnswer answer = consumer & consumerOfStepAwaitsCall ? StepAnswer::IsPromise : StepAnswer::ResumesDriver;
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

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseThen, JSCell*, (Instance* instance, JSCell* promise, EncodedJSValue onFulfilled, EncodedJSValue onRejected, JSCell* derived))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseThen);
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, JSValue::decode(onFulfilled), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope, derived);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewReactionChain, JSCell*, (Instance* instance, uint32_t description))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewReactionChain);
    DeferTerminationForAWhile deferTermination(vm);
    RELEASE_AND_RETURN(scope, newCellForCompiledCode(vm, ReactionChain::create(vm, instance->state->coroutines->reactionChainStructure(vm), description)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStartReactionChain, void, (Instance* instance, JSCell* promise, JSCell* chain))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStartReactionChain);
    DeferTerminationForAWhile deferTermination(vm);
    uncheckedDowncast<ReactionChain>(chain)->startAt(vm, uncheckedDowncast<JSPromise>(promise));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStartReactionChainOfValue, void, (Instance* instance, EncodedJSValue value, JSCell* chain))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStartReactionChainOfValue);
    DeferTerminationForAWhile deferTermination(vm);
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->fulfillPromise(vm, JSValue::decode(value));
    uncheckedDowncast<ReactionChain>(chain)->startAt(vm, promise);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseCatch, JSCell*, (Instance* instance, JSCell* promise, EncodedJSValue onRejected))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseCatch);
    JSPromise* derived = JSPromise::create(vm, WTF::opaque(globalObject->promiseStructure()));
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, jsUndefined(), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope, derived);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPromiseAll, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* elements, uint32_t directCalls))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPromiseAll);
    OPERATION_RETURN(scope, promiseAllOfElements(globalObject, std::span { elements, count }, directCalls));
}

static void ensureAllocatorOfGenerators(VM& vm, Instance& instance)
{
    if (!instance.coroutines.generatorAllocator)
        instance.coroutines.generatorAllocator = subspaceFor<Generator>(vm)->allocatorFor(sizeof(Generator), AllocatorForMode::EnsureAllocator).localAllocator();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewGenerator, JSCell*, (Instance* instance, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTNewGenerator;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    ensureAllocatorOfGenerators(vm, *instance);
    if (!closure)
        return Generator::create(vm, instance->state->coroutines->generatorStructure());
    JSObject* generator = createGenerator(globalObject, *instance, objectArgument(closure, entry), CoroutineKind::Generator);
    scope.releaseAssertNoException();
    return generator;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewGeneratorOfLibrary, JSCell*, (Instance* instance, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTNewGeneratorOfLibrary;
    AOT_OPERATION_BEGIN(instance, entry);
    ensureAllocatorOfGenerators(vm, *instance);
    OPERATION_RETURN(scope, createGenerator(globalObject, *instance, objectArgument(closure, entry), CoroutineKind::Generator));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewAsyncGenerator, JSCell*, (Instance* instance, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTNewAsyncGenerator;
    AOT_OPERATION_BEGIN(instance, entry);
    OPERATION_RETURN(scope, createGenerator(globalObject, *instance, objectArgument(closure, entry), CoroutineKind::AsyncGenerator));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewHeapFrame, JSCell*, (Instance* instance, uint32_t numberOfWords, void* resumeAddress))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewHeapFrame);
    DeferTerminationForAWhile deferTermination(vm);
    return SavedFrame::createHeapFrame(vm, *instance, numberOfWords, resumeAddress);
}

static CoroutineKind generatorKind(JSObject* generator, Entry entry)
{
    if (generator->inherits<Generator>())
        return CoroutineKind::Generator;
    if (!generator->inherits<JSAsyncGenerator>()) [[unlikely]]
        crashOnWrongOperand(entry);
    return CoroutineKind::AsyncGenerator;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGeneratorPrototype, JSCell*, (Instance* instance, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTGetGeneratorPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    Structure* structure = structureOfGenerator(globalObject, *instance, objectArgument(closure, entry), CoroutineKind::Generator);
    scope.releaseAssertNoException();
    return structure->storedPrototypeObject();
}

static bool findsReturnOfEngine(JSGlobalObject* realm, JSObject* prototype)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = prototype->getPropertySlot(realm, vm.propertyNames->returnKeyword, slot);
    scope.assertNoException();
    return isFound && !slot.isTaintedByOpaqueObject() && slot.slotBase() == realm->generatorPrototype();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTakeGeneratorLeftInPlace, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t leavesAtOnce))
{
    constexpr Entry entry = Entry::operationAOTTakeGeneratorLeftInPlace;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* holder = cellArgument<JSObject>(cell, entry);
    auto* generator = dynamicDowncast<Generator>(holder);
    JSObject* prototype = generator ? generator->structure()->storedPrototypeObject() : holder;
    if (findsReturnOfEngine(globalObject, prototype))
        return JSValue::encode(jsUndefined());
    if (!generator)
        generator = Generator::create(vm, Generator::createStructure(vm, globalObject, prototype));
    generator->leaveInPlace(leavesAtOnce);
    return JSValue::encode(generator);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTakeGeneratorPrototype, void, (Instance* instance, JSCell* generator, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTTakeGeneratorPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = cellArgument<JSObject>(generator, entry);
    Structure* structure = structureOfGenerator(globalObject, *instance, objectArgument(closure, entry), generatorKind(object, entry));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (object->structure() != structure)
        object->setStructure(vm, structure);
    OPERATION_RETURN(scope);
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
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]] {
        DeferGCForAWhile deferGC(vm);
        if (throwOutOfMemoryErrorIfOwed(vm, globalObject, scope))
            OPERATION_RETURN(scope, nullptr);
    }
    saved->save(vm, machineFrame, SuspensionKind::Yield);
    setSavedFrameOfGenerator(vm, object, saved);
    OPERATION_RETURN(scope, saved);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
