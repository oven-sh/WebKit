/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTGenericCopy.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class TestHoister {
    WTF_MAKE_NONCOPYABLE(TestHoister);
public:
    TestHoister(FunctionPassContext& context, const TestSites& sites, const TestConditions& conditions, std::span<const TestRequirement> requirements)
        : m_program(context.program)
        , m_function(context.function)
        , m_types(context.types)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
        , m_loops(context.analyses.naturalLoops())
        , m_useLists(context.analyses.useLists())
        , m_frequencies(context.analyses.blockFrequencies())
        , m_sites(sites)
        , m_conditions(conditions)
        , m_requirements(requirements)
        , m_entries(m_loops.numLoops())
    {
    }

    Vector<HoistedTest> run();

private:
    using Operands = Vector<ValueID, 2>;
    using SiteOperands = Vector<std::optional<Operands>, 4>;

    struct LoopEntry {
        Vector<Edge, 2> edges;
        uint32_t firstEdgeNumber { 0 };
        double highestSourceFrequency { 0 };
    };

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_types.at(m_function.at(value).type); }

    void offerTestsOfSite(uint32_t firstRequirement, uint32_t numberOfRequirements);
    const LoopEntry& entryOf(const NaturalLoop&) LIFETIME_BOUND;
    std::optional<Operands> findOperandsOutside(std::span<const TestRequirement> site, uint32_t index, const SiteOperands& earlierOperands, const NaturalLoop&) const;
    ValueID findDefinitionOutside(ValueID, const NaturalLoop&) const;

    const Program& m_program;
    const Function& m_function;
    const TypeArena& m_types;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const NaturalLoops& m_loops;
    const UseLists& m_useLists;
    const BlockFrequencies& m_frequencies;
    const TestSites& m_sites;
    const TestConditions& m_conditions;
    std::span<const TestRequirement> m_requirements;
    Vector<std::optional<LoopEntry>> m_entries;
    uint32_t m_numberOfEnteringEdges { 0 };
    BitVector m_isOffered;
    Vector<HoistedTest> m_hoistedTests;
};

Vector<HoistedTest> TestHoister::run()
{
    for (uint32_t first = 0; first < m_requirements.size();) {
        uint32_t end = first + 1;
        while (end < m_requirements.size() && m_requirements[end].site == m_requirements[first].site)
            ++end;
        offerTestsOfSite(first, end - first);
        first = end;
    }
    return WTF::move(m_hoistedTests);
}

void TestHoister::offerTestsOfSite(uint32_t firstRequirement, uint32_t numberOfRequirements)
{
    std::span<const TestRequirement> site = m_requirements.subspan(firstRequirement, numberOfRequirements);
    BlockID useBlock = site.front().point.block;

    for (const NaturalLoop* loop = m_loops.innerMostLoopOf(useBlock); loop; loop = m_loops.innerMostOuterLoop(*loop)) {
        if (isClosedOnlyByWaysBack(m_analyses, *loop))
            continue;

        SiteOperands operands;
        bool canLeaveLoop = false;
        for (uint32_t index = 0; index < numberOfRequirements; ++index) {
            operands.append(findOperandsOutside(site, index, operands, *loop));
            canLeaveLoop |= operands.last().has_value();
        }
        if (!canLeaveLoop)
            return;

        const LoopEntry& entry = entryOf(*loop);
        if (entry.highestSourceFrequency > m_frequencies.frequencyOf(useBlock))
            continue;

        for (uint32_t edgeIndex = 0; edgeIndex < entry.edges.size(); ++edgeIndex) {
            size_t firstBit = static_cast<size_t>(entry.firstEdgeNumber + edgeIndex) * m_conditions.size();
            for (uint32_t index = 0; index < numberOfRequirements; ++index) {
                TestConditionID condition = site[index].condition;
                if (!operands[index] || !m_isOffered.add(firstBit + condition.index()))
                    continue;
                m_hoistedTests.append(HoistedTest { entry.edges[edgeIndex], firstRequirement + index, condition, *operands[index] });
            }
        }
    }
}

auto TestHoister::entryOf(const NaturalLoop& loop) -> const LoopEntry&
{
    std::optional<LoopEntry>& entry = m_entries[loop.index()];
    if (entry)
        return *entry;

    entry.emplace();
    entry->firstEdgeNumber = m_numberOfEnteringEdges;
    for (Edge edge : m_cfg.incomingEdges(loop.header())) {
        if (edge.isHandlerEdge() || m_loops.belongsTo(edge.from, loop))
            continue;
        entry->edges.append(edge);
        entry->highestSourceFrequency = std::max(entry->highestSourceFrequency, m_frequencies.frequencyOf(edge.from));
    }
    m_numberOfEnteringEdges += entry->edges.size();
    return *entry;
}

auto TestHoister::findOperandsOutside(std::span<const TestRequirement> site, uint32_t index, const SiteOperands& earlierOperands, const NaturalLoop& loop) const -> std::optional<Operands>
{
    const Value& test = m_sites.tests[site[index].test];
    const Value& canonicalTest = m_conditions.at(site[index].condition).test;
    std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);

    Operands operands;
    for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
        if (guardIndex == operandIndex)
            continue;

        ValueID subject = canonicalTest.operand(operandIndex);
        bool isNarrowedByEarlierTest = false;
        for (uint32_t earlier = 0; earlier < index; ++earlier) {
            const TestCondition& condition = m_conditions.at(site[earlier].condition);
            if (condition.subject != subject)
                continue;
            if (!earlierOperands[earlier])
                return std::nullopt;
            isNarrowedByEarlierTest |= condition.lastsForever();
        }

        ValueID operand = test.operand(operandIndex);
        ValueID outside = findDefinitionOutside(operand, loop);
        if (!outside)
            return std::nullopt;
        // Inside the loop the test stands behind whatever decides that the value is there, the switch on the kind of an iteration for one. In front of the loop it would read through whatever the register holds.
        if (mayBeUnspecified(m_analyses, outside))
            return std::nullopt;

        bool canReplaceOperand = outside == operand
            || isNarrowedByEarlierTest
            || test.operandKind(operandIndex) == OperandKind::Any
            || typeOf(outside).isSubsetOf(typeOf(operand), m_program);
        if (!canReplaceOperand)
            return std::nullopt;
        operands.append(outside);
    }
    return operands;
}

ValueID TestHoister::findDefinitionOutside(ValueID value, const NaturalLoop& loop) const
{
    for (ValueID candidate : CastChain(m_function, value)) {
        ValueLocation location = m_useLists.locationOf(candidate);
        if (location.placement != ValuePlacement::Detached && !m_loops.belongsTo(location.block, loop))
            return candidate;
    }
    return { };
}

} // anonymous namespace

Vector<HoistedTest> findHoistedTests(FunctionPassContext& context, const TestSites& sites, const TestConditions& conditions, std::span<const TestRequirement> requirements)
{
    if (!context.analyses.cfg().hasCycle() || !context.analyses.naturalLoops().numLoops())
        return { };
    return TestHoister(context, sites, conditions, requirements).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
