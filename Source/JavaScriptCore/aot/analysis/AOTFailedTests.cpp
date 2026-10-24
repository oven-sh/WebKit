/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTSort.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t notTakenTargetIndex = 1;

static ValueID testedSubjectOf(FunctionAnalyses& analyses, const Value& test)
{
    if (!test.numberOfOperands())
        return { };
    ValueID subject = test.operand(0);
    return subject ? analyses.identityOf(subject) : ValueID();
}

FailedTests::FailedTests(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_function(analyses.function())
{
    const CFG& cfg = analyses.cfg();
    for (BlockID from : cfg.reversePostOrder()) {
        const Terminator& terminator = m_function.at(from).terminator;
        if (terminator.isConditionalBranch()) {
            ValueID id = terminator.conditionOperand();
            const Value& test = m_function.at(id);
            BlockID target = terminator.notTaken().block;
            if (provesOf(test.opcode) != Proof::None && cfg.singleIncomingEdge(target) == Edge { from, notTakenTargetIndex })
                m_failedEdges.append(FailedEdge { test.opcode, testedSubjectOf(analyses, test), id, target });
            continue;
        }
        if (terminator.kind != TerminatorKind::SwitchOnShape)
            continue;
        uint32_t otherwiseTargetIndex = terminator.targets.size() - 1;
        BlockID target = terminator.otherwise().block;
        if (cfg.singleIncomingEdge(target) == Edge { from, otherwiseTargetIndex })
            m_failedSwitches.append(FailedSwitch { analyses.identityOf(terminator.subjectOperand()), from, target });
    }

    AOT::sort(m_failedEdges, { }, [](const FailedEdge& edge) {
        return std::tuple { edge.opcode, edge.subject, edge.target };
    });
    AOT::sort(m_failedSwitches, { }, [](const FailedSwitch& entry) {
        return std::pair { entry.subject, entry.target };
    });
}

// What is none of several types is not one of them.
static bool failsWhereOtherFailed(FunctionAnalyses& analyses, const Value& test, const Value& failed)
{
    if (haveSameCondition(analyses, failed, test))
        return true;
    const Program& program = analyses.program();
    return test.opcode == Opcode::TypeTest && failed.opcode == Opcode::TypeTest && program.at(test.testedType()).isSubsetOf(program.at(failed.testedType()), program);
}

EarlierTest FailedTests::find(const Value& test, BlockID at) const
{
    // Where no test has failed nobody needs the identity of the subject or the dominators to know it.
    if (m_failedEdges.isEmpty() && m_failedSwitches.isEmpty())
        return EarlierTest::Unknown;
    if (!m_analyses.cfg().isReachable(at))
        return EarlierTest::Unknown;

    const Dominators& dominators = m_analyses.dominators();
    ValueID subject = testedSubjectOf(m_analyses, test);

    auto failedEdges = std::ranges::equal_range(m_failedEdges, std::pair { test.opcode, subject }, { }, [](const FailedEdge& edge) {
        return std::pair { edge.opcode, edge.subject };
    });
    for (const FailedEdge& edge : failedEdges) {
        if (failsWhereOtherFailed(m_analyses, test, m_function.at(edge.test)) && dominators.dominates(edge.target, at))
            return EarlierTest::Failed;
    }
    if (provesOf(test.opcode) != Proof::Shape)
        return EarlierTest::Unknown;

    for (const FailedSwitch& entry : std::ranges::equal_range(m_failedSwitches, subject, { }, &FailedSwitch::subject)) {
        if (dominators.dominates(entry.target, at) && isCoveredBySwitch(test, entry))
            return EarlierTest::Failed;
    }
    return EarlierTest::Unknown;
}

bool FailedTests::hasTestOf(ValueID value) const
{
    if (m_failedEdges.isEmpty() && m_failedSwitches.isEmpty())
        return false;
    ValueID subject = m_analyses.identityOf(value);
    return m_failedEdges.containsIf([&](const FailedEdge& edge) {
        return edge.subject == subject;
    }) || m_failedSwitches.containsIf([&](const FailedSwitch& entry) {
        return entry.subject == subject;
    });
}

bool FailedTests::isCoveredBySwitch(const Value& shapeTest, const FailedSwitch& entry) const
{
    const Program& program = m_analyses.program();
    std::span<const LayoutSetID> cases = m_function.at(entry.from).terminator.layoutSets();
    return std::ranges::all_of(program.at(shapeTest.layouts()).members, [&](LayoutID layout) {
        return std::ranges::any_of(cases, [&](LayoutSetID set) {
            return std::ranges::binary_search(program.at(set).members, layout);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
