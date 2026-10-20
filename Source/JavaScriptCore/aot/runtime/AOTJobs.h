/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRecordTypes.h"
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

void resumeWhenSettled(JSGlobalObject*, VM&, JSValue awaited, SavedFrame* waiter);
void resumeWhenCallSettles(Instance&, JSValue resultOfCallWithoutPromise, SavedFrame* waiter);

// The frame is that of an async function that returns next: the job is taken out of the queue, so no code of the program may run before the frame has returned to C++.
// Returns true if it fulfilled the promise. The promise's only waiter then runs as soon as the frame has returned.
bool tryScheduleImmediateResume(Instance&, const uint64_t* frame, JSPromise*, JSValue resolution);
void resumeRemainingWaiters(Instance&);

// A step whose consumer is on the stack below the generator, was entered as the last thing of a job, and suspends next unless the step finishes first.
// The jobs that lie between the end of the step and the consumer run nothing of the program, so while no other job is queued the consumer takes the result
// when the engine returns to it.
class AsyncGeneratorStepInProgress {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(AsyncGeneratorStepInProgress);
public:
    AsyncGeneratorStepInProgress(Instance&, JSObject* driver, JSAsyncGenerator*);
    ~AsyncGeneratorStepInProgress();

    JSObject* driver() const { return m_driver; }
    JSAsyncGenerator* generator() const { return m_generator; }
    bool isFinished() const { return !!m_value; }
    bool isDone() const { return m_isDone; }
    JSValue value() const { return m_value; }
    void finish(JSValue value, bool isDone);

private:
    Instance& m_instance;
    AsyncGeneratorStepInProgress* m_enclosingStep;
    JSObject* m_driver;
    JSAsyncGenerator* m_generator;
    JSValue m_value;
    bool m_isDone { false };
};

struct AsyncGeneratorStep {
    JSValue value;
    AsyncGeneratorStepStatus status;
};

// The frame is that of the coroutine that iterates over the generator.
AsyncGeneratorStep stepAsyncGenerator(Instance&, const uint64_t* frame, JSAsyncGenerator*, JSValue sent);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
