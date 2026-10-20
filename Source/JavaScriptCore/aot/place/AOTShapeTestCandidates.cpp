/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedSet {
    LayoutSetID set;
    SiteWeight weight { 0 };
    uint32_t sites { 0 };
};

class ShapeTestCandidateCollector {
public:
    ShapeTestCandidateCollector(const PlacementContext& context, const ShapeVariants& variants)
        : m_context(context)
        , m_variants(variants)
    {
    }

    ShapeTestCandidates run(const PlacedSites& sites, std::span<const FieldAccess> fieldAccesses, const BitVector& typesNeededAtRunTime)
    {
        collectTestedSets(sites, fieldAccesses);
        collectSetsTestedByRunTime(typesNeededAtRunTime);
        foldEqualSets();
        listTestedLayouts();
        orderListsByWeight();
        sumSingleLayoutTestWeights();
        return WTF::move(m_result);
    }

private:
    void collectTestedSets(const PlacedSites& sites, std::span<const FieldAccess> fieldAccesses)
    {
        RELEASE_ASSERT(fieldAccesses.size() == sites.fieldAccesses.size());
        for (auto& use : sites.shapeTests) {
            if (use.tested)
                m_testedSets.append(TestedSet { use.tested, use.weight, use.sites });
        }
        for (size_t i = 0; i < fieldAccesses.size(); ++i) {
            if (fieldAccesses[i].coverage == FieldAccess::Coverage::SomeMembers)
                m_testedSets.append(TestedSet { fieldAccesses[i].membersWithField, sites.fieldAccesses[i].weight, sites.fieldAccesses[i].sites });
        }
    }

    // They yield to every set that code tests. Where the numbers are still free a run of them saves the bit set in the tables, which is as wide as its members lie apart.
    void collectSetsTestedByRunTime(const BitVector& typesNeededAtRunTime)
    {
        for (size_t type : typesNeededAtRunTime) {
            if (LayoutSetID set = m_context.program.at(TypeID(type)).layouts)
                m_testedSets.append(TestedSet { set, 0, 0 });
        }
    }

    void foldEqualSets()
    {
        parallelSort(m_context.build.parallelLoops(), m_testedSets.mutableSpan(), [](const TestedSet& a, const TestedSet& b) {
            return a.set < b.set;
        });
        size_t numberOfSets = 0;
        for (auto& tested : m_testedSets) {
            if (numberOfSets && m_testedSets[numberOfSets - 1].set == tested.set) {
                TestedSet& folded = m_testedSets[numberOfSets - 1];
                folded.weight = saturatedSum(folded.weight, tested.weight);
                folded.sites = clampTo<uint32_t>(static_cast<uint64_t>(folded.sites) + tested.sites);
                continue;
            }
            m_testedSets[numberOfSets++] = tested;
        }
        m_testedSets.shrink(numberOfSets);
    }

    void listTestedLayouts()
    {
        auto& listedLayouts = m_result.listedLayouts;
        for (auto& tested : m_testedSets) {
            TestedLayoutList list { .set = tested.set, .firstLayout = static_cast<uint32_t>(listedLayouts.size()), .sites = tested.sites, .weight = tested.weight };
            for (LayoutID member : m_context.program.at(tested.set).members) {
                uint32_t numberOfShapes = m_variants.numberOfShapesOf(member);
                if (!m_context.isCompiledObject(member) || !numberOfShapes)
                    continue;
                listedLayouts.append(member.index());
                ++list.numberOfLayouts;
                list.numberOfShapes += numberOfShapes;
            }
            if (list.numberOfLayouts)
                m_result.lists.append(list);
        }
    }

    void orderListsByWeight()
    {
        auto& lists = m_result.lists;
        auto& listsByWeight = m_result.listsByWeight;
        for (unsigned i = 0; i < lists.size(); ++i) {
            if (lists[i].numberOfLayouts > 1)
                listsByWeight.append(i);
        }
        std::ranges::sort(listsByWeight, [&](uint32_t a, uint32_t b) {
            if (lists[a].weight != lists[b].weight)
                return lists[a].weight > lists[b].weight;
            if (lists[a].numberOfLayouts != lists[b].numberOfLayouts)
                return lists[a].numberOfLayouts < lists[b].numberOfLayouts;
            return a < b;
        });
    }

    void sumSingleLayoutTestWeights()
    {
        auto& singleLayoutTestWeight = m_result.singleLayoutTestWeight;
        singleLayoutTestWeight.fill(0, m_context.program.layouts.size());
        for (auto& list : m_result.lists) {
            if (list.numberOfLayouts != 1)
                continue;
            SiteWeight& weight = singleLayoutTestWeight[m_result.listedLayouts[list.firstLayout]];
            weight = saturatedSum(weight, list.weight);
        }
    }

    const PlacementContext& m_context;
    const ShapeVariants& m_variants;
    Vector<TestedSet> m_testedSets;
    ShapeTestCandidates m_result;
};

} // anonymous namespace

ShapeTestCandidates collectShapeTestCandidates(const PlacementContext& context, const PlacedSites& sites, std::span<const FieldAccess> fieldAccesses, const ShapeVariants& variants, const BitVector& typesNeededAtRunTime)
{
    return ShapeTestCandidateCollector(context, variants).run(sites, fieldAccesses, typesNeededAtRunTime);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
