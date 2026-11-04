/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Reg.h"
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct ImageFrame;

// What the stub LeaveForSlowBody has on the stack where it calls the operation, from low addresses to high.
struct RegistersLaidDownAtLeave {
    static constexpr unsigned numberOfWords = (Reg::maxIndex() + 2) & ~1u;

    // By Reg::index(). The words of the stack pointer and of the frame pointer say nothing.
    uint64_t byIndex[numberOfWords];
    // The record of the stub's own frame.
    uint64_t* framePointerOfFastBody;
    const void* returnAddress;
    // What the leaving code has pushed, the last first: PlaceInLeaveRow::Address says in which order the row takes them.
    const void* addresses[1];
};
static_assert(OBJECT_OFFSETOF(RegistersLaidDownAtLeave, framePointerOfFastBody) == RegistersLaidDownAtLeave::numberOfWords * sizeof(uint64_t));
static_assert(OBJECT_OFFSETOF(RegistersLaidDownAtLeave, addresses) == OBJECT_OFFSETOF(RegistersLaidDownAtLeave, framePointerOfFastBody) + 2 * sizeof(uint64_t));

struct StoreOfArrival {
    uint64_t* address;
    uint64_t word;
};

// What the operation hands the stub, in memory of the instance: from there on the stub reads nothing else, for the new frames may lie over the fast frame and over the registers that were laid down.
struct PlanOfArrival {
    const void* target;
    void* stackPointer;
    void* framePointer;
    uint64_t numberOfStores;
    // By Reg::index(): what the registers that a callee saves hold once the fast frame is gone, and what the registers hold that a unit takes its parameters in where it is entered by a call.
    uint64_t registers[RegistersLaidDownAtLeave::numberOfWords];

    StoreOfArrival* stores() { return reinterpret_cast<StoreOfArrival*>(this + 1); }
    static constexpr size_t wordsFor(size_t numberOfStores) { return (sizeof(PlanOfArrival) + numberOfStores * sizeof(StoreOfArrival)) / sizeof(uint64_t); }
};
static_assert(!(sizeof(PlanOfArrival) % sizeof(uint64_t)) && sizeof(StoreOfArrival) == 2 * sizeof(uint64_t));

struct HeapWordsOfArrival {
    uint64_t cell { 0 };
    uint64_t* words { nullptr };
};
using MakerOfHeapWords = ScopedLambda<HeapWordsOfArrival(uint32_t numberOfWords)>;

// It throws nothing, and allocates only through `makeHeapWords`, for a row that has such parts: whoever calls puts collections off until the stub has laid the frames. `fastFrame`: what holds at the site that leaves. The plan lies in `memoryOfPlan` until the next one is made there.
// Null: the frames would reach below `stackLimit`. Nothing has been touched or made, and the site that leaves throws.
JS_EXPORT_PRIVATE PlanOfArrival* layOutSlowFrames(Vector<uint64_t>& memoryOfPlan, const RegistersLaidDownAtLeave&, const ImageFrame& fastFrame, std::span<const uint32_t> wordsFromRowOn, const void* stackLimit, const MakerOfHeapWords&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
