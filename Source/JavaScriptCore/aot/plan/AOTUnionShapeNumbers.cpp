/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnionPlan.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include "AOTUnionConsecutiveSets.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

static uint32_t sumOfMentionsInUnionShapeNumbers(uint32_t first, uint32_t second)
{
    return first > UINT32_MAX - second ? UINT32_MAX : first + second;
}

namespace {

class OrderOfShapesInUnionShapeNumbers {
public:
    OrderOfShapesInUnionShapeNumbers(const Program& program, const Facts& facts, const StorageOfLayouts& storage)
        : m_program(program)
        , m_facts(facts)
        , m_storage(storage)
        , m_families(facts.families())
        , m_linked(facts.linked())
        , m_numberOfLayouts(static_cast<uint32_t>(program.layouts.size()))
    {
    }

    Vector<uint32_t> run()
    {
        listLayoutsInPreorder();
        collectMentionedSets();
        orderMentionedLayouts(orderLeaves());

        Vector<uint32_t> order = WTF::move(m_frontierByMentionsPerShape);
        BitVector isListed(m_numberOfLayouts);
        for (uint32_t layout : order)
            isListed.quickSet(layout);
        for (uint32_t layout : m_layoutsInPreorder) {
            if (!isListed.quickGet(layout))
                order.append(layout);
        }
        RELEASE_ASSERT(order.size() == m_numberOfLayouts);
        return order;
    }

private:
    static constexpr uint32_t noLeaf = UINT32_MAX;
    static constexpr uint32_t inNoRefusedSet = UINT32_MAX;

    struct MentionedSet {
        uint32_t firstLeaf { 0 };
        uint32_t numberOfLeaves { 0 };
        uint32_t mentions { 0 };
        bool isFamily { false };
        bool isKeptTogether { false };
    };

    struct Group {
        uint32_t firstPosition { 0 };
        uint32_t numberOfLayouts { 0 };
        uint64_t mentions { 0 };
    };

    struct WrittenSet {
        LayoutSetID layouts;
        uint32_t mentions { 0 };
    };

    bool hasShape(LayoutID id) const { return m_storage.holdsCompiledObjectsInUse(id); }

    std::span<const uint32_t> leavesOf(const MentionedSet& set) const LIFETIME_BOUND
    {
        return m_leavesOfSets.span().subspan(set.firstLeaf, set.numberOfLeaves);
    }

    void listLayoutsInPreorder()
    {
        constexpr uint32_t none = UINT32_MAX;
        Vector<uint32_t> firstBelow(FillWith { }, m_numberOfLayouts, none);
        Vector<uint32_t> nextBeside(FillWith { }, m_numberOfLayouts, none);
        for (uint32_t index = m_numberOfLayouts; index--;) {
            LayoutID base = writtenBaseOf(m_program.layouts[index]);
            if (!base)
                continue;
            nextBeside[index] = firstBelow[base.index()];
            firstBelow[base.index()] = index;
        }

        m_layoutsInPreorder.reserveInitialCapacity(m_numberOfLayouts);
        BitVector isListed(m_numberOfLayouts);
        Vector<uint32_t, 32> pending;
        auto listWithAllBelow = [&](uint32_t top) {
            if (isListed.quickSet(top))
                return;
            m_layoutsInPreorder.append(top);
            pending.append(firstBelow[top]);
            while (!pending.isEmpty()) {
                uint32_t current = pending.takeLast();
                if (current == none)
                    continue;
                pending.append(nextBeside[current]);
                if (isListed.quickSet(current))
                    continue;
                m_layoutsInPreorder.append(current);
                pending.append(firstBelow[current]);
            }
        };
        for (uint32_t index = 0; index < m_numberOfLayouts; ++index) {
            if (!writtenBaseOf(m_program.layouts[index]))
                listWithAllBelow(index);
        }
        for (uint32_t index = 0; index < m_numberOfLayouts; ++index)
            listWithAllBelow(index);

        m_preorder.fill(0, m_numberOfLayouts);
        for (uint32_t place = 0; place < m_numberOfLayouts; ++place)
            m_preorder[m_layoutsInPreorder[place]] = place;
    }

    void noteWrittenType(TypeID type)
    {
        if (!type || holdsOnlyPrimitives(type))
            return;
        LayoutSetID layouts = m_program.at(type).unnamedLayouts;
        if (!layouts)
            return;
        if (holdsOneMember(layouts)) {
            uint32_t& mentionsAlone = m_mentionsAlone[positionOf(layouts)];
            mentionsAlone = sumOfMentionsInUnionShapeNumbers(mentionsAlone, 1);
            return;
        }
        if (!liesInProgram(layouts))
            return;
        auto added = m_placesOfWrittenSets.add(layouts.bits(), static_cast<uint32_t>(m_writtenSets.size()));
        if (added.isNewEntry)
            m_writtenSets.append(WrittenSet { layouts, 0 });
        uint32_t& mentions = m_writtenSets[added.iterator->value].mentions;
        mentions = sumOfMentionsInUnionShapeNumbers(mentions, 1);
    }

    void countWrittenTypes()
    {
        for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
            if (index < m_linked.functions.size() && !m_linked.functions[index].isReached)
                continue;
            const Function& function = m_program.functions[index];
            noteWrittenType(function.thisType.written());
            for (const DeclaredType& parameter : function.parameters)
                noteWrittenType(parameter.written());
            for (const DeclaredType& result : function.results)
                noteWrittenType(result.written());
        }
        for (uint32_t index = 0; index < m_numberOfLayouts; ++index) {
            if (m_storage.isNeverAllocated(LayoutID(index)))
                continue;
            for (const Field& field : m_program.layouts[index].fields)
                noteWrittenType(field.declared.written());
        }
    }

    FamilyID familyThatSetWidensTo(LayoutSetID layouts) const
    {
        if (FamilyID family = m_families.familyOf(layouts))
            return family;
        SetView<LayoutID, LayoutSetID> members = m_program.at(layouts);
        LayoutID first = members.first();
        FamilyID family;
        members.anyOf([&](LayoutID member) {
            if (member == first)
                return false;
            family = family ? m_families.nearestCommonFamily(family, member) : m_families.nearestCommonFamily(first, member);
            return !family;
        });
        return family;
    }

    void noteMentionsOfLayoutsFrom(uint32_t first, uint32_t mentions, bool isFamily)
    {
        uint32_t numberOfLayouts = static_cast<uint32_t>(m_leavesOfSets.size()) - first;
        if (numberOfLayouts > 1) {
            m_sets.append(MentionedSet { .firstLeaf = first, .numberOfLeaves = numberOfLayouts, .mentions = mentions, .isFamily = isFamily });
            return;
        }
        if (numberOfLayouts) {
            uint32_t& mentionsAlone = m_mentionsAlone[m_leavesOfSets[first]];
            mentionsAlone = sumOfMentionsInUnionShapeNumbers(mentionsAlone, mentions);
        }
        m_leavesOfSets.shrink(first);
    }

    void appendMembersWithShapes(LayoutSetID layouts)
    {
        m_program.at(layouts).forEach([&](LayoutID member) {
            if (hasShape(member))
                m_leavesOfSets.append(member.index());
        });
    }

    void collectMentionedSets()
    {
        m_mentionsAlone.fill(0, m_numberOfLayouts);
        Vector<uint32_t> mentionsOfFamilies(FillWith { }, m_families.size(), 0u);
        countWrittenTypes();

        for (const WrittenSet& written : m_writtenSets) {
            FamilyID family = familyThatSetWidensTo(written.layouts);
            if (family) {
                uint32_t& mentionsOfFamily = mentionsOfFamilies[family.index()];
                mentionsOfFamily = sumOfMentionsInUnionShapeNumbers(mentionsOfFamily, written.mentions);
            }
            if (family && m_families.layoutsOf(family) == written.layouts)
                continue;
            uint32_t first = static_cast<uint32_t>(m_leavesOfSets.size());
            appendMembersWithShapes(written.layouts);
            noteMentionsOfLayoutsFrom(first, written.mentions, false);
        }
        BitVector familiesWithShapes = m_families.familiesWithSomeMemberThat([&](LayoutID member) {
            return hasShape(member);
        });
        for (uint32_t number = 0; number < m_families.size(); ++number) {
            if (!familiesWithShapes.quickGet(number))
                continue;
            uint32_t first = static_cast<uint32_t>(m_leavesOfSets.size());
            appendMembersWithShapes(m_families.layoutsOf(FamilyID(number)));
            noteMentionsOfLayoutsFrom(first, mentionsOfFamilies[number], true);
        }
        rankMentionedSets();
        replaceLayoutsOfSetsByLeaves();
    }

    void rankMentionedSets()
    {
        auto comesFirstByContent = [&](const MentionedSet& a, const MentionedSet& b) {
            if (a.numberOfLeaves != b.numberOfLeaves)
                return a.numberOfLeaves < b.numberOfLeaves;
            return std::ranges::lexicographical_compare(leavesOf(a), leavesOf(b));
        };
        AOT::sort(m_sets, comesFirstByContent);
        size_t numberOfSetsKept = 0;
        for (size_t index = 0; index < m_sets.size(); ++index) {
            MentionedSet set = m_sets[index];
            if (numberOfSetsKept && std::ranges::equal(leavesOf(m_sets[numberOfSetsKept - 1]), leavesOf(set))) {
                MentionedSet& kept = m_sets[numberOfSetsKept - 1];
                kept.mentions = sumOfMentionsInUnionShapeNumbers(kept.mentions, set.mentions);
                kept.isFamily |= set.isFamily;
                continue;
            }
            m_sets[numberOfSetsKept++] = set;
        }
        m_sets.shrink(numberOfSetsKept);
        AOT::sort(m_sets, [&](const MentionedSet& a, const MentionedSet& b) {
            if (a.isFamily != b.isFamily)
                return a.isFamily;
            if (!a.isFamily && a.mentions != b.mentions)
                return a.mentions > b.mentions;
            return comesFirstByContent(a, b);
        });
    }

    void replaceLayoutsOfSetsByLeaves()
    {
        m_leafOfLayout.fill(noLeaf, m_numberOfLayouts);
        Vector<uint32_t> leavesOfSets;
        for (MentionedSet& set : m_sets) {
            uint32_t first = static_cast<uint32_t>(leavesOfSets.size());
            for (uint32_t layout : leavesOf(set)) {
                uint32_t& leaf = m_leafOfLayout[layout];
                if (leaf == noLeaf) {
                    leaf = static_cast<uint32_t>(m_layoutOfLeaf.size());
                    m_layoutOfLeaf.append(layout);
                }
                leavesOfSets.append(leaf);
            }
            set.firstLeaf = first;
        }
        m_leavesOfSets = WTF::move(leavesOfSets);
    }

    Vector<uint32_t> orderLeaves()
    {
        uint32_t numberOfLeaves = static_cast<uint32_t>(m_layoutOfLeaf.size());
        if (!numberOfLeaves)
            return { };
        ConsecutiveSets consecutiveSets(numberOfLeaves);
        Vector<uint32_t> firstRefusedSetOfLeaf(FillWith { }, numberOfLeaves, inNoRefusedSet);
        uint32_t numberOfRefusedSets = 0;
        bool allSetsAreNested = true;
        for (MentionedSet& set : m_sets) {
            allSetsAreNested = allSetsAreNested && set.isFamily && consecutiveSets.tryKeepNestedSetTogether(leavesOf(set));
            set.isKeptTogether = allSetsAreNested || consecutiveSets.tryKeepTogether(leavesOf(set));
            RELEASE_ASSERT(set.isKeptTogether || !set.isFamily);
            if (set.isKeptTogether)
                continue;
            for (uint32_t leaf : leavesOf(set))
                firstRefusedSetOfLeaf[leaf] = std::min(firstRefusedSetOfLeaf[leaf], numberOfRefusedSets);
            ++numberOfRefusedSets;
        }

        auto rankOf = [&](uint32_t leaf) {
            uint32_t layout = m_layoutOfLeaf[leaf];
            return std::tuple { firstRefusedSetOfLeaf[leaf], m_preorder[layout], layout };
        };
        Vector<uint32_t> leavesByRank(numberOfLeaves, [](size_t leaf) {
            return static_cast<uint32_t>(leaf);
        });
        AOT::sort(leavesByRank, [&](uint32_t a, uint32_t b) {
            return rankOf(a) < rankOf(b);
        });
        Vector<uint64_t> keyOfLeaf(FillWith { }, numberOfLeaves, 0);
        for (uint32_t place = 0; place < numberOfLeaves; ++place)
            keyOfLeaf[leavesByRank[place]] = place;
        return consecutiveSets.order(keyOfLeaf.span());
    }

    void orderMentionedLayouts(const Vector<uint32_t>& orderOfLeaves)
    {
        uint32_t numberOfLeaves = static_cast<uint32_t>(m_layoutOfLeaf.size());
        RELEASE_ASSERT(orderOfLeaves.size() == numberOfLeaves);
        Vector<uint32_t> positionOfLeaf(FillWith { }, numberOfLeaves, 0);
        for (uint32_t position = 0; position < numberOfLeaves; ++position)
            positionOfLeaf[orderOfLeaves[position]] = position;

        Vector<int32_t> changeOfSetsSpanning(FillWith { }, numberOfLeaves + 1, 0);
        Vector<uint64_t> mentionsOfSetsBeginningAt(FillWith { }, numberOfLeaves, 0);
        Vector<int64_t> mentionsOfRangesBeginningAt(FillWith { }, numberOfLeaves, 0);
        Vector<int64_t> changeOfMentionsOfRangesSpanning(FillWith { }, numberOfLeaves + 1, 0);
        for (const MentionedSet& set : m_sets) {
            if (!set.mentions && !set.isFamily)
                continue;
            uint32_t first = UINT32_MAX;
            uint32_t last = 0;
            for (uint32_t leaf : leavesOf(set)) {
                first = std::min(first, positionOfLeaf[leaf]);
                last = std::max(last, positionOfLeaf[leaf]);
            }
            ASSERT(!set.isKeptTogether || last - first + 1 == set.numberOfLeaves);
            ++changeOfSetsSpanning[first + 1];
            --changeOfSetsSpanning[last + 1];
            mentionsOfSetsBeginningAt[first] += set.mentions;
            if (last - first + 1 == set.numberOfLeaves) {
                mentionsOfRangesBeginningAt[first] += set.mentions;
                changeOfMentionsOfRangesSpanning[first + 1] += set.mentions;
                changeOfMentionsOfRangesSpanning[last + 1] -= set.mentions;
            }
        }

        Vector<uint32_t> layoutsInOrder(numberOfLeaves, [&](size_t position) {
            return m_layoutOfLeaf[orderOfLeaves[position]];
        });
        constexpr int64_t rangesBegunForOneRangePulledApart = 6;
        std::optional<uint32_t> positionThatBeginsOrder;
        int64_t gainOfBeginningThere = 0;
        int64_t mentionsOfRangesSpanning = 0;
        Vector<Group> groups;
        int32_t numberOfSetsSpanning = 0;
        for (uint32_t position = 0; position < numberOfLeaves; ++position) {
            mentionsOfRangesSpanning += changeOfMentionsOfRangesSpanning[position];
            int64_t gain = mentionsOfRangesBeginningAt[position] - rangesBegunForOneRangePulledApart * mentionsOfRangesSpanning;
            if (gain > gainOfBeginningThere) {
                gainOfBeginningThere = gain;
                positionThatBeginsOrder = position;
            }
            numberOfSetsSpanning += changeOfSetsSpanning[position];
            if (!numberOfSetsSpanning)
                groups.append(Group { .firstPosition = position });
            Group& group = groups.last();
            ++group.numberOfLayouts;
            group.mentions += mentionsOfSetsBeginningAt[position] + m_mentionsAlone[layoutsInOrder[position]];
        }
        for (uint32_t layout : m_layoutsInPreorder) {
            if (m_leafOfLayout[layout] != noLeaf || !m_mentionsAlone[layout] || !hasShape(LayoutID(layout)))
                continue;
            groups.append(Group { .firstPosition = static_cast<uint32_t>(layoutsInOrder.size()), .numberOfLayouts = 1, .mentions = m_mentionsAlone[layout] });
            layoutsInOrder.append(layout);
        }

        AOT::sort(groups, [](const Group& a, const Group& b) {
            UInt128 mentionsOfA = static_cast<UInt128>(a.mentions) * static_cast<UInt128>(b.numberOfLayouts);
            UInt128 mentionsOfB = static_cast<UInt128>(b.mentions) * static_cast<UInt128>(a.numberOfLayouts);
            if (mentionsOfA != mentionsOfB)
                return mentionsOfA > mentionsOfB;
            return a.firstPosition < b.firstPosition;
        });
        m_frontierByMentionsPerShape.reserveInitialCapacity(m_numberOfLayouts);
        auto holdsPositionThatBeginsOrder = [&](const Group& group) {
            return positionThatBeginsOrder && *positionThatBeginsOrder - group.firstPosition < group.numberOfLayouts;
        };
        for (const Group& group : groups) {
            if (!holdsPositionThatBeginsOrder(group))
                continue;
            uint32_t numberInFront = *positionThatBeginsOrder - group.firstPosition;
            m_frontierByMentionsPerShape.append(layoutsInOrder.span().subspan(*positionThatBeginsOrder, group.numberOfLayouts - numberInFront));
            m_frontierByMentionsPerShape.append(layoutsInOrder.span().subspan(group.firstPosition, numberInFront));
        }
        for (const Group& group : groups) {
            if (!holdsPositionThatBeginsOrder(group))
                m_frontierByMentionsPerShape.append(layoutsInOrder.span().subspan(group.firstPosition, group.numberOfLayouts));
        }
    }

    const Program& m_program;
    const Facts& m_facts;
    const StorageOfLayouts& m_storage;
    const Families& m_families;
    const LinkTables& m_linked;
    uint32_t m_numberOfLayouts;
    Vector<uint32_t> m_layoutsInPreorder;
    Vector<uint32_t> m_preorder;
    Vector<WrittenSet> m_writtenSets;
    HashMap<uint32_t, uint32_t> m_placesOfWrittenSets;
    Vector<uint32_t> m_mentionsAlone;
    Vector<MentionedSet> m_sets;
    Vector<uint32_t> m_leavesOfSets;
    Vector<uint32_t> m_leafOfLayout;
    Vector<uint32_t> m_layoutOfLeaf;
    Vector<uint32_t> m_frontierByMentionsPerShape;
};

} // anonymous namespace

Vector<uint32_t> orderOfLayoutsForShapeNumbers(Program& program, const Facts& facts, const StorageOfLayouts& storage)
{
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedOperations))
        return { };
    return OrderOfShapesInUnionShapeNumbers(program, facts, storage).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
