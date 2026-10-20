/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirPolicy.h"

#if ENABLE(B3_JIT)

#include "Options.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace B3 { namespace Air {

Policy Policy::fromProcessOptions()
{
    Policy policy;
    policy.registers.randomizesRegisterOrder = Options::airRandomizeRegs();
    if (unsigned seed = Options::airRandomizeRegsSeed())
        policy.registers.registerOrderSeed = seed;
    policy.registers.rareBlockPenalty = Options::rareBlockPenalty();
    policy.registers.spillsEverything = Options::airGreedyRegAllocSpillsEverything();
    policy.splitting.splitsAroundLoops = Options::airGreedyRegAllocSplitAroundLoops();
    policy.splitting.splitMultiplier = Options::airGreedyRegAllocSplitMultiplier();
    policy.splitting.maxLoopFractionOfLiveRange = Options::airGreedyRegAllocLoopSplitMaxLoopFraction();
    policy.frame.coalescesSpillSlots = Options::coalesceSpillSlots();
    policy.peepholes.pairsSpillAccesses = Options::useAirOptimizePairedLoadStore();
    return policy;
}

#define AIR_POLICY_PRINT_IF_SET(part, field) \
    do { \
        if (part.field) \
            out.print(comma, #field ""_s); \
    } while (false)

#define AIR_POLICY_PRINT_IF_DIFFERENT(part, field) \
    do { \
        if (part.field != decltype(part) { }.field) \
            out.print(comma, #field " = "_s, part.field); \
    } while (false)

#define AIR_POLICY_PRINT_IF_NOT_EMPTY(part, field) \
    do { \
        if (!part.field.isEmpty()) \
            out.print(comma, #field ": "_s, part.field); \
    } while (false)

void Policy::dump(PrintStream& out) const
{
    CommaPrinter comma;

    AIR_POLICY_PRINT_IF_SET(registers, randomizesRegisterOrder);
    if (registers.registerOrderSeed)
        out.print(comma, "registerOrderSeed = "_s, *registers.registerOrderSeed);
    AIR_POLICY_PRINT_IF_DIFFERENT(registers, rareBlockPenalty);
    AIR_POLICY_PRINT_IF_SET(registers, spillsEverything);
    AIR_POLICY_PRINT_IF_SET(registers, chargesForCalleeSaves);
    AIR_POLICY_PRINT_IF_SET(registers, discountsPreferredRegisterMoves);
    AIR_POLICY_PRINT_IF_SET(registers, hintsTwoAddressOperands);
    AIR_POLICY_PRINT_IF_SET(registers, usesSecondPreferredRegister);
    AIR_POLICY_PRINT_IF_SET(registers, evictsHolderOfPreferredRegister);
    AIR_POLICY_PRINT_IF_NOT_EMPTY(registers, unsavedCalleeSaves);
    AIR_POLICY_PRINT_IF_NOT_EMPTY(registers, savedByCallee);
    AIR_POLICY_PRINT_IF_NOT_EMPTY(registers, calleeSavesClobberedAtSuspensions);
    AIR_POLICY_PRINT_IF_NOT_EMPTY(registers, pinned);

    AIR_POLICY_PRINT_IF_SET(splitting, splitsAroundLoops);
    AIR_POLICY_PRINT_IF_DIFFERENT(splitting, splitMultiplier);
    AIR_POLICY_PRINT_IF_DIFFERENT(splitting, maxLoopFractionOfLiveRange);
    AIR_POLICY_PRINT_IF_SET(splitting, splitsAroundClobbersEarly);
    AIR_POLICY_PRINT_IF_SET(splitting, spansStretchesOfClobbers);
    AIR_POLICY_PRINT_IF_SET(splitting, limitsClobberSitesPerSplit);
    AIR_POLICY_PRINT_IF_SET(splitting, splitsIntoClustersOverBlocks);
    AIR_POLICY_PRINT_IF_SET(splitting, splitsAroundFrame);

    AIR_POLICY_PRINT_IF_DIFFERENT(frame, coalescesSpillSlots);
    AIR_POLICY_PRINT_IF_SET(frame, emitsFrame);
    AIR_POLICY_PRINT_IF_SET(frame, shrinkWraps);
    AIR_POLICY_PRINT_IF_DIFFERENT(frame, maxBytesAllocatedBeforeStackCheck);
    AIR_POLICY_PRINT_IF_NOT_EMPTY(frame, registersClobberedByFrameSetup);
    AIR_POLICY_PRINT_IF_SET(frame, mayPlaceSlotsAboveFrameRecord);
    AIR_POLICY_PRINT_IF_SET(frame, mayPushCalleeSaves);
    AIR_POLICY_PRINT_IF_SET(frame, mayPushSpillsAtFrameSetup);
    AIR_POLICY_PRINT_IF_SET(frame, ordersSlotsByUse);
    AIR_POLICY_PRINT_IF_SET(frame, prefersStackPointerAddresses);

    AIR_POLICY_PRINT_IF_SET(layout, movesBlocksWithoutSuccessorsToEnd);
    AIR_POLICY_PRINT_IF_SET(layout, placesRareBlocksInGaps);
    AIR_POLICY_PRINT_IF_SET(layout, placesRareBlocksAfterGuards);
    AIR_POLICY_PRINT_IF_SET(layout, ordersBlocksByEdgeFrequency);
    AIR_POLICY_PRINT_IF_SET(layout, recordsRareCodeStart);
    AIR_POLICY_PRINT_IF_SET(layout, mergesIdenticalBlocks);
    AIR_POLICY_PRINT_IF_SET(layout, duplicatesReturns);

    AIR_POLICY_PRINT_IF_DIFFERENT(peepholes, pairsSpillAccesses);
    AIR_POLICY_PRINT_IF_SET(peepholes, forwardsRegisterCopies);
    AIR_POLICY_PRINT_IF_SET(peepholes, hoistsCommonMoves);
    AIR_POLICY_PRINT_IF_SET(peepholes, reusesConditionFlags);
    AIR_POLICY_PRINT_IF_SET(peepholes, pairsMemoryAccesses);
    AIR_POLICY_PRINT_IF_SET(peepholes, resequencesMovesBeforePatches);
    AIR_POLICY_PRINT_IF_DIFFERENT(peepholes, trapReach);
}

#undef AIR_POLICY_PRINT_IF_SET
#undef AIR_POLICY_PRINT_IF_DIFFERENT
#undef AIR_POLICY_PRINT_IF_NOT_EMPTY

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
