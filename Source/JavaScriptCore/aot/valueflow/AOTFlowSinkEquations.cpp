/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSinkEquations.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <optional>
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
    case SinkTransform::RepeatedArgument: {
        OperandUses result = uses - OperandUse::Returns;
        if (uses.containsAny({ OperandUse::Calls, OperandUse::CallsPerElement }))
            result.add(OperandUse::CallsRepeatedly);
        return result;
    }
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
    case SinkTransform::Held: {
        OperandUses result = applyTransform(SinkTransform::Lifetime, uses);
        if (uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }))
            result.add(applyTransform(SinkTransform::Leaves, uses));
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

// Tarjan's algorithm, which finishes a strongly connected component after every component it depends on. Each component is solved as it finishes.
class SinkSolver {
    WTF_MAKE_NONCOPYABLE(SinkSolver);
public:
    SinkSolver(SinkEquations& equations, std::span<const uint32_t> nodesAskedFor, const ParallelLoops& loops)
        : m_equations(equations)
        , m_nodesAskedFor(nodesAskedFor)
        , m_loops(loops)
        , m_isQueued(equations.size())
    {
    }

    void solve()
    {
        solveComponents();
        if (m_equations.baseOfFramePointers.isEmpty())
            return;

        Vector<OperandUses> sinks(m_equations.sinks.span());
        for (uint32_t node = 0; node < m_equations.size(); ++node)
            m_equations.sinks[node].add(m_equations.baseOfFramePointers[node]);
        solveComponents();
        m_equations.sinksOfFramePointers = Vector<OperandUses>(m_equations.sinks.span());
        for (uint32_t node = 0; node < m_equations.size(); ++node)
            m_equations.sinks[node] = sinks[node];
    }

private:
    static constexpr uint32_t unvisited = 0;
    static constexpr uint32_t solved = UINT32_MAX;

    struct Frame {
        uint32_t node;
        uint32_t nextDependency;
        uint32_t lowLink;
        bool hasSelfEdge;
    };

    void solveComponents()
    {
        constexpr uint32_t nodesPerShare = 1 << 16;
        uint32_t numberOfNodes = m_equations.size();
        m_visitIndices = Vector<uint32_t>(numberOfNodes);
        forEachIndexInParallel(m_loops, (numberOfNodes + nodesPerShare - 1) / nodesPerShare, [&](unsigned share) {
            uint32_t first = share * nodesPerShare;
            std::ranges::fill(m_visitIndices.mutableSpan().subspan(first, std::min(nodesPerShare, numberOfNodes - first)), unvisited);
        });
        for (uint32_t root : m_nodesAskedFor) {
            if (m_visitIndices[root] == unvisited && !solveIfTrivial(root))
                visitFrom(root);
        }
    }

    // Dependencies add nothing to a node that already has every use.
    bool solveIfTrivial(uint32_t node)
    {
        OperandUses uses = m_equations.base[node] | m_equations.sinks[node];
        if (uses != OperandUses::all() && m_equations.offsets[node] != m_equations.offsets[node + 1])
            return false;
        m_equations.sinks[node] = uses;
        m_visitIndices[node] = solved;
        return true;
    }

    void visitFrom(uint32_t root)
    {
        beginVisit(root);
        while (!m_frames.isEmpty()) {
            Frame frame = m_frames.last();
            if (std::optional<uint32_t> dependency = findUnvisitedDependency(frame)) {
                m_frames.last() = frame;
                beginVisit(*dependency);
                continue;
            }
            m_frames.removeLast();
            finishVisit(frame);
        }
    }

    void beginVisit(uint32_t node)
    {
        uint32_t index = ++m_unsolvedCount;
        m_visitIndices[node] = index;
        m_frames.append(Frame { node, m_equations.offsets[node], index, false });
    }

    // Does not advance past a dependency until both of its nodes are visited, so the caller comes back to it after visiting one.
    std::optional<uint32_t> findUnvisitedDependency(Frame& frame)
    {
        auto needsVisit = [&](uint32_t target) {
            uint32_t index = m_visitIndices[target];
            if (index == unvisited)
                return !solveIfTrivial(target);
            if (index != solved) {
                frame.lowLink = std::min(frame.lowLink, index);
                frame.hasSelfEdge |= target == frame.node;
            }
            return false;
        };

        uint32_t end = m_equations.offsets[frame.node + 1];
        for (; frame.nextDependency < end; ++frame.nextDependency) {
            const SinkDependency& dependency = m_equations.dependencies[frame.nextDependency];
            if (needsVisit(dependency.node))
                return dependency.node;
            uint32_t condition = conditionOf(dependency);
            if (condition != dependency.node && needsVisit(condition))
                return condition;
        }
        return std::nullopt;
    }

    void finishVisit(const Frame& frame)
    {
        uint32_t index = m_visitIndices[frame.node];
        bool isComponentRoot = frame.lowLink == index;
        if (isComponentRoot && m_unsolvedCount == index && !frame.hasSelfEdge) {
            evaluate(frame.node);
            m_visitIndices[frame.node] = solved;
            m_unsolvedCount = index - 1;
            return;
        }

        m_componentStack.append(frame.node);
        if (!isComponentRoot) {
            Frame& parent = m_frames.last();
            parent.lowLink = std::min(parent.lowLink, frame.lowLink);
            return;
        }

        size_t firstMember = m_componentStack.size() - (m_unsolvedCount - index + 1);
        std::span<const uint32_t> members = m_componentStack.subspan(firstMember);
        evaluateCycle(members, index);
        for (uint32_t member : members)
            m_visitIndices[member] = solved;
        m_componentStack.shrink(firstMember);
        m_unsolvedCount = index - 1;
    }

    // Everything a member depends on is either solved or another member.
    void forEachDependencyInCycle(std::span<const uint32_t> members, NOESCAPE const Invocable<void(uint32_t user, uint32_t dependency)> auto& functor) const
    {
        for (uint32_t user : members) {
            for (const SinkDependency& dependency : m_equations.dependenciesOf(user)) {
                uint32_t condition = conditionOf(dependency);
                if (m_visitIndices[dependency.node] != solved)
                    functor(user, dependency.node);
                if (condition != dependency.node && m_visitIndices[condition] != solved)
                    functor(user, condition);
            }
        }
    }

    void buildUsersInCycle(std::span<const uint32_t> members, uint32_t firstVisitIndex)
    {
        m_userOffsets.fill(0, members.size() + 1);
        forEachDependencyInCycle(members, [&](uint32_t, uint32_t dependency) {
            ++m_userOffsets[m_visitIndices[dependency] - firstVisitIndex + 1];
        });
        for (size_t member = 0; member < members.size(); ++member)
            m_userOffsets[member + 1] += m_userOffsets[member];

        m_userCursors = m_userOffsets;
        m_users.resize(m_userOffsets.last());
        forEachDependencyInCycle(members, [&](uint32_t user, uint32_t dependency) {
            m_users[m_userCursors[m_visitIndices[dependency] - firstVisitIndex]++] = user;
        });
    }

    bool evaluate(uint32_t node)
    {
        ParallelArray<OperandUses>& sinks = m_equations.sinks;
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

    // The members come in postorder and their visit indices are consecutive from firstVisitIndex.
    void evaluateCycle(std::span<const uint32_t> members, uint32_t firstVisitIndex)
    {
        buildUsersInCycle(members, firstVisitIndex);
        auto evaluateAndQueueUsers = [&](uint32_t node) {
            m_isQueued.quickClear(node);
            if (!evaluate(node))
                return;
            uint32_t member = m_visitIndices[node] - firstVisitIndex;
            for (uint32_t index = m_userOffsets[member]; index < m_userOffsets[member + 1]; ++index) {
                uint32_t user = m_users[index];
                if (!m_isQueued.quickGet(user)) {
                    m_isQueued.quickSet(user);
                    m_worklist.append(user);
                }
            }
        };

        for (uint32_t member : members)
            m_isQueued.quickSet(member);
        for (uint32_t member : members)
            evaluateAndQueueUsers(member);
        while (!m_worklist.isEmpty())
            evaluateAndQueueUsers(m_worklist.takeLast());
    }

    SinkEquations& m_equations;
    std::span<const uint32_t> m_nodesAskedFor;
    const ParallelLoops& m_loops;
    // unvisited, solved, or the one-based rank of the node among the visited nodes that are not solved yet.
    Vector<uint32_t> m_visitIndices;
    uint32_t m_unsolvedCount { 0 };
    Vector<Frame> m_frames;
    Vector<uint32_t> m_componentStack;
    Vector<uint32_t> m_userOffsets;
    Vector<uint32_t> m_userCursors;
    Vector<uint32_t> m_users;
    Vector<uint32_t> m_worklist;
    BitVector m_isQueued;
};

} // anonymous namespace

void solveSinkEquations(SinkEquations& equations, std::span<const uint32_t> nodesAskedFor, const ParallelLoops& loops)
{
    RELEASE_ASSERT(equations.base.size() == equations.sinks.size() && equations.offsets.size() == equations.sinks.size() + 1);
    RELEASE_ASSERT(equations.baseOfFramePointers.isEmpty() || equations.baseOfFramePointers.size() == equations.sinks.size());
    SinkSolver(equations, nodesAskedFor, loops).solve();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
