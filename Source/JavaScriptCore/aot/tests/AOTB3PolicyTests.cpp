/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTB3Policy.h"
#include "AOTBackEndReport.h"
#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTConvention.h"
#include "AOTFaultTable.h"
#include "AOTHeaps.h"
#include "AOTInstanceLayout.h"
#include "AOTNativeTest.h"
#include "AOTOutput.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRelocation.h"
#include "AOTStubBodyConvention.h"
#include "B3CompilationScratch.h"
#include "B3ProcedureInlines.h"
#include "B3RelocationLowering.h"
#include "CPU.h"
#include "GPRInfo.h"
#include "JSCJSValue.h"
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

class DecliningRelocationLowering final : public B3::RelocationLowering {
public:
    B3::Value* lower(B3::Procedure&, B3::InsertionSet&, size_t, const B3::RelocatableValue&) final { return nullptr; }
};

} // anonymous namespace

static Wire::CompileOptions optionsWith(std::initializer_list<String> words)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (const String& word : words)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

static FunctionBackEndTraits traitsFor(uint32_t function)
{
    return { .function = Typed::FunctionID(function) };
}

static B3::Policy policyWith(std::initializer_list<String> words, const FunctionBackEndTraits& traits, B3::RelocationLowering& lowering)
{
    BuildContext build(optionsWith(words));
    return b3PolicyFor(build, traits, lowering);
}

static void checkAddressingAndAllocation(const B3::Policy& policy, const B3::RelocationLowering& lowering)
{
    AOT_TEST_CHECK(policy.addressing.isPositionIndependent);
    AOT_TEST_CHECK(policy.addressing.namesPinnedRegistersDirectly);
    AOT_TEST_CHECK(policy.addressing.dataSectionSpace == static_cast<uint32_t>(RelocationSpace::Constants));
    AOT_TEST_CHECK(policy.addressing.jumpTableSpace == static_cast<uint32_t>(RelocationSpace::JumpTables));
    AOT_TEST_CHECK(policy.addressing.relocationLowering == &lowering);
    AOT_TEST_CHECK(policy.addressing.findPinnedConstant);

    AOT_TEST_CHECK(policy.allocation.usesArena);
    AOT_TEST_CHECK(policy.allocation.keepsValuesAfterLowering);
}

static void checkUpstreamPolicy(const B3::UpstreamPolicy& upstream)
{
    AOT_TEST_CHECK(upstream.optLevel == 2);
    AOT_TEST_CHECK(upstream.maxTailDuplicationBlockSize == 1);
    AOT_TEST_CHECK(upstream.maxTailDuplicationBlockSuccessors == 3);
    AOT_TEST_CHECK(upstream.specializesSelects);
    AOT_TEST_CHECK(upstream.duplicatesTails);
    AOT_TEST_CHECK(upstream.hoistsLoopInvariantValues);
    AOT_TEST_CHECK(!upstream.canonicalizesIncrements);
    AOT_TEST_CHECK(!upstream.eliminatesWasmGCAllocations);
    AOT_TEST_CHECK(!upstream.usesWasmFaultSignalHandler);
    AOT_TEST_CHECK(!upstream.allowsSIMD);
    AOT_TEST_CHECK(!upstream.forcesSIMD);
}

static void checkOptimizationPolicy(const B3::Policy& policy)
{
    const B3::OptimizationPolicy& optimization = policy.optimization;
    AOT_TEST_CHECK(optimization.redundancyElimination == B3::RedundancyElimination::MemoryNumbering);
    AOT_TEST_CHECK(optimization.splitsBranchConditions);
    AOT_TEST_CHECK(optimization.estimatesFrequencies);
    AOT_TEST_CHECK(!optimization.returnsAreUnlikely);
    AOT_TEST_CHECK(optimization.checksVariablesAreSet);
    AOT_TEST_CHECK(optimization.threadsJumps);
    AOT_TEST_CHECK(optimization.foldsImpliedComparisons);
    AOT_TEST_CHECK(optimization.reducesWithRanges);
    AOT_TEST_CHECK(optimization.narrowsIntegers);
    AOT_TEST_CHECK(optimization.chainsAssociativeOperations);
    AOT_TEST_CHECK(optimization.stopsSwitchInferenceAtRareEdges);
    AOT_TEST_CHECK(optimization.convertsBranchesToSelects);
    AOT_TEST_CHECK(optimization.mergesAdjacentConstantStores);
    AOT_TEST_CHECK(optimization.sinksValues);
    AOT_TEST_CHECK(optimization.placesConstantsByFrequency);
    AOT_TEST_CHECK(optimization.mergesBlocksAfterForwardingJumps);

    AOT_TEST_CHECK(policy.selection.extendsInstructionSelection);
    AOT_TEST_CHECK(policy.selection.attributesTrapsToAccesses);

    AOT_TEST_CHECK(policy.loops.rotates);
    AOT_TEST_CHECK(policy.loops.foldsComparisonsWithBounds);
    AOT_TEST_CHECK(policy.loops.versions);
    AOT_TEST_CHECK(policy.loops.clampsLoopBounds);
    AOT_TEST_CHECK(policy.loops.hoistsOnlyFromLikelyPaths);
    AOT_TEST_CHECK(policy.loops.hoistsImmutableLoadsPastWrites);

    AOT_TEST_CHECK(policy.switchLowering.usesBitTests);
    AOT_TEST_CHECK(policy.switchLowering.minCasesForJumpTable == 7);

    AOT_TEST_CHECK(policy.knownProperties.trustsKnownRanges);
    AOT_TEST_CHECK(policy.knownProperties.trustsKnownDereferenceableBytes);
    AOT_TEST_CHECK(policy.knownProperties.trustsKnownInductions);
}

static void checkRegisterAndSplitPolicy(const B3::Air::Policy& air)
{
    AOT_TEST_CHECK(air.registers.chargesForCalleeSaves);
    AOT_TEST_CHECK(air.registers.discountsPreferredRegisterMoves);
    AOT_TEST_CHECK(air.registers.hintsTwoAddressOperands);
    AOT_TEST_CHECK(air.registers.usesSecondPreferredRegister);
    AOT_TEST_CHECK(air.registers.evictsHolderOfPreferredRegister);
    AOT_TEST_CHECK(air.registers.unsavedCalleeSaves == unsavedCalleeSaves());
    AOT_TEST_CHECK(air.registers.savedByCallee.isEmpty());
    AOT_TEST_CHECK(air.registers.calleeSavesClobberedAtSuspensions.isEmpty());
    AOT_TEST_CHECK(air.registers.pinned.isEmpty());

    const B3::Air::LiveRangeSplitPolicy defaults { };
    AOT_TEST_CHECK(air.splitting.splitsAroundClobbersEarly);
    AOT_TEST_CHECK(air.splitting.spansStretchesOfClobbers);
    AOT_TEST_CHECK(air.splitting.limitsClobberSitesPerSplit);
    AOT_TEST_CHECK(air.splitting.splitsIntoClustersOverBlocks);
    AOT_TEST_CHECK(air.splitting.splitsAroundFrame);
    AOT_TEST_CHECK(air.splitting.splitsAroundLoops == defaults.splitsAroundLoops);
    AOT_TEST_CHECK(air.splitting.splitMultiplier == defaults.splitMultiplier);
    AOT_TEST_CHECK(air.splitting.maxLoopFractionOfLiveRange == defaults.maxLoopFractionOfLiveRange);
}

static void checkFrameAndLayoutPolicy(const B3::Air::Policy& air)
{
    AOT_TEST_CHECK(air.frame.emitsFrame);
    AOT_TEST_CHECK(air.frame.shrinkWraps);
    AOT_TEST_CHECK(air.frame.maxBytesAllocatedBeforeStackCheck == 4096);
    AOT_TEST_CHECK(air.frame.registersClobberedByFrameSetup == (isARM64() ? RegisterSet::macroClobberedGPRs() : RegisterSet()));
    AOT_TEST_CHECK(air.frame.mayPlaceSlotsAboveFrameRecord);
    AOT_TEST_CHECK(air.frame.mayPushCalleeSaves);
    AOT_TEST_CHECK(air.frame.mayPushSpillsAtFrameSetup);
    AOT_TEST_CHECK(air.frame.ordersSlotsByUse);
    AOT_TEST_CHECK(air.frame.prefersStackPointerAddresses);
    AOT_TEST_CHECK(air.frame.mayOmitFrameRecord == isX86_64());
    AOT_TEST_CHECK(air.frame.maxBytesAddedWithoutFrameRecord);

    AOT_TEST_CHECK(air.layout.movesBlocksWithoutSuccessorsToEnd);
    AOT_TEST_CHECK(air.layout.placesRareBlocksInGaps);
    AOT_TEST_CHECK(!air.layout.placesRareBlocksAfterGuards);
    AOT_TEST_CHECK(air.layout.ordersBlocksByEdgeFrequency);
    AOT_TEST_CHECK(air.layout.recordsRareCodeStart);
    AOT_TEST_CHECK(air.layout.mergesIdenticalBlocks);
    AOT_TEST_CHECK(air.layout.duplicatesReturns);

    AOT_TEST_CHECK(air.peepholes.pairsSpillAccesses);
    AOT_TEST_CHECK(air.peepholes.forwardsRegisterCopies);
    AOT_TEST_CHECK(air.peepholes.hoistsCommonMoves);
    AOT_TEST_CHECK(air.peepholes.reusesConditionFlags);
    AOT_TEST_CHECK(air.peepholes.pairsMemoryAccesses);
    AOT_TEST_CHECK(air.peepholes.resequencesMovesBeforePatches);
    AOT_TEST_CHECK(air.peepholes.emitsShufflesInPairOrder);
    AOT_TEST_CHECK(air.peepholes.trapReach && JSValue::ValueUndefined + air.peepholes.trapReach <= FaultSite::reach);
    AOT_TEST_CHECK((static_cast<uintptr_t>(undefinedCell32) << cell32Shift) + air.peepholes.trapReach <= FaultSite::reach);
}

static void testFunctionPolicy()
{
    DecliningRelocationLowering lowering;
    BuildContext build(optionsWith({ }));
    B3::Policy policy = b3PolicyFor(build, traitsFor(7), lowering);

    checkAddressingAndAllocation(policy, lowering);
    checkUpstreamPolicy(policy.upstream);
    checkOptimizationPolicy(policy);
    AOT_TEST_CHECK(policy.validation.level == B3::ValidationLevel::FromOptions);
    AOT_TEST_CHECK(!policy.validation.checksKnownPropertiesAtRunTime);
    AOT_TEST_CHECK(policy.limits == B3::LimitValues());
    AOT_TEST_CHECK(policy.targetFeatures == build.targetFeatures());
    AOT_TEST_CHECK(!policy.collectsRemarks);
    AOT_TEST_CHECK(policy.clientPhases.isEmpty());
    AOT_TEST_CHECK(!policy.disabledSwitches());
    checkRegisterAndSplitPolicy(policy.air);
    checkFrameAndLayoutPolicy(policy.air);
}

static void testFrameSavingFunctionPolicy()
{
    DecliningRelocationLowering lowering;
    String description = WTF::toString(policyWith({ }, traitsFor(7), lowering));

    FunctionBackEndTraits traits = traitsFor(7);
    traits.savesFrameOnSuspend = true;
    B3::Policy policy = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(policy.air.registers.calleeSavesClobberedAtSuspensions == calleeSavesClobberedAtSuspensions());
    AOT_TEST_CHECK(policy.air.registers.pinned == RegisterSet::fromIterable(pinnedFPRsInFrameSavingFunctions));
    AOT_TEST_CHECK(!policy.air.frame.shrinkWraps);
    AOT_TEST_CHECK(!policy.air.frame.mayPlaceSlotsAboveFrameRecord);
    AOT_TEST_CHECK(!policy.air.frame.mayPushCalleeSaves);
    AOT_TEST_CHECK(!policy.air.frame.mayPushSpillsAtFrameSetup);
    AOT_TEST_CHECK(!policy.air.frame.mayOmitFrameRecord);

    policy.air.registers.calleeSavesClobberedAtSuspensions = RegisterSet();
    policy.air.registers.pinned = RegisterSet();
    policy.air.frame.shrinkWraps = true;
    policy.air.frame.mayPlaceSlotsAboveFrameRecord = true;
    policy.air.frame.mayPushCalleeSaves = true;
    policy.air.frame.mayPushSpillsAtFrameSetup = true;
    policy.air.frame.mayOmitFrameRecord = isX86_64();
    AOT_TEST_CHECK(WTF::toString(policy) == description);
}

static void testFunctionTraits()
{
    DecliningRelocationLowering lowering;
    const B3::Policy warmPolicy = policyWith({ }, traitsFor(7), lowering);
    String description = WTF::toString(warmPolicy);

    for (bool FunctionBackEndTraits::* trait : { &FunctionBackEndTraits::readsCallerFrame, &FunctionBackEndTraits::isAsync }) {
        FunctionBackEndTraits traits = traitsFor(7);
        traits.*trait = true;
        B3::Policy policy = policyWith({ }, traits, lowering);
        AOT_TEST_CHECK(!policy.air.frame.mayPlaceSlotsAboveFrameRecord);
        policy.air.frame.mayPlaceSlotsAboveFrameRecord = true;
        AOT_TEST_CHECK(WTF::toString(policy) == description);
    }

    FunctionBackEndTraits traits = traitsFor(7);
    traits.isCold = true;
    B3::Policy coldPolicy = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(!coldPolicy.air.layout.placesRareBlocksAfterGuards && coldPolicy.air.layout.makesGapsForRareBlocks);
    AOT_TEST_CHECK(!coldPolicy.air.frame.maxBytesAddedWithoutFrameRecord);
    coldPolicy.air.layout.makesGapsForRareBlocks = false;
    coldPolicy.air.frame.maxBytesAddedWithoutFrameRecord = warmPolicy.air.frame.maxBytesAddedWithoutFrameRecord;
    AOT_TEST_CHECK(WTF::toString(coldPolicy) == description);

    traits.runsOnce = true;
    B3::Policy policyOfCodeThatRunsOnce = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(policyOfCodeThatRunsOnce.air.layout.placesRareBlocksAfterGuards && !policyOfCodeThatRunsOnce.air.layout.makesGapsForRareBlocks);
    policyOfCodeThatRunsOnce.air.layout.placesRareBlocksAfterGuards = false;
    policyOfCodeThatRunsOnce.air.frame.maxBytesAddedWithoutFrameRecord = warmPolicy.air.frame.maxBytesAddedWithoutFrameRecord;
    AOT_TEST_CHECK(WTF::toString(policyOfCodeThatRunsOnce) == description);

    traits = traitsFor(7);
    traits.isHot = true;
    B3::Policy hotPolicy = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(hotPolicy.air.frame.maxBytesAddedWithoutFrameRecord > warmPolicy.air.frame.maxBytesAddedWithoutFrameRecord);
    hotPolicy.air.frame.maxBytesAddedWithoutFrameRecord = warmPolicy.air.frame.maxBytesAddedWithoutFrameRecord;
    AOT_TEST_CHECK(WTF::toString(hotPolicy) == description);

    traits = traitsFor(7);
    traits.rejectsOnThrow = true;
    B3::Policy rejectingPolicy = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(!rejectingPolicy.air.frame.mayOmitFrameRecord && rejectingPolicy.air.frame.setsUpFrameAtEntry);
    rejectingPolicy.air.frame.mayOmitFrameRecord = isX86_64();
    rejectingPolicy.air.frame.setsUpFrameAtEntry = false;
    AOT_TEST_CHECK(WTF::toString(rejectingPolicy) == description);

    traits = traitsFor(7);
    traits.hasHandlers = true;
    B3::Policy policyWithHandlers = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(!warmPolicy.air.frame.setsUpFrameAtEntry && policyWithHandlers.air.frame.shrinkWraps && policyWithHandlers.air.frame.setsUpFrameAtEntry);
    policyWithHandlers.air.frame.setsUpFrameAtEntry = false;
    AOT_TEST_CHECK(WTF::toString(policyWithHandlers) == description);

    traits = traitsFor(7);
    traits.returnsAreUnlikely = true;
    B3::Policy unlikelyReturnPolicy = policyWith({ }, traits, lowering);
    AOT_TEST_CHECK(unlikelyReturnPolicy.optimization.returnsAreUnlikely);
    unlikelyReturnPolicy.optimization.returnsAreUnlikely = false;
    AOT_TEST_CHECK(WTF::toString(unlikelyReturnPolicy) == description);

    AOT_TEST_CHECK(WTF::toString(policyWith({ }, traitsFor(8), lowering)) == description);
}

static void testStubBodyPolicy()
{
    DecliningRelocationLowering lowering;
    String description = WTF::toString(policyWith({ }, traitsFor(7), lowering));

    FunctionBackEndTraits traits = traitsFor(7);
    traits.convention = BackEndConvention::StubBody;
    B3::Policy policy = policyWith({ }, traits, lowering);
    B3::Air::RegisterPolicy& registers = policy.air.registers;

    RegisterSet savedAndRestoredByUnwinding = StubBodyConvention::registersSavedByCallee();
    savedAndRestoredByUnwinding.filter(RegisterSet::vmCalleeSaveRegisters());
    AOT_TEST_CHECK(registers.savedByCallee == StubBodyConvention::registersSavedByCallee());
    AOT_TEST_CHECK(registers.unsavedCalleeSaves == unsavedCalleeSaves());
    AOT_TEST_CHECK(registers.pinned == savedAndRestoredByUnwinding);
    AOT_TEST_CHECK(registers.calleeSavesClobberedAtSuspensions.isEmpty());

    RegisterSet savedAndClobbered = registers.savedByCallee;
    savedAndClobbered.filter(StubBodyConvention::registersClobberedByCalls());
    AOT_TEST_CHECK(savedAndClobbered.isEmpty());
    AOT_TEST_CHECK(!registers.savedByCallee.contains(instanceGPR, IgnoreVectors));
    AOT_TEST_CHECK(!registers.savedByCallee.contains(cellCageBaseGPR, IgnoreVectors));

    RegisterSet pinnedAndClobberedByCallees = registers.pinned;
    pinnedAndClobberedByCallees.filter(registersClobberedByCompiledCalls());
    AOT_TEST_CHECK(pinnedAndClobberedByCallees.isEmpty());

    registers.savedByCallee = RegisterSet();
    registers.pinned = RegisterSet();
    AOT_TEST_CHECK(WTF::toString(policy) == description);
}

static void testBuildOptions()
{
    DecliningRelocationLowering lowering;
    String description = WTF::toString(policyWith({ }, traitsFor(7), lowering));

    B3::Policy checkedPolicy = policyWith({ "check=validate-back-end"_s, "report=declines"_s }, traitsFor(7), lowering);
    AOT_TEST_CHECK(checkedPolicy.validation.level == B3::ValidationLevel::AtEachPhase);
    AOT_TEST_CHECK(!checkedPolicy.validation.checksKnownPropertiesAtRunTime);
    AOT_TEST_CHECK(checkedPolicy.collectsRemarks);
    AOT_TEST_CHECK(!checkedPolicy.air.frame.mayOmitFrameRecord);
    checkedPolicy.validation.level = B3::ValidationLevel::FromOptions;
    checkedPolicy.collectsRemarks = false;
    checkedPolicy.air.frame.mayOmitFrameRecord = isX86_64();
    AOT_TEST_CHECK(WTF::toString(checkedPolicy) == description);

    B3::Policy verifyingPolicy = policyWith({ "check=verify-each-pass"_s }, traitsFor(7), lowering);
    AOT_TEST_CHECK(verifyingPolicy.validation.level == B3::ValidationLevel::AtBoundaries);
    AOT_TEST_CHECK(!verifyingPolicy.validation.checksKnownPropertiesAtRunTime);
    verifyingPolicy.validation.level = B3::ValidationLevel::FromOptions;
    AOT_TEST_CHECK(WTF::toString(verifyingPolicy) == description);

    B3::Policy validatingPolicy = policyWith({ "validate"_s }, traitsFor(7), lowering);
    AOT_TEST_CHECK(validatingPolicy.validation.level == B3::ValidationLevel::AtBoundaries);
    AOT_TEST_CHECK(validatingPolicy.validation.checksKnownPropertiesAtRunTime);
    validatingPolicy.validation.level = B3::ValidationLevel::FromOptions;
    validatingPolicy.validation.checksKnownPropertiesAtRunTime = false;
    AOT_TEST_CHECK(WTF::toString(validatingPolicy) == description);

    AOT_TEST_CHECK(B3::shouldValidateIR());
}

static void testSwitches()
{
    DecliningRelocationLowering lowering;
    for (unsigned i = 0; i < B3::numberOfSwitches; ++i) {
        B3::Policy policy = policyWith({ makeString("disable=b3:"_s, B3::nameOf(static_cast<B3::PolicySwitch>(i))) }, traitsFor(7), lowering);
        for (unsigned j = 0; j < B3::numberOfSwitches; ++j)
            AOT_TEST_CHECK(policy.isDisabled(static_cast<B3::PolicySwitch>(j)) == (i == j));
    }

    for (uint32_t function : { 4u, 5u, 6u }) {
        B3::Policy policy = policyWith({ "disable=b3:ThreadJumps"_s, "disable-in=5:1"_s }, traitsFor(function), lowering);
        AOT_TEST_CHECK(policy.isDisabled(B3::PolicySwitch::ThreadJumps) == (function == 5));
        AOT_TEST_CHECK(policy.optimization.threadsJumps == (function != 5));
    }

    AOT_TEST_CHECK(!policyWith({ "disable=b3:HoistLoopInvariantValues"_s }, traitsFor(7), lowering).upstream.hoistsLoopInvariantValues);
    AOT_TEST_CHECK(!policyWith({ "disable=b3:PinnedConstants"_s }, traitsFor(7), lowering).addressing.findPinnedConstant);
    AOT_TEST_CHECK(!policyWith({ "disable=b3:ShrinkWrapFrame"_s }, traitsFor(7), lowering).air.frame.shrinkWraps);
}

static void testLimits()
{
    DecliningRelocationLowering lowering;
    constexpr uint64_t loweredValue = 3;
    static_assert(loweredValue < B3::defaultValueOf(B3::Limit::MaxThreadedPathCost));

    B3::Policy policy = policyWith({ makeString("limit="_s, nameOf(LimitID::MaxThreadedPathCost), ':', loweredValue) }, traitsFor(7), lowering);
    for (unsigned i = 0; i < B3::numberOfLimits; ++i) {
        auto limit = static_cast<B3::Limit>(i);
        AOT_TEST_CHECK(policy.limits.get(limit) == (limit == B3::Limit::MaxThreadedPathCost ? loweredValue : B3::defaultValueOf(limit)));
    }

    B3::Policy policyWithOtherLimit = policyWith({ makeString("limit="_s, nameOf(LimitID::MaxLandingPadsSearched), ":1"_s) }, traitsFor(7), lowering);
    AOT_TEST_CHECK(policyWithOtherLimit.limits == B3::LimitValues());
}

static void testPinnedConstants()
{
    DecliningRelocationLowering lowering;
    B3::PinnedConstantFinder find = policyWith({ }, traitsFor(7), lowering).addressing.findPinnedConstant;

    FunctionBackEndTraits stubBodyTraits = traitsFor(7);
    stubBodyTraits.convention = BackEndConvention::StubBody;
    AOT_TEST_CHECK(find == policyWith({ }, stubBodyTraits, lowering).addressing.findPinnedConstant);

    auto numberTag = find(static_cast<uint64_t>(JSValue::NumberTag));
    auto notCellMask = find(static_cast<uint64_t>(JSValue::NotCellMask));
    AOT_TEST_CHECK(numberTag && notCellMask);
    AOT_TEST_CHECK(numberTag->offset.has_value() == isX86_64());
    AOT_TEST_CHECK(notCellMask->offset.has_value() == isX86_64());
    AOT_TEST_CHECK(numberTag->reg == Reg(isX86_64() ? instanceGPR : GPRInfo::numberTagRegister));
    AOT_TEST_CHECK(notCellMask->reg == Reg(isX86_64() ? instanceGPR : GPRInfo::notCellMaskRegister));
    AOT_TEST_CHECK(numberTag->reg != notCellMask->reg || numberTag->offset != notCellMask->offset);
    AOT_TEST_CHECK(!numberTag->replacesImmediates && !notCellMask->replacesImmediates);

    auto notUndefinedOrNullMask = find(~static_cast<uint64_t>(JSValue::ValueUndefined));
    AOT_TEST_CHECK(notUndefinedOrNullMask.has_value() == isX86_64());
    AOT_TEST_CHECK(!notUndefinedOrNullMask || (notUndefinedOrNullMask->reg == Reg(instanceGPR) && notUndefinedOrNullMask->offset && notUndefinedOrNullMask->replacesImmediates));

    for (uint64_t bits : { std::bit_cast<uint64_t>(1.0), PNaNAsBits, static_cast<uint64_t>(JSValue::DoubleEncodeOffset), static_cast<uint64_t>(JSValue::NumberTag | 1), static_cast<uint64_t>(undefinedInteger), static_cast<uint64_t>(nullInteger), static_cast<uint64_t>(absentInteger), undefinedDouble, nullDouble, absentDouble }) {
        auto number = find(bits);
        AOT_TEST_CHECK(number.has_value() == isX86_64());
        AOT_TEST_CHECK(!number || (number->reg == Reg(instanceGPR) && number->offset && *number->offset >= std::numeric_limits<int8_t>::min() && *number->offset < 0 && !number->replacesImmediates));
    }

    // What a truncation gives for a number that fits no integer: farther away, and still three bytes and an instruction less than the immediate.
    auto minInt64 = find(static_cast<uint64_t>(std::numeric_limits<int64_t>::min()));
    AOT_TEST_CHECK(minInt64.has_value() == isX86_64());
    AOT_TEST_CHECK(!minInt64 || (minInt64->reg == Reg(instanceGPR) && minInt64->offset && *minInt64->offset < 0 && !minInt64->replacesImmediates));

    AOT_TEST_CHECK(!find(42));
    AOT_TEST_CHECK(!find(std::bit_cast<uint64_t>(2.5)));
    AOT_TEST_CHECK(!find(std::bit_cast<uint64_t>(3.25)));
}

static void checkPinnedValues(InstanceSizes sizes)
{
    DecliningRelocationLowering lowering;
    B3::CompilationScratch scratch(createAOTHeaps);
    B3::Procedure proc(policyWith({ }, traitsFor(7), lowering), &scratch);
    Typed::pinRegisters(proc);

    Output out(proc);
    out.initialize(AOTHeaps::of(proc));
    LBasicBlock root = out.newBlock();
    out.appendTo(root);
    out.initializeConstants(proc, root);

    Typed::PinnedValues pinned;
    Typed::definePinnedValues(out, proc, sizes, pinned);

    AOT_TEST_CHECK(pinned.instance->opcode() == B3::ArgumentReg && pinned.instance->as<B3::ArgumentRegValue>()->argumentReg() == Reg(instanceGPR));
    AOT_TEST_CHECK(pinned.cellCageBase->opcode() == B3::ArgumentReg && pinned.cellCageBase->as<B3::ArgumentRegValue>()->argumentReg() == Reg(cellCageBaseGPR));
    AOT_TEST_CHECK(pinned.numberTag->isInt64(JSValue::NumberTag));
    AOT_TEST_CHECK(pinned.notCellMask->isInt64(JSValue::NotCellMask));

    AOT_TEST_CHECK(pinned.instancePrefix->opcode() == B3::Add && pinned.instancePrefix->child(0) == pinned.instance);
    AOT_TEST_CHECK(pinned.instancePrefix->child(1)->isInt64(-static_cast<int64_t>(sizeof(InstancePrefix))));
    AOT_TEST_CHECK(pinned.statics->opcode() == B3::Add && pinned.statics->child(0) == pinned.instance);
    AOT_TEST_CHECK(pinned.statics->child(1)->isInt64(InstanceLayout::offsetOfStatics(sizes)));

    AOT_TEST_CHECK(proc.knownDereferenceableBytes(pinned.instance) == InstanceLayout::bytesReservedFromInstance(sizes));
    AOT_TEST_CHECK(proc.knownDereferenceableBytes(pinned.instancePrefix) == sizeof(InstancePrefix));
    AOT_TEST_CHECK(proc.knownDereferenceableBytes(pinned.statics) == sizes.sizeOfStatics);
    AOT_TEST_CHECK(!proc.knownDereferenceableBytes(pinned.cellCageBase));

    AOT_TEST_CHECK(proc.isKnownDereferenceable(pinned.instancePrefix, 0, sizeof(InstancePrefix)));
    AOT_TEST_CHECK(!proc.isKnownDereferenceable(pinned.instancePrefix, 0, sizeof(InstancePrefix) + 1));
    AOT_TEST_CHECK(!proc.isKnownDereferenceable(pinned.instance, -static_cast<int64_t>(sizeof(void*)), sizeof(void*)));
    AOT_TEST_CHECK(proc.isKnownDereferenceable(pinned.statics, 0, sizeof(void*)) == (sizes.sizeOfStatics >= sizeof(void*)));
}

static void testPinnedValues()
{
    checkPinnedValues({ .sizeOfStatics = 4096, .numberOfStringCells = 10, .numberOfSiteCaches = 20 });
    checkPinnedValues({ .sizeOfStatics = 0, .numberOfStringCells = 0, .numberOfSiteCaches = 0 });
}

std::span<const NativeTest> b3PolicyTests()
{
    static constexpr NativeTest tests[] = {
        { "b3-policy-function"_s, testFunctionPolicy },
        { "b3-policy-frame-saving-function"_s, testFrameSavingFunctionPolicy },
        { "b3-policy-function-traits"_s, testFunctionTraits },
        { "b3-policy-stub-body"_s, testStubBodyPolicy },
        { "b3-policy-build-options"_s, testBuildOptions },
        { "b3-policy-switches"_s, testSwitches },
        { "b3-policy-limits"_s, testLimits },
        { "b3-policy-pinned-constants"_s, testPinnedConstants },
        { "b3-policy-pinned-values"_s, testPinnedValues },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
