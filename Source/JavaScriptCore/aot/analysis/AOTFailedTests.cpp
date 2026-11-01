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

    for (const FailedEdge& edge : m_failedEdges) {
        if (edge.subject)
            m_subjects.set(edge.subject.index());
    }
    for (const FailedSwitch& entry : m_failedSwitches)
        m_subjects.set(entry.subject.index());
    analyses.context().build.statistics().count(StatisticID::FailedTestsReadForSubjects, m_failedEdges.size() + m_failedSwitches.size());

    nestFailedEdges();
    AOT::sort(m_failedSwitches, { }, [](const FailedSwitch& entry) {
        return std::pair { entry.subject, entry.target };
    });
}

void FailedTests::nestFailedEdges()
{
    if (m_failedEdges.isEmpty())
        return;

    Vector<uint32_t> lastNumberBelow(FillWith { }, m_function.blocks.size(), 0u);
    m_numberInDominatorTree.fill(UINT32_MAX, m_function.blocks.size());
    uint32_t nextNumber = 0;
    m_analyses.dominators().walkTree([&](BlockID block) {
        m_numberInDominatorTree[block.index()] = nextNumber++;
    }, [&](BlockID block) {
        lastNumberBelow[block.index()] = nextNumber - 1;
    });

    AOT::sortInLine(m_failedEdges, { }, [&](const FailedEdge& edge) {
        return std::tuple { edge.opcode, edge.subject, m_numberInDominatorTree[edge.target.index()] };
    });

    Vector<uint32_t, 16> open;
    auto closeEdgesBefore = [&](uint32_t number) {
        while (!open.isEmpty() && lastNumberBelow[m_failedEdges[open.last()].target.index()] < number) {
            const FailedEdge& closed = m_failedEdges[open.takeLast()];
            m_boundaries.append({ lastNumberBelow[closed.target.index()] + 1, closed.enclosing });
        }
    };
    for (uint32_t index = 0; index < m_failedEdges.size(); ++index) {
        FailedEdge& edge = m_failedEdges[index];
        bool startsRun = !index || m_failedEdges[index - 1].opcode != edge.opcode || m_failedEdges[index - 1].subject != edge.subject;
        uint32_t number = m_numberInDominatorTree[edge.target.index()];
        closeEdgesBefore(startsRun ? UINT32_MAX : number);
        if (!open.isEmpty())
            edge.enclosing = open.last();
        open.append(index);
        m_boundaries.append({ number, index });
    }
    closeEdgesBefore(UINT32_MAX);
}

static bool failsWhereOtherFailed(FunctionAnalyses& analyses, const Value& test, const Value& failed)
{
    if (haveSameCondition(analyses, failed, test))
        return true;
    const Program& program = analyses.program();
    return test.opcode == Opcode::TypeTest && failed.opcode == Opcode::TypeTest && program.at(test.testedType()).isSubsetOf(program.at(failed.testedType()), program);
}

EarlierTest FailedTests::find(const Value& test, BlockID at) const
{
    if (m_failedEdges.isEmpty() && m_failedSwitches.isEmpty())
        return EarlierTest::Unknown;
    if (!m_analyses.cfg().isReachable(at))
        return EarlierTest::Unknown;

    const Dominators& dominators = m_analyses.dominators();
    ValueID subject = testedSubjectOf(m_analyses, test);

    auto failedEdges = std::ranges::equal_range(m_failedEdges, std::pair { test.opcode, subject }, { }, [](const FailedEdge& edge) {
        return std::pair { edge.opcode, edge.subject };
    });
    if (!failedEdges.empty()) {
        size_t first = failedEdges.begin() - m_failedEdges.begin();
        auto boundaries = m_boundaries.subspan(2 * first, 2 * failedEdges.size());
        auto boundary = std::ranges::upper_bound(boundaries, m_numberInDominatorTree[at.index()], { }, &Boundary::number);
        for (uint32_t index = boundary == boundaries.begin() ? noEdge : std::prev(boundary)->edge; index != noEdge; index = m_failedEdges[index].enclosing) {
            if (failsWhereOtherFailed(m_analyses, test, m_function.at(m_failedEdges[index].test)))
                return EarlierTest::Failed;
        }
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
    return m_subjects.get(m_analyses.identityOf(value).index());
}

bool FailedTests::isCoveredBySwitch(const Value& shapeTest, const FailedSwitch& entry) const
{
    const Program& program = m_analyses.program();
    std::span<const LayoutSetID> cases = m_function.at(entry.from).terminator.layoutSets();
    return std::ranges::all_of(program.at(shapeTest.layouts()).completeMembers(), [&](LayoutID layout) {
        return std::ranges::any_of(cases, [&](LayoutSetID set) {
            return std::ranges::binary_search(program.at(set).completeMembers(), layout);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
