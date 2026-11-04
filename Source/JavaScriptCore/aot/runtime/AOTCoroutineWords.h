/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <stdint.h>
#include <type_traits>

namespace JSC {

class JSCell;

namespace AOT {

class AsyncGeneratorStepInProgress;

struct CoroutineWords {
    uint64_t savedFrameHeader;
    // The cell header of a generator whose prototype is %GeneratorPrototype% itself.
    uint64_t plainGeneratorHeader;
    uint32_t emptyGeneratorPrototypeStructureID;
    uint32_t emptyAsyncGeneratorPrototypeStructureID;
    // The waiter of an async function that has just finished and returns next. It runs under waiterAsyncContext, which it captured when it was suspended.
    JSCell* waiterToResume;
    EncodedJSValue waiterResolution;
    EncodedJSValue waiterAsyncContext;
    AsyncGeneratorStepInProgress* stepInProgress;
    // The frame pointer of the coroutine frame that a job entered as the last thing it does. Set only while that frame is on the stack and awaitMustSuspend is clear.
    const uint64_t* frameThatMayContinue;
    // The first word of a queued CompiledAsyncFunctionResume job: fulfilled, rejected.
    uint64_t resumeJobWords[2];
    // The first words of queued async generator jobs. AsyncGeneratorBodyCallNormal: fulfilled, rejected. The other two: fulfilled only.
    uint64_t bodyCallJobWords[2];
    uint64_t yieldAwaitedJobWord;
    uint64_t driverResumeJobWord;
    // The first word of a PromiseReactionJob: fulfilled, rejected, then both again with an async context as the last argument.
    uint64_t reactionJobWords[4];
    // The first word of a BunPerformMicrotaskJob, which is what queueMicrotask queues.
    uint64_t performMicrotaskJobWord;
    // The first word of a fulfilled PromiseAllResolveJob.
    uint64_t promiseAllResolveJobWord;
    void* asyncGeneratorAllocator;
    // Null until the first generator is made.
    void* generatorAllocator;
    // The second result of the operations behind the stub StepAsyncGenerator.
    uint32_t stepStatus;
    // Set by whoever enters the adapter as the last thing that a job does, and cleared by the adapter: only then does it go on with the jobs that follow.
    uint8_t nextEntryEndsJob;
    // Whoever entered the adapter runs its job under an AsyncContextSwapScope that was made while async contexts were tracked, so that scope restores the context that was current before the job.
    uint8_t asyncContextIsPutBack;
    uint8_t unused[2];
    // The SavedFrame entered last: resumeSavedAsyncFunction stores the first one, the adapter each one that it goes on with.
    JSCell* enteredFrame;
    JSCell* enteredAsyncGenerator;
    // The promise that waits for what the entered function returns or throws, if that function is the handler of a reaction. Null once it is settled.
    JSCell* promiseOfEnteredHandler;
    // VM::fastAsyncGeneratorSentinel(): returned by a body that has arranged its own resumption.
    JSCell* asyncGeneratorSentinel;
};

static_assert(std::is_trivially_default_constructible_v<CoroutineWords>);

// The stub StepAsyncGenerator keeps the generator whose body it enters in this slot of its own frame.
inline constexpr int32_t asyncGeneratorSlotInStubFrame = -1;
// Other slots of that frame: the bits that describe the consumer, the site word, and the consumer's driver.
inline constexpr int32_t consumerSlotInStubFrame = -2;
inline constexpr int32_t siteWordSlotInStubFrame = -3;
inline constexpr int32_t driverSlotInStubFrame = -4;
// The driver is a SavedFrame that was made at this step, for an async function that was never suspended.
inline constexpr int32_t consumerOfStepHasNewSavedFrame = 4;
// The stub could not allocate that SavedFrame. The driver's slot holds the value of SavedFrame::preservedRegister instead.
inline constexpr int32_t consumerOfStepNeedsSavedFrame = 8;
// The consumer goes on under the async context of its await, so it awaits the promise that `next` returns.
inline constexpr int32_t consumerOfStepAwaitsPromise = 16;
// The step is an awaited call of `next`, and no step of a loop or of `yield*`.
inline constexpr int32_t consumerOfStepAwaitsCall = 32;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
