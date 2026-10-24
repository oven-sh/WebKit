/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestStretches.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static uint32_t firstValueAfter(const TestEvent& event)
{
    switch (event.kind) {
    case TestEvent::Kind::ParameterProof:
    case TestEvent::Kind::Barrier:
    case TestEvent::Kind::Requirement:
        return event.index;
    case TestEvent::Kind::Ending:
    case TestEvent::Kind::Suspension:
    case TestEvent::Kind::ValueProof:
        return event.index + 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool haveCommonBit(const BitVector& a, const BitVector& b)
{
    for (size_t bit : a) {
        if (b.get(bit))
            return true;
    }
    return false;
}

static void markTestPrerequisites(const TestEvents& events, uint32_t requirement, BitVector& wanted)
{
    forEachTestPrerequisite(events.requirements(), events.conditions(), requirement, [&](const TestPrerequisite& prerequisite) {
        wanted.set(prerequisite.condition.index());
    });
}

WantedProofs::WantedProofs(const TestEvents& events, const TestCoverage& coverage)
    : m_events(events)
    , m_coverage(coverage)
    , m_wantedAtHead(events.cfg().numNodes())
    , m_wantedAtTail(events.cfg().numNodes())
    , m_wantedHoistedTests(events.hoistedTests().size())
    , m_wantedProofs(events.proofs().size())
    , m_valuesInStretch(events.function().values.size())
    , m_headsInStretch(events.cfg().numNodes())
    , m_terminatorsInStretch(events.cfg().numNodes())
{
    const CFG& cfg = m_events.cfg();
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : cfg.reversePostOrder() | std::views::reverse) {
            BitVector wanted = wantedAtTail(block, Recording::Off);
            walkBackward(block, wanted, Recording::Off);
            if (wanted != m_wantedAtHead[block.index()]) {
                m_wantedAtHead[block.index()] = WTF::move(wanted);
                changed = true;
            }
        }
        // Without a cycle a block comes behind every block that it leads to, so another round would find the same.
        changed &= cfg.hasCycle();
    }
    RELEASE_ASSERT(m_wantedAtHead[cfg.root().index()].isEmpty());

    for (BlockID block : cfg.reversePostOrder()) {
        BitVector wanted = wantedAtTail(block, Recording::On);
        if (!wanted.isEmpty())
            m_terminatorsInStretch.set(block.index());
        m_wantedAtTail[block.index()] = wanted;
        walkBackward(block, wanted, Recording::On);
        ASSERT(wanted == m_wantedAtHead[block.index()]);
        if (!m_wantedAtHead[block.index()].isEmpty())
            m_headsInStretch.set(block.index());
    }
}

// The points of one block are asked for one after another, so one walk over the events of the block answers for all of them.
bool WantedProofs::isAnyProofWantedAt(ProgramPoint point)
{
    std::span<const TestEvent> events = m_events.eventsOf(point.block);
    if (m_blockOfEventFlags != point.block) {
        m_blockOfEventFlags = point.block;
        m_isProofWantedInFrontOfEvent.clearAll();
        BitVector wanted = m_wantedAtTail[point.block.index()];
        for (size_t index = events.size();; --index) {
            if (!wanted.isEmpty())
                m_isProofWantedInFrontOfEvent.set(index);
            if (!index)
                break;
            applyEventBackward(point.block, events[index - 1], wanted, Recording::Off);
        }
    }
    size_t firstEventBehindPoint = std::ranges::lower_bound(events, point.index, { }, &TestEvent::index) - events.begin();
    return m_isProofWantedInFrontOfEvent.get(firstEventBehindPoint);
}

BitVector WantedProofs::wantedAtTail(BlockID block, Recording recording)
{
    BitVector wanted;
    uint32_t numberOfTargets = m_events.function().at(block).terminator.targets.size();
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex)
        wanted.merge(wantedOn(Edge { block, targetIndex }, recording));
    return wanted;
}

BitVector WantedProofs::wantedOn(Edge edge, Recording recording)
{
    if (edge.isHandlerEdge())
        return { };

    bool isRecording = recording == Recording::On;
    BitVector wanted = m_wantedAtHead[targetOf(m_events.function(), edge).block.index()];

    // An edge that has left stays out, so that the solve ends.
    if (const BitVector* missing = m_coverage.missingOn(edge)) {
        bool leaves = m_edgesIntoGenericCopy.contains(edge);
        if (!leaves && haveCommonBit(wanted, *missing)) {
            m_edgesIntoGenericCopy.append(edge);
            leaves = true;
        }
        if (leaves)
            return { };
    }

    applyHoistedTestsBackward(edge, wanted, recording);
    m_events.forEachProofOn(edge, [&](uint32_t proof) {
        if (wanted.clear(m_events.proofs()[proof].condition.index()) && isRecording)
            m_wantedProofs.set(proof);
    });
    return wanted;
}

void WantedProofs::applyHoistedTestsBackward(Edge edge, BitVector& wanted, Recording recording)
{
    Vector<uint32_t, 4> hoistedTests;
    m_events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
        hoistedTests.append(hoistedTest);
    });
    for (uint32_t hoistedTest : hoistedTests | std::views::reverse) {
        switch (m_coverage.ofHoistedTest(hoistedTest)) {
        case RequirementCoverage::Unknown:
        case RequirementCoverage::Covered:
            break;
        case RequirementCoverage::NeedsTest: {
            const HoistedTest& hoisted = m_events.hoistedTests()[hoistedTest];
            if (!wanted.clear(hoisted.condition.index()))
                break;
            if (recording == Recording::On)
                m_wantedHoistedTests.set(hoistedTest);
            markTestPrerequisites(m_events, hoisted.requirement, wanted);
            break;
        }
        }
    }
}

BitVector WantedProofs::wantedInFrontOfTestsOn(Edge edge)
{
    BitVector wanted = m_wantedAtHead[targetOf(m_events.function(), edge).block.index()];
    applyHoistedTestsBackward(edge, wanted, Recording::Off);
    return wanted;
}

void WantedProofs::walkBackward(BlockID block, BitVector& wanted, Recording recording)
{
    uint32_t end = m_events.function().at(block).values.size();
    auto markValuesFrom = [&](uint32_t begin) {
        if (begin >= end)
            return;
        if (recording == Recording::On && !wanted.isEmpty())
            markValues(block, begin, end);
        end = begin;
    };

    for (const TestEvent& event : m_events.eventsOf(block) | std::views::reverse) {
        markValuesFrom(firstValueAfter(event));
        applyEventBackward(block, event, wanted, recording);
    }
    markValuesFrom(0);
}

void WantedProofs::markValues(BlockID block, uint32_t begin, uint32_t end)
{
    const BlockValues& values = m_events.function().at(block).values;
    for (uint32_t index = begin; index < end; ++index)
        m_valuesInStretch.set(values[index].index());
}

void WantedProofs::applyEventBackward(BlockID block, const TestEvent& event, BitVector& wanted, Recording recording)
{
    bool isRecording = recording == Recording::On;
    switch (event.kind) {
    case TestEvent::Kind::ParameterProof:
    case TestEvent::Kind::ValueProof:
        if (wanted.clear(m_events.proofs()[event.payload].condition.index()) && isRecording)
            m_wantedProofs.set(event.payload);
        return;
    case TestEvent::Kind::Barrier:
    case TestEvent::Kind::Suspension:
        wanted.clearAll();
        return;
    case TestEvent::Kind::Ending:
        wanted.exclude(m_events.endedBy(event));
        return;
    case TestEvent::Kind::Requirement:
        break;
    }

    const TestRequirement& requirement = m_events.requirements()[event.payload];
    RELEASE_ASSERT(requirement.kind == RequirementKind::PlacesTest);
    if (isRecording)
        m_valuesInStretch.set(m_events.function().at(block).values[event.index].index());

    size_t condition = requirement.condition.index();
    switch (m_coverage.of(event.payload)) {
    case RequirementCoverage::Unknown:
        return;
    case RequirementCoverage::NeedsTest:
        wanted.clear(condition);
        return;
    case RequirementCoverage::Covered:
        wanted.set(condition);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
