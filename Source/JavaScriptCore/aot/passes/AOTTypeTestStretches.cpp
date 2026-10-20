/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestStretches.h"

#if ENABLE(AOT)

#include "AOTCodeSizeEstimate.h"
#include <algorithm>
#include <utility>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RangeSet : WTF::UnionFind<RangeSet> { };

} // anonymous namespace

static bool needsTestInFront(RequirementCoverage coverage)
{
    switch (coverage) {
    case RequirementCoverage::NeedsTest:
    case RequirementCoverage::NeedsRenewal:
        return true;
    case RequirementCoverage::Unknown:
    case RequirementCoverage::Covered:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TestStretches::TestStretches(const TestEvents& events, const TestCoverage& coverage, const WantedProofs& wanted)
    : m_events(events)
    , m_coverage(coverage)
    , m_wanted(wanted)
    , m_hasTestInFront(events.function().values.size())
    , m_isPlannedUse(events.function().values.size())
    , m_blocksWhereCopyIsEntered(events.function().blocks.size())
    , m_firstRangeOf(events.function().blocks.size())
{
    markTestPoints();
    for (BlockID block : m_events.cfg().reversePostOrder())
        findRangesIn(block);
    joinRanges();
    markClonedRanges();
    measureStretches();
}

bool TestStretches::startsNewCopy(ProgramPoint point) const
{
    return m_hasTestInFront.get(m_events.function().at(point.block).values[point.index].index());
}

bool TestStretches::isCopyEnteredAtHeadOf(BlockID block) const
{
    return m_blocksWhereCopyIsEntered.get(block.index());
}

unsigned TestStretches::firstRangeOf(BlockID block) const
{
    std::optional<unsigned> first = m_firstRangeOf[block.index()];
    RELEASE_ASSERT(first && !m_ranges[*first].begin);
    return *first;
}

void TestStretches::forEachNextRange(unsigned index, NOESCAPE const Invocable<void(unsigned)> auto& functor) const
{
    BlockID id = m_ranges[index].block;
    uint32_t end = m_ranges[index].end;
    if (index + 1 < m_ranges.size() && m_ranges[index + 1].block == id && m_ranges[index + 1].begin == end)
        functor(index + 1);

    const Block& block = m_events.function().at(id);
    if (end != block.values.size())
        return;
    for (const Target& target : block.terminator.targets) {
        if (m_wanted.isInStretchAtHead(target.block))
            functor(firstRangeOf(target.block));
    }
}

void TestStretches::markTestPoints()
{
    const Function& function = m_events.function();
    std::span<const TestRequirement> requirements = m_events.requirements();
    for (uint32_t index = 0; index < requirements.size(); ++index) {
        RequirementCoverage coverage = m_coverage.of(index);
        if (coverage == RequirementCoverage::Unknown)
            continue;
        ProgramPoint point = requirements[index].point;
        size_t use = function.at(point.block).values[point.index].index();
        m_isPlannedUse.set(use);
        if (needsTestInFront(coverage))
            m_hasTestInFront.set(use);
    }

    std::span<const HoistedTest> hoistedTests = m_events.hoistedTests();
    for (uint32_t index = 0; index < hoistedTests.size(); ++index) {
        if (m_wanted.isHoistedTestWanted(index))
            m_blocksWhereCopyIsEntered.set(targetOf(function, hoistedTests[index].edge).block.index());
    }

    for (Edge edge : m_wanted.edgesIntoGenericCopy()) {
        BlockID target = targetOf(function, edge).block;
        if (m_wanted.isInStretchAtHead(target))
            m_blocksWhereCopyIsEntered.set(target.index());
    }
}

void TestStretches::appendRange(BlockID block, uint32_t begin, uint32_t end)
{
    std::optional<unsigned>& first = m_firstRangeOf[block.index()];
    if (!first)
        first = m_ranges.size();
    m_ranges.append(StretchRange { .stretch = 0, .block = block, .begin = begin, .end = end, .isCloned = false });
}

void TestStretches::findRangesIn(BlockID block)
{
    const BlockValues& values = m_events.function().at(block).values;
    uint32_t numberOfValues = values.size();
    if (m_wanted.isInStretchAtHead(block) && (!numberOfValues || startsNewCopy({ block, 0 })))
        appendRange(block, 0, 0);

    std::optional<uint32_t> begin;
    uint32_t extendedUntil = 0;
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        bool isInStretch = m_wanted.isInStretch(values[index]) || index < extendedUntil;
        if (begin && !isInStretch) {
            extendedUntil = m_events.endOfResultReadsAfter({ block, index });
            isInStretch = index < extendedUntil;
        }
        if (begin && (!isInStretch || startsNewCopy({ block, index }))) {
            appendRange(block, *begin, index);
            begin = std::nullopt;
        }
        if (isInStretch && !begin)
            begin = index;
    }
    if (begin)
        appendRange(block, *begin, numberOfValues);
}

void TestStretches::joinRanges()
{
    unsigned numberOfRanges = m_ranges.size();
    Vector<RangeSet> sets(numberOfRanges);
    for (unsigned index = 0; index < numberOfRanges; ++index) {
        forEachNextRange(index, [&](unsigned next) {
            sets[next].unify(&sets[index]);
        });
    }

    Vector<std::optional<unsigned>> stretchOfRoot(numberOfRanges);
    for (unsigned index = 0; index < numberOfRanges; ++index) {
        size_t root = sets[index].find() - sets.span().data();
        std::optional<unsigned>& stretch = stretchOfRoot[root];
        if (!stretch) {
            stretch = m_stretches.size();
            m_stretches.append(Stretch { });
        }
        m_ranges[index].stretch = *stretch;
    }
}

void TestStretches::markClonedRanges()
{
    Vector<unsigned> worklist;
    auto markCloned = [&](unsigned index) {
        if (!std::exchange(m_ranges[index].isCloned, true))
            worklist.append(index);
    };
    for (unsigned index = 0; index < m_ranges.size(); ++index) {
        const StretchRange& range = m_ranges[index];
        bool startsAtTest = range.begin < range.end && startsNewCopy({ range.block, range.begin });
        bool isEnteredAtHead = isCopyEnteredAtHeadOf(range.block) && firstRangeOf(range.block) == index;
        if (startsAtTest || isEnteredAtHead)
            markCloned(index);
    }
    while (!worklist.isEmpty())
        forEachNextRange(worklist.takeLast(), markCloned);
}

void TestStretches::measureStretches()
{
    const Program& program = m_events.analyses().program();
    const Function& function = m_events.function();
    for (const StretchRange& range : m_ranges) {
        const Block& block = function.at(range.block);
        Stretch& stretch = m_stretches[range.stretch];
        for (uint32_t index = range.begin; index < range.end; ++index) {
            ValueID value = block.values[index];
            if (m_isPlannedUse.get(value.index()))
                ++stretch.numberOfUses;
            else if (range.isCloned)
                stretch.clonedBytes += estimatedCodeBytes(program, function, function.at(value));
        }
        if (range.isCloned && range.end == block.values.size())
            stretch.clonedBytes += estimatedCodeBytes(block.terminator);
    }

    std::span<const TestRequirement> requirements = m_events.requirements();
    for (uint32_t index = 0; index < requirements.size(); ++index) {
        if (needsTestInFront(m_coverage.of(index)))
            ++m_stretches[stretchAt(requirements[index].point)].numberOfTests;
    }

    std::span<const HoistedTest> hoistedTests = m_events.hoistedTests();
    for (uint32_t index = 0; index < hoistedTests.size(); ++index) {
        if (m_wanted.isHoistedTestWanted(index))
            ++m_stretches[stretchAtHeadOf(targetOf(function, hoistedTests[index].edge).block)].numberOfTests;
    }

    for (Stretch& stretch : m_stretches)
        stretch.outcome = StretchOutcome::Guarded;
}

unsigned TestStretches::stretchAt(ProgramPoint point) const
{
    std::optional<unsigned> first = m_firstRangeOf[point.block.index()];
    RELEASE_ASSERT(first);
    std::span<const StretchRange> candidates = m_ranges.subspan(*first);
    auto firstLaterRange = std::ranges::partition_point(candidates, [&](const StretchRange& range) {
        return range.block == point.block && range.begin <= point.index;
    });
    RELEASE_ASSERT(firstLaterRange != candidates.begin());
    const StretchRange& range = *(firstLaterRange - 1);
    RELEASE_ASSERT(point.index < range.end);
    return range.stretch;
}

unsigned TestStretches::stretchAtHeadOf(BlockID block) const
{
    return m_ranges[firstRangeOf(block)].stretch;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
