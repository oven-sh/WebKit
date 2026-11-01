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
#include "AOTHeaps.h"
#include "AOTRelocation.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubBodyConvention.h"
#include "AOTThreshold.h"
#include "CPU.h"
#include "GPRInfo.h"
#include "JSCJSValue.h"
#include "MinimumReservedZoneSize.h"
#include <cmath>
#include <limits>
#include <optional>

#if CPU(X86_64)
#include "AOTInstance.h"
#endif

namespace JSC { namespace AOT {

AOT_THRESHOLD(unsigned, minCasesForJumpTableInConstantData, 7);
static constexpr unsigned maxBytesAllocatedBeforeStackCheck = 4 * KB;
// With this much the code of a large program is no larger than with a record in every frame: the functions that shrink make up for those that grow.
AOT_THRESHOLD(unsigned, maxBytesAddedToHotFunctionWithoutFrameRecord, 96);
AOT_THRESHOLD(unsigned, maxBytesAddedToWarmFunctionWithoutFrameRecord, 16);

static_assert(maxBytesAllocatedBeforeStackCheck + maxStackUsedWithoutCheck < minimumReservedZoneSize);

static constexpr uint32_t trapReach = FaultSite::reach - (undefinedCell32 << cell32Shift);

static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueUndefined == undefinedCell32, "a trapping access faults for these two bases, and for what these two words decode to");

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
    // Three vector instructions make 1 or NaN in a register, and ten bytes any of the others. An operand in memory costs no instruction and four bytes.
    struct Constant {
        uint64_t bits;
        ptrdiff_t offset;
    };
    static constexpr Constant constants[] = {
        { std::bit_cast<uint64_t>(1.0), AOT_INSTANCE_PREFIX_OFFSET(one) },
        { PNaNAsBits, AOT_INSTANCE_PREFIX_OFFSET(pureNaN) },
        { static_cast<uint64_t>(JSValue::DoubleEncodeOffset), AOT_INSTANCE_PREFIX_OFFSET(doubleEncodeOffset) },
        { static_cast<uint64_t>(JSValue::NumberTag | 1), AOT_INSTANCE_PREFIX_OFFSET(boxedOne) },
        { static_cast<uint64_t>(undefinedInteger), AOT_INSTANCE_PREFIX_OFFSET(undefinedInteger) },
        { static_cast<uint64_t>(nullInteger), AOT_INSTANCE_PREFIX_OFFSET(nullInteger) },
        { static_cast<uint64_t>(absentInteger), AOT_INSTANCE_PREFIX_OFFSET(absentInteger) },
        { undefinedDouble, AOT_INSTANCE_PREFIX_OFFSET(undefinedDouble) },
        { nullDouble, AOT_INSTANCE_PREFIX_OFFSET(nullDouble) },
        { absentDouble, AOT_INSTANCE_PREFIX_OFFSET(absentDouble) },
        { static_cast<uint64_t>(std::numeric_limits<int64_t>::min()), AOT_INSTANCE_PREFIX_OFFSET(minInt64) },
        { static_cast<uint64_t>(minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(minIntegerAddedTo) },
        { static_cast<uint64_t>(maxIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(maxIntegerAddedTo) },
        { static_cast<uint64_t>(-minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(distanceOfIntegersAddedToFromZero) },
        { static_cast<uint64_t>(maxIntegerAddedTo - minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(spanOfIntegersAddedTo) },
    };
    for (const Constant& constant : constants) {
        if (bits == constant.bits)
            return B3::PinnedConstant { .reg = instanceGPR, .offset = static_cast<int32_t>(constant.offset), .replacesImmediates = false };
    }
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
    upstream.allowsSIMD = true;
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
    optimization.mergesBlocksAfterForwardingJumps = true;
    optimization.keepsOriginsOfDemotedValues = true;
    return optimization;
}

static B3::LoopPolicy loopPolicyFor(const FunctionBackEndTraits& traits)
{
    B3::LoopPolicy loops;
    loops.rotates = true;
    loops.foldsComparisonsWithBounds = true;
    loops.versions = true;
    loops.clampsLoopBounds = true;
    loops.pollsBetweenStretches = true;
    loops.vectorizes = std::isfinite(traits.leastFrequencyOfWarmCode);
    loops.unrolls = std::isfinite(traits.leastFrequencyOfWarmCode);
    loops.promotesMemory = true;
    loops.minFrequencyOfUnrolledLoop = traits.leastFrequencyOfWarmCode;
    loops.hoistsOnlyFromLikelyPaths = true;
    loops.hoistsImmutableLoadsPastWrites = true;
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
        validation.level = lowestValidationLevel;
    validation.checksKnownPropertiesAtRunTime = build.validates();
    validation.checksSpillSlots = build.options().checksSpillSlots;
    validation.checksAllocatedRegisters = build.options().checksAllocatedRegisters;
    validation.overlooksWritesOnUnlikelyPaths = build.isSabotaged(Sabotage::MergedLoadsOutlastWritesOnUnlikelyPaths);
    validation.omitsStridedLimitGuard = build.isSabotaged(Sabotage::StridedIndexesStayInRange);
    validation.overlooksWritesBehindWordsReadAgain = build.isSabotaged(Sabotage::WordsReadAgainOutlastWrites);
    validation.overlooksWritesToWhatLoopsReadAgain = build.isSabotaged(Sabotage::VariablesReadAgainInLoopsOutlastWrites);
    validation.overlooksWritesToWordsCarriedAroundLoops = build.isSabotaged(Sabotage::WordsCarriedAroundLoopsOutlastWrites);
    validation.overlooksWritesBetweenLoadsAndTheirUsers = build.isSabotaged(Sabotage::LoadsFoldedIntoTheirUsersOutlastWrites);
    validation.overlooksWritesBetweenPairedLoads = build.isSabotaged(Sabotage::PairedLoadsOutlastWrites);
    validation.leavesRoomForOneTurnTooFew = build.isSabotaged(Sabotage::WrittenOutTurnsAlwaysFit);
    validation.overlooksWritesBetweenReadsOfBound = build.isSabotaged(Sabotage::BoundsReadTwiceOutlastWrites);
    validation.takesAccessesOfVectorTurnsToLieApart = build.isSabotaged(Sabotage::VectorTurnsMeetNoOtherAccess);
    validation.takesLanesToHoldWholeValues = build.isSabotaged(Sabotage::LanesHoldWhateverIsComputed);
    validation.takesSumsOfNumbersToBeWhole = build.isSabotaged(Sabotage::SumsOfNumbersAreWhole);
    validation.comparesAllLanesWithSign = build.isSabotaged(Sabotage::SignsOfComparedLanesDoNotMatter);
    validation.setsUpFrameBehindWrites = build.isSabotaged(Sabotage::FramesAlwaysFit);
    validation.overlooksColdUsesOfClustersOverBlocks = build.isSabotaged(Sabotage::LiveRangesSplitTwiceHaveNoColdUses);
    using MoveLeftOut = B3::MoveLeftOutByRegisterAllocator;
    static constexpr std::pair<Sabotage, MoveLeftOut> movesLeftOut[] = {
        { Sabotage::StoresBehindClustersAreNotNeeded, MoveLeftOut::StoreBehindCluster },
        { Sabotage::LoadsAheadOfClustersAreNotNeeded, MoveLeftOut::LoadAheadOfCluster },
        { Sabotage::StoresAheadOfCallsAreNotNeeded, MoveLeftOut::StoreAheadOfClobber },
        { Sabotage::LoadsBehindCallsAreNotNeeded, MoveLeftOut::LoadBehindClobber },
        { Sabotage::StoresBehindSpilledDefinitionsAreNotNeeded, MoveLeftOut::StoreBehindSpilledDefinition },
    };
    for (auto [sabotage, move] : movesLeftOut) {
        if (build.isSabotaged(sabotage))
            validation.moveLeftOutByRegisterAllocator = move;
    }
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
    registers.patchpointsKeepOnlyLowerHalvesOfVectors = true;
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
    splitting.splitsAroundClobbersThatRead = true;
    splitting.keepsUnusedTmpsOnStackThroughLoops = true;
    // Elsewhere nothing but a move reads the stack.
    splitting.spillsTmpsReadInPlaceFirst = isX86();
    splitting.spansStretchesOfClobbers = true;
    splitting.leastCostOfSplitAroundClobber = 1.0f / 1024;
    splitting.splitsIntoClustersOverBlocks = true;
    splitting.splitsAroundFrame = true;
    return splitting;
}

static B3::Air::FramePolicy framePolicyFor(const BuildContext& build, const FunctionBackEndTraits& traits)
{
    B3::Air::FramePolicy frame;
    frame.emitsFrame = true;
    // The stack is tested where the frame is set up, and a frame that does not fit is an error at the call in the caller. Behind the entry the function may stand in a try of its own, which the error would pass by.
    frame.shrinkWraps = !traits.savesFrameOnSuspend;
    frame.setsUpFrameAtEntry = traits.hasHandlers || traits.rejectsOnThrow;
    frame.maxBytesAllocatedBeforeStackCheck = maxBytesAllocatedBeforeStackCheck;
    frame.smallestFrameTestedAtSetup = traits.smallestFrameTestedAtSetup;
    if constexpr (isARM64())
        frame.registersClobberedByFrameSetup = RegisterSet::macroClobberedGPRs();
    frame.mayPlaceSlotsAboveFrameRecord = !traits.readsCallerFrame && !traits.isAsync && !traits.savesFrameOnSuspend;
    frame.mayPushCalleeSaves = !traits.savesFrameOnSuspend;
    frame.mayPushSpillsAtFrameSetup = !traits.savesFrameOnSuspend;
    frame.ordersSlotsByUse = true;
    frame.prefersStackPointerAddresses = true;
    // A saved frame is laid out around its record, and the stub that rejects a promise returns from the frame through it.
    frame.mayOmitFrameRecord = isX86_64() && !traits.savesFrameOnSuspend && !traits.rejectsOnThrow && !build.options().validatesBackEnd;
    frame.maxBytesAddedWithoutFrameRecord = traits.isHot ? maxBytesAddedToHotFunctionWithoutFrameRecord : traits.isCold ? 0 : maxBytesAddedToWarmFunctionWithoutFrameRecord;
    return frame;
}

static B3::Air::LayoutPolicy layoutPolicyFor(const FunctionBackEndTraits& traits)
{
    B3::Air::LayoutPolicy layout;
    layout.movesBlocksWithoutSuccessorsToEnd = true;
    layout.placesRareBlocksInGaps = true;
    // Most calls of a large program enter functions that are taken for cold. Behind its guard a rare block makes the usual way jump at every guard, which only code that runs once can afford.
    layout.placesRareBlocksAfterGuards = traits.runsOnce;
    layout.makesGapsForRareBlocks = traits.isCold && !traits.runsOnce;
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
    peepholes.emitsShufflesInPairOrder = true;
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
    policy.loops = loopPolicyFor(traits);
    // Lists are. Views are not.
    policy.holdsMostlyShortArrays = createSharedTask<bool(B3::Procedure&, const B3::HeapRange&)>([](B3::Procedure& procedure, const B3::HeapRange& range) {
        AOTHeaps& heaps = AOTHeaps::of(procedure);
        return !range.overlaps(heaps.rangeOf(heaps.entire(Typed::AbstractHeap::TypedArrayElements)));
    });
    policy.switchLowering.usesBitTests = true;
    policy.switchLowering.usesValueTables = true;
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
    policy.air.frame = framePolicyFor(build, traits);
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
