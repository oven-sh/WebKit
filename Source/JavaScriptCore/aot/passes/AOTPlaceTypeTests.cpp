/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTExpectedTypesByUse.h"
#include "AOTFlowApply.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTSplitUnions.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static BlockFrequency frequencyOfGenericCopy(BlockFrequency original)
{
    return original == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
}

static bool acceptsNarrowedValue(const Function& function, const Use& use)
{
    if (use.kind != Use::Kind::Operand && use.kind != Use::Kind::TerminatorOperand)
        return true;
    OperandKind kind = use.kind == Use::Kind::Operand ? function.at(use.user).operandKind(use.operandIndex) : function.at(use.block).terminator.operandKind(use.operandIndex);
    return !mustBeDefinition(kind) && kind != OperandKind::Guard;
}

namespace {

// Puts the tests of a plan into the function, and beside the code that relies on them a generic copy, which takes over where one fails.
struct TestPlacer {
    using PointKey = std::pair<BlockID, uint32_t>;
    using Definitions = Vector<ValueID, 2>;

    struct Cut {
        PointKey key() const { return { point.block, point.index }; }

        ProgramPoint point;
        BlockID blockBefore;
        BlockID blockAfter;
        bool hasTest { false };
    };

    struct PreparedLoop {
        const PlannedWayBackIntoLoop* planned { nullptr };
        BlockID blockOnEnteringEdge;
        Vector<BlockID, 2> blocksOnBackEdges;
        bool copyComesBack { false };
    };

    struct SubjectOperand {
        uint32_t use { 0 };
        uint32_t operandIndex { 0 };
        uint32_t requirement { 0 };
    };

    struct EmittedTest {
        uint32_t plannedTest { 0 };
        BlockID block;
        BlockID passed;
        BlockID failed;
        ValueID provenWhenPassed;
        ValueID provenWhenFailed;
    };

    FunctionPassContext& context;
    const TestPlacementPlan& plan;
    const ExpectedTypesByUse& expectations;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    TypeArena& types { context.types };
    IRBuilder builder { analyses.context(), function, types, &analyses };
    bool expectsCheckedTypes { function.has(FunctionFlags::IsChecked) && !isLibraryCode(context.program, function) };
    std::optional<GenericCopy> copy { };
    GenericCopy::Extent extentOfCopy { GenericCopy::Extent::NotCopied };
    BitVector unclonableStretches { };
    // Of a cold function, as it was before the placer changed it.
    std::optional<BitVector> stretchesInFrontOfLoops { };
    BitVector stretchesThatGuessLayouts { };
    Vector<Cut, 8> cuts { };
    Vector<std::pair<Edge, BlockID>, 4> splitEdges { };
    Vector<PreparedLoop, 2> preparedLoops { };
    BitVector isOnWayBackIntoLoop { };
    Vector<ProgramPoint> rejoinPoints { };
    BitVector isRejoinBlock { };
    Vector<ProgramPoint> rejoinPointsWithoutTests { };
    BitVector isRejoinBlockWithoutTests { };
    Vector<SubjectOperand> subjectOperands { };
    Vector<Definitions> definitions = Vector<Definitions>(plan.conditions->size());
    BitVector isDefinition { };
    Vector<ValueID> adoptedProofs { };
    Vector<EmittedTest> emittedTests { };
    Vector<ValueID> changedValues { };

    PassOutcome run()
    {
        // In a very large program the copies of the loops of cold functions are megabytes, and no run shows the time that they save. In any other program a kernel that is taken for cold would lose half its speed.
        const Heat& heat = context.inputs.heat;
        if (isCold(heat.of(context.program, context.id)))
            stretchesInFrontOfLoops = heat.isOfVeryLargeProgram() ? BitVector(plan.stretches.size()) : findStretchesInFrontOfLoops();
        collectSubjectOperands();
        findStretchesThatGuessLayouts();

        auto blocksBeforeSplitting = function.blocks;
        size_t numberOfValuesBeforeSplitting = function.values.size();

        // A proof adopted on an edge that also carries tests has to become a parameter of the block the tests stand in.
        splitTestedEdges();
        adoptExistingProofs();
        collectCuts();
        splitBlocks();
        findRejoinBlocks(rejoinPoints.span(), isRejoinBlock);
        findRejoinBlocks(rejoinPointsWithoutTests.span(), isRejoinBlockWithoutTests);
        checkStretches();
        if (!numberOfGuardedStretches()) {
            // Left split, the function would be simplified, planned, split and rejected again in every round of the fixpoint.
            function.replaceBlocks(WTF::move(blocksBeforeSplitting));
            function.editAllValues().shrink(numberOfValuesBeforeSplitting);
            analyses.didChangeControlFlow();
            return PassOutcome::Unchanged;
        }

        cloneGenericCopies();
        redirectSlowPaths();
        emitTests();
        rewriteUses();
        if (copy)
            copy->addWaysBack(changedValues.span(), GenericCopy::TypesTestedOnWayBack::Expected);
        repairProofs();
        replaceDominatedUses();
        narrowValueTypes(TypePropagationInputs { context.program, context.id, function, analyses, types, context.build, &context.typingMemo }, changedValues.span());
        return PassOutcome::Changed;
    }

    bool isPlaced(unsigned stretch) const { return plan.isGuarded(stretch) && !unclonableStretches.get(stretch); }
    bool shouldClone(const StretchRange& range) const { return range.isCloned && isPlaced(range.stretch); }
    auto clonedRanges() const { return plan.ranges | std::views::filter([this](const StretchRange& range) { return shouldClone(range); }); }
    ValueID useOf(const PlannedTest& test) const { return plan.sites.sites[plan.requirements[test.requirement].site].use; }

    unsigned numberOfGuardedStretches() const
    {
        unsigned numberOfStretches = 0;
        for (unsigned stretch = 0; stretch < plan.stretches.size(); ++stretch)
            numberOfStretches += isPlaced(stretch) && plan.stretches[stretch].numberOfUses;
        return numberOfStretches;
    }

    // Those that lie in a loop, and those from which a loop can still be reached.
    BitVector findStretchesInFrontOfLoops()
    {
        const CFG& cfg = analyses.cfg();
        BitVector stretches(plan.stretches.size());
        const NaturalLoops* loops = cfg.hasCycle() ? &analyses.naturalLoops() : nullptr;
        if (!loops || !loops->numLoops())
            return stretches;

        BitVector reachesLoop(function.blocks.size());
        Vector<BlockID, 16> worklist;
        for (BlockID block : cfg.reversePostOrder()) {
            if (loops->headerOf(block) && reachesLoop.add(block.index()))
                worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
                if (reachesLoop.add(edge.from.index()))
                    worklist.append(edge.from);
            }
        }
        for (const StretchRange& range : plan.ranges) {
            if (reachesLoop.get(range.block.index()))
                stretches.quickSet(range.stretch);
        }
        return stretches;
    }

    void collectSubjectOperands()
    {
        const ValueIdentities& identities = analyses.identities();
        for (uint32_t useIndex = 0; useIndex < plan.uses.size(); ++useIndex) {
            const PlannedUse& planned = plan.uses[useIndex];
            const TestSite& site = plan.sites.sites[planned.site];
            const Value& use = function.at(site.use);
            use.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (use.operandKind(operandIndex) == OperandKind::Guard)
                    return;
                ValueID subject = identities.identityOf(operand);
                for (uint32_t testIndex = site.numberOfTests; testIndex--;) {
                    uint32_t requirement = planned.firstRequirement + testIndex;
                    const TestCondition& condition = plan.conditions->at(plan.requirements[requirement].condition);
                    if (!condition.lastsForever() || condition.subject != subject)
                        continue;
                    subjectOperands.append(SubjectOperand { useIndex, operandIndex, requirement });
                    break;
                }
            });
        }
    }

    // Every test of such a stretch looks at a value of which layouts are expected and nobody has checked that: nothing is known of it but the names it is asked for, or the function is not a checked function of the program.
    void findStretchesThatGuessLayouts()
    {
        stretchesThatGuessLayouts.ensureSize(plan.stretches.size());
        BitVector stretchesWithOtherTests(plan.stretches.size());
        for (const PlannedTest& test : plan.tests) {
            const Value& written = plan.sites.tests[plan.requirements[test.requirement].test];
            ValueID tested = !test.operands.isEmpty() ? test.operands[testedOperandIndex] : written.numberOfOperands() ? written.operand(testedOperandIndex) : ValueID();
            bool isGuess = tested && expectations.isGuessedFromNames(tested);
            if (tested && !expectsCheckedTypes) {
                TypeID expected = function.at(tested).expected();
                isGuess |= expected && types.at(expected).layouts;
            }
            (isGuess ? stretchesThatGuessLayouts : stretchesWithOtherTests).quickSet(test.stretch);
        }
        stretchesThatGuessLayouts.exclude(stretchesWithOtherTests);
    }

    // Behind the stretch the copy keeps apart what the code that passed the test has proven.
    bool goesOnBehindStretch(const StretchRange& range) const
    {
        bool isColdWithoutLoopBehind = stretchesInFrontOfLoops && !stretchesInFrontOfLoops->quickGet(range.stretch);
        return shouldClone(range) && !stretchesThatGuessLayouts.quickGet(range.stretch) && !isColdWithoutLoopBehind;
    }

    bool goesOnBehindSomeStretch() const
    {
        return std::ranges::any_of(plan.ranges, [&](const StretchRange& range) { return goesOnBehindStretch(range); });
    }

    BlockID ensureSplit(Edge edge)
    {
        for (auto [splitEdge, block] : splitEdges) {
            if (splitEdge == edge)
                return block;
        }
        builder.setOrigin(function.at(edge.from).terminator.origin());
        splitEdges.append({ edge, builder.splitEdge(edge) });
        return splitEdges.last().second;
    }

    void splitTestedEdges()
    {
        for (const PlannedTest& test : plan.tests) {
            if (test.position == PlannedTestPosition::OnEdge && plan.isGuarded(test.stretch))
                ensureSplit(test.edge);
        }
        // The generic copy of a turn of a loop leaves the copy on an edge that goes around the loop, where a block is put for it, and goes on from there to the block on an edge that enters the loop.
        for (const PlannedWayBackIntoLoop& planned : plan.waysBackIntoLoops) {
            if (!plan.isGuarded(planned.stretch))
                continue;
            PreparedLoop loop { &planned, ensureSplit(planned.enteringEdge), { }, false };
            passArgumentsThrough(planned.enteringEdge, loop.blockOnEnteringEdge);
            for (Edge edge : planned.backEdges)
                loop.blocksOnBackEdges.append(ensureSplit(edge));
            preparedLoops.append(WTF::move(loop));
        }
    }

    // What the block passes on becomes its parameters, so that another edge into it can pass other values.
    void passArgumentsThrough(Edge edge, BlockID blockOnEdge)
    {
        Target next = function.at(blockOnEdge).terminator.next();
        RELEASE_ASSERT(function.at(blockOnEdge).parameters.isEmpty());

        builder.setOrigin(function.at(blockOnEdge).terminator.origin());
        Arguments parameters;
        for (ValueID parameter : Vector<ValueID, 4>(function.at(next.block).parameters.span())) {
            ValueID added = builder.addParameter(blockOnEdge, function.at(parameter).type);
            function.edit(added, ValueEdit::Type).setExpected(function.at(parameter).expected());
            parameters.append(added);
        }
        builder.redirectEdge(edge, blockOnEdge, WTF::move(next.arguments));
        builder.redirectEdge(Edge { blockOnEdge, 0 }, next.block, WTF::move(parameters));
    }

    void adoptExistingProofs()
    {
        adoptedProofs.reserveInitialCapacity(plan.proofs.size());
        for (const ExistingProof& proof : plan.proofs) {
            if (proof.source == ProofSource::GuardValue || proof.source == ProofSource::Allocation) {
                adoptedProofs.append(proof.value);
                continue;
            }
            BlockID targetBefore = targetOf(function, proof.edge).block;
            adoptedProofs.append(ensureEdgeResult(builder, analyses, proof.edge, testedOperandIndex));
            BlockID target = targetOf(function, proof.edge).block;
            if (target != targetBefore)
                splitEdges.append({ proof.edge, target });
        }
    }

    void collectCuts()
    {
        auto standsBeforeUse = [&](const PlannedTest& test) { return test.position == PlannedTestPosition::BeforeUse && plan.isGuarded(test.stretch); };
        auto addCut = [&](ProgramPoint point) { cuts.append(Cut { point, BlockID(), BlockID() }); };
        for (const PlannedTest& test : plan.tests) {
            if (standsBeforeUse(test))
                addCut(test.point);
        }
        // A copy that holds its stretches alone gets to no place where it could come back. Where tests stand at such a place, they still get a block of their own, which the copy of the range in front of them leads to.
        bool goesOnBehindStretches = goesOnBehindSomeStretch();
        Vector<PointKey, 8> pointsOfTests;
        if (!goesOnBehindStretches) {
            for (const Cut& cut : cuts)
                pointsOfTests.append(cut.key());
            AOT::sort(pointsOfTests);
        }
        for (const StretchRange& range : clonedRanges()) {
            if (range.begin)
                addCut({ range.block, range.begin });
            if (range.end < function.at(range.block).values.size())
                addCut({ range.block, range.end });
        }

        // No copy gets to any other place where it could come back.
        BitVector isBehindClonedRange = findBlocksBehindRanges(function, plan.ranges.span(), [&](const StretchRange& range) { return shouldClone(range); });
        auto keepRejoinPoints = [&](const Vector<ProgramPoint>& planned, Vector<ProgramPoint>& kept) {
            for (ProgramPoint point : planned) {
                if (!isBehindClonedRange.get(point.block.index()) || (!goesOnBehindStretches && !std::ranges::binary_search(pointsOfTests, PointKey { point.block, point.index })))
                    continue;
                kept.append(point);
                if (point.index)
                    addCut(point);
            }
        };
        keepRejoinPoints(plan.rejoinPoints, rejoinPoints);
        keepRejoinPoints(plan.rejoinPointsWithoutTests, rejoinPointsWithoutTests);

        AOT::sort(cuts, { }, &Cut::key);
        removeRepeatedElements(cuts, [](const Cut& a, const Cut& b) { return a.point == b.point; });
        for (const PlannedTest& test : plan.tests) {
            if (standsBeforeUse(test))
                findCut(test.point)->hasTest = true;
        }
    }

    void splitBlocks()
    {
        Vector<uint32_t, 8> indexes;
        for (size_t next = 0; next < cuts.size();) {
            BlockID block = cuts[next].point.block;
            indexes.shrink(0);
            for (size_t i = next; i < cuts.size() && cuts[i].point.block == block; ++i)
                indexes.append(cuts[i].point.index);

            builder.setOrigin(function.at(function.at(block).values[indexes.first()]).origin());
            BlockID previous = block;
            for (BlockID part : builder.splitBlock(block, indexes.span())) {
                Cut& cut = cuts[next++];
                cut.blockBefore = previous;
                cut.blockAfter = part;
                previous = part;
            }
        }
    }

    Cut* findCut(ProgramPoint point)
    {
        auto found = std::ranges::lower_bound(cuts, PointKey { point.block, point.index }, { }, &Cut::key);
        return found != cuts.end() && found->point == point ? &*found : nullptr;
    }

    const Cut* findLastCut(BlockID block, uint32_t index = std::numeric_limits<uint32_t>::max()) const
    {
        auto next = std::ranges::upper_bound(cuts, PointKey { block, index }, { }, &Cut::key);
        return next != cuts.begin() && std::prev(next)->point.block == block ? &*std::prev(next) : nullptr;
    }

    BlockID lastPartOf(BlockID block) const
    {
        const Cut* last = findLastCut(block);
        return last ? last->blockAfter : block;
    }

    Edge behindCuts(Edge edge) const { return { lastPartOf(edge.from), edge.targetIndex }; }

    // The copy comes back in front of the tests that stand at the point, which get a block of their own.
    void findRejoinBlocks(std::span<const ProgramPoint> points, BitVector& rejoinBlocks)
    {
        for (ProgramPoint point : points) {
            if (!point.index) {
                rejoinBlocks.set(point.block.index());
                continue;
            }
            Cut* cut = findCut(point);
            RELEASE_ASSERT(cut);
            if (cut->hasTest) {
                builder.setOrigin(function.at(cut->blockBefore).terminator.origin());
                cut->blockBefore = builder.splitEdge(Edge { cut->blockBefore, 0 });
            }
            rejoinBlocks.set((cut->hasTest ? cut->blockBefore : cut->blockAfter).index());
        }
    }

    BlockID blockOf(const StretchRange& range)
    {
        if (range.begin == range.end)
            return range.block;
        const Cut* cut = findCut({ range.block, range.begin });
        RELEASE_ASSERT(cut || !range.begin);
        return cut ? cut->blockAfter : range.block;
    }

    BlockID guardedBlockOf(const PlannedTest& test)
    {
        if (test.position == PlannedTestPosition::OnEdge)
            return function.at(ensureSplit(test.edge)).terminator.next().block;
        const Cut* cut = findCut(test.point);
        RELEASE_ASSERT(cut);
        return cut->blockAfter;
    }

    void forEachEntryOfCopy(NOESCAPE const Invocable<void(unsigned stretch, BlockID)> auto& functor)
    {
        BitVector isEntry(function.blocks.size());
        for (const PlannedTest& test : plan.tests) {
            if (BlockID entry = isPlaced(test.stretch) ? guardedBlockOf(test) : BlockID(); entry && isEntry.add(entry.index()))
                functor(test.stretch, entry);
        }
        for (const PlannedSlowPath& slowPath : plan.slowPaths) {
            if (BlockID entry = targetOf(function, behindCuts(slowPath.edge)).block; isPlaced(slowPath.stretch) && isEntry.add(entry.index()))
                functor(slowPath.stretch, entry);
        }
    }

    void checkStretches()
    {
        struct StretchRegion {
            Vector<BlockID, 8> blocks;
            Vector<BlockID, 2> entries;
        };
        Vector<StretchRegion> regions(plan.stretches.size());
        for (const StretchRange& range : clonedRanges())
            regions[range.stretch].blocks.append(blockOf(range));
        if (!splitEdges.isEmpty()) {
            using BlockAndStretch = std::pair<uint32_t, unsigned>;
            Vector<BlockAndStretch> stretchesByBlock;
            for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
                for (BlockID block : regions[stretch].blocks)
                    stretchesByBlock.append({ block.index(), stretch });
            }
            AOT::sort(stretchesByBlock);
            removeRepeatedElements(stretchesByBlock);
            auto stretchesOf = [&](BlockID block) { return std::ranges::equal_range(stretchesByBlock, block.index(), { }, &BlockAndStretch::first); };
            for (auto [edge, blockOnEdge] : splitEdges) {
                auto stretchesOfTarget = stretchesOf(function.at(blockOnEdge).terminator.next().block);
                if (stretchesOfTarget.empty())
                    continue;
                for (auto [block, stretch] : stretchesOf(lastPartOf(edge.from))) {
                    if (std::ranges::binary_search(stretchesOfTarget, stretch, { }, &BlockAndStretch::second))
                        regions[stretch].blocks.append(blockOnEdge);
                }
            }
        }
        forEachEntryOfCopy([&](unsigned stretch, BlockID entry) { regions[stretch].entries.append(entry); });

        // A copy that goes on behind a stretch asks what is live in the rest of the function. One liveness serves every stretch and the copy of them all, which may hold any block on a split edge.
        bool goesOnBehindStretches = goesOnBehindSomeStretch();
        std::unique_ptr<Liveness> livenessInsideStretches;
        auto liveness = [&]() -> const Liveness& {
            if (goesOnBehindStretches)
                return analyses.liveness();
            if (!livenessInsideStretches) {
                Vector<BlockID, 16> blocks;
                Vector<BlockID, 16> entries;
                for (const StretchRegion& region : regions) {
                    blocks.appendVector(region.blocks);
                    entries.appendVector(region.entries);
                }
                for (auto [edge, blockOnEdge] : splitEdges)
                    blocks.append(blockOnEdge);
                livenessInsideStretches = RegionCloner::computeLivenessInside(analyses, blocks.span(), entries.span());
            }
            return *livenessInsideStretches;
        };
        for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
            const StretchRegion& region = regions[stretch];
            if (region.blocks.isEmpty())
                continue;
            CloneCheck check = RegionCloner::checkCanDuplicate(analyses, region.blocks.span(), region.entries.span(), liveness);
            if (check != CloneCheck::CanClone && check != CloneCheck::FunctionMayNotBeCloned)
                unclonableStretches.set(stretch);
        }
        extendCopiesBehindStretches(liveness);
        if (unclonableStretches.isEmpty())
            return;

        BitVector isDeclined(plan.stretches.size());
        for (const PlannedUse& use : plan.uses) {
            if (unclonableStretches.get(use.stretch) && isDeclined.add(use.stretch))
                context.addDecline(Wire::DeclineKind::StretchNotGuarded, plan.sites.sites[use.site].use);
        }
    }

    // Where a test fails, the copy goes on behind the stretch, so nothing joins the code behind the test that passed until that code has no proof left to lose.
    void extendCopiesBehindStretches(const RegionCloner::LivenessSource& liveness)
    {
        Vector<BlockID, 8> blocksOfStretches;
        for (const StretchRange& range : clonedRanges())
            blocksOfStretches.append(blockOf(range));
        if (blocksOfStretches.isEmpty())
            return;

        for (PreparedLoop& loop : preparedLoops) {
            loop.copyComesBack = canComeBackInto(loop);
            if (!loop.copyComesBack)
                continue;
            for (BlockID block : loop.blocksOnBackEdges)
                isOnWayBackIntoLoop.set(block.index());
        }
        extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);

        // A copy that comes back nowhere holds the blocks on the edges that go around its loops.
        if (extentOfCopy != GenericCopy::Extent::ToWaysBack && !isOnWayBackIntoLoop.isEmpty()) {
            for (PreparedLoop& loop : preparedLoops)
                loop.copyComesBack = false;
            isOnWayBackIntoLoop.clearAll();
            extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);
        }
        if (copy->wasCutShortByLimit())
            context.gaveUp(LimitID::MaxBlocksCopiedBehindStretches);
        if (extentOfCopy != GenericCopy::Extent::NotCopied)
            return;

        copy.reset();
        for (const StretchRange& range : clonedRanges())
            unclonableStretches.set(range.stretch);
    }

    // A proof adopted on an edge is a parameter that the copy has nothing to pass for.
    bool canComeBackInto(const PreparedLoop& loop) const
    {
        return isPlaced(loop.planned->stretch) && function.at(loop.blockOnEnteringEdge).parameters.size() == function.at(loop.planned->head).parameters.size()
            && std::ranges::all_of(loop.blocksOnBackEdges, [&](BlockID block) { return function.at(block).parameters.isEmpty(); });
    }

    GenericCopy::Extent planGenericCopy(std::span<const BlockID> blocksOfStretches, const RegionCloner::LivenessSource& liveness)
    {
        copy.emplace(context, builder);
        copy->leaveAtRarerRejoinBlocks();
        copy->testOnlyWaysBackIntoLoops();

        BitVector isInRegion(function.blocks.size());
        for (BlockID block : blocksOfStretches) {
            isInRegion.quickSet(block.index());
            copy->addCopiedBlock(block);
        }
        // Left out, the block on a split edge would lead the generic copy through the tests on the edge into code that relies on proofs the copy does not have.
        for (auto [edge, blockOnEdge] : splitEdges) {
            if (!isOnWayBackIntoLoop.get(blockOnEdge.index()) && isInRegion.quickGet(lastPartOf(edge.from).index()) && isInRegion.quickGet(function.at(blockOnEdge).terminator.next().block.index()))
                copy->addCopiedBlock(blockOnEdge);
        }
        bool goesOnBehindStretches = false;
        for (const StretchRange& range : plan.ranges) {
            if (!goesOnBehindStretch(range))
                continue;
            copy->addStart(blockOf(range));
            goesOnBehindStretches = true;
        }
        forEachEntryOfCopy([&](unsigned, BlockID entry) { copy->addEntry(entry); });
        for (const PlannedUse& use : plan.uses) {
            if (isPlaced(use.stretch))
                copy->addValueToBeNarrowed(plan.sites.sites[use.site].use);
        }
        // A copy that holds its stretches alone leaves where they end: it looks for no place to come back at.
        if (!goesOnBehindStretches)
            return copy->plan(context.limit(LimitID::MaxBlocksCopiedBehindStretches), liveness);

        for (size_t index : isRejoinBlock)
            copy->addRejoinBlock(BlockID(index));
        for (size_t index : isOnWayBackIntoLoop)
            copy->addRejoinBlock(BlockID(index));
        for (size_t index : isRejoinBlockWithoutTests)
            copy->addRejoinBlockWithoutTests(BlockID(index));

        for (const PlannedUse& use : plan.uses) {
            const TestSite& site = plan.sites.sites[use.site];
            if (isPlaced(use.stretch) && !expectedTypeTellsResultOf(context, site))
                copy->addValueNarrowedInUnknownWay(site.use);
        }
        for (ValueID subject : collectSubjectsKnownBeyondTheirTypes(context, plan))
            copy->addValueKnownBeyondItsType(subject);
        return copy->plan(context.limit(LimitID::MaxBlocksCopiedBehindStretches));
    }

    void cloneGenericCopies()
    {
        if (!copy)
            return;
        // In a checked function of the program the type that a test expects is what the checker says. Elsewhere it is a guess, and the copy runs.
        copy->clone(expectsCheckedTypes ? +[](BlockFrequency) { return BlockFrequency::Rare; } : frequencyOfGenericCopy);

        RELEASE_ASSERT(extentOfCopy != GenericCopy::Extent::NotCopied);
        if (extentOfCopy == GenericCopy::Extent::ToWaysBack)
            context.build.statistics().count(StatisticID::StretchCopiesToWaysBack);
        else if (extentOfCopy == GenericCopy::Extent::ToEndOfFunction)
            context.build.statistics().count(StatisticID::StretchCopiesToEndOfFunction);

        for (const PreparedLoop& loop : preparedLoops) {
            if (loop.copyComesBack)
                addWayBackIntoLoop(loop);
        }
    }

    // Every proof that the head of the loop relies on is made again on the way back in: here, or by the tests on the entering edge that the way back joins.
    void addWayBackIntoLoop(const PreparedLoop& loop)
    {
        const PlannedWayBackIntoLoop& planned = *loop.planned;
        BlockID headOfCopy = copy->copyOf(planned.head);
        RELEASE_ASSERT(headOfCopy);

        // A test of a value that has a copy would be about another value than the test it repeats.
        bool canRepeatTests = std::ranges::none_of(planned.tests, [&](const PlannedTest& test) {
            return std::ranges::any_of(test.operands, [&](ValueID operand) { return !!copy->copyOf(operand); });
        });

        builder.setHandler(std::nullopt);
        for (uint32_t index = 0; index < planned.backEdges.size(); ++index) {
            BlockID block = loop.blocksOnBackEdges[index];
            Target next = function.at(block).terminator.next();
            RELEASE_ASSERT(next.block == planned.head);

            // The tested code goes around the loop past the block, so whatever enters the block comes out of the copy.
            builder.redirectEdge(behindCuts(planned.backEdges[index]), planned.head, Arguments(next.arguments));

            BlockID copyOfBlock = copy->copyOf(block);
            Target nextInCopy = copyOfBlock ? Target { copyOfBlock, { }, { } } : Target { headOfCopy, next.arguments, { } };

            builder.setOrigin(function.at(block).terminator.origin());
            Block& onBackEdge = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
            onBackEdge.frequency = function.at(headOfCopy).frequency;
            onBackEdge.terminator = Terminator { };
            builder.moveToEndOf(block);
            if (!canRepeatTests) {
                builder.emitJump(nextInCopy.block, WTF::move(nextInCopy.arguments));
                continue;
            }
            auto goOnWherePassed = [&](ValueID test) {
                BlockFrequency frequency = function.at(builder.currentBlock()).frequency;
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
                builder.moveToEndOf(branch.notTaken);
                builder.emitJump(nextInCopy.block, Arguments(nextInCopy.arguments));
                builder.moveToEndOf(branch.taken);
                return branch.provenOnTaken;
            };
            for (const PlannedTest& test : planned.tests) {
                builder.setOrigin(function.at(useOf(test)).origin());
                addDefinition(test.condition, goOnWherePassed(builder.append(createTest(test))));
            }
            for (uint32_t parameterIndex = 0; parameterIndex < planned.typesOfCounters.size(); ++parameterIndex) {
                if (TypeID type = planned.typesOfCounters[parameterIndex])
                    next.arguments[parameterIndex] = goOnWherePassed(builder.appendTypeTest(next.arguments[parameterIndex], type));
            }
            builder.emitJump(loop.blockOnEnteringEdge, WTF::move(next.arguments));
        }
    }

    void redirectSlowPaths()
    {
        for (const PlannedSlowPath& slowPath : plan.slowPaths) {
            if (!isPlaced(slowPath.stretch))
                continue;
            Edge edge = behindCuts(slowPath.edge);
            Target target = targetOf(function, edge);
            target.block = copy->copyOf(target.block);
            RELEASE_ASSERT(target.block);
            builder.redirectEdge(edge, WTF::move(target));
        }
    }

    void defineAdoptedProofs()
    {
        if (adoptedProofs.isEmpty())
            return;
        // An insertion ends the life of the use lists.
        const UseLists& useLists = analyses.useLists();
        Vector<ProgramPoint> pointsAfter(adoptedProofs.size(), [&](size_t index) { return useLists.pointAfter(adoptedProofs[index]); });

        for (uint32_t index = 0; index < adoptedProofs.size(); ++index) {
            TestConditionID condition = plan.proofs[index].condition;
            ValueID definition = adoptedProofs[index];
            if (definitions[condition.index()].contains(definition))
                continue;
            if (isDefinition.get(definition.index())) {
                builder.setOrigin(function.at(definition).origin());
                definition = builder.insertBefore(pointsAfter[index], IRBuilder::make(Opcode::AllGuards, { definition }));
            }
            addDefinition(condition, definition);
        }
        builder.applyInsertions();
    }

    void emitTests()
    {
        defineAdoptedProofs();

        using TestsAtOnePosition = Vector<uint32_t, 4>;
        Vector<TestsAtOnePosition, 8> waiting;
        for (uint32_t index = 0; index < plan.tests.size(); ++index) {
            const PlannedTest& test = plan.tests[index];
            if (!isPlaced(test.stretch))
                continue;
            const PlannedTest* previous = waiting.isEmpty() ? nullptr : &plan.tests[waiting.last().last()];
            if (!previous || previous->position != test.position || previous->point != test.point || previous->edge != test.edge)
                waiting.append(TestsAtOnePosition { });
            waiting.last().append(index);
        }

        // Where a slow path comes into a loop behind its head, the order of the blocks has the head behind the body, which rests on the tests of the head.
        // Every way through the tested code makes a proof in front of the tests that name it, so the first of the waiting tests on such a way waits for none of the others: each turn emits some.
        while (!waiting.isEmpty()) {
            unsigned numberOfPositionsEmitted = waiting.removeAllMatching([&](const TestsAtOnePosition& tests) {
                if (!hasProofsNamedBy(tests.span()))
                    return false;
                emitTestsAtOnePosition(tests.span());
                return true;
            });
            RELEASE_ASSERT(numberOfPositionsEmitted);
        }
    }

    bool hasProofsNamedBy(std::span<const uint32_t> testsAtOnePosition) const
    {
        bool hasProofs = true;
        for (size_t index = 0; index < testsAtOnePosition.size(); ++index) {
            forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, plan.tests[testsAtOnePosition[index]].requirement, [&](const TestPrerequisite& prerequisite) {
                hasProofs = hasProofs && (!definitions[prerequisite.condition.index()].isEmpty() || std::ranges::any_of(testsAtOnePosition.first(index), [&](uint32_t earlier) {
                    return plan.tests[earlier].condition == prerequisite.condition;
                }));
            });
        }
        return hasProofs;
    }

    void emitTestsAtOnePosition(std::span<const uint32_t> plannedTests)
    {
        RELEASE_ASSERT(copy);
        const PlannedTest& first = plan.tests[plannedTests.front()];
        // Then that block takes the proof of the last test itself.
        bool isOnlyWayToPassedTarget = first.position == PlannedTestPosition::BeforeUse;
        BlockID block;
        Target passedTarget;
        if (isOnlyWayToPassedTarget) {
            const Cut* cut = findCut(first.point);
            RELEASE_ASSERT(cut);
            block = cut->blockBefore;
            passedTarget.block = cut->blockAfter;
        } else {
            block = ensureSplit(first.edge);
            passedTarget = function.at(block).terminator.next();
        }
        Target failedTarget { copy->copyOf(passedTarget.block), passedTarget.arguments, { } };
        BlockFrequency frequency = function.at(passedTarget.block).frequency;
        RELEASE_ASSERT(failedTarget.block);

        Terminator& jump = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        RELEASE_ASSERT(jump.kind == TerminatorKind::Jump && jump.next().block == passedTarget.block);
        jump = Terminator { };

        builder.setHandler(std::nullopt);
        builder.moveToEndOf(block);
        builder.emitFailureOfPredictions(Target(failedTarget));
        for (const uint32_t& plannedTest : plannedTests) {
            const PlannedTest& planned = plan.tests[plannedTest];
            builder.setOrigin(function.at(useOf(planned)).origin());
            ValueID test = builder.append(createTest(planned));
            BlockID blockOfTest = builder.currentBlock();

            // No block is made that only jumps: SimplifyControlFlow would merge it away in the same round, and every pass in between would walk it.
            bool provesNothingWhenFailed = provesForStretch(provesOf(function.at(test).opcode));
            bool entersPassedTarget = &plannedTest == &plannedTests.back() && isOnlyWayToPassedTarget;
            IRBuilder::ProvenBranchTargets targets;
            if (entersPassedTarget)
                targets.enteredOnlyWhenTaken = passedTarget.block;
            if (provesNothingWhenFailed)
                targets.whenNotTaken = failedTarget;
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequencyOfGenericCopy(frequency), WTF::move(targets));
            if (!provesNothingWhenFailed) {
                builder.moveToEndOf(branch.notTaken);
                builder.emitJump(failedTarget.block, Arguments(failedTarget.arguments));
            }
            if (!entersPassedTarget)
                builder.moveToEndOf(branch.taken);

            emittedTests.append(EmittedTest { plannedTest, blockOfTest, branch.taken, branch.notTaken, branch.provenOnTaken, branch.provenOnNotTaken });
            addDefinition(planned.condition, branch.provenOnTaken);
        }
        if (!isOnlyWayToPassedTarget)
            builder.emitJump(passedTarget.block, WTF::move(passedTarget.arguments));
    }

    // The test names the proofs of its site that it rests on: what has passed an earlier test in place of an operand. Answers those that it needs as guards.
    Vector<ValueID, 4> namePrerequisites(Value& test, uint32_t requirement, NOESCAPE const Invocable<ValueID(Value&&)> auto& emit)
    {
        Vector<ValueID, 4> guards;
        forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
            ValueID proof = representativeOf(prerequisite.condition);
            if (!prerequisite.narrowedOperand) {
                guards.appendIfNotContains(proof);
                return;
            }
            if (std::optional<TypeID> castType = findTypeOfOperand(test, *prerequisite.narrowedOperand, proof))
                proof = emit(makeAssumeType(proof, *castType));
            test.setOperand(*prerequisite.narrowedOperand, proof);
        });

        // The site was planned with the types of its own operands. The value that stands for an operand here may be the one in front of a cast, of which less is known.
        if (Value::opcodesWithKeyIsIntegral.contains(test.opcode) && test.keyIsIntegral()) {
            ValueID key = OperandPosition::property(test.opcode) ? test.propertyOperand() : test.positionOperand();
            if (!context.program.typeOf(function, key).hasOnlyPrimitives(Primitive::NumberWithoutFraction))
                test.setKeyIsIntegral(false);
        }
        return guards;
    }

    Value createTest(const PlannedTest& planned)
    {
        RELEASE_ASSERT(planned.condition == plan.requirements[planned.requirement].condition);
        Value test = plan.sites.tests[plan.requirements[planned.requirement].test];
        std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);

        if (planned.position == PlannedTestPosition::OnEdge) {
            uint32_t numberOfHoistedOperands = 0;
            for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
                if (guardIndex != operandIndex)
                    test.setOperand(operandIndex, planned.operands[numberOfHoistedOperands++]);
            }
        }
        Vector<ValueID, 4> guards = namePrerequisites(test, planned.requirement, [&](Value&& cast) { return builder.append(WTF::move(cast)); });
        if (!guards.isEmpty())
            test.setOperand(*guardIndex, builder.appendAllGuards(guards.span()));
        return test;
    }

    // An operand that has to be an integer was one at the site: by its own type and the test that it passed. The value that passed the test may be the one in front of a cast.
    std::optional<TypeID> findTypeOfOperand(const Value& test, uint32_t operandIndex, ValueID passed) const
    {
        const Type& passedType = types.at(function.at(passed).type);
        const Type& operandType = types.at(function.at(test.operand(operandIndex)).type);
        bool hasToBeInteger = test.operandKind(operandIndex) == OperandKind::Integer;
        if (!hasToBeInteger && operandIndex != testedOperandIndex)
            return std::nullopt;
        if (hasToBeInteger ? passedType.hasOnlyPrimitives(Primitive::Integer) : passedType.isSubsetOf(operandType, context.program))
            return std::nullopt;
        Type both = operandType.intersectWith(passedType, types);
        if (both.isNever() || both == passedType || (hasToBeInteger && !both.hasOnlyPrimitives(Primitive::Integer)))
            return std::nullopt;
        return types.intern(both);
    }

    void addDefinition(TestConditionID condition, ValueID proof)
    {
        Definitions& proofs = definitions[condition.index()];
        if (proofs.contains(proof))
            return;
        if (isDefinition.get(proof.index()))
            proof = builder.append(Opcode::AllGuards, { proof });
        isDefinition.set(proof.index());
        proofs.append(proof);
    }

    ValueID representativeOf(TestConditionID condition) const
    {
        RELEASE_ASSERT(!definitions[condition.index()].isEmpty());
        return definitions[condition.index()].first();
    }

    // SSARepair redirects every use of a definition, and an adopted proof has other uses: an allocation is also the object.
    void wrapAdoptedDefinitions()
    {
        if (adoptedProofs.isEmpty())
            return;
        const UseLists& useLists = analyses.useLists();
        BitVector isAdopted(useLists.numberOfValues());
        for (ValueID proof : adoptedProofs) {
            RELEASE_ASSERT(proof.index() < useLists.numberOfValues());
            isAdopted.quickSet(proof.index());
        }
        Vector<std::pair<ValueID*, ProgramPoint>, 4> adoptedDefinitions;
        for (size_t condition : plan.conditions->stretchConditions()) {
            for (ValueID& proof : definitions[condition]) {
                if (definitions[condition].size() >= 2 && isAdopted.get(proof.index()))
                    adoptedDefinitions.append({ &proof, useLists.pointAfter(proof) });
            }
        }
        if (adoptedDefinitions.isEmpty())
            return;

        Vector<ValueID> wrappers(function.values.size());
        for (auto [definition, pointAfter] : adoptedDefinitions) {
            RELEASE_ASSERT(!wrappers[definition->index()]);
            builder.setOrigin(function.at(*definition).origin());
            ValueID wrapper = builder.insertBefore(pointAfter, IRBuilder::make(Opcode::AllGuards, { *definition }));
            wrappers[definition->index()] = wrapper;
            *definition = wrapper;
            isDefinition.set(wrapper.index());
        }
        auto redirectGuardOperandsOf = [&](ValueID user) {
            Value& value = function.edit(user, ValueEdit::Definition);
            value.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Guard && operand.index() < wrappers.size() && wrappers[operand.index()])
                    operand = wrappers[operand.index()];
            });
        };
        for (const EmittedTest& emitted : emittedTests) {
            ValueID test = function.at(emitted.block).terminator.conditionOperand();
            const Value& value = function.at(test);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Guard && !isDefinition.get(operand.index()))
                    redirectGuardOperandsOf(operand);
            });
            redirectGuardOperandsOf(test);
        }
    }

    void rewriteUses()
    {
        // Where a use stands now follows from where it stood and from the cuts: the lists of all uses of the function are not made for that.
        Vector<ProgramPoint> usePoints(plan.uses.size(), [&](size_t index) {
            const TestSite& site = plan.sites.sites[plan.uses[index].site];
            const Cut* cut = findLastCut(site.point.block, site.point.index);
            ProgramPoint point = cut ? ProgramPoint { cut->blockAfter, site.point.index - cut->point.index } : site.point;
            const BlockValues& values = function.at(point.block).values;
            if (point.index < values.size() && values[point.index] == site.use)
                return point;
            // Something was put in front of it.
            ValueLocation location = analyses.useLists().locationOf(site.use);
            RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
            return ProgramPoint { location.block, location.index };
        });
        wrapAdoptedDefinitions();

        Vector<std::pair<ValueID, TypeID>, 4> typesOfRepresentatives;
        for (const Definitions& proofs : definitions) {
            if (proofs.size() < 2)
                continue;
            TypeID typeOfFirst = function.at(proofs.first()).type;
            if (std::ranges::all_of(proofs, [&](ValueID proof) { return function.at(proof).type == typeOfFirst; }))
                continue;
            Type typeOfAll = types.at(typeOfFirst);
            for (ValueID proof : proofs.subspan(1))
                typeOfAll = typeOfAll.unionWith(types.at(function.at(proof).type), types);
            typesOfRepresentatives.append({ proofs.first(), typeOfFirst });
            function.edit(proofs.first(), ValueEdit::Type).type = types.intern(typeOfAll);
        }
        for (uint32_t useIndex = 0; useIndex < plan.uses.size(); ++useIndex) {
            if (isPlaced(plan.uses[useIndex].stretch))
                rewriteUse(plan.uses[useIndex], useIndex, usePoints[useIndex]);
        }
        for (auto [representative, type] : typesOfRepresentatives)
            function.edit(representative, ValueEdit::Type).type = type;
        builder.applyInsertions();
        // The uses and the guard operands of the tests were written past the builder.
        analyses.didChangeValues();
    }

    // One value stands for everything that has the identity of the tested value. The site was planned with the type of its own operand, which a branch or a cast may have narrowed in another way than the test.
    ValueID narrowToOperand(ValueID passed, ValueID operand, ProgramPoint point)
    {
        Type passedType = types.at(function.at(passed).type);
        Type operandType = types.at(function.at(operand).type);
        if (passedType.isSubsetOf(operandType, context.program))
            return passed;
        // An intersection names an object as its second type does: as the test did.
        Type both = operandType.intersectWith(passedType, types);
        if (both.isNever() || both == passedType)
            return passed;

        TypeID castType = types.intern(both);
        if (context.build.validates())
            builder.insertBefore(point, makeValidateType(passed, castType));
        return builder.insertBefore(point, makeAssumeType(passed, castType));
    }

    void rewriteUse(const PlannedUse& planned, uint32_t useIndex, ProgramPoint point)
    {
        const TestSite& site = plan.sites.sites[planned.site];
        auto insert = [&](Value&& value) { return builder.insertBefore(point, WTF::move(value)); };
        auto insertAllGuards = [&](std::span<const ValueID> guards) {
            Value allGuards = IRBuilder::make(Opcode::AllGuards);
            allGuards.setGuardsOperands(guards);
            return insert(WTF::move(allGuards));
        };

        builder.setOrigin(function.at(site.use).origin());
        for (const SubjectOperand& narrowed : std::ranges::equal_range(subjectOperands, useIndex, { }, &SubjectOperand::use)) {
            ValueID operand = function.at(site.use).operand(narrowed.operandIndex);
            ValueID replacement = narrowToOperand(representativeOf(plan.requirements[narrowed.requirement].condition), operand, point);
            if (context.build.validates() && replacement != operand)
                insert(IRBuilder::make(Opcode::ValidateSameValue, { replacement, operand }));
            function.edit(site.use, ValueEdit::Definition).setOperand(narrowed.operandIndex, replacement);
        }

        Vector<ValueID, 4> proofs;
        Vector<ValueID, 4> guards;
        for (const TestRequirement& requirement : plan.requirements.subspan(planned.firstRequirement, site.numberOfTests)) {
            proofs.append(representativeOf(requirement.condition));
            if (plan.conditions->at(requirement.condition).lastsForStretch())
                guards.appendIfNotContains(proofs.last());
        }
        ValueID guard;
        if (guards.size() == 1)
            guard = guards.first();
        else if (!guards.isEmpty())
            guard = insertAllGuards(guards.span());

        // One test stands for every use of its stretch. In a validating build each use makes the tests that last for a stretch again, and their answers are compared with the one it relies on.
        for (uint32_t test = 0; context.build.validates() && test < site.numberOfTests; ++test) {
            uint32_t requirement = planned.firstRequirement + test;
            if (!plan.conditions->at(plan.requirements[requirement].condition).lastsForStretch())
                continue;
            Value repeated = plan.sites.tests[plan.requirements[requirement].test];
            Vector<ValueID, 4> prerequisites = namePrerequisites(repeated, requirement, insert);
            if (!prerequisites.isEmpty())
                repeated.setGuardOperand(insertAllGuards(prerequisites.span()));
            Value expected = IRBuilder::make(Opcode::Boolean);
            expected.setBoolean(true);
            ValueID answer = insert(WTF::move(repeated));
            ValueID expectedAnswer = insert(WTF::move(expected));
            insert(IRBuilder::make(Opcode::ValidateSameValue, { answer, expectedAnswer }));
        }

        changedValues.append(site.use);
        TestSiteRewriter rewriter(TestSiteRewriter::Inputs { context, builder, site, plan.sites.testsOf(site), proofs.span(), guard, point, changedValues });

        // Until SSA is repaired a site behind the join names the value of the fast copy. Its rule has to see the type the site was planned with, which is the type of the join.
        TypeID type = function.at(site.use).type;
        TypeID expected = function.at(site.use).expected();
        ruleOf(site.rule).rewrite(rewriter);
        Value& use = function.edit(site.use, ValueEdit::Type);
        use.type = type;
        use.setExpected(expected);
    }

    void repairProofs()
    {
        SSARepair repair(builder, analyses);
        for (const Definitions& proofs : definitions) {
            if (proofs.size() < 2)
                continue;
            SSARepair::Variable variable = repair.addVariable(proofs.first());
            for (ValueID proof : proofs.subspan(1))
                repair.addDefinition(variable, proof);
        }
        // An adopted proof in a copied range stands for its condition at uses it does not reach, so one repair has to know it, its copy and the other proofs as one variable.
        if (copy)
            copy->addVariablesTo(repair);
        repair.run();
    }

    // The uses of everything that has the identity of a tested value are listed once for all tests.
    void replaceDominatedUses()
    {
        struct NarrowableUse {
            ValueID identity;
            ValueID alias;
            Use use;
            bool getsCast { false };
        };
        struct PlannedCast {
            Edge edge;
            ValueID proven;
            TypeID castType;
            const Use* use { nullptr };
        };
        struct Narrowing {
            TypeID replacedType;
            bool holdsProvenType { false };
            std::optional<TypeID> castType;
        };

        Vector<const EmittedTest*, 8> narrowingTests;
        for (const EmittedTest& emitted : emittedTests) {
            if (plan.conditions->at(plan.tests[emitted.plannedTest].condition).lastsForever())
                narrowingTests.append(&emitted);
        }
        BitVector isInUse;
        if (narrowingTests.isEmpty()) {
            leaveOutBlocksOfUnusedProofs(isInUse);
            return;
        }

        const CFG& cfg = analyses.cfg();
        auto edgeInto = [&](BlockID block) {
            std::optional<Edge> edge = cfg.singleIncomingEdge(block);
            RELEASE_ASSERT(edge);
            return *edge;
        };
        // Where the copies meet, the repair of SSA has given the test the join of what each of them passes on.
        auto subjectOf = [&](const EmittedTest& emitted) {
            ValueID test = function.at(edgeInto(emitted.passed).from).terminator.conditionOperand();
            return function.at(test).operand(testedOperandIndex);
        };

        BitVector isSubject(function.values.size());
        for (const EmittedTest* emitted : narrowingTests)
            isSubject.set(analyses.identityOf(subjectOf(*emitted)).index());
        Vector<NarrowableUse> uses;
        const ValueIdentities& identities = analyses.identities();
        const UseLists& useLists = analyses.useLists();
        for (size_t index = 0; index < function.values.size(); ++index) {
            ValueID alias(index);
            ValueID identity = identities.identityOf(alias);
            if (!isSubject.get(identity.index()))
                continue;
            for (const Use& use : useLists.usesOf(alias)) {
                if (acceptsNarrowedValue(function, use))
                    uses.append(NarrowableUse { identity, alias, use });
            }
        }
        AOT::stableSort(uses, { }, &NarrowableUse::identity);

        bool hasComparisons = false;
        auto replace = [&](const Use& use, ValueID replacement) {
            // In a validating build: what takes the place of a value is that value.
            bool isCompared = context.build.validates() && use.kind != Use::Kind::HandlerArgument && use.kind != Use::Kind::BlockSubject;
            if (ValueID replaced = isCompared ? usedValueOf(function, use) : ValueID(); replaced && context.program.at(function.at(replaced).type).isJavaScriptValue()) {
                ProgramPoint point = pointBeforeTerminatorOf(function, use.block);
                if (use.kind == Use::Kind::Operand)
                    point.index = analyses.useLists().locationOf(use.user).index;
                builder.insertBefore(point, IRBuilder::make(Opcode::ValidateSameValue, { replacement, replaced }));
                hasComparisons = true;
            }
            replaceUse(function, use, replacement);
            if (use.kind == Use::Kind::Operand)
                changedValues.append(use.user);
        };
        auto isReachedByCastIn = [&](BlockID target, const Use& use) {
            if (use.block != target)
                return analyses.dominators().terminatorDominates(target, use.block);
            return use.kind != Use::Kind::HandlerArgument && use.kind != Use::Kind::BlockSubject;
        };

        Vector<PlannedCast> plannedCasts;
        Vector<Narrowing, 4> narrowings;
        auto replaceUsesDominatedBy = [&](Edge edge, ValueID subject, ValueID proven) {
            TypeID provenTypeID = function.at(proven).type;
            Type provenType = types.at(provenTypeID);
            ProgramPoint pointOfTest = pointBeforeTerminatorOf(function, edge.from);
            BlockID target = targetOf(function, edge).block;
            narrowings.shrink(0);

            for (NarrowableUse& narrowable : std::ranges::equal_range(uses, analyses.identityOf(subject), { }, &NarrowableUse::identity)) {
                // A use that gets a cast will name none of the values that were listed.
                if (narrowable.alias == proven || narrowable.getsCast)
                    continue;
                const Use& use = narrowable.use;
                if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateSameValue)
                    continue;
                ValueID current = usedValueOf(function, use);
                if (current == proven || !analyses.dominates(edge, use.block))
                    continue;
                TypeID currentTypeID = function.at(current).type;
                if (currentTypeID == provenTypeID)
                    continue;
                size_t found = narrowings.findIf([&](const Narrowing& known) { return known.replacedType == currentTypeID; });
                if (found == notFound) {
                    found = narrowings.size();
                    narrowings.append(Narrowing { currentTypeID, provenType.isSubsetOf(types.at(currentTypeID), context.program), std::nullopt });
                }
                Narrowing& narrowing = narrowings[found];
                if (narrowing.holdsProvenType) {
                    replace(use, proven);
                    isInUse.set(proven.index());
                    continue;
                }
                // What a value defined behind the edge says holds from its definition on.
                if (!analyses.dominates(current, pointOfTest) || !isReachedByCastIn(target, use))
                    continue;
                // The type of the value may say what no test finds out, such as what flow proved about a class.
                if (!narrowing.castType) {
                    Type replacedType = types.at(currentTypeID);
                    Type both = provenType.intersectWith(replacedType, types);
                    bool saysMore = !both.isNever() && both != replacedType && both.isSubsetOf(replacedType, context.program);
                    narrowing.castType = saysMore ? types.intern(both) : TypeID();
                }
                if (*narrowing.castType) {
                    narrowable.getsCast = true;
                    plannedCasts.append(PlannedCast { edge, proven, *narrowing.castType, &use });
                    isInUse.set(proven.index());
                }
            }
        };
        for (const EmittedTest* emitted : narrowingTests) {
            ValueID subject = subjectOf(*emitted);
            replaceUsesDominatedBy(edgeInto(emitted->passed), subject, emitted->provenWhenPassed);
            if (emitted->provenWhenFailed)
                replaceUsesDominatedBy(edgeInto(emitted->failed), subject, emitted->provenWhenFailed);
        }

        Vector<std::pair<TypeID, ValueID>, 2> castsOfProvenValue;
        ValueID proven;
        for (const PlannedCast& planned : plannedCasts) {
            if (planned.proven != proven) {
                proven = planned.proven;
                castsOfProvenValue.shrink(0);
                builder.setOrigin(function.at(planned.edge.from).terminator.origin());
            }
            size_t found = castsOfProvenValue.findIf([&](const auto& known) { return known.first == planned.castType; });
            if (found == notFound) {
                found = castsOfProvenValue.size();
                BlockID target = targetOf(function, planned.edge).block;
                ProgramPoint head { target, indexAfterCaughtValues(function, function.at(target)) };
                if (context.build.validates())
                    builder.insertBefore(head, makeValidateType(proven, planned.castType));
                castsOfProvenValue.append({ planned.castType, builder.insertBefore(head, makeAssumeType(proven, planned.castType)) });
            }
            replace(*planned.use, castsOfProvenValue[found].second);
        }
        if (!plannedCasts.isEmpty() || hasComparisons)
            builder.applyInsertions();
        analyses.didChangeValues();
        leaveOutBlocksOfUnusedProofs(isInUse);
    }

    // What a test proves where it fails is a parameter of a block on the failing edge.
    void leaveOutBlocksOfUnusedProofs(const BitVector& isUsed)
    {
        bool changed = false;
        for (const EmittedTest& emitted : emittedTests) {
            if (!emitted.provenWhenFailed || isUsed.get(emitted.provenWhenFailed.index()))
                continue;
            // A rewritten use may have split the block of the test, so the branch is looked for from its target.
            std::optional<Edge> edge = analyses.cfg().singleIncomingEdge(emitted.failed);
            RELEASE_ASSERT(edge && function.at(emitted.failed).terminator.kind == TerminatorKind::Jump);
            // The repair of SSA may have led a value through the block, and a cast may stand in it.
            if (!function.at(emitted.failed).values.isEmpty() || function.at(emitted.failed).parameters.size() != 1)
                continue;
            Block& failed = function.edit(emitted.failed, { BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
            Target& notTaken = editTarget(function, *edge);
            notTaken = Target { failed.terminator.next().block, WTF::move(failed.terminator.next().arguments), { } };
            failed.parameters.clear();
            failed.terminator = Terminator { };
            changed = true;
        }
        if (changed)
            analyses.didChangeControlFlow();
    }
};

} // anonymous namespace

// What nobody could follow is in an expected type beside whatever else reached the place.
static TypeID narrowExpectedTypeByUses(FunctionPassContext& context, std::span<const ValueID> values, TypeID expected)
{
    const Type& expectedType = context.types.at(expected);
    if (maxNumberOfUnionParts(expectedType) < 2)
        return expected;
    UnionParts parts = unionPartsOf(expectedType);
    // Only a part that has objects can go, and never the last one. undefined | null stands beside every reference type.
    if (std::ranges::count_if(parts, [](const UnionPart& part) { return part.type.hasObjects(); }) < 2)
        return expected;

    // One access alone contradicts nothing.
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, parts);
    if (evidence.numberOfResolvableAccesses < 2)
        return expected;
    Type narrowed = Type::never();
    bool expectsObject = false;
    for (size_t part = 0; part < parts.size(); ++part) {
        bool servesEveryAccess = evidence.countOfPart[part] == evidence.numberOfResolvableAccesses;
        if (!parts[part].type.hasObjects() || servesEveryAccess)
            narrowed = narrowed.unionWith(parts[part].type, context.types);
        expectsObject |= parts[part].type.hasObjects() && servesEveryAccess;
    }
    return expectsObject ? context.types.intern(narrowed) : expected;
}

// Of a value that nobody could follow, its uses are all there is to go by. What is read or stored under a number, and asked for nothing that an array does not have, is expected to be an array.
static TypeID expectArrayByUses(FunctionPassContext& context, std::span<const ValueID> values)
{
    bool isAccessedByKey = std::ranges::any_of(values, [&](ValueID id) {
        return std::ranges::any_of(context.analyses.useLists().usesOf(id), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && elementAccessOf(context.function.at(use.user).opcode);
        });
    });
    if (!isAccessedByKey)
        return { };
    UnionParts arrayAlone = unionPartsOf(Type::builtinClass(BuiltinClasses::Array).withoutClassProofs());
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, arrayAlone);
    if (!evidence.numberOfResolvableElementAccesses || evidence.numberOfResolvableAccesses < evidence.numberOfAccesses)
        return { };
    return context.types.intern(arrayAlone[0].type);
}

// Every rule then tests a value for the same parts. A program reads `row.tags` again wherever it uses it: what is read from one object under one name is used as one value.
static void settleExpectedTypesByUses(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    struct PropertyRead {
        std::tuple<ValueID, PropertyKeyID, TypeID> place;
        ValueID read;
    };
    const Function& function = context.function;
    auto settle = [&](std::span<const ValueID> values) {
        TypeID expected = function.at(values[0]).expected();
        TypeID settled = expected ? narrowExpectedTypeByUses(context, values, expected) : expectArrayByUses(context, values);
        if (settled == expected)
            return;
        for (ValueID id : values)
            expectations.expect(id, settled);
    };

    Vector<PropertyRead> reads;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        const Value& value = function.at(id);
        if (value.expected() == value.type || (!value.expected() && !context.types.at(value.type).anyObject) || !context.analyses.useLists().useCount(id))
            continue;
        if (value.opcode == Opcode::PropGet)
            reads.append(PropertyRead { { value.objectOperand(), value.key(), value.expected() }, id });
        else
            settle(singleElementSpan(id));
    }

    AOT::sort(reads, { }, [](const PropertyRead& read) { return std::pair { read.place, read.read }; });
    Vector<ValueID, 4> readsOfOnePlace;
    for (size_t index = 0; index < reads.size(); ++index) {
        readsOfOnePlace.append(reads[index].read);
        if (index + 1 < reads.size() && reads[index + 1].place == reads[index].place)
            continue;
        settle(readsOfOnePlace.span());
        readsOfOnePlace.shrink(0);
    }
}

static PassOutcome placePlannedTests(FunctionPassContext& context, const ExpectedTypesByUse& expectations)
{
    TestPlacementPlan plan = planTestPlacement(context);
    if (!plan.hasGuardedStretch())
        return PassOutcome::Unchanged;

    TestPlacer placer { context, plan, expectations };
    PassOutcome outcome = placer.run();
    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::StretchesGuarded, placer.numberOfGuardedStretches());
    statistics.count(StatisticID::StretchesNotClonable, placer.unclonableStretches.bitCount());
    statistics.count(StatisticID::WaysBackFromStretchCopies, placer.copy ? placer.copy->numberOfWaysBack() : 0);
    statistics.count(StatisticID::TypeTestsPlaced, placer.emittedTests.size());
    return outcome;
}

static bool asksForElementOfArray(const FunctionPassContext& context, const Value& test)
{
    if (test.opcode == Opcode::HasArrayElement)
        return true;
    if (test.opcode != Opcode::ElemHas || !test.keyIsIntegral())
        return false;
    const ClassPart* receiver = context.program.typeOf(context.function, test.objectOperand()).singleBuiltinClass();
    return receiver && receiver->builtinClass == BuiltinClasses::Array;
}

// Arrays seldom have holes. The arm that a hole takes, in the loop of map() for one, is a slow path: what holds in front of the question holds behind it on the typed path, and the arm comes back through the tests.
// A new array takes what is stored into it but where it has left the storage that it was made with: the arm behind a store that failed gets no tests.
static PassOutcome markArmsThatAreSeldomTaken(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    const CFG& cfg = context.analyses.cfg();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = context.function.at(id);
        if (!block.terminator.isConditionalBranch() || cfg.incomingEdges(block.terminator.notTaken().block).size() != 1)
            continue;
        const Value& test = context.function.at(block.terminator.conditionOperand());
        const Block& arm = context.function.at(block.terminator.notTaken().block);
        BlockFrequency frequency = arm.frequency;
        if (test.opcode == Opcode::TryPutByValDirect)
            frequency = BlockFrequency::Rare;
        // An arm with code of its own is code of the program, which gets its tests as any other.
        else if (block.frequency == BlockFrequency::Normal && arm.frequency == BlockFrequency::Normal && arm.values.isEmpty() && arm.terminator.kind == TerminatorKind::Jump && asksForElementOfArray(context, test))
            frequency = BlockFrequency::Unlikely;
        if (frequency == arm.frequency)
            continue;
        context.function.edit(block.terminator.notTaken().block, BlockEdit::Rest).frequency = frequency;
        outcome = PassOutcome::Changed;
    }
    if (outcome == PassOutcome::Changed)
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
    return outcome;
}

PassOutcome placeTypeTests(FunctionPassContext& context)
{
    const Heat& heat = context.inputs.heat;
    bool hasUsesLeftByLastRound = std::exchange(context.hasUsesAwaitingExpansion, false);
    // An operator whose one operand has become an integer is worth a test of the other.
    hasUsesLeftByLastRound |= std::exchange(context.hasCountersConvertedSinceTestsWerePlaced, false);
    // What a test in a cold function buys, it buys at run time: the passes of the rounds gain little from it, and they would carry its arm and its join: a twentieth of the build of a very large program.
    auto placesTestsBehindRounds = [&] { return heat.isOfVeryLargeProgram() && isCold(heat.of(context.program, context.id)); };
    if (context.isBehindRounds ? !hasUsesLeftByLastRound && !placesTestsBehindRounds() : context.isInRounds && placesTestsBehindRounds())
        return PassOutcome::Unchanged;
    // No block runs more often than the function is entered unless it is in a cycle.
    if (heat.of(context.program, context.id) == HeatClass::RunsOnce && !context.analyses.cfg().hasCycle())
        return PassOutcome::Unchanged;
    // Where a test at the entrance has failed. A test in here would guess again what has just proved wrong, and cost a copy.
    if (context.function.runsOnlyBehindFailedTest())
        return PassOutcome::Unchanged;

    PassOutcome marked = markArmsThatAreSeldomTaken(context);
    ExpectedTypesByUse expectations(context.function);
    PassOutcome outcome = PassOutcome::Unchanged;
    if (context.build.recordedOptions().usesExpectedTypes()) {
        if (expectLayoutsByNames(context, expectations))
            outcome = PassOutcome::Changed;
        settleExpectedTypesByUses(context, expectations);
        if (!expectations.isEmpty())
            context.analyses.didChangeTypes();
    }
    outcome |= placePlannedTests(context, expectations);
    outcome |= addEmptySlotCases(context);
    if (outcome == PassOutcome::Unchanged && !expectations.isEmpty()) {
        expectations.takeBack();
        context.analyses.didChangeTypes();
    }
    // Whoever edits blocks, operands or edges here drops what that makes stale.
    context.keptAnalyses = outcome == PassOutcome::Unchanged ? allAnalyses - FunctionAnalysis::Effects : controlFlowAnalyses | analysesThatReadNoTypes;
    return outcome | marked;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
