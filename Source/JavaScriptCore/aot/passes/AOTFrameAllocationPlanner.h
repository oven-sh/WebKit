/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLosses.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class Heat;
class IRBuilder;

struct FrameDecision {
    ValueID allocation;
    AllocationStorage storage { AllocationStorage::Heap };
    AllocationHeapCause cause { AllocationHeapCause::None };
    // AOTFrameAllocationPointers.h, isLentTo().
    bool isLent { false };
    // Uses that let a closure out, each of them the last thing that happens to it. The closure of the frame serves the others, and a closure of the heap is made for each of these where it stands.
    Vector<Use, 1> madeAgainAt;
    // A closure of the heap that need not be made: the closure that the closure table holds stands for it.
    bool isHeldByClosureTable { false };
    std::optional<Use> forcedBy;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
};

inline constexpr std::array statisticsOfFramePlans {
    StatisticID::ClosuresInFrames, StatisticID::ClosuresLent, StatisticID::EnvironmentsInFrames, StatisticID::EnvironmentsOnHeap, StatisticID::EnvironmentsMaterializedLate,
    StatisticID::ObjectsInFrames, StatisticID::EmptyArgumentListsShared, StatisticID::ArraysInFrames, StatisticID::RestArraysInFrames,
};

struct FramePlan {
    FunctionID function;
    Vector<FrameDecision> decisions;
    Vector<ValueID, 2> emptyArgumentLists;
    // The frame has to outlive the call, which takes a cell that lies in it.
    Vector<BlockID, 1> demotedReturnCalls;
    // They lie in the frame and hold no pointer into it: an environment is materialized in front of them.
    Vector<ValueID, 1> frameClosuresOverHeapCells;
    Vector<std::pair<LayoutID, AllocationStorage>> cells;
    Vector<std::pair<FunctionID, AllocationStorage>> closures;
    Vector<FieldReference> fieldsHoldingFramePointers;
    Vector<CaptureReference> capturesHoldingFramePointers;
    std::array<unsigned, statisticsOfFramePlans.size()> counts { };

    void count(StatisticID id)
    {
        size_t index = std::ranges::find(statisticsOfFramePlans, id) - statisticsOfFramePlans.begin();
        RELEASE_ASSERT(index < counts.size());
        ++counts[index];
    }
    void add(const Program&, const Value& allocation, AllocationStorage, bool isLent, bool isMadeAgain = false);
};

bool hasStorageChoice(const Value&);
bool isEmptyArrayLiteral(const Value&);

// Locked stack slots are never shared, so the arrays of a function take up to 32 * (24 + 8 * MaxFrameArrayLength) bytes of its frame.
AOT_THRESHOLD(unsigned, maxFollowedArrays, 32);
// What the planner finds out about the uses of arrays, without the analyses that it needs for everything else.
bool mayPutSomeArrayInFrame(const Program&, const FlowResults&, const BuildContext&, const Function&);

FramePlan planFrame(FunctionAnalyses&, const Heat&);
// Of a closure or an object: whatever the analyses say of its uses, planFrame() leaves it on the heap and records no more than that and what isLikeEveryClosureOfItsFunction() says.
bool staysOnHeapByForm(const Program&, const FlowResults&, const BuildContext&, const Function& maker, const Value& allocation);
bool mayCreateFieldsLater(const Layout&, size_t numberOfFieldsAtAllocation);
bool isLikeEveryClosureOfItsFunction(const Program&, const FlowResults*, const Function& maker, const Value& closure);

// A closure of the frame in whose place this use is going to take a closure of the heap, which is made in front of it.
struct ClosureMadeAgain {
    ValueID closure;
    Use use;
};
bool canMaterializeEnvironmentLate(FunctionAnalyses&, ValueID environment, const BitVector& frameClosures, std::span<const ClosureMadeAgain>);
// Finds the closures made again.
void materializeEnvironmentLate(IRBuilder&, FunctionAnalyses&, ValueID environment, const BitVector& frameClosures);

AllocationRemark remarkOf(const Function&, const FrameDecision&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
