/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include <algorithm>
#include <iterator>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StretchRegion {
    Vector<BlockID, 8> blocks;
    Vector<BlockID, 2> entries;
};

} // anonymous namespace

static bool canCopyInsideFunction(CloneCheck check)
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

void TestPlacer::collectCuts()
{
    auto addCut = [&](ProgramPoint point) {
        m_cuts.append(Cut { point, BlockID(), BlockID() });
    };
    for (const PlannedTest& test : m_plan.tests) {
        if (test.position == PlannedTestPosition::BeforeUse && m_plan.isGuarded(test.stretch))
            addCut(test.point);
    }
    for (const StretchRange& range : m_plan.ranges) {
        if (!shouldClone(range))
            continue;
        if (range.begin)
            addCut({ range.block, range.begin });
        if (range.end < m_function.at(range.block).values.size())
            addCut({ range.block, range.end });
    }
    std::ranges::sort(m_cuts, { }, &Cut::key);
    removeRepeatedElements(m_cuts, [](const Cut& a, const Cut& b) {
        return a.point == b.point;
    });
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

// Left out, the block on a split edge would lead the generic copy through the tests on the edge into code that relies on proofs the copy does not have.
void TestPlacer::appendSplitEdgesInside(Vector<BlockID, 8>& region) const
{
    if (m_splitEdges.isEmpty() || region.isEmpty())
        return;

    BitVector isInRegion(m_function.blocks.size());
    for (BlockID block : region)
        isInRegion.quickSet(block.index());
    for (const SplitEdge& split : m_splitEdges) {
        BlockID target = m_function.at(split.block).terminator.next().block;
        if (isInRegion.quickGet(lastPartOf(split.edge.from).index()) && isInRegion.quickGet(target.index()))
            region.append(split.block);
    }
}

void TestPlacer::checkStretches()
{
    Vector<StretchRegion> regions(m_plan.stretches.size());
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            regions[range.stretch].blocks.append(blockOf(range));
    }
    for (StretchRegion& region : regions)
        appendSplitEdgesInside(region.blocks);

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

    for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
        const StretchRegion& region = regions[stretch];
        if (region.blocks.isEmpty())
            continue;
        if (!canCopyInsideFunction(RegionCloner::checkCanDuplicate(m_context.analyses, region.blocks.span(), region.entries.span())))
            m_unclonableStretches.set(stretch);
    }
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
    Vector<BlockID, 8> region;
    for (const StretchRange& range : m_plan.ranges) {
        if (shouldClone(range))
            region.append(blockOf(range));
    }
    if (region.isEmpty())
        return;
    appendSplitEdgesInside(region);

    m_cloner.emplace(m_builder, m_function);
    m_cloner->cloneBlocks(region.span(), [](ValueID, Value&) { });
    for (BlockID block : region)
        m_function.at(m_cloner->cloneOf(block)).frequency = TestPlacementModel::frequencyOfGenericCopy(m_function.at(block).frequency);
}

void TestPlacer::redirectSlowPaths()
{
    for (const PlannedSlowPath& slowPath : m_plan.slowPaths) {
        if (!isPlaced(slowPath.stretch))
            continue;
        Edge edge { lastPartOf(slowPath.edge.from), slowPath.edge.targetIndex };
        Target target = targetOf(m_function, edge);
        target.block = m_cloner->cloneOf(target.block);
        RELEASE_ASSERT(target.block);
        m_builder.redirectEdge(edge, WTF::move(target));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
