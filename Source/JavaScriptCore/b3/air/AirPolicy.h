/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "RegisterSet.h"
#include <optional>
#include <wtf/PrintStream.h>

namespace JSC { namespace B3 { namespace Air {

struct RegisterPolicy {
    bool randomizesRegisterOrder { false };
    std::optional<unsigned> registerOrderSeed;
    double rareBlockPenalty { 0.001 };
    bool spillsEverything { false };

    bool chargesForCalleeSaves { false };
    bool discountsPreferredRegisterMoves { false };
    bool hintsTwoAddressOperands { false };
    bool usesSecondPreferredRegister { false };
    bool evictsHolderOfPreferredRegister { false };

    RegisterSet unsavedCalleeSaves;
    RegisterSet savedByCallee;
    RegisterSet calleeSavesClobberedAtSuspensions;
    RegisterSet pinned;
};

struct LiveRangeSplitPolicy {
    bool splitsAroundLoops { false };
    double splitMultiplier { 2.0 };
    double maxLoopFractionOfLiveRange { 0.75 };

    bool splitsAroundClobbersEarly { false };
    bool spansStretchesOfClobbers { false };
    bool limitsClobberSitesPerSplit { false };
    bool splitsIntoClustersOverBlocks { false };
    bool splitsAroundFrame { false };
};

struct FramePolicy {
    bool coalescesSpillSlots { true };

    bool emitsFrame { false };
    bool shrinkWraps { false };
    unsigned maxBytesAllocatedBeforeStackCheck { 0 };
    RegisterSet registersClobberedByFrameSetup;

    bool mayPlaceSlotsAboveFrameRecord { false };
    bool mayPushCalleeSaves { false };
    bool mayPushSpillsAtFrameSetup { false };
    bool ordersSlotsByUse { false };
    bool prefersStackPointerAddresses { false };
};

struct LayoutPolicy {
    bool movesBlocksWithoutSuccessorsToEnd { false };
    bool placesRareBlocksInGaps { false };
    bool placesRareBlocksAfterGuards { false };
    bool ordersBlocksByEdgeFrequency { false };
    bool recordsRareCodeStart { false };
    bool mergesIdenticalBlocks { false };
    bool duplicatesReturns { false };
};

struct PeepholePolicy {
    bool pairsSpillAccesses { true };

    bool forwardsRegisterCopies { false };
    bool hoistsCommonMoves { false };
    bool reusesConditionFlags { false };
    bool pairsMemoryAccesses { false };
    bool resequencesMovesBeforePatches { false };
    uint32_t trapReach { 0 };
};

struct Policy {
    RegisterPolicy registers;
    LiveRangeSplitPolicy splitting;
    FramePolicy frame;
    LayoutPolicy layout;
    PeepholePolicy peepholes;

    JS_EXPORT_PRIVATE static Policy fromProcessOptions();
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
