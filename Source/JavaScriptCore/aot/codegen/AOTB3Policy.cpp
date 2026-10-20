/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTB3Policy.h"

#if ENABLE(AOT)

#include "AOTBackEndReport.h"
#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTFaultTable.h"
#include "AOTRelocation.h"
#include "AOTStubBodyConvention.h"
#include "CPU.h"
#include "GPRInfo.h"
#include "JSCJSValue.h"
#include "MinimumReservedZoneSize.h"
#include <limits>
#include <optional>

#if CPU(X86_64)
#include "AOTInstance.h"
#endif

namespace JSC { namespace AOT {

static constexpr unsigned minCasesForJumpTableInConstantData = 7;
static constexpr unsigned maxBytesAllocatedBeforeStackCheck = 4 * KB;

static_assert(maxBytesAllocatedBeforeStackCheck + maxStackUsedWithoutCheck < minimumReservedZoneSize);

static constexpr uint32_t trapReach = FaultSite::reach - JSValue::ValueUndefined;

static_assert(JSValue::ValueNull < JSValue::ValueUndefined, "a trapping access faults for these two bases");

#if CPU(X86_64)

static_assert(AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask) >= std::numeric_limits<int8_t>::min() && AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask) <= std::numeric_limits<int8_t>::max());

static std::optional<B3::PinnedConstant> findPinnedConstant(uint64_t bits)
{
    if (bits == static_cast<uint64_t>(JSValue::NumberTag))
        return B3::PinnedConstant { .reg = instanceGPR, .offset = static_cast<int32_t>(AOT_INSTANCE_OFFSET(numberTag)), .replacesImmediates = false };
    if (bits == static_cast<uint64_t>(JSValue::NotCellMask))
        return B3::PinnedConstant { .reg = instanceGPR, .offset = static_cast<int32_t>(AOT_INSTANCE_OFFSET(notCellMask)), .replacesImmediates = false };
    if (bits == ~static_cast<uint64_t>(JSValue::ValueUndefined))
        return B3::PinnedConstant { .reg = instanceGPR, .offset = static_cast<int32_t>(AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask)), .replacesImmediates = true };
    return std::nullopt;
}

#else

static std::optional<B3::PinnedConstant> findPinnedConstant(uint64_t bits)
{
    if (bits == static_cast<uint64_t>(JSValue::NumberTag))
        return B3::PinnedConstant { .reg = GPRInfo::numberTagRegister, .offset = std::nullopt, .replacesImmediates = false };
    if (bits == static_cast<uint64_t>(JSValue::NotCellMask))
        return B3::PinnedConstant { .reg = GPRInfo::notCellMaskRegister, .offset = std::nullopt, .replacesImmediates = false };
    return std::nullopt;
}

#endif

static B3::AddressingPolicy addressingPolicyFor(B3::RelocationLowering& lowering)
{
    B3::AddressingPolicy addressing;
    addressing.isPositionIndependent = true;
    addressing.namesPinnedRegistersDirectly = true;
    addressing.dataSectionSpace = static_cast<uint32_t>(RelocationSpace::Constants);
    addressing.jumpTableSpace = static_cast<uint32_t>(RelocationSpace::JumpTables);
    addressing.relocationLowering = &lowering;
    addressing.findPinnedConstant = findPinnedConstant;
    return addressing;
}

static B3::UpstreamPolicy upstreamPolicy()
{
    B3::UpstreamPolicy upstream;
    upstream.optLevel = 2;
    upstream.maxTailDuplicationBlockSize = 1;
    upstream.maxTailDuplicationBlockSuccessors = 3;
    upstream.specializesSelects = true;
    upstream.duplicatesTails = true;
    upstream.canonicalizesIncrements = false;
    upstream.hoistsLoopInvariantValues = true;
    upstream.eliminatesWasmGCAllocations = false;
    upstream.usesWasmFaultSignalHandler = false;
    upstream.allowsSIMD = false;
    upstream.forcesSIMD = false;
    return upstream;
}

static B3::OptimizationPolicy optimizationPolicyFor(const FunctionBackEndTraits& traits)
{
    B3::OptimizationPolicy optimization;
    optimization.redundancyElimination = B3::RedundancyElimination::MemoryNumbering;
    optimization.splitsBranchConditions = true;
    optimization.estimatesFrequencies = true;
    optimization.returnsAreUnlikely = traits.returnsAreUnlikely;
    optimization.checksVariablesAreSet = true;
    optimization.threadsJumps = true;
    optimization.foldsImpliedComparisons = true;
    optimization.reducesWithRanges = true;
    optimization.narrowsIntegers = true;
    optimization.chainsAssociativeOperations = true;
    optimization.stopsSwitchInferenceAtRareEdges = true;
    optimization.convertsBranchesToSelects = true;
    optimization.mergesAdjacentConstantStores = true;
    optimization.sinksValues = true;
    optimization.placesConstantsByFrequency = true;
    return optimization;
}

static B3::LoopPolicy loopPolicy()
{
    B3::LoopPolicy loops;
    loops.rotates = true;
    loops.foldsComparisonsWithBounds = true;
    loops.versions = true;
    loops.clampsLoopBounds = true;
    loops.stripMinesPolls = true;
    loops.hoistsOnlyFromLikelyPaths = true;
    return loops;
}

static B3::ValidationPolicy validationPolicyFor(const BuildContext& build)
{
    B3::ValidationPolicy validation;
    if (build.options().validatesBackEnd)
        validation.level = B3::ValidationLevel::AtEachPhase;
    else if (build.options().verifiesAfterEveryPass || build.validates())
        validation.level = B3::ValidationLevel::AtBoundaries;
    else
        validation.level = B3::ValidationLevel::FromOptions;
    validation.checksKnownPropertiesAtRunTime = build.validates();
    return validation;
}

static B3::Air::RegisterPolicy registerPolicyFor(const FunctionBackEndTraits& traits)
{
    B3::Air::RegisterPolicy registers;
    registers.chargesForCalleeSaves = true;
    registers.discountsPreferredRegisterMoves = true;
    registers.hintsTwoAddressOperands = true;
    registers.usesSecondPreferredRegister = true;
    registers.evictsHolderOfPreferredRegister = true;
    registers.unsavedCalleeSaves = unsavedCalleeSaves();
    switch (traits.convention) {
    case BackEndConvention::Function:
        break;
    case BackEndConvention::StubBody:
        registers.savedByCallee = StubBodyConvention::registersSavedByCallee();
        // A frame record lists the registers that unwinding restores as one run of slots. A stub body would save others between them, so it uses none of them.
        registers.pinned = registers.savedByCallee;
        registers.pinned.filter(RegisterSet::vmCalleeSaveRegisters());
        break;
    }
    if (traits.savesFrameOnSuspend) {
        registers.calleeSavesClobberedAtSuspensions = calleeSavesClobberedAtSuspensions();
        registers.pinned.merge(RegisterSet::fromIterable(pinnedFPRsInFrameSavingFunctions));
    }
    return registers;
}

static B3::Air::LiveRangeSplitPolicy liveRangeSplitPolicy()
{
    B3::Air::LiveRangeSplitPolicy splitting;
    splitting.splitsAroundClobbersEarly = true;
    splitting.spansStretchesOfClobbers = true;
    splitting.limitsClobberSitesPerSplit = true;
    splitting.splitsIntoClustersOverBlocks = true;
    splitting.splitsAroundFrame = true;
    return splitting;
}

static B3::Air::FramePolicy framePolicyFor(const FunctionBackEndTraits& traits)
{
    B3::Air::FramePolicy frame;
    frame.emitsFrame = true;
    frame.shrinkWraps = !traits.savesFrameOnSuspend;
    frame.maxBytesAllocatedBeforeStackCheck = maxBytesAllocatedBeforeStackCheck;
    if constexpr (isARM64())
        frame.registersClobberedByFrameSetup = RegisterSet::macroClobberedGPRs();
    frame.mayPlaceSlotsAboveFrameRecord = !traits.readsCallerFrame && !traits.isAsync && !traits.savesFrameOnSuspend;
    frame.mayPushCalleeSaves = !traits.savesFrameOnSuspend;
    frame.mayPushSpillsAtFrameSetup = !traits.savesFrameOnSuspend;
    frame.ordersSlotsByUse = true;
    frame.prefersStackPointerAddresses = true;
    return frame;
}

static B3::Air::LayoutPolicy layoutPolicyFor(const FunctionBackEndTraits& traits)
{
    B3::Air::LayoutPolicy layout;
    layout.movesBlocksWithoutSuccessorsToEnd = true;
    layout.placesRareBlocksInGaps = true;
    layout.placesRareBlocksAfterGuards = traits.isCold;
    layout.ordersBlocksByEdgeFrequency = true;
    layout.recordsRareCodeStart = true;
    layout.mergesIdenticalBlocks = true;
    layout.duplicatesReturns = true;
    return layout;
}

static B3::Air::PeepholePolicy peepholePolicy()
{
    B3::Air::PeepholePolicy peepholes;
    peepholes.pairsSpillAccesses = true;
    peepholes.forwardsRegisterCopies = true;
    peepholes.hoistsCommonMoves = true;
    peepholes.reusesConditionFlags = true;
    peepholes.pairsMemoryAccesses = true;
    peepholes.resequencesMovesBeforePatches = true;
    peepholes.trapReach = trapReach;
    return peepholes;
}

B3::Policy b3PolicyFor(const BuildContext& build, const FunctionBackEndTraits& traits, B3::RelocationLowering& lowering)
{
    RELEASE_ASSERT(traits.convention == BackEndConvention::Function || !traits.savesFrameOnSuspend);

    B3::Policy policy;
    policy.addressing = addressingPolicyFor(lowering);
    policy.allocation.usesArena = true;
    policy.allocation.keepsValuesAfterLowering = true;
    policy.upstream = upstreamPolicy();
    policy.optimization = optimizationPolicyFor(traits);
    policy.selection.extendsInstructionSelection = true;
    policy.selection.attributesTrapsToAccesses = true;
    policy.loops = loopPolicy();
    policy.switchLowering.usesBitTests = true;
    policy.switchLowering.minCasesForJumpTable = minCasesForJumpTableInConstantData;
    policy.knownProperties.trustsKnownRanges = true;
    policy.knownProperties.trustsKnownDereferenceableBytes = true;
    policy.knownProperties.trustsKnownInductions = true;
    policy.validation = validationPolicyFor(build);
    policy.limits = b3LimitValuesFrom(build);
    policy.targetFeatures = build.targetFeatures();
    policy.collectsRemarks = build.options().collectsDeclines();

    policy.air.registers = registerPolicyFor(traits);
    policy.air.splitting = liveRangeSplitPolicy();
    policy.air.frame = framePolicyFor(traits);
    policy.air.layout = layoutPolicyFor(traits);
    policy.air.peepholes = peepholePolicy();

    for (unsigned i = 0; i < B3::numberOfSwitches; ++i) {
        auto policySwitch = static_cast<B3::PolicySwitch>(i);
        if (!build.recordedOptions().isEnabled(policySwitch, traits.function))
            policy.disable(policySwitch);
    }
    policy.applyDisabledSwitches();
    return policy;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
