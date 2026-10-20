/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSinkEquations.h"

#if ENABLE(AOT)

#include "AOTStronglyConnectedComponents.h"
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses contentsUses {
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::PassesToUserCode,
};

OperandUses applyTransform(SinkTransform transform, OperandUses uses)
{
    switch (transform) {
    case SinkTransform::Same:
    case SinkTransform::IfReturned:
        return uses;
    case SinkTransform::Argument:
        return uses - OperandUse::Returns;
    case SinkTransform::Leaves: {
        OperandUses result = uses - OperandUse::Returns;
        if (uses.contains(OperandUse::Returns))
            result.add(OperandUse::Retains);
        return result;
    }
    case SinkTransform::Lifetime: {
        if (!uses.containsAny(escapingUses))
            return { };
        OperandUses result { OperandUse::Retains };
        if (uses.contains(OperandUse::PassesToUserCode))
            result.add(OperandUse::PassesToUserCode);
        return result;
    }
    case SinkTransform::Contents: {
        if (uses.containsOnly(OperandUse::Inspects))
            return { };
        OperandUses result = (uses & contentsUses) | OperandUses { OperandUse::AppliesTransitively };
        if (uses.containsAny(escapingUses))
            result.add(OperandUse::RetainsContents);
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t conditionOf(const SinkDependency& dependency)
{
    return dependency.transform == SinkTransform::IfReturned ? dependency.condition : dependency.node;
}

namespace {

struct SinkDependencyNodes {
    std::span<const SinkDependency> dependencies;

    size_t size() const { return dependencies.size() * 2; }
    uint32_t operator[](size_t index) const { return index % 2 ? conditionOf(dependencies[index / 2]) : dependencies[index / 2].node; }
};

class SinkSolver {
    WTF_MAKE_NONCOPYABLE(SinkSolver);
public:
    explicit SinkSolver(SinkEquations& equations)
        : m_equations(equations)
        , m_components(computeStronglyConnectedComponents(equations.size(), [&](unsigned node) {
            return SinkDependencyNodes { equations.dependenciesOf(node) };
        }))
        , m_isQueued(equations.size())
    {
    }

    void solve()
    {
        buildComponentMembers();
        buildUsersInCycles();
        evaluateComponents();

        Vector<OperandUses> sinks = m_equations.sinks;
        for (uint32_t node = 0; node < m_equations.size(); ++node)
            m_equations.sinks[node].add(m_equations.baseOfFramePointers[node]);
        evaluateComponents();
        m_equations.sinksOfFramePointers = std::exchange(m_equations.sinks, WTF::move(sinks));
    }

private:
    void evaluateComponents()
    {
        for (uint32_t component = 0; component < m_components.numberOfComponents; ++component) {
            uint32_t first = m_memberOffsets[component];
            std::span<const uint32_t> members = m_members.subspan(first, m_memberOffsets[component + 1] - first);
            if (m_components.isInCycle.quickGet(members[0]))
                evaluateCycle(members);
            else
                evaluate(members[0]);
        }
    }

    void buildComponentMembers()
    {
        m_memberOffsets.fill(0, m_components.numberOfComponents + 1);
        for (uint32_t component : m_components.componentOf)
            ++m_memberOffsets[component + 1];
        for (uint32_t component = 0; component < m_components.numberOfComponents; ++component)
            m_memberOffsets[component + 1] += m_memberOffsets[component];

        Vector<uint32_t> cursors = m_memberOffsets;
        m_members.grow(m_equations.size());
        for (uint32_t node = 0; node < m_equations.size(); ++node)
            m_members[cursors[m_components.componentOf[node]]++] = node;
    }

    void forEachDependencyInCycle(NOESCAPE const Invocable<void(uint32_t user, uint32_t dependency)> auto& functor) const
    {
        const Vector<uint32_t>& componentOf = m_components.componentOf;
        for (uint32_t user = 0; user < m_equations.size(); ++user) {
            for (const SinkDependency& dependency : m_equations.dependenciesOf(user)) {
                uint32_t condition = conditionOf(dependency);
                if (componentOf[dependency.node] == componentOf[user])
                    functor(user, dependency.node);
                if (condition != dependency.node && componentOf[condition] == componentOf[user])
                    functor(user, condition);
            }
        }
    }

    void buildUsersInCycles()
    {
        m_userOffsets.fill(0, m_equations.size() + 1);
        forEachDependencyInCycle([&](uint32_t, uint32_t dependency) {
            ++m_userOffsets[dependency + 1];
        });
        for (uint32_t node = 0; node < m_equations.size(); ++node)
            m_userOffsets[node + 1] += m_userOffsets[node];

        Vector<uint32_t> cursors = m_userOffsets;
        m_users.grow(m_userOffsets.last());
        forEachDependencyInCycle([&](uint32_t user, uint32_t dependency) {
            m_users[cursors[dependency]++] = user;
        });
    }

    bool evaluate(uint32_t node)
    {
        Vector<OperandUses>& sinks = m_equations.sinks;
        OperandUses uses = m_equations.base[node] | sinks[node];
        for (const SinkDependency& dependency : m_equations.dependenciesOf(node)) {
            if (dependency.transform == SinkTransform::IfReturned && !sinks[dependency.condition].contains(OperandUse::Returns))
                continue;
            uses.add(applyTransform(dependency.transform, sinks[dependency.node]));
        }
        if (uses == sinks[node])
            return false;
        sinks[node] = uses;
        return true;
    }

    void evaluateCycle(std::span<const uint32_t> members)
    {
        Vector<uint32_t> worklist;
        worklist.append(members);
        for (uint32_t member : members)
            m_isQueued.quickSet(member);

        while (!worklist.isEmpty()) {
            uint32_t node = worklist.takeLast();
            m_isQueued.quickClear(node);
            if (!evaluate(node))
                continue;
            for (uint32_t index = m_userOffsets[node]; index < m_userOffsets[node + 1]; ++index) {
                uint32_t user = m_users[index];
                if (!m_isQueued.quickGet(user)) {
                    m_isQueued.quickSet(user);
                    worklist.append(user);
                }
            }
        }
    }

    SinkEquations& m_equations;
    StronglyConnectedComponents m_components;
    Vector<uint32_t> m_memberOffsets;
    Vector<uint32_t> m_members;
    Vector<uint32_t> m_userOffsets;
    Vector<uint32_t> m_users;
    BitVector m_isQueued;
};

} // anonymous namespace

void solveSinkEquations(SinkEquations& equations)
{
    RELEASE_ASSERT(equations.base.size() == equations.sinks.size() && equations.baseOfFramePointers.size() == equations.sinks.size() && equations.offsets.size() == equations.sinks.size() + 1);
    SinkSolver(equations).solve();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
