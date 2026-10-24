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
    // The same of the jobs of async generators: AsyncGeneratorBodyCallNormal fulfilled, rejected. The other two fulfilled.
    uint64_t bodyCallJobWords[2];
    uint64_t yieldAwaitedJobWord;
    uint64_t driverResumeJobWord;
    // The same of a PromiseReactionJob: fulfilled, rejected, and the two with an async context as the last argument.
    uint64_t reactionJobWords[4];
    // The same of a BunPerformMicrotaskJob, which is what queueMicrotask queues.
    uint64_t performMicrotaskJobWord;
    // The same of a fulfilled PromiseAllResolveJob.
    uint64_t promiseAllResolveJobWord;
    void* asyncGeneratorAllocator;
    // The second result of the operations behind the stub StepAsyncGenerator.
    uint32_t stepStatus;
    uint8_t savedFramesAreListed;
    // Set by whoever enters the adapter as the last thing that a job does, and cleared by the adapter: only then does it go on with the jobs that follow.
    uint8_t nextEntryEndsJob;
    // Whoever entered the adapter runs a job under an AsyncContextSwapScope that was made while async contexts were tracked, so it puts back what was there in front of its job.
    uint8_t asyncContextIsPutBack;
    uint8_t unused[1];
    // The SavedFrame entered last: resumeSavedAsyncFunction stores the first one, the adapter each one that it goes on with.
    JSCell* enteredFrame;
    JSCell* enteredAsyncGenerator;
    // The promise that waits for what that function returns or throws, if it is the handler of a reaction. Null once it is settled.
    JSCell* promiseOfEnteredHandler;
    // VM::fastAsyncGeneratorSentinel(): what a body returns that has seen to its own resumption.
    JSCell* asyncGeneratorSentinel;
};

static_assert(std::is_trivially_default_constructible_v<CoroutineWords>);

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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
