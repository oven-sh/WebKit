/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueue.h"
#include "VM.h"
#include <array>
#include <wtf/CompactPointerTuple.h>

namespace JSC { namespace AOT {

enum class AwaitConditionBase : uint8_t {
    Instance,
    VM,
    MicrotaskQueue,
    AwaitedCellHeader,
    AwaitedPromise,
};

enum class AwaitConditionTest : uint8_t {
    HasAnyBit,
    HasNoBit,
    IsZero,
    EqualsOther,
    EqualsFramePointer,
};

struct AwaitConditionWord {
    AwaitConditionBase base;
    ptrdiff_t offset;
    uint8_t sizeInBytes;
    bool isImmutable;

    friend constexpr bool operator==(const AwaitConditionWord&, const AwaitConditionWord&) = default;
};

struct AwaitCondition {
    AwaitConditionWord word;
    AwaitConditionTest test;
    uint64_t bits;
    AwaitConditionWord other;

    friend constexpr bool operator==(const AwaitCondition&, const AwaitCondition&) = default;
};

inline constexpr unsigned promiseFlagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
inline constexpr ptrdiff_t offsetOfMicrotaskQueue = AOT_INSTANCE_OFFSET(realm.microtaskQueue);
inline constexpr ptrdiff_t offsetOfPromiseHeader = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].header);
inline constexpr AwaitConditionWord noWord { AwaitConditionBase::Instance, 0, 0, false };

// The first one stands for three: a job entered the frame as the last thing it does, the frame is still that one, and awaitMustSuspend is clear.
inline constexpr std::array<AwaitCondition, 2> awaitConditionsOnState { {
    { { AwaitConditionBase::Instance, AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue), 8, false }, AwaitConditionTest::EqualsFramePointer, 0, noWord },
    { { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfStart(), 8, false }, AwaitConditionTest::EqualsOther, 0, { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfEnd(), 8, false } },
} };

inline constexpr AwaitCondition awaitedHasPromiseStructure { { AwaitConditionBase::AwaitedCellHeader, JSCell::structureIDOffset(), 4, false }, AwaitConditionTest::EqualsOther, 0, { AwaitConditionBase::Instance, offsetOfPromiseHeader, 4, true } };
inline constexpr AwaitCondition awaitedIsFulfilled { { AwaitConditionBase::AwaitedPromise, JSPromise::offsetOfPacked(), 8, false }, AwaitConditionTest::HasAnyBit, static_cast<uint64_t>(JSPromise::Status::Fulfilled) << promiseFlagsShift, noWord };
inline constexpr std::array<AwaitCondition, 2> awaitConditionsOnObject { awaitedHasPromiseStructure, awaitedIsFulfilled };

// A pending promise without a reaction takes one in its own two words, with the async context, a cell, in the place that the reaction of another task has for its cell.
inline constexpr uint64_t pendingPromiseWithoutReactionBits = (static_cast<uint64_t>(JSPromise::isHandledFlag) | JSPromise::isFirstResolvingFunctionCalledFlag) << promiseFlagsShift;
inline constexpr AwaitCondition tracksNoAsyncContext { { AwaitConditionBase::VM, OBJECT_OFFSETOF(VM, m_asyncContextTrackingEnabled), 1, false }, AwaitConditionTest::IsZero, 0, noWord };
inline constexpr AwaitCondition awaitedTakesInlineReaction { { AwaitConditionBase::AwaitedPromise, JSPromise::offsetOfPacked(), 8, false }, AwaitConditionTest::HasNoBit, ~pendingPromiseWithoutReactionBits, noWord };
constexpr uint64_t inlineReactionBitsOf(InternalMicrotask task)
{
    return (static_cast<uint64_t>(JSPromise::isHandledFlag)
        | static_cast<uint64_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
        | static_cast<uint64_t>(task) << JSPromise::inlineReactionMicrotaskShift) << promiseFlagsShift;
}
inline constexpr uint64_t resumeReactionBits = inlineReactionBitsOf(InternalMicrotask::CompiledAsyncFunctionResume);
static_assert(static_cast<uint64_t>(InternalMicrotask::CompiledAsyncFunctionResume) << JSPromise::inlineReactionMicrotaskShift <= JSPromise::inlineReactionMicrotaskMask);
static_assert(static_cast<uint64_t>(InternalMicrotask::AsyncGeneratorBodyCallNormal) << JSPromise::inlineReactionMicrotaskShift <= JSPromise::inlineReactionMicrotaskMask);
static_assert(static_cast<uint64_t>(InternalMicrotask::AsyncGeneratorYieldAwaited) << JSPromise::inlineReactionMicrotaskShift <= JSPromise::inlineReactionMicrotaskMask);

inline constexpr uint64_t promiseIsHandledBits = static_cast<uint64_t>(JSPromise::isHandledFlag) << promiseFlagsShift;
inline constexpr uint64_t fulfilledPromiseBits = (static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << promiseFlagsShift;

static_assert(!static_cast<unsigned>(JSPromise::Status::Pending) && static_cast<unsigned>(JSPromise::Status::Fulfilled) == 1 && static_cast<unsigned>(JSPromise::Status::Rejected) == 2 && JSPromise::stateMask == 3);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
