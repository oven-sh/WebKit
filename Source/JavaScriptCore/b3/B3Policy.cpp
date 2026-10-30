/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3Policy.h"

#if ENABLE(B3_JIT)

#include "MacroAssembler.h"
#include "Options.h"
#include <limits>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace B3 {

Policy Policy::fromProcessOptions()
{
    Policy policy;
    policy.upstream.optLevel = Options::defaultB3OptLevel();
    policy.upstream.maxTailDuplicationBlockSize = Options::maxB3TailDupBlockSize();
    policy.upstream.maxTailDuplicationBlockSuccessors = Options::maxB3TailDupBlockSuccessors();
    policy.upstream.maxWasmGCEpochSnapshotEntries = Options::maxB3WasmGCEpochSnapshotEntries();
    policy.upstream.specializesSelects = Options::useB3SpecializeSelect();
    policy.upstream.duplicatesTails = Options::useB3TailDup();
    policy.upstream.canonicalizesIncrements = Options::useB3CanonicalizePrePostIncrements();
    policy.upstream.hoistsLoopInvariantValues = Options::useB3HoistLoopInvariantValues();
    policy.upstream.eliminatesWasmGCAllocations = Options::useB3EliminateWasmGCAllocations();
    policy.upstream.usesWasmFaultSignalHandler = Options::useWasmFaultSignalHandler();
    policy.upstream.allowsSIMD = Options::useWasmSIMD();
    policy.upstream.forcesSIMD = Options::forceAllFunctionsToUseSIMD();
    policy.limits.set(Limit::MaxLeavesToRebalance, std::numeric_limits<uint64_t>::max());
    policy.limits.set(Limit::MaxCompareChainSearchSize, std::numeric_limits<uint64_t>::max());
    policy.air = Air::Policy::fromProcessOptions();
    return policy;
}

#define CLEAR_FIELD_CASE(policySwitch, field) \
    case PolicySwitch::policySwitch: \
        field = false; \
        break

void Policy::disable(PolicySwitch policySwitch)
{
#if ENABLE(AOT)
    switch (policySwitch) {
    case PolicySwitch::MemoryNumbering:
        optimization.redundancyElimination = RedundancyElimination::CommonSubexpressions;
        break;
    CLEAR_FIELD_CASE(SplitBranchConditions, optimization.splitsBranchConditions);
    CLEAR_FIELD_CASE(EstimateFrequencies, optimization.estimatesFrequencies);
    CLEAR_FIELD_CASE(ThreadJumps, optimization.threadsJumps);
    CLEAR_FIELD_CASE(FoldImpliedComparisons, optimization.foldsImpliedComparisons);
    CLEAR_FIELD_CASE(ReduceWithRanges, optimization.reducesWithRanges);
    CLEAR_FIELD_CASE(NarrowIntegers, optimization.narrowsIntegers);
    CLEAR_FIELD_CASE(ChainAssociativeOperations, optimization.chainsAssociativeOperations);
    CLEAR_FIELD_CASE(ConvertBranchesToSelects, optimization.convertsBranchesToSelects);
    CLEAR_FIELD_CASE(MergeAdjacentConstantStores, optimization.mergesAdjacentConstantStores);
    CLEAR_FIELD_CASE(SinkValues, optimization.sinksValues);
    CLEAR_FIELD_CASE(PlaceConstantsByFrequency, optimization.placesConstantsByFrequency);
    CLEAR_FIELD_CASE(ExtendInstructionSelection, selection.extendsInstructionSelection);
    case PolicySwitch::PinnedConstants:
        addressing.findPinnedConstant = nullptr;
        break;
    CLEAR_FIELD_CASE(HoistLoopInvariantValues, upstream.hoistsLoopInvariantValues);
    CLEAR_FIELD_CASE(RotateLoops, loops.rotates);
    CLEAR_FIELD_CASE(FoldComparisonsWithBounds, loops.foldsComparisonsWithBounds);
    CLEAR_FIELD_CASE(VersionLoops, loops.versions);
    CLEAR_FIELD_CASE(ClampLoopBounds, loops.clampsLoopBounds);
    CLEAR_FIELD_CASE(VectorizeLoops, loops.vectorizes);
    case PolicySwitch::UnrollLoops:
        loops.unrolls = false;
        loops.pollsBetweenStretches = false;
        break;
    CLEAR_FIELD_CASE(BitTests, switchLowering.usesBitTests);
    CLEAR_FIELD_CASE(KnownRanges, knownProperties.trustsKnownRanges);
    CLEAR_FIELD_CASE(KnownDereferenceableBytes, knownProperties.trustsKnownDereferenceableBytes);
    CLEAR_FIELD_CASE(KnownInductions, knownProperties.trustsKnownInductions);
    CLEAR_FIELD_CASE(ChargeForCalleeSaves, air.registers.chargesForCalleeSaves);
    case PolicySwitch::PreferredRegisters:
        air.registers.discountsPreferredRegisterMoves = false;
        air.registers.hintsTwoAddressOperands = false;
        air.registers.usesSecondPreferredRegister = false;
        air.registers.evictsHolderOfPreferredRegister = false;
        break;
    case PolicySwitch::SplitLiveRanges:
        air.splitting.splitsAroundClobbersEarly = false;
        air.splitting.spansStretchesOfClobbers = false;
        air.splitting.limitsClobberSitesPerSplit = false;
        air.splitting.splitsIntoClustersOverBlocks = false;
        break;
    CLEAR_FIELD_CASE(SplitLiveRangesAroundFrame, air.splitting.splitsAroundFrame);
    CLEAR_FIELD_CASE(ShrinkWrapFrame, air.frame.shrinkWraps);
    case PolicySwitch::FrameLayout:
        air.frame.mayPlaceSlotsAboveFrameRecord = false;
        air.frame.mayPushCalleeSaves = false;
        air.frame.mayPushSpillsAtFrameSetup = false;
        air.frame.ordersSlotsByUse = false;
        air.frame.prefersStackPointerAddresses = false;
        break;
    CLEAR_FIELD_CASE(OmitFrameRecord, air.frame.mayOmitFrameRecord);
    CLEAR_FIELD_CASE(MoveBlocksWithoutSuccessorsToEnd, air.layout.movesBlocksWithoutSuccessorsToEnd);
    case PolicySwitch::RareBlockLayout:
        air.layout.placesRareBlocksInGaps = false;
        air.layout.placesRareBlocksAfterGuards = false;
        air.layout.makesGapsForRareBlocks = false;
        air.layout.ordersBlocksByEdgeFrequency = false;
        break;
    CLEAR_FIELD_CASE(MergeIdenticalBlocks, air.layout.mergesIdenticalBlocks);
    CLEAR_FIELD_CASE(DuplicateReturns, air.layout.duplicatesReturns);
    CLEAR_FIELD_CASE(ForwardRegisterCopies, air.peepholes.forwardsRegisterCopies);
    CLEAR_FIELD_CASE(HoistCommonMoves, air.peepholes.hoistsCommonMoves);
    CLEAR_FIELD_CASE(ReuseConditionFlags, air.peepholes.reusesConditionFlags);
    CLEAR_FIELD_CASE(PairMemoryAccesses, air.peepholes.pairsMemoryAccesses);
    CLEAR_FIELD_CASE(ResequenceMovesBeforePatches, air.peepholes.resequencesMovesBeforePatches);
    case PolicySwitch::FoldTrapsIntoAccesses:
        air.peepholes.trapReach = 0;
        break;
    }
#endif
    m_disabledSwitches |= maskOf(policySwitch);
}

#undef CLEAR_FIELD_CASE

void Policy::applyDisabledSwitches()
{
    for (unsigned i = 0; i < numberOfSwitches; ++i) {
        auto policySwitch = static_cast<PolicySwitch>(i);
        if (isDisabled(policySwitch))
            disable(policySwitch);
    }
}

bool Policy::supportsFloatingPointRounding() const
{
    return MacroAssembler::supportsFloatingPointRounding(targetFeatures);
}

bool Policy::supportsRoundingFloatToIntegerFloat() const
{
#if CPU(ARM64)
    return MacroAssembler::supportsRoundFloatToIntegerFloat(targetFeatures);
#else
    return false;
#endif
}

bool Policy::supportsAtomicMemoryOperations() const
{
#if CPU(ARM64)
    return MacroAssembler::supportsLSE(targetFeatures);
#else
    return false;
#endif
}

bool Policy::supportsSHA3() const
{
#if CPU(ARM64)
    return MacroAssembler::supportsSHA3(targetFeatures);
#else
    return false;
#endif
}

#define PRINT_IF_DIFFERENT(policy, field) \
    do { \
        if (policy.field != defaults.field) \
            out.print(comma, #field " = "_s, policy.field); \
    } while (false)

#define PRINT_IF_SET(policy, field) \
    do { \
        if (policy.field) \
            out.print(comma, #field ""_s); \
    } while (false)

static void dumpAddressing(PrintStream& out, CommaPrinter& comma, const AddressingPolicy& addressing)
{
    const AddressingPolicy defaults { };
    PRINT_IF_SET(addressing, isPositionIndependent);
    PRINT_IF_SET(addressing, namesPinnedRegistersDirectly);
    PRINT_IF_DIFFERENT(addressing, dataSectionSpace);
    PRINT_IF_DIFFERENT(addressing, jumpTableSpace);
    PRINT_IF_SET(addressing, relocationLowering);
    PRINT_IF_SET(addressing, findPinnedConstant);
}

static void dumpAllocation(PrintStream& out, CommaPrinter& comma, const AllocationPolicy& allocation)
{
    PRINT_IF_SET(allocation, usesArena);
    PRINT_IF_SET(allocation, keepsValuesAfterLowering);
}

static void dumpUpstream(PrintStream& out, CommaPrinter& comma, const UpstreamPolicy& upstream)
{
    const UpstreamPolicy defaults { };
    PRINT_IF_DIFFERENT(upstream, optLevel);
    PRINT_IF_DIFFERENT(upstream, maxTailDuplicationBlockSize);
    PRINT_IF_DIFFERENT(upstream, maxTailDuplicationBlockSuccessors);
    PRINT_IF_DIFFERENT(upstream, maxWasmGCEpochSnapshotEntries);
    PRINT_IF_DIFFERENT(upstream, specializesSelects);
    PRINT_IF_DIFFERENT(upstream, duplicatesTails);
    PRINT_IF_DIFFERENT(upstream, canonicalizesIncrements);
    PRINT_IF_DIFFERENT(upstream, hoistsLoopInvariantValues);
    PRINT_IF_DIFFERENT(upstream, eliminatesWasmGCAllocations);
    PRINT_IF_DIFFERENT(upstream, usesWasmFaultSignalHandler);
    PRINT_IF_DIFFERENT(upstream, allowsSIMD);
    PRINT_IF_DIFFERENT(upstream, forcesSIMD);
}

static void dumpOptimization(PrintStream& out, CommaPrinter& comma, const OptimizationPolicy& optimization)
{
    if (optimization.redundancyElimination != RedundancyElimination::CommonSubexpressions)
        out.print(comma, "redundancyElimination = "_s, optimization.redundancyElimination);
    PRINT_IF_SET(optimization, splitsBranchConditions);
    PRINT_IF_SET(optimization, estimatesFrequencies);
    PRINT_IF_SET(optimization, returnsAreUnlikely);
    PRINT_IF_SET(optimization, checksVariablesAreSet);
    PRINT_IF_SET(optimization, threadsJumps);
    PRINT_IF_SET(optimization, foldsImpliedComparisons);
    PRINT_IF_SET(optimization, reducesWithRanges);
    PRINT_IF_SET(optimization, narrowsIntegers);
    PRINT_IF_SET(optimization, chainsAssociativeOperations);
    PRINT_IF_SET(optimization, stopsSwitchInferenceAtRareEdges);
    PRINT_IF_SET(optimization, convertsBranchesToSelects);
    PRINT_IF_SET(optimization, mergesAdjacentConstantStores);
    PRINT_IF_SET(optimization, sinksValues);
    PRINT_IF_SET(optimization, placesConstantsByFrequency);
    PRINT_IF_SET(optimization, mergesBlocksAfterForwardingJumps);
    PRINT_IF_SET(optimization, keepsOriginsOfDemotedValues);
}

static void dumpSelection(PrintStream& out, CommaPrinter& comma, const SelectionPolicy& selection)
{
    PRINT_IF_SET(selection, extendsInstructionSelection);
    PRINT_IF_SET(selection, attributesTrapsToAccesses);
}

static void dumpLoops(PrintStream& out, CommaPrinter& comma, const LoopPolicy& loops)
{
    const LoopPolicy defaults { };
    PRINT_IF_SET(loops, rotates);
    PRINT_IF_SET(loops, foldsComparisonsWithBounds);
    PRINT_IF_SET(loops, versions);
    PRINT_IF_SET(loops, clampsLoopBounds);
    PRINT_IF_SET(loops, pollsBetweenStretches);
    PRINT_IF_SET(loops, vectorizes);
    PRINT_IF_SET(loops, unrolls);
    PRINT_IF_DIFFERENT(loops, minFrequencyOfUnrolledLoop);
    PRINT_IF_SET(loops, hoistsOnlyFromLikelyPaths);
    PRINT_IF_SET(loops, hoistsImmutableLoadsPastWrites);
}

static void dumpSwitchLowering(PrintStream& out, CommaPrinter& comma, const SwitchLoweringPolicy& switchLowering)
{
    const SwitchLoweringPolicy defaults { };
    PRINT_IF_SET(switchLowering, usesBitTests);
    PRINT_IF_SET(switchLowering, usesValueTables);
    PRINT_IF_DIFFERENT(switchLowering, minCasesForJumpTable);
}

static void dumpKnownProperties(PrintStream& out, CommaPrinter& comma, const KnownPropertyPolicy& knownProperties)
{
    PRINT_IF_SET(knownProperties, trustsKnownRanges);
    PRINT_IF_SET(knownProperties, trustsKnownDereferenceableBytes);
    PRINT_IF_SET(knownProperties, trustsKnownInductions);
}

static void dumpValidation(PrintStream& out, CommaPrinter& comma, const ValidationPolicy& validation)
{
    if (validation.level != ValidationLevel::FromOptions)
        out.print(comma, "validation = "_s, validation.level);
    PRINT_IF_SET(validation, checksKnownPropertiesAtRunTime);
    PRINT_IF_SET(validation, logsMovedLoads);
    PRINT_IF_SET(validation, logsDecidedComparisons);
    PRINT_IF_SET(validation, keepsOriginsOfAccessesThatTakeTraps);
}

#undef PRINT_IF_DIFFERENT
#undef PRINT_IF_SET

static void dumpLimits(PrintStream& out, CommaPrinter& comma, const LimitValues& limits)
{
    for (unsigned i = 0; i < numberOfLimits; ++i) {
        auto limit = static_cast<Limit>(i);
        if (limits.get(limit) != defaultValueOf(limit))
            out.print(comma, limit, " = "_s, limits.get(limit));
    }
}

static void dumpAir(PrintStream& out, CommaPrinter& comma, const Air::Policy& air)
{
    StringPrintStream airOut;
    air.dump(airOut);
    if (airOut.length())
        out.print(comma, airOut.toUTF8CString());
}

static void dumpClientPhases(PrintStream& out, CommaPrinter& comma, const Vector<ClientPhase, 4>& clientPhases)
{
    if (clientPhases.isEmpty())
        return;
    out.print(comma, "clientPhases = ["_s);
    CommaPrinter phaseComma;
    for (const ClientPhase& phase : clientPhases)
        out.print(phaseComma, phase.name, "@"_s, phase.point);
    out.print("]"_s);
}

static void dumpDisabledSwitches(PrintStream& out, CommaPrinter& comma, const Policy& policy)
{
    out.print(comma, "disabled = ["_s);
    CommaPrinter switchComma;
    for (unsigned i = 0; i < numberOfSwitches; ++i) {
        auto policySwitch = static_cast<PolicySwitch>(i);
        if (policy.isDisabled(policySwitch))
            out.print(switchComma, policySwitch);
    }
    out.print("]"_s);
}

void Policy::dump(PrintStream& out) const
{
    CommaPrinter comma;
    dumpAddressing(out, comma, addressing);
    dumpAllocation(out, comma, allocation);
    dumpUpstream(out, comma, upstream);
    dumpOptimization(out, comma, optimization);
    dumpSelection(out, comma, selection);
    dumpLoops(out, comma, loops);
    dumpSwitchLowering(out, comma, switchLowering);
    dumpKnownProperties(out, comma, knownProperties);
    dumpValidation(out, comma, validation);
    dumpLimits(out, comma, limits);
    if (targetFeatures != CPUFeatureSet::host())
        out.print(comma, "targetFeatures = "_s, targetFeatures.bits());
    dumpAir(out, comma, air);
    if (collectsRemarks)
        out.print(comma, "collectsRemarks"_s);
    dumpClientPhases(out, comma, clientPhases);
    if (m_disabledSwitches)
        dumpDisabledSwitches(out, comma, *this);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
