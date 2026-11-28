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

inline constexpr int32_t mustSuspendAtNextAwaitBit = 1;
inline constexpr unsigned numberOfReleasedHeapFrames = 8;

struct CoroutineWords {
    uint64_t savedFrameHeader;
    uint64_t plainGeneratorHeader;
    uint32_t emptyGeneratorPrototypeStructureID;
    uint32_t emptyAsyncGeneratorPrototypeStructureID;
    uint32_t structureIDsOfGeneratorsOfLibrary[2];
    JSCell* waiterToResume;
    void* lastPromiseResolvedWithSettled;
    EncodedJSValue waiterResolution;
    EncodedJSValue waiterAsyncContext;
    EncodedJSValue delegatedResult;
    AsyncGeneratorStepInProgress* stepInProgress;
    const uint64_t* resumableFrame;
    uint64_t resumeJobWords[2];
    uint64_t bodyCallJobWords[2];
    uint64_t yieldAwaitedJobWord;
    uint64_t driverResumeJobWord;
    uint64_t reactionJobWords[4];
    uint64_t performMicrotaskJobWord;
    uint64_t promiseAllResolveJobWord;
    uint64_t promiseRaceResolveJobWord;
    uint64_t reactionChainHeader;
    uint64_t reactionChainStepJobWord;
    uint64_t handlerOfReactionChain[2 + 4];
    uint64_t promiseAllSettledResolveJobWords[2];
    uint64_t resolveThenableJobWord;
    uint64_t resolveWithoutHandlerJobWord;
    void* asyncGeneratorAllocator;
    void* generatorAllocator;
    uint32_t stepStatus;
    uint8_t nextEntryEndsJob;
    uint8_t asyncContextIsPutBack;
    uint8_t handlerReturnsFulfillment;
    uint8_t unused;
    JSCell* enteredFrame;
    JSCell* enteredAsyncGenerator;
    JSCell* promiseOfEnteredHandler;
    JSCell* asyncGeneratorSentinel;
    JSCell* releasedHeapFrames[numberOfReleasedHeapFrames];
};

static_assert(std::is_trivially_default_constructible_v<CoroutineWords>);

inline constexpr int32_t asyncGeneratorSlotInStubFrame = -1;
inline constexpr int32_t consumerSlotInStubFrame = -2;
inline constexpr int32_t siteWordSlotInStubFrame = -3;
inline constexpr int32_t driverSlotInStubFrame = -4;
inline constexpr int32_t consumerOfStepHasNewSavedFrame = 4;
inline constexpr int32_t consumerOfStepNeedsSavedFrame = 8;
inline constexpr int32_t consumerOfStepAwaitsCall = 16;
inline constexpr int32_t consumerOfStepHasAwaited = 32;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
