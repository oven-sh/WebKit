/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3CheckVariablesAreSet.h"
#include "B3CompilationScratch.h"
#include "B3InsertionSetInlines.h"
#include "B3LowerMacros.h"
#include "B3MaterializePinnedConstants.h"
#include "B3Operations.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "MathCommon.h"
#include <wtf/StringPrintStream.h>

static bool hasFailure(Procedure& proc, FailureKind kind) { return proc.report().failure && proc.report().failure->kind == kind; }
static String dumpOf(const Policy& policy) { return toString(policy); }
static std::optional<PinnedConstant> findNoPinnedConstant(uint64_t) { return std::nullopt; }

static Policy positionIndependentPolicy()
{
    return policyWith([](Policy& policy) {
        policy.addressing.isPositionIndependent = true;
        policy.addressing.dataSectionSpace = testDataSectionSpace;
        policy.addressing.jumpTableSpace = testJumpTableSpace;
        policy.switchLowering.minCasesForJumpTable = std::numeric_limits<unsigned>::max();
    });
}

static void enableEveryOptimizationSwitch(Policy& policy)
{
    OptimizationPolicy& optimization = policy.optimization;
    optimization.redundancyElimination = RedundancyElimination::MemoryNumbering;
    optimization.splitsBranchConditions = true;
    optimization.estimatesFrequencies = true;
    optimization.threadsJumps = true;
    optimization.foldsImpliedComparisons = true;
    optimization.reducesWithRanges = true;
    optimization.narrowsIntegers = true;
    optimization.chainsAssociativeOperations = true;
    optimization.convertsBranchesToSelects = true;
    optimization.mergesAdjacentConstantStores = true;
    optimization.sinksValues = true;
    optimization.placesConstantsByFrequency = true;
    policy.selection.extendsInstructionSelection = true;
    policy.knownProperties.trustsKnownRanges = true;
    policy.knownProperties.trustsKnownDereferenceableBytes = true;
    policy.knownProperties.trustsKnownInductions = true;
}

static void enableEveryAirSwitch(Air::Policy& air)
{
    air.registers.chargesForCalleeSaves = true;
    air.registers.discountsPreferredRegisterMoves = true;
    air.registers.hintsTwoAddressOperands = true;
    air.registers.usesSecondPreferredRegister = true;
    air.registers.evictsHolderOfPreferredRegister = true;
    air.splitting.splitsAroundClobbersEarly = true;
    air.splitting.splitsAroundClobbersThatRead = true;
    air.splitting.keepsUnusedTmpsOnStackThroughLoops = true;
    air.splitting.spillsTmpsReadInPlaceFirst = true;
    air.splitting.spansStretchesOfClobbers = true;
    air.splitting.leastCostOfSplitAroundClobber = 1.0f / 1024;
    air.splitting.splitsIntoClustersOverBlocks = true;
    air.splitting.splitsAroundFrame = true;
    air.frame.shrinkWraps = true;
    air.frame.mayPlaceSlotsAboveFrameRecord = true;
    air.frame.mayPushCalleeSaves = true;
    air.frame.mayPushSpillsAtFrameSetup = true;
    air.frame.ordersSlotsByUse = true;
    air.frame.prefersStackPointerAddresses = true;
    air.frame.mayOmitFrameRecord = true;
    air.layout.movesBlocksWithoutSuccessorsToEnd = true;
    air.layout.placesRareBlocksInGaps = true;
    air.layout.placesRareBlocksAfterGuards = true;
    air.layout.makesGapsForRareBlocks = true;
    air.layout.ordersBlocksByEdgeFrequency = true;
    air.layout.recordsRareCodeStart = true;
    air.layout.mergesIdenticalBlocks = true;
    air.layout.duplicatesReturns = true;
    air.peepholes.forwardsRegisterCopies = true;
    air.peepholes.hoistsCommonMoves = true;
    air.peepholes.reusesConditionFlags = true;
    air.peepholes.pairsMemoryAccesses = true;
    air.peepholes.resequencesMovesBeforePatches = true;
    air.peepholes.trapReach = 4096;
}

static Policy policyWithEverySwitchOn()
{
    Policy policy;
    policy.addressing.findPinnedConstant = findNoPinnedConstant;
    enableEveryOptimizationSwitch(policy);
    policy.upstream.hoistsLoopInvariantValues = true;
    policy.loops.rotates = true;
    policy.loops.foldsComparisonsWithBounds = true;
    policy.loops.versions = true;
    policy.loops.clampsLoopBounds = true;
    policy.loops.vectorizes = true;
    policy.loops.unrolls = true;
    policy.loops.promotesMemory = true;
    policy.switchLowering.usesBitTests = true;
    enableEveryAirSwitch(policy.air);
    return policy;
}

static Policy policyWithEverySwitchOff()
{
    Policy policy;
    policy.upstream.hoistsLoopInvariantValues = false;
    return policy;
}

struct SwitchFields {
    PolicySwitch policySwitch;
    ASCIILiteral dumpedName;
    bool (*areEqual)(const Policy&, const Policy&);
};

static bool hasSamePreferredRegisters(const Policy& policy, const Policy& other)
{
    auto fieldsOf = [](const Air::RegisterPolicy& registers) {
        return std::tuple(registers.discountsPreferredRegisterMoves, registers.hintsTwoAddressOperands, registers.usesSecondPreferredRegister, registers.evictsHolderOfPreferredRegister);
    };
    return fieldsOf(policy.air.registers) == fieldsOf(other.air.registers);
}

static bool hasSameLiveRangeSplitting(const Policy& policy, const Policy& other)
{
    auto fieldsOf = [](const Air::LiveRangeSplitPolicy& splitting) {
        return std::tuple(splitting.splitsAroundClobbersEarly, splitting.splitsAroundClobbersThatRead, splitting.keepsUnusedTmpsOnStackThroughLoops, splitting.spillsTmpsReadInPlaceFirst, splitting.spansStretchesOfClobbers, splitting.leastCostOfSplitAroundClobber, splitting.splitsIntoClustersOverBlocks);
    };
    return fieldsOf(policy.air.splitting) == fieldsOf(other.air.splitting);
}

static bool hasSameFrameLayout(const Policy& policy, const Policy& other)
{
    auto fieldsOf = [](const Air::FramePolicy& frame) {
        return std::tuple(frame.mayPlaceSlotsAboveFrameRecord, frame.mayPushCalleeSaves, frame.mayPushSpillsAtFrameSetup, frame.ordersSlotsByUse, frame.prefersStackPointerAddresses);
    };
    return fieldsOf(policy.air.frame) == fieldsOf(other.air.frame);
}

static bool hasSameRareBlockLayout(const Policy& policy, const Policy& other)
{
    auto fieldsOf = [](const Air::LayoutPolicy& layout) {
        return std::tuple(layout.placesRareBlocksInGaps, layout.placesRareBlocksAfterGuards, layout.makesGapsForRareBlocks, layout.ordersBlocksByEdgeFrequency);
    };
    return fieldsOf(policy.air.layout) == fieldsOf(other.air.layout);
}

#define SWITCH_FIELD(policySwitch, part, field) \
    SwitchFields { PolicySwitch::policySwitch, #field ""_s, [](const Policy& policy, const Policy& other) { return policy.part.field == other.part.field; } }

static std::span<const SwitchFields> switchFields()
{
    static constexpr SwitchFields fields[] = {
        SWITCH_FIELD(MemoryNumbering, optimization, redundancyElimination),
        SWITCH_FIELD(SplitBranchConditions, optimization, splitsBranchConditions),
        SWITCH_FIELD(EstimateFrequencies, optimization, estimatesFrequencies),
        SWITCH_FIELD(ThreadJumps, optimization, threadsJumps),
        SWITCH_FIELD(FoldImpliedComparisons, optimization, foldsImpliedComparisons),
        SWITCH_FIELD(ReduceWithRanges, optimization, reducesWithRanges),
        SWITCH_FIELD(NarrowIntegers, optimization, narrowsIntegers),
        SWITCH_FIELD(ChainAssociativeOperations, optimization, chainsAssociativeOperations),
        SWITCH_FIELD(ConvertBranchesToSelects, optimization, convertsBranchesToSelects),
        SWITCH_FIELD(MergeAdjacentConstantStores, optimization, mergesAdjacentConstantStores),
        SWITCH_FIELD(SinkValues, optimization, sinksValues),
        SWITCH_FIELD(PlaceConstantsByFrequency, optimization, placesConstantsByFrequency),
        SWITCH_FIELD(ExtendInstructionSelection, selection, extendsInstructionSelection),
        SWITCH_FIELD(PinnedConstants, addressing, findPinnedConstant),
        SWITCH_FIELD(HoistLoopInvariantValues, upstream, hoistsLoopInvariantValues),
        SWITCH_FIELD(RotateLoops, loops, rotates),
        SWITCH_FIELD(FoldComparisonsWithBounds, loops, foldsComparisonsWithBounds),
        SWITCH_FIELD(VersionLoops, loops, versions),
        SWITCH_FIELD(ClampLoopBounds, loops, clampsLoopBounds),
        SWITCH_FIELD(VectorizeLoops, loops, vectorizes),
        SWITCH_FIELD(UnrollLoops, loops, unrolls),
        SWITCH_FIELD(PromoteLoopMemory, loops, promotesMemory),
        SWITCH_FIELD(BitTests, switchLowering, usesBitTests),
        SWITCH_FIELD(KnownRanges, knownProperties, trustsKnownRanges),
        SWITCH_FIELD(KnownDereferenceableBytes, knownProperties, trustsKnownDereferenceableBytes),
        SWITCH_FIELD(KnownInductions, knownProperties, trustsKnownInductions),
        SWITCH_FIELD(ChargeForCalleeSaves, air.registers, chargesForCalleeSaves),
        SwitchFields { PolicySwitch::PreferredRegisters, "usesSecondPreferredRegister"_s, hasSamePreferredRegisters },
        SwitchFields { PolicySwitch::SplitLiveRanges, "splitsAroundClobbersEarly"_s, hasSameLiveRangeSplitting },
        SWITCH_FIELD(SplitLiveRangesAroundFrame, air.splitting, splitsAroundFrame),
        SWITCH_FIELD(ShrinkWrapFrame, air.frame, shrinkWraps),
        SwitchFields { PolicySwitch::FrameLayout, "ordersSlotsByUse"_s, hasSameFrameLayout },
        SWITCH_FIELD(OmitFrameRecord, air.frame, mayOmitFrameRecord),
        SWITCH_FIELD(MoveBlocksWithoutSuccessorsToEnd, air.layout, movesBlocksWithoutSuccessorsToEnd),
        SwitchFields { PolicySwitch::RareBlockLayout, "placesRareBlocksInGaps"_s, hasSameRareBlockLayout },
        SWITCH_FIELD(MergeIdenticalBlocks, air.layout, mergesIdenticalBlocks),
        SWITCH_FIELD(DuplicateReturns, air.layout, duplicatesReturns),
        SWITCH_FIELD(ForwardRegisterCopies, air.peepholes, forwardsRegisterCopies),
        SWITCH_FIELD(HoistCommonMoves, air.peepholes, hoistsCommonMoves),
        SWITCH_FIELD(ReuseConditionFlags, air.peepholes, reusesConditionFlags),
        SWITCH_FIELD(PairMemoryAccesses, air.peepholes, pairsMemoryAccesses),
        SWITCH_FIELD(ResequenceMovesBeforePatches, air.peepholes, resequencesMovesBeforePatches),
        SWITCH_FIELD(FoldTrapsIntoAccesses, air.peepholes, trapReach),
    };
    static_assert(std::size(fields) == numberOfSwitches);
    return fields;
}

#undef SWITCH_FIELD

#include "testb3_bun_pipeline.h"

static void testPolicySwitchNamesAreDistinct()
{
    HashSet<String> names;
    for (unsigned i = 0; i < numberOfSwitches; ++i) {
        ASCIILiteral name = nameOf(static_cast<PolicySwitch>(i));
        CHECK(!name.isEmpty());
        names.add(String(name));
    }
    CHECK_EQ(names.size(), numberOfSwitches);
}

static void testDefaultPolicyDumpsNothing()
{
    CHECK(dumpOf(Policy()).isEmpty());

    Policy sinking;
    sinking.optimization.sinksValues = true;
    CHECK(dumpOf(sinking) == "sinksValues"_s);

    Policy duplicating;
    duplicating.air.layout.duplicatesReturns = true;
    CHECK(dumpOf(duplicating) == "duplicatesReturns"_s);

    CHECK(dumpOf(policyWithEverySwitchOff()) == "hoistsLoopInvariantValues = false"_s);
}

static void testDisableResetsOnlyOwnFields()
{
    const Policy defaultPolicy { };
    const Policy disabledPolicy = policyWithEverySwitchOff();
    const Policy enabledPolicy = policyWithEverySwitchOn();
    for (unsigned i = 0; i < numberOfSwitches; ++i) {
        auto policySwitch = static_cast<PolicySwitch>(i);
        CHECK(switchFields()[i].policySwitch == policySwitch);
        Policy policy = enabledPolicy;
        policy.disable(policySwitch);

        for (unsigned j = 0; j < numberOfSwitches; ++j)
            CHECK(policy.isDisabled(static_cast<PolicySwitch>(j)) == (i == j));
        CHECK_EQ(policy.disabledSwitches(), 1ull << i);

        String dump = dumpOf(policy);
        for (const SwitchFields& fields : switchFields()) {
            bool isOwnEntry = fields.policySwitch == policySwitch;
            CHECK(fields.areEqual(policy, isOwnEntry ? disabledPolicy : enabledPolicy));
            CHECK(dump.contains(fields.dumpedName) == !fields.areEqual(policy, defaultPolicy));
        }
        CHECK(policy.air.layout.recordsRareCodeStart);

        policy.disable(policySwitch);
        CHECK(dumpOf(policy) == dump);
    }
}

static void testDisabledSwitchSurvivesLaterWrites()
{
    Policy disabled;
    disabled.disable(PolicySwitch::FrameLayout);
    disabled.air.frame.ordersSlotsByUse = true;
    CHECK(disabled.air.frame.ordersSlotsByUse);
    disabled.applyDisabledSwitches();
    CHECK(!disabled.air.frame.ordersSlotsByUse);

    Policy enabled;
    enabled.air.frame.ordersSlotsByUse = true;
    enabled.applyDisabledSwitches();
    CHECK(enabled.air.frame.ordersSlotsByUse);
}

static void testConstructorAppliesDisabledSwitches()
{
    Policy disabled;
    disabled.disable(PolicySwitch::FrameLayout);
    disabled.air.frame.ordersSlotsByUse = true;
    Procedure proc(disabled);
    CHECK(proc.policy().isDisabled(PolicySwitch::FrameLayout));
    CHECK(!proc.policy().air.frame.ordersSlotsByUse);

    Policy enabled;
    enabled.air.frame.ordersSlotsByUse = true;
    Procedure procWithFrameLayout(enabled);
    CHECK(!procWithFrameLayout.policy().disabledSwitches());
    CHECK(procWithFrameLayout.policy().air.frame.ordersSlotsByUse);
}

static void testRandomProgramPolicyCoversSwitches()
{
    RandomProgramState state;
    RandomProgramRelocationLowering lowering(state);
    const Policy policy = randomProgramPolicy(lowering);
    const Policy disabledPolicy = policyWithEverySwitchOff();
    for (const SwitchFields& fields : switchFields()) {
        bool isLeftOff = fields.policySwitch == PolicySwitch::PinnedConstants
            || fields.policySwitch == PolicySwitch::ShrinkWrapFrame
            || fields.policySwitch == PolicySwitch::OmitFrameRecord
            || fields.policySwitch == PolicySwitch::DuplicateReturns
            || fields.policySwitch == PolicySwitch::ClampLoopBounds;
        if (fields.areEqual(policy, disabledPolicy) == isLeftOff)
            continue;
        crashLock.lock();
        dataLogLn("randomProgramPolicy() ", isLeftOff ? "enables " : "does not enable ", nameOf(fields.policySwitch));
        CRASH();
    }
}

static void testLimitTableIsConsistent()
{
    HashSet<String> names;
    const LimitValues defaultValues;
    for (unsigned i = 0; i < numberOfLimits; ++i) {
        auto limit = static_cast<Limit>(i);
        String name = nameOf(limit);
        CHECK(name.startsWith("Max"_s));
        names.add(name);
        CHECK(!degradesTo(limit).isEmpty());
        CHECK(purposeOf(limit) <= LimitPurpose::Memory);
        CHECK(defaultValueOf(limit));
        CHECK_EQ(defaultValues.get(limit), defaultValueOf(limit));

        uint64_t loweredValue = defaultValueOf(limit) - 1;
        Policy policy;
        policy.limits.set(limit, loweredValue);
        Procedure proc(policy);
        for (unsigned j = 0; j < numberOfLimits; ++j) {
            auto other = static_cast<Limit>(j);
            CHECK_EQ(proc.limit(other), i == j ? loweredValue : defaultValueOf(other));
        }
    }
    CHECK_EQ(names.size(), numberOfLimits);
    CHECK_EQ(defaultValueOf(Limit::MaxSlotsForDenseInterference), 8192u);
}

static void testReportStartsEmpty()
{
    for (const Policy& policy : { Policy(), policyWithEverySwitchOn() }) {
        Procedure proc(policy);
        const CompilationReport& report = proc.report();
        CHECK(!report.failure);
        CHECK(report.remarks.isEmpty());
        for (unsigned i = 0; i < numberOfLimits; ++i) {
            auto limit = static_cast<Limit>(i);
            CHECK(!report.hitsOf(limit));
            CHECK(!report.firstOriginOf(limit));
        }
        for (uint32_t count : report.counters)
            CHECK(!count);
    }
}

static void testLimitHitKeepsFirstOrigin()
{
    Procedure proc;
    proc.didReachLimit(Limit::MaxHoistedLoadsPerLoop, numberedOrigin(1));
    proc.didReachLimit(Limit::MaxHoistedLoadsPerLoop, numberedOrigin(2));
    proc.didReachLimit(Limit::MaxSelectsPerJoin);
    proc.didReachLimit(Limit::MaxSelectsPerJoin, numberedOrigin(3));

    const CompilationReport& report = proc.report();
    CHECK_EQ(report.hitsOf(Limit::MaxHoistedLoadsPerLoop), 2u);
    CHECK(hasNumber(report.firstOriginOf(Limit::MaxHoistedLoadsPerLoop), 1));
    CHECK_EQ(report.hitsOf(Limit::MaxSelectsPerJoin), 2u);
    CHECK(!report.firstOriginOf(Limit::MaxSelectsPerJoin));
    CHECK(!report.hitsOf(Limit::MaxLoopsLeftBySinking));
}

static void checkBlockChainOrders(Procedure& proc, const Vector<BasicBlock*>& chain)
{
    Vector<BasicBlock*> reversedChain = chain;
    reversedChain.reverse();
    CHECK(proc.blocksInPreOrder() == chain);
    CHECK(proc.cachedBlocksInPreOrder() == chain);
    CHECK(proc.blocksInPostOrder() == reversedChain);
    CHECK(proc.cachedBlocksInPostOrder() == reversedChain);
}

static void testCachedBlockOrdersAreInvalidated()
{
    for (bool usesArena : { true, false }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.allocation.usesArena = usesArena;
        }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        graph.jump(root, exit);
        graph.ret(exit, graph.int64(exit, 1));
        proc.resetReachability();
        checkBlockChainOrders(proc, { root, exit });
        checkBlockChainOrders(proc, { root, exit });

        BasicBlock* middle = proc.addBlock();
        root->setSuccessors(FrequentedBlock(middle));
        graph.jump(middle, exit);
        proc.resetReachability();
        proc.invalidateCFG();
        checkBlockChainOrders(proc, { root, middle, exit });

        BasicBlock* last = proc.addBlock();
        exit->replaceLastWithNew<Value>(proc, Jump, Origin());
        exit->setSuccessors(FrequentedBlock(last));
        graph.ret(last, graph.int64(last, 2));
        checkBlockChainOrders(proc, { root, middle, exit, last });

        root->setSuccessors(FrequentedBlock(exit));
        proc.invalidateCFG();
        checkBlockChainOrders(proc, { root, exit, last });

        root->setSuccessors(FrequentedBlock(middle));
        proc.resetReachability();
        checkBlockChainOrders(proc, { root, middle, exit, last });
    }
}

static void testCachedBlockOrdersFollowSwappedSuccessors()
{
    Procedure proc(policyWith([](Policy&) { }));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* isZero = proc.addBlock();
    BasicBlock* isNotZero = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.branch(root, graph.make(root, Equal, argument, graph.int64(root, 0)), isZero, isNotZero);
    graph.ret(isZero, graph.int64(isZero, 1));
    graph.ret(isNotZero, graph.int64(isNotZero, 2));
    proc.resetReachability();
    proc.setOptLevel(2);
    Vector<BasicBlock*> preOrderBefore = proc.cachedBlocksInPreOrder();
    Vector<BasicBlock*> postOrderBefore = proc.cachedBlocksInPostOrder();

    CHECK(reduceStrength(proc, ReduceStrengthPass::Initial));
    CHECK(root->successorBlock(0) == isNotZero);
    CHECK(proc.cachedBlocksInPreOrder() == proc.blocksInPreOrder());
    CHECK(proc.cachedBlocksInPostOrder() == proc.blocksInPostOrder());
    CHECK(proc.cachedBlocksInPreOrder() != preOrderBefore);
    CHECK(proc.cachedBlocksInPostOrder() != postOrderBefore);
}

static void testCachedBlockOrdersAreReused()
{
    Procedure proc;
    if (shouldValidateIRAtEachPhase(proc))
        return;

    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* middle = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    graph.jump(root, middle);
    graph.jump(middle, exit);
    graph.ret(exit, graph.int64(exit, 1));
    checkBlockChainOrders(proc, { root, middle, exit });

    root->setSuccessors(FrequentedBlock(exit));
    CHECK(proc.blocksInPreOrder() == (Vector<BasicBlock*> { root, exit }));
    CHECK(proc.cachedBlocksInPreOrder() == (Vector<BasicBlock*> { root, middle, exit }));
    CHECK(proc.blocksInPostOrder() == (Vector<BasicBlock*> { exit, root }));
    CHECK(proc.cachedBlocksInPostOrder() == (Vector<BasicBlock*> { exit, middle, root }));

    proc.invalidateCFG();
    checkBlockChainOrders(proc, { root, exit });
}

static void testHasCycle()
{
    enum class LastEdge : uint8_t { ToExit, ToItself, ToBlockAbove };
    for (LastEdge lastEdge : { LastEdge::ToExit, LastEdge::ToItself, LastEdge::ToBlockAbove }) {
        Procedure proc(policyWith([](Policy&) { }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* left = proc.addBlock();
        BasicBlock* right = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        BasicBlock* otherExit = proc.addBlock();
        Value* condition = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.branch(root, condition, left, right);
        graph.jump(left, join);
        graph.branch(right, condition, join, left);
        graph.branch(join, condition, exit, lastEdge == LastEdge::ToExit ? otherExit : lastEdge == LastEdge::ToItself ? join : right);
        graph.ret(exit, graph.int64(exit, 1));
        graph.ret(otherExit, graph.int64(otherExit, 2));
        proc.resetReachability();
        CHECK_EQ(proc.hasCycle(), lastEdge != LastEdge::ToExit);
        CHECK_EQ(proc.hasCycle(), lastEdge != LastEdge::ToExit);

        join->successorBlock(1) = lastEdge == LastEdge::ToExit ? left : exit;
        proc.resetReachability();
        CHECK_EQ(proc.hasCycle(), lastEdge == LastEdge::ToExit);
    }
}

static void testPointerConstantFailsWhenPositionIndependent()
{
    int64_t word = 0;
    Procedure failing(positionIndependentPolicy());
    failing.add<ConstPtrValue>(numberedOrigin(3), &word);
    CHECK(hasFailure(failing, FailureKind::AbsoluteAddress));
    CHECK(hasNumber(failing.report().failure->origin, 3));

    Procedure independent(positionIndependentPolicy());
    independent.add<ConstPtrValue>(Origin(), static_cast<int64_t*>(nullptr));
    independent.add<ConstPtrValue>(Origin(), static_cast<intptr_t>(8));
    CHECK(!independent.report().failure);

    Procedure plain;
    plain.add<ConstPtrValue>(Origin(), &word);
    CHECK(!plain.report().failure);
}

static void testFirstFailureWins()
{
    Procedure proc;
    proc.fail(FailureKind::RelocationNotLowered, numberedOrigin(1));
    proc.fail(FailureKind::AbsoluteAddress, numberedOrigin(2));
    CHECK(hasFailure(proc, FailureKind::RelocationNotLowered));
    CHECK(hasNumber(proc.report().failure->origin, 1));
}

static void testRemarksAreCollectedOnRequest()
{
    for (bool collectsRemarks : { false, true }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.collectsRemarks = collectsRemarks;
        }));
        proc.addRemark({ RemarkKind::RangeChecksRemoved, GaveUpReason::None, Origin(), std::nullopt }, 3);
        proc.addRemark({ RemarkKind::LoopNotVersioned, GaveUpReason::LimitReached, Origin(), Limit::MaxClonedValuesPerLoopVersion });

        const CompilationReport& report = proc.report();
        for (unsigned i = 0; i < numberOfCounters; ++i) {
            auto counter = static_cast<Counter>(i);
            CHECK_EQ(report.countOf(counter), counter == Counter::RangeChecksRemoved ? 3u : 0u);
        }
        CHECK(report.remarks.size() == (collectsRemarks ? 2u : 0u));
        CHECK_EQ(report.hitsOf(Limit::MaxClonedValuesPerLoopVersion), 0u);
    }
}

static void testKnownRangeFollowsCloneAndDelete()
{
    Procedure proc(policyWith([](Policy&) { }));
    Value* pointer = proc.add<ArgumentRegValue>(Origin(), GPRInfo::argumentGPR0);
    Value* value = proc.add<MemoryValue>(Load, Int32, Origin(), pointer);

    CHECK(!proc.knownRange(value));
    proc.setKnownRange(value, IntRange(0, 39));
    CHECK(*proc.knownRange(value) == IntRange(0, 39));
    CHECK(*proc.knownRange(proc.clone(value)) == IntRange(0, 39));
    proc.setKnownRange(value, IntRange(1, 2));
    CHECK(*proc.knownRange(value) == IntRange(1, 2));

    unsigned index = value->index();
    proc.deleteValue(value);
    Value* reused = nullptr;
    for (unsigned attempt = 0; attempt < 2 && !reused; ++attempt) {
        Value* candidate = proc.add<MemoryValue>(Load, Int32, Origin(), pointer);
        if (candidate->index() == index)
            reused = candidate;
    }
    CHECK(reused);
    CHECK(!proc.knownRange(reused));
}

static void testPinnedRegistersOfPolicy()
{
    Procedure proc(policyWith([](Policy& policy) {
        policy.air.registers.pinned.add(GPRInfo::regCS0, IgnoreVectors);
        policy.air.registers.pinned.add(GPRInfo::argumentGPR1, IgnoreVectors);
    }));
    CHECK(proc.code().isPinned(GPRInfo::regCS0));
    CHECK(proc.code().isPinned(GPRInfo::argumentGPR1));
    CHECK(!proc.code().isPinned(GPRInfo::regCS1));
}

static std::array<int64_t, numberOfRuntimeFunctions> runtimeFunctionBits()
{
    return {
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::fmodDouble)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::ceilDouble)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::ceilFloat)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::floorDouble)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::floorFloat)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::truncDouble)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(Math::truncFloat)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(operationMemoryMove)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(operationGCSafeMemoryMove)),
        std::bit_cast<int64_t>(tagCFunction<OperationPtrTag>(operationMemoryFill)),
    };
}

static void testAddressOfRuntimeFunction()
{
    std::array<int64_t, numberOfRuntimeFunctions> callees = runtimeFunctionBits();
    Procedure plain;
    Procedure independent(positionIndependentPolicy());
    for (uint32_t i = 0; i < numberOfRuntimeFunctions; ++i) {
        auto function = static_cast<RuntimeFunction>(i);

        Value* constant = addressOf(plain, function);
        CHECK(constant->opcode() == Const64);
        CHECK(constant->isInt64(callees[i]));

        RelocatableValue* relocatable = addressOf(independent, function)->as<RelocatableValue>();
        CHECK(relocatable);
        CHECK(relocatable->symbol() == (RelocationSymbol { runtimeFunctionSpace, i }));
        CHECK(!relocatable->addend());
    }
    CHECK(!plain.report().failure);
    CHECK(!independent.report().failure);

    independent.add<ConstPtrValue>(Origin(), tagCFunction<OperationPtrTag>(Math::fmodDouble));
    CHECK(hasFailure(independent, FailureKind::AbsoluteAddress));
}

static void buildFmodProcedure(Procedure& proc)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double>(proc, root);
    Value* left = arguments[0];
    Value* right = arguments[1];
    Value* remainder = graph.make(root, Mod, left, right);
    remainder->setOrigin(numberedOrigin(7));
    graph.ret(root, remainder);
    proc.resetReachability();
}

static Value* findCCallee(Procedure& proc)
{
    for (Value* value : proc.values()) {
        if (value->opcode() == CCall)
            return value->child(0);
    }
    return nullptr;
}

static void testRuntimeFunctionAddresses()
{
    Procedure plain;
    buildFmodProcedure(plain);
    lowerMacros(plain);
    Value* constant = findCCallee(plain);
    CHECK(constant);
    CHECK(constant->opcode() == Const64);

    Procedure independent(positionIndependentPolicy());
    buildFmodProcedure(independent);
    lowerMacros(independent);
    Value* callee = findCCallee(independent);
    CHECK(callee);
    RelocatableValue* relocatable = callee->as<RelocatableValue>();
    CHECK(relocatable);
    CHECK(relocatable->symbol() == (RelocationSymbol { runtimeFunctionSpace, static_cast<uint32_t>(RuntimeFunction::FmodDouble) }));
}

class TableLowering final : public RelocationLowering {
public:
    explicit TableLowering(bool declines)
        : m_declines(declines)
    {
    }

    Value* lower(Procedure&, InsertionSet& insertionSet, size_t index, const RelocatableValue& value) final
    {
        RelocationSymbol symbol = value.symbol();
        if (m_declines || symbol.space != runtimeFunctionSpace)
            return nullptr;

        void* callee = std::bit_cast<void*>(runtimeFunctionBits()[symbol.index]);
        PatchpointValue* result = insertionSet.insert<PatchpointValue>(index, pointerType(), value.origin());
        result->effects = Effects::none();
        result->pureFunction = PureFunction { 400 + symbol.index };
        result->setGenerator([callee](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(CCallHelpers::TrustedImmPtr(callee), params[0].gpr());
        });
        return result;
    }

private:
    bool m_declines;
};

static void testFmodThroughRelocation()
{
    for (bool declines : { false, true }) {
        TableLowering lowering(declines);
        Policy policy = positionIndependentPolicy();
        policy.addressing.relocationLowering = &lowering;
        Procedure proc(policy);
        buildFmodProcedure(proc);
        if (declines) {
            proc.setOptLevel(2);
            prepareForGeneration(proc);
            CHECK(hasFailure(proc, FailureKind::RelocationNotLowered));
            CHECK(hasNumber(proc.report().failure->origin, 7));
            continue;
        }
        std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);
        CHECK(invoke<double>(*code, 7.5, 2.0) == 1.5);
        CHECK(invoke<double>(*code, -7.5, 2.0) == -1.5);
    }
}

static void setEveryPatchpointField(PatchpointValue* patchpoint)
{
    patchpoint->isLoopPoll = true;
    patchpoint->pureFunction = PureFunction { 9, true };
    patchpoint->clientTag = { 1, 2, 3 };
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->clobberLateOnSuccessor(1, RegisterSet(GPRInfo::argumentGPR1));
}

static void testPatchpointFieldsFollowClone()
{
    Procedure proc;
    Graph graph { proc };
    PatchpointValue* patchpoint = graph.emptyPatchpoint(proc.addBlock(), Int32);
    CHECK(!patchpoint->isLoopPoll);
    CHECK(!patchpoint->pureFunction);
    CHECK(!patchpoint->clientTag.kind);
    CHECK(!patchpoint->clientTag.flags);
    CHECK(!patchpoint->clientTag.data);
    CHECK(!patchpoint->runsWithoutFrame());
    CHECK(!patchpoint->numberOfSuccessorClobbers());
    CHECK(patchpoint->lateClobberedOnSuccessor(1).isEmpty());

    setEveryPatchpointField(patchpoint);
    PatchpointValue* clone = proc.clone(patchpoint)->as<PatchpointValue>();
    CHECK(clone->isLoopPoll);
    CHECK(clone->pureFunction == (PureFunction { 9, true }));
    CHECK_EQ(clone->clientTag.kind, 1u);
    CHECK_EQ(clone->clientTag.flags, 2u);
    CHECK_EQ(clone->clientTag.data, 3u);
    CHECK(clone->runsWithoutFrame());
    CHECK_EQ(clone->numberOfSuccessorClobbers(), 2u);
    CHECK(clone->lateClobberedOnSuccessor(0).isEmpty());
    CHECK(clone->lateClobberedOnSuccessor(1) == RegisterSet(GPRInfo::argumentGPR1));
}

static void testPatchpointDump()
{
    static constexpr ASCIILiteral fieldNames[] = { "isLoopPoll"_s, "pureFunction"_s, "clientTag"_s, "runsWithoutFrame"_s, "lateClobberedOnSuccessor1"_s };

    Procedure proc;
    Graph graph { proc };
    PatchpointValue* patchpoint = graph.emptyPatchpoint(proc.addBlock(), Int32);

    String freshDump = toString(deepDump(proc, patchpoint));
    for (ASCIILiteral name : fieldNames)
        CHECK(!freshDump.contains(name));

    setEveryPatchpointField(patchpoint);

    String dump = toString(deepDump(proc, patchpoint));
    for (ASCIILiteral name : fieldNames)
        CHECK(dump.contains(name));
    CHECK(!dump.contains("lateClobberedOnSuccessor0"_s));
}

template<typename Operand>
static void buildBinaryMacro(Graph& graph, BasicBlock* block, B3::Kind kind)
{
    auto arguments = cCallArgumentValues<Operand, Operand>(graph.proc, block);
    Value* left = arguments[0];
    Value* right = arguments[1];
    graph.ret(block, block->appendNew<Value>(graph.proc, kind, Origin(), left, right));
}

template<B3::Opcode opcode> static void buildDoubleMacro(Graph& graph, BasicBlock* block) { buildBinaryMacro<double>(graph, block, opcode); }
template<B3::Opcode opcode> static void buildInt32Macro(Graph& graph, BasicBlock* block) { buildBinaryMacro<int32_t>(graph, block, opcode); }
template<B3::Opcode opcode> static void buildChillMacro(Graph& graph, BasicBlock* block) { buildBinaryMacro<int32_t>(graph, block, chill(opcode)); }
static void buildDependMacro(Graph& graph, BasicBlock* block) { graph.ret(block, graph.make(block, Depend, cCallArgumentValues<int64_t>(graph.proc, block)[0])); }

static void buildSwitchMacro(Graph& graph, BasicBlock* block)
{
    Value* subject = cCallArgumentValues<int64_t>(graph.proc, block)[0];
    SwitchValue* switchValue = block->appendNew<SwitchValue>(graph.proc, Origin(), subject);
    for (int64_t caseValue : { 1, 2, 3 }) {
        BasicBlock* target = graph.proc.addBlock();
        graph.ret(target, graph.int64(target, caseValue * 10));
        switchValue->appendCase(SwitchCase(caseValue, FrequentedBlock(target)));
    }
    BasicBlock* fallThrough = graph.proc.addBlock();
    graph.ret(fallThrough, graph.int64(fallThrough, 0));
    switchValue->setFallThrough(FrequentedBlock(fallThrough));
}

template<B3::Opcode opcode, Width width>
static void buildAtomicExchange(Graph& graph, BasicBlock* block)
{
    Value* pointer = cCallArgumentValues<void*>(graph.proc, block)[0];
    graph.ret(block, block->appendNew<AtomicValue>(graph.proc, opcode, Origin(), width, graph.int32(block, 1), pointer));
}

template<B3::Opcode opcode>
static void buildCompareAndSwap(Graph& graph, BasicBlock* block)
{
    Value* pointer = cCallArgumentValues<int8_t*>(graph.proc, block)[0];
    Value* expected = graph.int32(block, 1);
    Value* replacement = graph.int32(block, 2);
    graph.ret(block, block->appendNew<AtomicValue>(graph.proc, opcode, Origin(), Width8, expected, replacement, pointer));
}

template<B3::Opcode opcode>
static void buildFencedLoad(Graph& graph, BasicBlock* block)
{
    Value* pointer = cCallArgumentValues<void*>(graph.proc, block)[0];
    MemoryValue* load = block->appendNew<MemoryValue>(graph.proc, opcode, Origin(), pointer);
    load->setFenceRange(firstHeap);
    graph.ret(block, load);
}

template<B3::Opcode opcode> static bool hasNoValue(Procedure& proc) { return !numberOfValues(proc, opcode); }
template<B3::Opcode opcode> static bool hasOneValue(Procedure& proc) { return numberOfValues(proc, opcode) == 1; }
template<B3::Opcode removed, B3::Opcode added> static bool hasReplacedValue(Procedure& proc) { return hasNoValue<removed>(proc) && hasOneValue<added>(proc); }
static bool hasSeveralBlocks(Procedure& proc) { return numberOfBlocks(proc) > 1; }
static bool hasNoFloatingPointExtremum(Procedure& proc) { return hasNoValue<FMax>(proc) && hasNoValue<FMin>(proc); }
static bool isChillMod(Value* value) { return value->opcode() == Mod && value->isChill(); }
static bool hasNoChillMod(Procedure& proc) { return !numberOfValues(proc, isChillMod); }

struct MacroCase {
    ASCIILiteral name;
    bool isLoweredOnThisCPU;
    void (*build)(Graph&, BasicBlock*);
    bool (*isLowered)(Procedure&);
};

static constexpr MacroCase macroCases[] = {
    { "Mod on doubles"_s, true, buildDoubleMacro<Mod>, hasReplacedValue<Mod, CCall> },
    { "chill(Mod)"_s, true, buildChillMacro<Mod>, hasNoChillMod },
    { "chill(Div)"_s, isX86(), buildChillMacro<Div>, hasSeveralBlocks },
    { "UMod"_s, isARM64(), buildInt32Macro<UMod>, hasReplacedValue<UMod, UDiv> },
    { "FMax"_s, isX86(), buildDoubleMacro<FMax>, hasNoFloatingPointExtremum },
    { "FMin"_s, isX86(), buildDoubleMacro<FMin>, hasNoFloatingPointExtremum },
    { "Switch"_s, true, buildSwitchMacro, hasNoValue<Switch> },
    { "Depend"_s, isX86(), buildDependMacro, hasReplacedValue<Depend, Fence> },
    { "AtomicStrongCAS"_s, true, buildCompareAndSwap<AtomicStrongCAS>, hasOneValue<SExt8> },
    { "AtomicXchg"_s, true, buildAtomicExchange<AtomicXchg, Width8>, hasOneValue<SExt8> },
    { "AtomicXchgAdd"_s, true, buildAtomicExchange<AtomicXchgAdd, Width8>, hasOneValue<SExt8> },
    { "AtomicXchgSub"_s, isX86(), buildAtomicExchange<AtomicXchgSub, Width32>, hasReplacedValue<AtomicXchgSub, Neg> },
    { "AtomicWeakCAS"_s, isARM64(), buildCompareAndSwap<AtomicWeakCAS>, hasOneValue<BitAnd> },
    { "AtomicXchgAnd"_s, isARM64(), buildAtomicExchange<AtomicXchgAnd, Width8>, hasOneValue<SExt8> },
    { "AtomicXchgOr"_s, isARM64(), buildAtomicExchange<AtomicXchgOr, Width8>, hasOneValue<SExt8> },
    { "AtomicXchgXor"_s, isARM64(), buildAtomicExchange<AtomicXchgXor, Width8>, hasOneValue<SExt8> },
    { "Load8Z"_s, isARM64(), buildFencedLoad<Load8Z>, hasOneValue<Load8S> },
    { "Load16Z"_s, isARM64(), buildFencedLoad<Load16Z>, hasOneValue<Load16S> },
};

static void testLowerMacrosPredicateCoversSwitch()
{
    for (const MacroCase& macroCase : macroCases) {
        if (!macroCase.isLoweredOnThisCPU)
            continue;
        Procedure proc;
        Graph graph { proc };
        macroCase.build(graph, proc.addBlock());
        proc.resetReachability();
        lowerMacros(proc);
        validate(proc);
        bool isLowered = macroCase.isLowered(proc);
        if (!isLowered)
            dataLogLn("lowerMacros left ", macroCase.name, " in place:\n", proc);
        CHECK(isLowered);
    }
}

static void testOpcodeSetIsValidated()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* sum = graph.make(root, Add, x, x);
    Value* fence = root->appendNew<FenceValue>(proc, Origin());
    Value* product = graph.make(taken, Mul, x, x);
    graph.upsilon(root, x, product);
    graph.branch(root, x, taken, notTaken);
    Value* result = graph.make(taken, BitXor, product, sum);
    graph.ret(taken, result);
    graph.ret(notTaken, x);
    proc.resetReachability();

    sum->replaceWithIdentity(x);
    fence->replaceWithNop();
    product->replaceWithPhi();
    notTaken->last()->replaceWithOops(notTaken);
    CHECK(!findViolatedRule(proc));
    root->last()->replaceWithJump(root, FrequentedBlock(taken));
    proc.resetReachability();
    CHECK(!findViolatedRule(proc));

    result->setOpcodeUnsafely(Sub);
    CHECK(findViolatedRule(proc) == ValidationRule::OpcodeEverAdded);
    proc.didAddOpcode(Sub);
    CHECK(!findViolatedRule(proc));
}

static void buildRepeatedLoad(Graph& graph, BasicBlock* block)
{
    Value* pointer = cCallArgumentValues<int64_t*>(graph.proc, block)[0];
    MemoryValue* first = graph.load(block, Int64, pointer, 0, firstHeap);
    MemoryValue* second = graph.load(block, Int64, pointer, 0, firstHeap);
    graph.ret(block, graph.make(block, Add, first, second));
}

static void buildSmallWordExtension(Graph& graph, BasicBlock* block)
{
    Value* word = cCallArgumentValues<int64_t>(graph.proc, block)[0];
    graph.proc.setKnownRange(word, IntRange(0, 100));
    graph.ret(block, graph.make(block, SExt32, graph.make(block, Trunc, word)));
}

static void buildTaggedInt32Comparison(Graph& graph, BasicBlock* block)
{
    constexpr int64_t tag = static_cast<int64_t>(0xfffe000000000000ull);
    Value* extended = graph.make(block, ZExt32, cCallArgumentValues<int32_t>(graph.proc, block)[0]);
    Value* tagged = graph.make(block, Add, extended, graph.int64(block, tag));
    graph.ret(block, graph.make(block, AboveEqual, tagged, graph.int64(block, tag)));
}

static void buildLowWordOfWideSum(Graph& graph, BasicBlock* block)
{
    auto words = cCallArgumentValues<int32_t, int32_t>(graph.proc, block);
    Value* sum = graph.make(block, Add, graph.make(block, SExt32, words[0]), graph.make(block, SExt32, words[1]));
    graph.ret(block, graph.make(block, Trunc, sum));
}

static void buildComparisonOfReplacedConstant(Graph& graph, BasicBlock* block)
{
    Value* word = cCallArgumentValues<int64_t>(graph.proc, block)[0];
    Value* select = graph.select(block, graph.make(block, Equal, word, graph.int64(block, 4)), graph.int64(block, 10), word);
    graph.ret(block, graph.make(block, AboveEqual, select, graph.int64(block, 16)));
}

struct StrengthReductionCase {
    PolicySwitch policySwitch;
    void (*build)(Graph&, BasicBlock*);
    B3::Opcode countedOpcode;
    unsigned reducedCount;
    unsigned upstreamCount;
};

static constexpr StrengthReductionCase strengthReductionCases[] = {
    { PolicySwitch::MemoryNumbering, buildRepeatedLoad, Load, 1, 2 },
    { PolicySwitch::ReduceWithRanges, buildSmallWordExtension, SExt32, 0, 1 },
    { PolicySwitch::ReduceWithRanges, buildTaggedInt32Comparison, AboveEqual, 0, 1 },
    { PolicySwitch::NarrowIntegers, buildLowWordOfWideSum, SExt32, 0, 2 },
    { PolicySwitch::FoldImpliedComparisons, buildComparisonOfReplacedConstant, Select, 0, 1 },
};

static unsigned countAfterStrengthReduction(const StrengthReductionCase& reduction, const Policy& policy)
{
    Procedure proc(policy);
    Graph graph { proc };
    reduction.build(graph, proc.addBlock());
    reduceStrengthOnce(proc);
    return numberOfValues(proc, reduction.countedOpcode);
}

static void testStrengthReductionSwitchesRestoreUpstream()
{
    for (const StrengthReductionCase& reduction : strengthReductionCases) {
        CHECK_EQ(countAfterStrengthReduction(reduction, strengthReductionPolicy()), reduction.reducedCount);
        CHECK_EQ(countAfterStrengthReduction(reduction, Policy()), reduction.upstreamCount);

        Policy othersDisabled = strengthReductionPolicy();
        for (const StrengthReductionCase& other : strengthReductionCases) {
            if (other.policySwitch != reduction.policySwitch)
                othersDisabled.disable(other.policySwitch);
        }
        CHECK_EQ(countAfterStrengthReduction(reduction, othersDisabled), reduction.reducedCount);

        Policy ownDisabled = strengthReductionPolicy();
        ownDisabled.disable(reduction.policySwitch);
        CHECK_EQ(countAfterStrengthReduction(reduction, ownDisabled), reduction.upstreamCount);
    }
}

static unsigned invariantProductCountInLoops(const Policy& policy)
{
    unsigned result = 0;
    auto inspect = [&](Procedure& optimized) {
        CHECK_EQ(numberOfValues(optimized, Mul), 1u);
        result = valueCountInLoops(optimized, Mul);
    };
    Procedure proc(policyWithInspection(policy, inspect));
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.make(loop.body, Mul, loop.extra, loop.extra));
    loop.endBody(loop.body);

    auto code = compileProc(proc, 2);
    CHECK_EQ(run(*code, 4, nullptr, 3), 36);
    return result;
}

static void testDisabledHoistingLeavesValuesInLoops()
{
    for (const Policy& policy : { Policy(), optimizingPolicy() }) {
        Policy enabled = policy;
        enabled.upstream.hoistsLoopInvariantValues = true;
        CHECK_EQ(invariantProductCountInLoops(enabled), 0u);

        Policy disabled = enabled;
        disabled.disable(PolicySwitch::HoistLoopInvariantValues);
        CHECK_EQ(invariantProductCountInLoops(disabled), 1u);
    }
}

static void testSquareRootThatIsNoNumberIsNotFolded()
{
    Procedure proc;
    CHECK(!proc.add<ConstDoubleValue>(Origin(), -1.0)->sqrtConstant(proc));
    CHECK(!proc.add<ConstDoubleValue>(Origin(), -std::numeric_limits<double>::infinity())->sqrtConstant(proc));
    CHECK(!proc.add<ConstDoubleValue>(Origin(), PNaN)->sqrtConstant(proc));
    CHECK(!proc.add<ConstFloatValue>(Origin(), -1.0f)->sqrtConstant(proc));
    CHECK(proc.add<ConstDoubleValue>(Origin(), 4.0)->sqrtConstant(proc)->isEqualToDouble(2.0));
    CHECK(proc.add<ConstDoubleValue>(Origin(), -0.0)->sqrtConstant(proc)->isNegativeZero());
    CHECK(proc.add<ConstFloatValue>(Origin(), 4.0f)->sqrtConstant(proc)->asFloat() == 2.0f);

    Procedure folded;
    BasicBlock* root = folded.addBlock();
    root->appendNew<Value>(folded, Return, Origin(), root->appendNew<Value>(folded, Sqrt, Origin(), root->appendNew<ConstDoubleValue>(folded, Origin(), -1.0)));
    Procedure computed;
    root = computed.addBlock();
    auto arguments = cCallArgumentValues<double>(computed, root);
    root->appendNew<Value>(computed, Return, Origin(), root->appendNew<Value>(computed, Sqrt, Origin(), arguments[0]));
    CHECK(isIdentical(compileAndRun<double>(folded), compileAndRun<double>(computed, -1.0)));
}

void addBunPolicyTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testPolicySwitchNamesAreDistinct());
    RUN(testDefaultPolicyDumpsNothing());
    RUN(testDisableResetsOnlyOwnFields());
    RUN(testDisabledSwitchSurvivesLaterWrites());
    RUN(testConstructorAppliesDisabledSwitches());
    RUN(testRandomProgramPolicyCoversSwitches());
    RUN(testLimitTableIsConsistent());
    RUN(testReportStartsEmpty());
    RUN(testLimitHitKeepsFirstOrigin());
    RUN(testCachedBlockOrdersAreInvalidated());
    RUN(testCachedBlockOrdersFollowSwappedSuccessors());
    RUN(testCachedBlockOrdersAreReused());
    RUN(testHasCycle());
    RUN(testPointerConstantFailsWhenPositionIndependent());
    RUN(testFirstFailureWins());
    RUN(testRemarksAreCollectedOnRequest());
    RUN(testKnownRangeFollowsCloneAndDelete());
    RUN(testPinnedRegistersOfPolicy());
    RUN(testAddressOfRuntimeFunction());
    RUN(testRuntimeFunctionAddresses());
    RUN(testFmodThroughRelocation());
    RUN(testPatchpointFieldsFollowClone());
    RUN(testPatchpointDump());
    RUN(testLowerMacrosPredicateCoversSwitch());
    RUN(testOpcodeSetIsValidated());
    RUN(testStrengthReductionSwitchesRestoreUpstream());
    RUN(testDisabledHoistingLeavesValuesInLoops());
    RUN(testSquareRootThatIsNoNumberIsNotFolded());
    addPipelineTests(config, tasks);
}

#endif // ENABLE(B3_JIT)
