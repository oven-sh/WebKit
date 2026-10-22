/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRecordTypes.h"
#include "Microtask.h"
#include "MicrotaskQueue.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSAsyncGenerator;
class JSPromise;

namespace AOT {

class SavedFrame;

ALWAYS_INLINE bool nextJobRunsImmediately(const Instance& instance)
{
    return !instance.awaitMustSuspend && instance.realm.microtaskQueue->isEmpty();
}

void resumeWhenSettled(JSGlobalObject*, VM&, JSValue awaited, SavedFrame* waiter, InternalMicrotask = InternalMicrotask::CompiledAsyncFunctionResume);
void resumeWhenCallSettles(Instance&, JSValue resultOfCallWithoutPromise, SavedFrame* waiter);

// The frame is that of an async function that returns next: the job is taken out of the queue, so no code of the program may run before the frame has returned to the adapter.
// Returns true if it fulfilled the promise. The promise's only waiter then runs as soon as the frame has returned: the adapter resumes it, or resumeRemainingWaiters where the adapter declines.
bool tryScheduleImmediateResume(Instance&, const uint64_t* frame, JSPromise*, JSValue resolution);
void resumeRemainingWaiters(Instance&);

// A step whose consumer is on the stack below the producer, was entered as the last thing of a job, and suspends next unless the step finishes first.
// The jobs that lie between the end of the step and the consumer run nothing of the program, so while no other job is queued the consumer takes the result
// when the engine returns to it. The producer is an async generator or the wrapper of a sync iterator.
class AsyncGeneratorStepInProgress {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(AsyncGeneratorStepInProgress);
public:
    AsyncGeneratorStepInProgress(Instance&, JSObject* driver, JSObject* producer);
    ~AsyncGeneratorStepInProgress();

    JSObject* driver() const { return m_driver; }
    JSObject* producer() const { return m_producer; }
    bool isFinished() const { return !!m_value; }
    bool isDone() const { return m_isDone; }
    JSValue value() const { return m_value; }
    void finish(JSValue value, bool isDone);

private:
    Instance& m_instance;
    AsyncGeneratorStepInProgress* m_enclosingStep;
    JSObject* m_driver;
    JSObject* m_producer;
    JSValue m_value;
    bool m_isDone { false };
};

struct AsyncGeneratorStep {
    JSValue value;
    AsyncGeneratorStepStatus status;
};

// The stub StepAsyncGenerator keeps the generator whose body it enters in this slot of its own frame.
inline constexpr int32_t asyncGeneratorSlotInStubFrame = -1;
// Of the same frame: what the stub knows of the consumer, the word of the site, and the driver of the consumer.
inline constexpr int32_t consumerSlotInStubFrame = -2;
inline constexpr int32_t siteWordSlotInStubFrame = -3;
inline constexpr int32_t driverSlotInStubFrame = -4;
// The driver is a SavedFrame that was made at this step, for an async function that was never suspended.
inline constexpr int32_t consumerOfStepHasNewSavedFrame = 4;
// The stub could not allocate that SavedFrame. In the place of the driver is what SavedFrame::preservedRegister holds.
inline constexpr int32_t consumerOfStepNeedsSavedFrame = 8;
inline constexpr int32_t consumerOfStepAwaitsPromise = 16;
inline constexpr int32_t consumerOfStepAwaitsCall = 32;

// The generator, if the frame is that of a body that the stub StepAsyncGenerator entered.
JSAsyncGenerator* asyncGeneratorEnteredByStub(const Instance&, const uint64_t* frame);
// The SavedFrame that the frame was restored from, whoever restored it. A coroutine that has not been suspended yet has none.
SavedFrame* coroutineOfFrame(const Instance&, const uint64_t* frame);

// With the status Result, the value is what the consumer has to await: the sentinel if the producer resumes it, else the promise of an iterator result.
// The frame is that of the coroutine that iterates. The iterator is an async generator or the wrapper of a sync iterator; sent is empty if next() gets no argument.
enum class StepAnswer : uint8_t { ResumesDriver, IsPromise };
AsyncGeneratorStep stepAsyncIterator(Instance&, const uint64_t* frame, SavedFrame* newSavedFrame, StepAnswer, JSObject* iterator, JSValue sent);
AsyncGeneratorStep finishAsyncGeneratorStep(Instance&, const uint64_t* slotsOfStub, JSAsyncGenerator*, JSValue returned, bool consumerMayContinue);
void rejectAsyncGeneratorStep(Instance&, const uint64_t* slotsOfStub, JSAsyncGenerator*, JSValue error);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
