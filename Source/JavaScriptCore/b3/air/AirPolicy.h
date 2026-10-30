/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "RegisterSet.h"
#include <limits>
#include <optional>
#include <wtf/PrintStream.h>

namespace JSC { namespace B3 { namespace Air {

// Only the ahead-of-time compiler sets these. Without it they are constants, so what hangs on them is not in the binary.
#if ENABLE(AOT)
#define AIR_POLICY_OF_CLIENT(type, name, value) type name { value }
#else
#define AIR_POLICY_OF_CLIENT(type, name, value) static constexpr type name { value }
#endif

struct RegisterPolicy {
    bool randomizesRegisterOrder { false };
    std::optional<unsigned> registerOrderSeed;
    double rareBlockPenalty { 0.001 };
    bool spillsEverything { false };

    AIR_POLICY_OF_CLIENT(bool, chargesForCalleeSaves, false);
    AIR_POLICY_OF_CLIENT(bool, discountsPreferredRegisterMoves, false);
    AIR_POLICY_OF_CLIENT(bool, hintsTwoAddressOperands, false);
    AIR_POLICY_OF_CLIENT(bool, usesSecondPreferredRegister, false);
    AIR_POLICY_OF_CLIENT(bool, evictsHolderOfPreferredRegister, false);
    // What a patchpoint runs keeps at most the lower 64 bits of a vector register that the patchpoint does not clobber. So no vector is held in a register across one.
    AIR_POLICY_OF_CLIENT(bool, patchpointsKeepOnlyLowerHalvesOfVectors, false);

    RegisterSet unsavedCalleeSaves;
    RegisterSet savedByCallee;
    RegisterSet calleeSavesClobberedAtSuspensions;
    RegisterSet pinned;
};

struct LiveRangeSplitPolicy {
    bool splitsAroundLoops { false };
    double splitMultiplier { 2.0 };
    double maxLoopFractionOfLiveRange { 0.75 };

    AIR_POLICY_OF_CLIENT(bool, splitsAroundClobbersEarly, false);
    AIR_POLICY_OF_CLIENT(bool, spansStretchesOfClobbers, false);
    AIR_POLICY_OF_CLIENT(bool, limitsClobberSitesPerSplit, false);
    AIR_POLICY_OF_CLIENT(bool, splitsIntoClustersOverBlocks, false);
    AIR_POLICY_OF_CLIENT(bool, splitsAroundFrame, false);
};

struct FramePolicy {
    bool coalescesSpillSlots { true };

    AIR_POLICY_OF_CLIENT(bool, emitsFrame, false);
    AIR_POLICY_OF_CLIENT(bool, shrinkWraps, false);
    // With shrinkWraps: a function of which no path needs a frame has none, and a frame that some path needs is set up at the entry.
    AIR_POLICY_OF_CLIENT(bool, setsUpFrameAtEntry, false);
    AIR_POLICY_OF_CLIENT(unsigned, maxBytesAllocatedBeforeStackCheck, 0);
    // A frame of at least this many bytes is tested against the end of the stack where it is set up, and one that does not fit is an error at the call in the caller. Nothing is written ahead of such a frame.
    AIR_POLICY_OF_CLIENT(unsigned, smallestFrameTestedAtSetup, std::numeric_limits<unsigned>::max());
    RegisterSet registersClobberedByFrameSetup;

    AIR_POLICY_OF_CLIENT(bool, mayPlaceSlotsAboveFrameRecord, false);
    AIR_POLICY_OF_CLIENT(bool, mayPushCalleeSaves, false);
    AIR_POLICY_OF_CLIENT(bool, mayPushSpillsAtFrameSetup, false);
    AIR_POLICY_OF_CLIENT(bool, ordersSlotsByUse, false);
    AIR_POLICY_OF_CLIENT(bool, prefersStackPointerAddresses, false);
    AIR_POLICY_OF_CLIENT(bool, mayOmitFrameRecord, false);
    // A frame without a record saves instructions each time it is set up, and its addresses are a byte longer. For a function that sets its frame up at most entries.
    AIR_POLICY_OF_CLIENT(unsigned, maxBytesAddedWithoutFrameRecord, 0);
};

struct LayoutPolicy {
    AIR_POLICY_OF_CLIENT(bool, movesBlocksWithoutSuccessorsToEnd, false);
    AIR_POLICY_OF_CLIENT(bool, placesRareBlocksInGaps, false);
    AIR_POLICY_OF_CLIENT(bool, placesRareBlocksAfterGuards, false);
    AIR_POLICY_OF_CLIENT(bool, makesGapsForRareBlocks, false);
    AIR_POLICY_OF_CLIENT(bool, ordersBlocksByEdgeFrequency, false);
    AIR_POLICY_OF_CLIENT(bool, recordsRareCodeStart, false);
    AIR_POLICY_OF_CLIENT(bool, mergesIdenticalBlocks, false);
    AIR_POLICY_OF_CLIENT(bool, duplicatesReturns, false);
};

struct PeepholePolicy {
    bool pairsSpillAccesses { true };

    AIR_POLICY_OF_CLIENT(bool, forwardsRegisterCopies, false);
    AIR_POLICY_OF_CLIENT(bool, hoistsCommonMoves, false);
    AIR_POLICY_OF_CLIENT(bool, reusesConditionFlags, false);
    AIR_POLICY_OF_CLIENT(bool, pairsMemoryAccesses, false);
    AIR_POLICY_OF_CLIENT(bool, resequencesMovesBeforePatches, false);
    AIR_POLICY_OF_CLIENT(bool, emitsShufflesInPairOrder, false);
    AIR_POLICY_OF_CLIENT(uint32_t, trapReach, 0);
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
