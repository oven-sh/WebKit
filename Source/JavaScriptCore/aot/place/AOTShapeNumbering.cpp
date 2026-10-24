/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTPQTree.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Int128.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

inline constexpr uint64_t bytesSavedPerBlockTest = 6;
inline constexpr uint32_t numberOfShapesWithShortConstants = 128;

constexpr uint32_t blockStartAtOrAfter(uint32_t shape)
{
    return static_cast<uint32_t>(WTF::roundUpToMultipleOf<shapesPerBlock>(shape - 1) + 1);
}

class ShapeNumberer {
public:
    ShapeNumberer(const PlacementContext& context, const ShapeTestCandidates& candidates, const ShapeVariants& variants)
        : m_context(context)
        , m_candidates(candidates)
        , m_variants(variants)
        , m_numberOfLayouts(context.program.layouts.size())
    {
    }

    ShapeNumbering run()
    {
        m_result.shapesOfLayout.fill(ShapeRange { }, m_numberOfLayouts);
        assignLeaves();
        if (!m_leafLayout.isEmpty()) {
            groupFrontier(computeFrontier());
            chooseNumbering();
        }
        numberRemainingLayouts();
        checkLimit();
        return WTF::move(m_result);
    }

private:
    struct Group {
        uint32_t first;
        uint32_t count { 0 };
        SiteWeight weight { 0 };
    };

    struct Blocks {
        BitVector begins;
        BitVector ends;
    };

    struct SequenceNumbering {
        Vector<uint32_t> firstShapeOfLayout;
        uint32_t nextShape { 1 };
        SiteWeight estimatedInstructions { 0 };
    };

    struct SequenceWithHead {
        Vector<uint32_t> sequence;
        uint32_t headLength;
    };

    uint32_t numberOfShapesOf(uint32_t layout) const
    {
        uint32_t numberOfShapes = m_variants.numberOfShapesOf(LayoutID(layout));
        RELEASE_ASSERT(numberOfShapes);
        return numberOfShapes;
    }

    Vector<uint32_t> positionsIn(const Vector<uint32_t>& sequence) const
    {
        Vector<uint32_t> positionOf(FillWith { }, m_numberOfLayouts, 0);
        for (uint32_t position = 0; position < sequence.size(); ++position)
            positionOf[sequence[position]] = position;
        return positionOf;
    }

    std::pair<uint32_t, uint32_t> extentOf(const TestedLayoutList& list, const Vector<uint32_t>& positionOf) const
    {
        uint32_t first = std::numeric_limits<uint32_t>::max();
        uint32_t last = 0;
        for (uint32_t layout : m_candidates.layoutsOf(list)) {
            first = std::min(first, positionOf[layout]);
            last = std::max(last, positionOf[layout]);
        }
        return std::pair { first, last };
    }

    void assignLeaves()
    {
        m_leafOfLayout.fill(std::nullopt, m_numberOfLayouts);
        for (uint32_t index : m_candidates.listsByWeight) {
            for (uint32_t layout : m_candidates.layoutsOf(m_candidates.lists[index])) {
                if (m_leafOfLayout[layout])
                    continue;
                m_leafOfLayout[layout] = static_cast<uint32_t>(m_leafLayout.size());
                m_leafLayout.append(layout);
            }
        }
        for (unsigned i = 0; i < m_numberOfLayouts; ++i) {
            if (m_context.isCompiledObject(LayoutID(i)) && !m_leafOfLayout[i])
                m_layoutsTestedAlone.append(i);
        }
        AOT::stableSort(m_layoutsTestedAlone, [&](uint32_t a, uint32_t b) {
            return m_candidates.singleLayoutTestWeight[a] > m_candidates.singleLayoutTestWeight[b];
        });
    }

    Vector<uint32_t> computeFrontier() const
    {
        PQTree tree(m_leafLayout.size());
        Vector<uint64_t> leafKey(m_leafLayout.size(), [](size_t leaf) {
            return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) << 32 | leaf;
        });
        uint64_t rejectedCount = 0;
        Vector<uint32_t> leaves;
        for (uint32_t index : m_candidates.listsByWeight) {
            leaves.shrink(0);
            for (uint32_t layout : m_candidates.layoutsOf(m_candidates.lists[index]))
                leaves.append(*m_leafOfLayout[layout]);
            if (tree.reduce(leaves.span()))
                continue;
            for (uint32_t leaf : leaves)
                leafKey[leaf] = std::min(leafKey[leaf], rejectedCount << 32 | leaf);
            ++rejectedCount;
        }
        Vector<uint32_t> frontier;
        for (uint32_t leaf : tree.frontier(leafKey.span()))
            frontier.append(m_leafLayout[leaf]);
        return frontier;
    }

    void groupFrontier(Vector<uint32_t>&& frontier)
    {
        Vector<uint32_t> positionOf = positionsIn(frontier);
        Vector<int32_t> numberOfListsSpanning(FillWith { }, frontier.size() + 1, 0);
        Vector<SiteWeight> weightOfListsBeginningAt(FillWith { }, frontier.size(), 0);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            // A list without weight ties no groups together: it would move tested shapes behind untested ones.
            if (!list.weight)
                continue;
            auto [first, last] = extentOf(list, positionOf);
            ++numberOfListsSpanning[first + 1];
            --numberOfListsSpanning[last + 1];
            weightOfListsBeginningAt[first] = saturatedSum(weightOfListsBeginningAt[first], list.weight);
        }
        Vector<Group> groups;
        int32_t spanning = 0;
        for (uint32_t position = 0; position < frontier.size(); ++position) {
            spanning += numberOfListsSpanning[position];
            if (!spanning)
                groups.append(Group { position });
            auto& group = groups.last();
            ++group.count;
            group.weight = saturatedSum(group.weight, saturatedSum(weightOfListsBeginningAt[position], m_candidates.singleLayoutTestWeight[frontier[position]]));
        }
        while (m_numberOfLayoutsTestedAloneInFrontier < m_layoutsTestedAlone.size() && m_candidates.singleLayoutTestWeight[m_layoutsTestedAlone[m_numberOfLayoutsTestedAloneInFrontier]]) {
            uint32_t layout = m_layoutsTestedAlone[m_numberOfLayoutsTestedAloneInFrontier++];
            groups.append(Group { static_cast<uint32_t>(frontier.size()), 1, m_candidates.singleLayoutTestWeight[layout] });
            frontier.append(layout);
        }
        AOT::stableSort(groups, [](const Group& a, const Group& b) {
            return static_cast<UInt128>(a.weight) * static_cast<UInt128>(b.count) > static_cast<UInt128>(b.weight) * static_cast<UInt128>(a.count);
        });
        m_groupOf.fill(0, m_numberOfLayouts);
        for (uint32_t index = 0; index < groups.size(); ++index) {
            for (uint32_t layout : frontier.subspan(groups[index].first, groups[index].count)) {
                m_groupOf[layout] = index;
                m_frontierByTestsPerShape.append(layout);
            }
        }
    }

    Blocks chooseBlocks(const Vector<uint32_t>& sequence, uint32_t headLength) const
    {
        Vector<uint32_t> positionOf = positionsIn(sequence);
        BitVector isInBlock(sequence.size());
        Blocks blocks { BitVector(sequence.size()), BitVector(sequence.size()) };
        Vector<int32_t> numberOfBitSetsSpanning(FillWith { }, sequence.size() + 2, 0);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            auto [first, last] = extentOf(list, positionOf);
            if (last - first + 1 == list.numberOfLayouts)
                continue;
            ++numberOfBitSetsSpanning[first + 1];
            --numberOfBitSetsSpanning[last + 1];
        }
        for (uint32_t position = 1; position < numberOfBitSetsSpanning.size(); ++position)
            numberOfBitSetsSpanning[position] += numberOfBitSetsSpanning[position - 1];
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            if (list.numberOfShapes > shapesPerBlock)
                continue;
            auto [first, last] = extentOf(list, positionOf);
            if (last - first + 1 != list.numberOfLayouts || first < headLength)
                continue;
            bool isFree = true;
            for (uint32_t position = first; position <= last; ++position)
                isFree &= !isInBlock.quickGet(position);
            uint64_t unusedNumbersBefore = shapesPerBlock / 2;
            uint64_t unusedNumbersAfter = shapesPerBlock - list.numberOfShapes;
            uint64_t dataBytes = (unusedNumbersBefore + unusedNumbersAfter) * sizeof(uint32_t) + (unusedNumbersBefore * numberOfBitSetsSpanning[first] + unusedNumbersAfter * numberOfBitSetsSpanning[last + 1]) / 8;
            if (!isFree || static_cast<uint64_t>(list.sites) * bytesSavedPerBlockTest < dataBytes)
                continue;
            for (uint32_t position = first; position <= last; ++position)
                isInBlock.quickSet(position);
            blocks.begins.quickSet(first);
            blocks.ends.quickSet(last);
        }
        return blocks;
    }

    SequenceNumbering assignShapes(const Vector<uint32_t>& sequence, uint32_t headLength, const Blocks& blocks) const
    {
        SequenceNumbering result;
        result.firstShapeOfLayout.fill(0, m_numberOfLayouts);
        bool isAligned = false;
        for (uint32_t position = 0; position < sequence.size(); ++position) {
            if (headLength && position == headLength)
                result.nextShape = blockStartAtOrAfter(result.nextShape);
            if (blocks.begins.quickGet(position) && result.nextShape > numberOfShapesWithShortConstants && blockStartAtOrAfter(result.nextShape) / shapesPerBlock < maxTestableBlocks) {
                result.nextShape = blockStartAtOrAfter(result.nextShape);
                isAligned = true;
            }
            result.firstShapeOfLayout[sequence[position]] = result.nextShape;
            result.nextShape += numberOfShapesOf(sequence[position]);
            if (blocks.ends.quickGet(position) && std::exchange(isAligned, false))
                result.nextShape = blockStartAtOrAfter(result.nextShape);
        }
        if (headLength == sequence.size())
            result.nextShape = blockStartAtOrAfter(result.nextShape);
        return result;
    }

    Vector<uint32_t> computeShapeCountPrefixSums(const Vector<uint32_t>& sequence, const SequenceNumbering& numbering) const
    {
        Vector<uint32_t> sums(FillWith { }, numbering.nextShape + m_numberOfShapesAfterFrontier, 0);
        for (uint32_t layout : sequence)
            std::ranges::fill(sums.mutableSubspan(numbering.firstShapeOfLayout[layout], numberOfShapesOf(layout)), 1);
        std::ranges::fill(sums.mutableSubspan(numbering.nextShape), 1);
        for (uint32_t shape = 1; shape < sums.size(); ++shape)
            sums[shape] += sums[shape - 1];
        return sums;
    }

    SiteWeight estimateInstructions(const Vector<uint32_t>& sequence, const SequenceNumbering& numbering) const
    {
        Vector<uint32_t> countPrefixSums = computeShapeCountPrefixSums(sequence, numbering);
        AssignedShapes assigned { countPrefixSums.span(), static_cast<uint32_t>(countPrefixSums.size() - 1) };
        SiteWeight estimatedInstructions = 0;
        Vector<uint32_t> shapes;
        auto addTest = [&](std::span<const uint32_t> layouts, SiteWeight weight) {
            shapes.shrink(0);
            for (uint32_t layout : layouts) {
                uint32_t first = numbering.firstShapeOfLayout[layout];
                for (uint32_t shape = first; shape < first + numberOfShapesOf(layout); ++shape)
                    shapes.append(shape);
            }
            AOT::sort(shapes);
            estimatedInstructions = saturatedSum(estimatedInstructions, saturatedProduct(weight, instructionsFor(chooseShapeTestForm(shapes.span(), assigned))));
        };
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            addTest(m_candidates.layoutsOf(list), list.weight);
        }
        for (const uint32_t& layout : sequence) {
            if (SiteWeight weight = m_candidates.singleLayoutTestWeight[layout])
                addTest(singleElementSpan(layout), weight);
        }
        return estimatedInstructions;
    }

    SequenceNumbering numberSequence(const Vector<uint32_t>& sequence, uint32_t headLength, bool usesBlocks) const
    {
        RELEASE_ASSERT(sequence.size() == m_frontierByTestsPerShape.size());
        Blocks blocks = usesBlocks ? chooseBlocks(sequence, headLength) : Blocks { BitVector(sequence.size()), BitVector(sequence.size()) };
        SequenceNumbering result = assignShapes(sequence, headLength, blocks);
        result.estimatedInstructions = estimateInstructions(sequence, result);
        return result;
    }

    std::optional<SequenceWithHead> findSequenceWithHead(const Vector<uint32_t>& frontier) const
    {
        Vector<uint32_t> positionOf = positionsIn(frontier);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            auto [first, last] = extentOf(list, positionOf);
            uint32_t length = list.numberOfLayouts;
            // A list without weight may lie in several groups.
            if (!list.weight || last - first + 1 != length || list.numberOfShapes > shapesPerBlock * maxTestableBlocks / 2)
                continue;
            if (static_cast<uint64_t>(list.sites) * bytesSavedPerBlockTest < (WTF::roundUpToMultipleOf<shapesPerBlock>(list.numberOfShapes) - list.numberOfShapes) * sizeof(uint32_t))
                continue;
            uint32_t group = m_groupOf[frontier[first]];
            uint32_t groupBegin = first;
            while (groupBegin && m_groupOf[frontier[groupBegin - 1]] == group)
                --groupBegin;
            uint32_t groupEnd = last + 1;
            while (groupEnd < frontier.size() && m_groupOf[frontier[groupEnd]] == group)
                ++groupEnd;
            Vector<uint32_t> before(frontier.subspan(groupBegin, first - groupBegin));
            std::ranges::reverse(before);
            Vector<uint32_t> sequence(frontier.subspan(first, length));
            sequence.append(frontier.subspan(last + 1, groupEnd - last - 1));
            sequence.appendVector(before);
            for (uint32_t layout : m_frontierByTestsPerShape) {
                if (m_groupOf[layout] != group)
                    sequence.append(layout);
            }
            return SequenceWithHead { WTF::move(sequence), length };
        }
        return std::nullopt;
    }

    uint32_t countShapesAfterFrontier() const
    {
        uint32_t numberOfShapes = static_cast<uint32_t>(m_variants.ordered.bitCount() + m_variants.unordered.bitCount());
        for (uint32_t layout : m_frontierByTestsPerShape)
            numberOfShapes -= numberOfShapesOf(layout);
        return numberOfShapes;
    }

    std::optional<SequenceNumbering> numberSequenceWithHead(const Vector<uint32_t>& frontier) const
    {
        auto withHead = findSequenceWithHead(frontier);
        if (!withHead)
            return std::nullopt;
        return numberSequence(withHead->sequence, withHead->headLength, true);
    }

    void chooseNumbering()
    {
        m_numberOfShapesAfterFrontier = countShapesAfterFrontier();
        Vector<uint32_t> reversedFrontier = m_frontierByTestsPerShape;
        reversedFrontier.reverse();
        std::array<std::optional<SequenceNumbering>, 4> numberings;
        runTasksInParallel(m_context.build.parallelLoops(), {
            [&] { numberings[0] = numberSequence(m_frontierByTestsPerShape, 0, false); },
            [&] { numberings[1] = numberSequence(m_frontierByTestsPerShape, 0, true); },
            [&] { numberings[2] = numberSequenceWithHead(m_frontierByTestsPerShape); },
            [&] { numberings[3] = numberSequenceWithHead(reversedFrontier); },
        });
        SequenceNumbering best = WTF::move(*numberings[0]);
        for (std::optional<SequenceNumbering>& numbering : std::span(numberings).subspan(1)) {
            if (numbering && numbering->estimatedInstructions < best.estimatedInstructions)
                best = WTF::move(*numbering);
        }
        for (uint32_t layout : m_frontierByTestsPerShape)
            m_result.shapesOfLayout[layout] = ShapeRange { best.firstShapeOfLayout[layout], numberOfShapesOf(layout) };
        m_nextShape = best.nextShape;
    }

    void numberRemainingLayouts()
    {
        auto number = [&](uint32_t layout) {
            m_result.shapesOfLayout[layout] = ShapeRange { m_nextShape, numberOfShapesOf(layout) };
            m_nextShape += numberOfShapesOf(layout);
        };
        for (uint32_t layout : m_layoutsTestedAlone.span().subspan(m_numberOfLayoutsTestedAloneInFrontier))
            number(layout);
        m_result.firstEnvironmentShape = m_nextShape;
        for (unsigned i = 0; i < m_numberOfLayouts; ++i) {
            if (m_context.isEnvironment(LayoutID(i)))
                number(i);
        }
        number(m_context.closureLayout.index());
        m_result.numberOfShapes = m_nextShape - 1;
    }

    void checkLimit()
    {
        if (m_context.build.exceeds(LimitID::MaxShapes, m_result.numberOfShapes))
            m_context.build.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyShapes, .limit = LimitID::MaxShapes, .count = m_result.numberOfShapes });
    }

    const PlacementContext& m_context;
    const ShapeTestCandidates& m_candidates;
    const ShapeVariants& m_variants;
    unsigned m_numberOfLayouts;
    Vector<std::optional<uint32_t>> m_leafOfLayout;
    Vector<uint32_t> m_leafLayout;
    Vector<uint32_t> m_layoutsTestedAlone;
    size_t m_numberOfLayoutsTestedAloneInFrontier { 0 };
    Vector<uint32_t> m_frontierByTestsPerShape;
    Vector<uint32_t> m_groupOf;
    uint32_t m_numberOfShapesAfterFrontier { 0 };
    uint32_t m_nextShape { 1 };
    ShapeNumbering m_result;
};

} // anonymous namespace

ShapeNumbering numberShapes(const PlacementContext& context, const ShapeTestCandidates& candidates, const ShapeVariants& variants)
{
    return ShapeNumberer(context, candidates, variants).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
