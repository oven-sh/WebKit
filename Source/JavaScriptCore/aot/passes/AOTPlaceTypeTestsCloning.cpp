/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StretchRegion {
    Vector<BlockID, 8> blocks;
    Vector<BlockID, 2> entries;
};

} // anonymous namespace

static bool canCloneInsideFunction(CloneCheck check)
{
    switch (check) {
    case CloneCheck::CanClone:
    case CloneCheck::FunctionMayNotBeCloned:
        return true;
    case CloneCheck::Unknown:
    case CloneCheck::OperandMustBeDefinition:
    case CloneCheck::BoundToFrame:
    case CloneCheck::HandlerOutsideRegion:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestPlacer::collectSubjectOperands()
{
    const ValueIdentities& identities = m_context.analyses.identities();
    for (uint32_t useIndex = 0; useIndex < m_plan.uses.size(); ++useIndex) {
        const PlannedUse& planned = m_plan.uses[useIndex];
        const TestSite& site = m_plan.sites.sites[planned.site];
        const Value& use = m_function.at(site.use);
        use.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (use.operandKind(operandIndex) == OperandKind::Guard)
                return;
            ValueID subject = identities.identityOf(operand);
            for (uint32_t testIndex = site.numberOfTests; testIndex--;) {
                uint32_t requirement = planned.firstRequirement + testIndex;
                const TestCondition& condition = conditionOf(requirementAt(requirement).condition);
                if (!condition.lastsForever() || condition.subject != subject)
                    continue;
                m_subjectOperands.append(SubjectOperand { useIndex, operandIndex, requirement });
                break;
            }
        });
    }
}

void TestPlacer::splitTestedEdges()
{
    for (const PlannedTest& test : m_plan.tests) {
        if (test.position == PlannedTestPosition::OnEdge && m_plan.isGuarded(test.stretch))
            ensureSplit(test.edge);
    }
    prepareLoops();
}

BlockID TestPlacer::ensureSplit(Edge edge)
{
    for (const SplitEdge& split : m_splitEdges) {
        if (split.edge == edge)
            return split.block;
    }
    m_builder.setOrigin(m_function.at(edge.from).terminator.origin());
    BlockID block = m_builder.splitEdge(edge);
    m_splitEdges.append(SplitEdge { edge, block });
    return block;
}

// The generic copy of a turn of a loop leaves the copy on an edge that goes around the loop, where a block is put for it, and goes on from there to the block on an edge that enters the loop.
void TestPlacer::prepareLoops()
{
    for (uint32_t index = 0; index < m_plan.waysBackIntoLoops.size(); ++index) {
        const PlannedWayBackIntoLoop& planned = m_plan.waysBackIntoLoops[index];
        if (!m_plan.isGuarded(planned.stretch))
            continue;

        PreparedLoop loop;
        loop.plannedWayBack = index;
        loop.blockOnEnteringEdge = ensureSplit(planned.enteringEdge);
        passArgumentsThrough(planned.enteringEdge, loop.blockOnEnteringEdge);
        for (Edge edge : planned.backEdges)
            loop.blocksOnBackEdges.append(ensureSplit(edge));
        m_loops.append(WTF::move(loop));
    }
}

// What the block passes on becomes its parameters, so that another edge into it can pass other values.
void TestPlacer::passArgumentsThrough(Edge edge, BlockID blockOnEdge)
{
    Target next = m_function.at(blockOnEdge).terminator.next();
    RELEASE_ASSERT(m_function.at(blockOnEdge).parameters.isEmpty());

    m_builder.setOrigin(m_function.at(blockOnEdge).terminator.origin());
    Arguments parameters;
    for (ValueID parameter : Vector<ValueID, 4>(m_function.at(next.block).parameters.span())) {
        ValueID added = m_builder.addParameter(blockOnEdge, m_function.at(parameter).type);
        m_function.edit(added, ValueEdit::Type).setExpected(m_function.at(parameter).expected());
        parameters.append(added);
    }
    m_builder.redirectEdge(edge, blockOnEdge, WTF::move(next.arguments));
    m_builder.redirectEdge(Edge { blockOnEdge, 0 }, next.block, WTF::move(parameters));
}

BitVector TestPlacer::findBlocksBehindClonedRanges() const
{
    BitVector isBehind(m_function.blocks.size());
    Vector<BlockID, 16> worklist;
    auto add = [&](BlockID block) {
        if (isBehind.add(block.index()))
            worklist.append(block);
    };
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            add(range.block);
    }
    while (!worklist.isEmpty()) {
        m_function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            add(target.block);
        });
    }
    return isBehind;
}

void TestPlacer::collectCuts()
{
    auto addCut = [&](ProgramPoint point) {
        m_cuts.append(Cut { point, BlockID(), BlockID() });
    };
    for (const PlannedTest& test : m_plan.tests) {
        if (test.position == PlannedTestPosition::BeforeUse && m_plan.isGuarded(test.stretch))
            addCut(test.point);
    }
    size_t numberOfCutsForTests = m_cuts.size();
    for (const StretchRange& range : m_plan.ranges) {
        if (!shouldClone(range))
            continue;
        if (range.begin)
            addCut({ range.block, range.begin });
        if (range.end < m_function.at(range.block).values.size())
            addCut({ range.block, range.end });
    }

    // A copy that holds its stretches alone gets to no place where it could come back. Where tests stand at such a place, they still get a block of their own, which the copy of the range in front of them leads to.
    bool goesOnBehindStretches = std::ranges::any_of(m_plan.ranges, [&](const StretchRange& range) {
        return goesOnBehindStretch(range);
    });
    Vector<PointKey, 8> pointsOfTests;
    if (!goesOnBehindStretches) {
        for (const Cut& cut : m_cuts)
            pointsOfTests.append(cut.key());
        pointsOfTests.shrink(numberOfCutsForTests);
        AOT::sort(pointsOfTests);
    }
    auto mattersToCopy = [&](ProgramPoint point) {
        return goesOnBehindStretches || std::ranges::binary_search(pointsOfTests, PointKey { point.block, point.index });
    };

    // No copy gets to any other place where it could come back.
    BitVector isBehindClonedRange = findBlocksBehindClonedRanges();
    for (ProgramPoint point : m_plan.rejoinPoints) {
        if (!isBehindClonedRange.get(point.block.index()) || !mattersToCopy(point))
            continue;
        m_rejoinPoints.append(point);
        if (point.index)
            addCut(point);
    }
    for (ProgramPoint point : m_plan.rejoinPointsWithoutTests) {
        if (!isBehindClonedRange.get(point.block.index()) || !mattersToCopy(point))
            continue;
        m_rejoinPointsWithoutTests.append(point);
        if (point.index)
            addCut(point);
    }

    AOT::sort(m_cuts, { }, &Cut::key);
    removeRepeatedElements(m_cuts, [](const Cut& a, const Cut& b) {
        return a.point == b.point;
    });
    for (const PlannedTest& test : m_plan.tests) {
        if (test.position != PlannedTestPosition::BeforeUse || !m_plan.isGuarded(test.stretch))
            continue;
        findCut(test.point)->hasTest = true;
    }
}

void TestPlacer::splitBlocks()
{
    Vector<uint32_t, 8> indexes;
    for (size_t next = 0; next < m_cuts.size();) {
        BlockID block = m_cuts[next].point.block;
        indexes.shrink(0);
        for (size_t i = next; i < m_cuts.size() && m_cuts[i].point.block == block; ++i)
            indexes.append(m_cuts[i].point.index);

        m_builder.setOrigin(m_function.at(m_function.at(block).values[indexes.first()]).origin());
        BlockID previous = block;
        for (BlockID part : m_builder.splitBlock(block, indexes.span())) {
            Cut& cut = m_cuts[next++];
            cut.blockBefore = previous;
            cut.blockAfter = part;
            previous = part;
        }
    }
}

auto TestPlacer::findCut(ProgramPoint point) const -> const Cut*
{
    auto found = std::ranges::lower_bound(m_cuts, PointKey { point.block, point.index }, { }, &Cut::key);
    return found != m_cuts.end() && found->point == point ? &*found : nullptr;
}

auto TestPlacer::findCut(ProgramPoint point) -> Cut*
{
    return const_cast<Cut*>(std::as_const(*this).findCut(point));
}

// The copy comes back in front of the tests that stand at the point, which get a block of their own.
void TestPlacer::findRejoinBlocks()
{
    findRejoinBlocks(m_rejoinPoints.span(), m_isRejoinBlock);
    findRejoinBlocks(m_rejoinPointsWithoutTests.span(), m_isRejoinBlockWithoutTests);
}

void TestPlacer::findRejoinBlocks(std::span<const ProgramPoint> points, BitVector& isRejoinBlock)
{
    for (ProgramPoint point : points) {
        if (!point.index) {
            isRejoinBlock.set(point.block.index());
            continue;
        }
        Cut* cut = findCut(point);
        RELEASE_ASSERT(cut);
        if (!cut->hasTest) {
            isRejoinBlock.set(cut->blockAfter.index());
            continue;
        }
        m_builder.setOrigin(m_function.at(cut->blockBefore).terminator.origin());
        cut->blockBefore = m_builder.splitEdge(Edge { cut->blockBefore, 0 });
        isRejoinBlock.set(cut->blockBefore.index());
    }
}

BlockID TestPlacer::blockOf(const StretchRange& range) const
{
    if (range.begin == range.end)
        return range.block;
    const Cut* cut = findCut({ range.block, range.begin });
    RELEASE_ASSERT(cut || !range.begin);
    return cut ? cut->blockAfter : range.block;
}

BlockID TestPlacer::guardedBlockOf(const PlannedTest& test)
{
    switch (test.position) {
    case PlannedTestPosition::BeforeUse: {
        const Cut* cut = findCut(test.point);
        RELEASE_ASSERT(cut);
        return cut->blockAfter;
    }
    case PlannedTestPosition::OnEdge:
        return m_function.at(ensureSplit(test.edge)).terminator.next().block;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BlockID TestPlacer::lastPartOf(BlockID block) const
{
    auto next = std::ranges::upper_bound(m_cuts, PointKey { block, std::numeric_limits<uint32_t>::max() }, { }, &Cut::key);
    if (next == m_cuts.begin())
        return block;
    const Cut& last = *std::prev(next);
    return last.point.block == block ? last.blockAfter : block;
}

// A point of the plan, in the part of its block that holds it now.
ProgramPoint TestPlacer::pointBehindCuts(ProgramPoint point) const
{
    auto next = std::ranges::upper_bound(m_cuts, PointKey { point.block, point.index }, { }, &Cut::key);
    if (next == m_cuts.begin())
        return point;
    const Cut& last = *std::prev(next);
    return last.point.block == point.block ? ProgramPoint { last.blockAfter, point.index - last.point.index } : point;
}

Vector<BlockID, 8> TestPlacer::collectCopiedBlocks() const
{
    Vector<BlockID, 8> region;
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            region.append(blockOf(range));
    }
    return region;
}

// Left out, the block on a split edge would lead the generic copy through the tests on the edge into code that relies on proofs the copy does not have.
void TestPlacer::appendSplitEdgesInside(Vector<BlockID, 8>& region) const
{
    if (m_splitEdges.isEmpty() || region.isEmpty())
        return;

    BitVector isInRegion(m_function.blocks.size());
    for (BlockID block : region)
        isInRegion.quickSet(block.index());
    for (const SplitEdge& split : m_splitEdges) {
        if (m_isOnWayBackIntoLoop.get(split.block.index()))
            continue;
        BlockID target = m_function.at(split.block).terminator.next().block;
        if (isInRegion.quickGet(lastPartOf(split.edge.from).index()) && isInRegion.quickGet(target.index()))
            region.append(split.block);
    }
}

// A proof adopted on an edge is a parameter that the copy has nothing to pass for.
bool TestPlacer::canComeBackInto(const PreparedLoop& loop) const
{
    const PlannedWayBackIntoLoop& planned = plannedWayBackOf(loop);
    if (!isPlaced(planned.stretch))
        return false;
    if (m_function.at(loop.blockOnEnteringEdge).parameters.size() != m_function.at(planned.head).parameters.size())
        return false;
    return std::ranges::all_of(loop.blocksOnBackEdges, [&](BlockID block) {
        return m_function.at(block).parameters.isEmpty();
    });
}

// Every test of such a stretch looks at a value of which layouts are expected and nobody has checked that: nothing is known of it but the names it is asked for, or the function is not a checked function of the program.
BitVector TestPlacer::findStretchesThatGuessLayouts() const
{
    bool expectsCheckedTypes = m_context.function.has(FunctionFlags::IsChecked) && !isLibraryCode(m_context.program, m_context.function);
    BitVector stretches(m_plan.stretches.size());
    BitVector stretchesWithOtherTests(m_plan.stretches.size());
    for (const PlannedTest& test : m_plan.tests) {
        const Value& written = m_plan.sites.tests[requirementAt(test.requirement).test];
        ValueID tested = !test.operands.isEmpty() ? test.operands[testedOperandIndex] : written.numberOfOperands() ? written.operand(testedOperandIndex) : ValueID();
        bool isGuess = tested && m_expectations.isGuessedFromNames(tested);
        if (tested && !expectsCheckedTypes) {
            TypeID expected = m_function.at(tested).expected();
            isGuess |= expected && m_context.types.at(expected).layouts;
        }
        (isGuess ? stretches : stretchesWithOtherTests).quickSet(test.stretch);
    }
    stretches.exclude(stretchesWithOtherTests);
    return stretches;
}

// Behind the stretch the copy keeps apart what the code that passed the test has proven.
bool TestPlacer::goesOnBehindStretch(const StretchRange& range) const
{
    bool isColdWithoutLoopBehind = m_stretchesInFrontOfLoops && !m_stretchesInFrontOfLoops->quickGet(range.stretch);
    return shouldClone(range) && !m_stretchesThatGuessLayouts.quickGet(range.stretch) && !isColdWithoutLoopBehind;
}

GenericCopy::Extent TestPlacer::planGenericCopy(std::span<const BlockID> blocksOfStretches, const RegionCloner::LivenessSource& liveness)
{
    m_copy.emplace(m_context, m_builder);
    m_copy->leaveAtRarerRejoinBlocks();
    m_copy->testOnlyWaysBackIntoLoops();

    Vector<BlockID, 8> region(blocksOfStretches);
    appendSplitEdgesInside(region);
    for (BlockID block : region)
        m_copy->addCopiedBlock(block);
    bool goesOnBehindStretches = false;
    for (const StretchRange& range : m_plan.ranges) {
        if (!goesOnBehindStretch(range))
            continue;
        m_copy->addStart(blockOf(range));
        goesOnBehindStretches = true;
    }

    BitVector isEntry(m_function.blocks.size());
    auto addEntry = [&](BlockID block) {
        if (isEntry.add(block.index()))
            m_copy->addEntry(block);
    };
    for (const PlannedTest& test : m_plan.tests) {
        if (isPlaced(test.stretch))
            addEntry(guardedBlockOf(test));
    }
    for (const PlannedSlowPath& slowPath : m_plan.slowPaths) {
        if (isPlaced(slowPath.stretch))
            addEntry(targetOf(m_function, Edge { lastPartOf(slowPath.edge.from), slowPath.edge.targetIndex }).block);
    }

    for (const PlannedUse& use : m_plan.uses) {
        if (isPlaced(use.stretch))
            m_copy->addValueToBeNarrowed(m_plan.sites.sites[use.site].use);
    }
    // A copy that holds its stretches alone leaves where they end: it looks for no place to come back at.
    if (!goesOnBehindStretches)
        return m_copy->plan(m_context.limit(LimitID::MaxBlocksCopiedBehindStretches), liveness);

    for (size_t index : m_isRejoinBlock)
        m_copy->addRejoinBlock(BlockID(index));
    for (size_t index : m_isOnWayBackIntoLoop)
        m_copy->addRejoinBlock(BlockID(index));
    for (size_t index : m_isRejoinBlockWithoutTests)
        m_copy->addRejoinBlockWithoutTests(BlockID(index));

    for (const PlannedUse& use : m_plan.uses) {
        const TestSite& site = m_plan.sites.sites[use.site];
        if (isPlaced(use.stretch) && !expectedTypeTellsResultOf(m_context, site))
            m_copy->addValueNarrowedInUnknownWay(site.use);
    }
    for (ValueID subject : collectSubjectsKnownBeyondTheirTypes(m_context, m_plan))
        m_copy->addValueKnownBeyondItsType(subject);
    return m_copy->plan(m_context.limit(LimitID::MaxBlocksCopiedBehindStretches));
}

// Where a test fails, the copy goes on behind the stretch, so nothing joins the code behind the test that passed until that code has no proof left to lose.
void TestPlacer::extendCopiesBehindStretches(const RegionCloner::LivenessSource& liveness)
{
    Vector<BlockID, 8> blocksOfStretches = collectCopiedBlocks();
    if (blocksOfStretches.isEmpty())
        return;

    for (PreparedLoop& loop : m_loops) {
        loop.copyComesBack = canComeBackInto(loop);
        if (!loop.copyComesBack)
            continue;
        for (BlockID block : loop.blocksOnBackEdges)
            m_isOnWayBackIntoLoop.set(block.index());
    }
    m_extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);

    // A copy that comes back nowhere holds the blocks on the edges that go around its loops.
    if (m_extentOfCopy != GenericCopy::Extent::ToWaysBack && !m_isOnWayBackIntoLoop.isEmpty()) {
        for (PreparedLoop& loop : m_loops)
            loop.copyComesBack = false;
        m_isOnWayBackIntoLoop.clearAll();
        m_extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);
    }
    if (m_copy->wasCutShortByLimit())
        m_context.gaveUp(LimitID::MaxBlocksCopiedBehindStretches);
    if (m_extentOfCopy != GenericCopy::Extent::NotCopied)
        return;

    m_copy.reset();
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            m_unclonableStretches.set(range.stretch);
    }
}

void TestPlacer::checkStretches()
{
    Vector<StretchRegion> regions(m_plan.stretches.size());
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            regions[range.stretch].blocks.append(blockOf(range));
    }
    if (!m_splitEdges.isEmpty()) {
        using BlockAndStretch = std::pair<uint32_t, unsigned>;
        Vector<BlockAndStretch> stretchesByBlock;
        for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
            for (BlockID block : regions[stretch].blocks)
                stretchesByBlock.append({ block.index(), stretch });
        }
        AOT::sort(stretchesByBlock);
        removeRepeatedElements(stretchesByBlock);
        auto stretchesOf = [&](BlockID block) {
            return std::ranges::equal_range(stretchesByBlock, block.index(), { }, &BlockAndStretch::first);
        };
        for (const SplitEdge& split : m_splitEdges) {
            auto stretchesOfTarget = stretchesOf(m_function.at(split.block).terminator.next().block);
            if (stretchesOfTarget.empty())
                continue;
            for (auto [block, stretch] : stretchesOf(lastPartOf(split.edge.from))) {
                if (std::ranges::binary_search(stretchesOfTarget, stretch, { }, &BlockAndStretch::second))
                    regions[stretch].blocks.append(split.block);
            }
        }
    }

    BitVector isEntry(m_function.blocks.size());
    for (const PlannedTest& test : m_plan.tests) {
        if (!m_plan.isGuarded(test.stretch))
            continue;
        BlockID entry = guardedBlockOf(test);
        if (isEntry.add(entry.index()))
            regions[test.stretch].entries.append(entry);
    }
    for (const PlannedSlowPath& slowPath : m_plan.slowPaths) {
        BlockID entry = targetOf(m_function, Edge { lastPartOf(slowPath.edge.from), slowPath.edge.targetIndex }).block;
        if (m_plan.isGuarded(slowPath.stretch) && isEntry.add(entry.index()))
            regions[slowPath.stretch].entries.append(entry);
    }

    // A copy that goes on behind a stretch asks what is live in the rest of the function. One liveness serves every stretch and the copy of them all, which may hold any block on a split edge.
    FunctionAnalyses& analyses = m_context.analyses;
    bool goesOnBehindStretches = std::ranges::any_of(m_plan.ranges, [&](const StretchRange& range) {
        return goesOnBehindStretch(range);
    });
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
            for (const SplitEdge& split : m_splitEdges)
                blocks.append(split.block);
            livenessInsideStretches = RegionCloner::computeLivenessInside(analyses, blocks.span(), entries.span());
        }
        return *livenessInsideStretches;
    };
    for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
        const StretchRegion& region = regions[stretch];
        if (region.blocks.isEmpty())
            continue;
        if (!canCloneInsideFunction(RegionCloner::checkCanDuplicate(analyses, region.blocks.span(), region.entries.span(), liveness)))
            m_unclonableStretches.set(stretch);
    }
    extendCopiesBehindStretches(liveness);
    if (m_unclonableStretches.isEmpty())
        return;

    BitVector isDeclined(m_plan.stretches.size());
    for (const PlannedUse& use : m_plan.uses) {
        if (m_unclonableStretches.get(use.stretch) && isDeclined.add(use.stretch))
            m_context.addDecline(Wire::DeclineKind::StretchNotGuarded, m_plan.sites.sites[use.site].use);
    }
}

void TestPlacer::cloneGenericCopies()
{
    if (!m_copy)
        return;
    // In a checked function of the program the type that a test expects is what the checker says. Elsewhere it is a guess, and the copy runs.
    bool expectsCheckedTypes = m_context.function.has(FunctionFlags::IsChecked) && !isLibraryCode(m_context.program, m_context.function);
    m_copy->clone(expectsCheckedTypes ? TestPlacementModel::frequencyOfCopyAgainstCheckedTypes : TestPlacementModel::frequencyOfGenericCopy);

    Statistics& statistics = m_context.build.statistics();
    switch (m_extentOfCopy) {
    case GenericCopy::Extent::ToWaysBack:
        statistics.count(StatisticID::StretchCopiesToWaysBack);
        break;
    case GenericCopy::Extent::ToEndOfFunction:
        statistics.count(StatisticID::StretchCopiesToEndOfFunction);
        break;
    case GenericCopy::Extent::OnlyCopiedBlocks:
        break;
    case GenericCopy::Extent::NotCopied:
        RELEASE_ASSERT_NOT_REACHED();
    }

    for (const PreparedLoop& loop : m_loops) {
        if (loop.copyComesBack)
            addWayBackIntoLoop(loop);
    }
}

// Every proof that the head of the loop relies on is made again on the way back in: here, or by the tests on the entering edge that the way back joins.
void TestPlacer::addWayBackIntoLoop(const PreparedLoop& loop)
{
    const PlannedWayBackIntoLoop& planned = plannedWayBackOf(loop);
    BlockID headOfCopy = m_copy->copyOf(planned.head);
    RELEASE_ASSERT(headOfCopy);

    // A test of a value that has a copy would be about another value than the test it repeats.
    bool canRepeatTests = std::ranges::none_of(planned.tests, [&](const PlannedTest& test) {
        return std::ranges::any_of(test.operands, [&](ValueID operand) {
            return !!m_copy->copyOf(operand);
        });
    });

    m_builder.setHandler(std::nullopt);
    for (uint32_t index = 0; index < planned.backEdges.size(); ++index) {
        BlockID block = loop.blocksOnBackEdges[index];
        Target next = m_function.at(block).terminator.next();
        RELEASE_ASSERT(next.block == planned.head);

        // The tested code goes around the loop past the block, so whatever enters the block comes out of the copy.
        Edge backEdge { lastPartOf(planned.backEdges[index].from), planned.backEdges[index].targetIndex };
        m_builder.redirectEdge(backEdge, planned.head, Arguments(next.arguments));

        BlockID copy = m_copy->copyOf(block);
        Target nextInCopy = copy ? Target { copy, { }, { } } : Target { headOfCopy, next.arguments, { } };

        m_builder.setOrigin(m_function.at(block).terminator.origin());
        Block& onBackEdge = m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
        onBackEdge.frequency = m_function.at(headOfCopy).frequency;
        onBackEdge.terminator = Terminator { };
        m_builder.moveToEndOf(block);
        if (!canRepeatTests) {
            m_builder.emitJump(nextInCopy.block, WTF::move(nextInCopy.arguments));
            continue;
        }
        emitTestsOnWayBack(planned, nextInCopy);
        for (uint32_t parameterIndex = 0; parameterIndex < planned.typesOfCounters.size(); ++parameterIndex) {
            if (!planned.typesOfCounters[parameterIndex])
                continue;
            BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
            IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(m_builder.appendTypeTest(next.arguments[parameterIndex], planned.typesOfCounters[parameterIndex]), testedOperandIndex, frequency, frequency);
            m_builder.moveToEndOf(branch.notTaken);
            m_builder.emitJump(nextInCopy.block, Arguments(nextInCopy.arguments));
            m_builder.moveToEndOf(branch.taken);
            next.arguments[parameterIndex] = branch.provenOnTaken;
        }
        m_builder.emitJump(loop.blockOnEnteringEdge, WTF::move(next.arguments));
    }
}

void TestPlacer::emitTestsOnWayBack(const PlannedWayBackIntoLoop& planned, const Target& failedTarget)
{
    BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
    for (const PlannedTest& test : planned.tests) {
        m_builder.setOrigin(m_function.at(m_plan.sites.sites[requirementAt(test.requirement).site].use).origin());
        ValueID value = m_builder.append(createTest(test));
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(value, testedOperandIndex, frequency, frequency);
        m_builder.moveToEndOf(branch.notTaken);
        m_builder.emitJump(failedTarget.block, Arguments(failedTarget.arguments));
        m_builder.moveToEndOf(branch.taken);
        addDefinition(test.condition, branch.provenOnTaken);
    }
}

void TestPlacer::redirectSlowPaths()
{
    for (const PlannedSlowPath& slowPath : m_plan.slowPaths) {
        if (!isPlaced(slowPath.stretch))
            continue;
        Edge edge { lastPartOf(slowPath.edge.from), slowPath.edge.targetIndex };
        Target target = targetOf(m_function, edge);
        target.block = m_copy->copyOf(target.block);
        RELEASE_ASSERT(target.block);
        m_builder.redirectEdge(edge, WTF::move(target));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
