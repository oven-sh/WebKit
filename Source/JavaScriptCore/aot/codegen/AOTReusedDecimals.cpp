/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReusedDecimals.h"

#if ENABLE(AOT)

#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class FinderOfReusedDecimals {
public:
    struct Candidate {
        ValueID value;
        BlockID home;
        // 0 for a parameter.
        uint32_t position;
        uint32_t group;
        BitVector isAliveAtHead { };
        BitVector isAliveAtEnd { };
        Vector<std::pair<BlockID, uint32_t>, 4> reads { };
        Vector<std::pair<BlockID, BlockID>, 2> placesWhereLetGo { };
        bool isRefused { false };

        bool isParameter() const { return !position; }
    };

    FinderOfReusedDecimals(const Code& code, const DecimalRuns& runs, std::span<const BlockID> order)
        : m_code(code)
        , m_runs(runs)
        , m_order(order)
    {
    }

    bool findCandidates()
    {
        // They are compared with one another.
        constexpr size_t mostCandidates = 256;
        bool someAreMadeInLoops = false;
        for (BlockID id : m_order) {
            const Block& block = m_code.at(id);
            auto add = [&](ValueID value, uint32_t position) {
                m_places.add(value.index(), m_candidates.size());
                m_candidates.append({ value, id, position, static_cast<uint32_t>(m_candidates.size()) });
            };
            for (ValueID parameter : block.parameters()) {
                if (!block.isEntry && !block.isHandlerBlock() && m_code.isNeeded(parameter) && m_runs.isLikelyDecimal(parameter))
                    add(parameter, 0);
            }
            std::span<const ValueID> values = m_code.valuesOf(id);
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (!m_runs.makesDecimal(values[index]))
                    continue;
                add(values[index], index + 1);
                someAreMadeInLoops |= block.isInLoop;
            }
        }
        return someAreMadeInLoops && m_candidates.size() <= mostCandidates;
    }

    void findLoops(const ScopedLambda<bool(BlockID, BlockID)>& dominates)
    {
        m_predecessors.grow(m_code.numberOfBlocks());
        for (BlockID id : m_order) {
            forEachEdgeFrom(id, [&](const Edge& edge) {
                m_predecessors[edge.to.index()].append(id);
            });
        }

        // An inner loop comes behind those around it.
        m_innermostLoops.fill(notFound, m_code.numberOfBlocks());
        for (BlockID head : m_order) {
            if (!m_code.at(head).isLoopHead)
                continue;
            BitVector loop;
            loop.set(head.index());
            Vector<BlockID, 16> pending { head };
            while (!pending.isEmpty()) {
                BlockID block = pending.takeLast();
                m_innermostLoops[block.index()] = m_loops.size();
                for (BlockID predecessor : m_predecessors[block.index()]) {
                    if ((block != head || dominates(head, predecessor)) && !loop.set(predecessor.index()))
                        pending.append(predecessor);
                }
            }
            m_loops.append(WTF::move(loop));
        }
    }

    void findReads()
    {
        for (BlockID id : m_order) {
            const Block& block = m_code.at(id);
            std::span<const ValueID> values = m_code.valuesOf(id);
            for (uint32_t index = 0; index < values.size(); ++index) {
                m_code.at(values[index]).forEachChild([&](ValueID operand) {
                    Candidate* candidate = find(operand);
                    if (!candidate)
                        return;
                    noteRead(*candidate, id, index + 1);
                    if (!m_runs.onlyReadsWordsOf(values[index], operand))
                        noteLetGo(*candidate, id, id);
                });
            }
            block.terminator.forEachChild([&](ValueID operand) {
                if (Candidate* candidate = find(operand)) {
                    noteRead(*candidate, id, behindAllValues);
                    noteLetGo(*candidate, id, id);
                }
            });
            forEachEdgeFrom(id, [&](const Edge& edge) {
                bool isTakenByValuesOnly = block.terminator.kind == TerminatorKind::Jump && edge.to != m_code.at(block.terminator.next()).to;
                for (ValueID argument : edge.arguments()) {
                    if (Candidate* candidate = find(argument))
                        noteRead(*candidate, id, isTakenByValuesOnly ? values.size() : behindAllValues);
                }
            });
            if (Candidate* candidate = find(block.subject))
                candidate->isRefused = true;
        }
    }

    void findGroups()
    {
        for (BlockID id : m_order) {
            forEachEdgeFrom(id, [&](const Edge& edge) {
                std::span<const ValueID> arguments = edge.arguments();
                std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
                for (size_t index = 0; index < arguments.size(); ++index) {
                    Candidate* argument = find(arguments[index]);
                    if (!argument || !m_code.isNeeded(parameters[index]))
                        continue;
                    Candidate* parameter = find(parameters[index]);
                    if (parameter && !argument->isAliveAtHead.get(edge.to.index()) && std::ranges::count(arguments, argument->value) == 1)
                        groupOf(*argument).group = groupOf(*parameter).group;
                    else
                        noteLetGo(*argument, id, edge.to);
                }
            });
        }
    }

    void forEachGroupWorthIt(NOESCAPE const Invocable<void(std::span<Candidate* const>)> auto& functor)
    {
        Vector<Vector<Candidate*, 4>> groups(m_candidates.size());
        for (Candidate& candidate : m_candidates)
            groups[groupOf(candidate).group].append(&candidate);
        for (auto& group : groups) {
            if (!group.isEmpty() && isWorthIt(group.span()) && takeTurns(group.span()))
                functor(group.span());
        }
    }

private:
    static constexpr uint32_t behindAllValues = std::numeric_limits<uint32_t>::max();

    Candidate* find(ValueID value)
    {
        if (!value)
            return nullptr;
        auto found = m_places.find(value.index());
        return found == m_places.end() ? nullptr : &m_candidates[found->value];
    }

    Candidate& groupOf(Candidate& candidate)
    {
        Candidate* root = &candidate;
        while (&m_candidates[root->group] != root)
            root = &m_candidates[root->group];
        candidate.group = root->group;
        return *root;
    }

    void forEachEdgeFrom(BlockID id, NOESCAPE const Invocable<void(const Edge&)> auto& functor) const
    {
        const Terminator& terminator = m_code.at(id).terminator;
        for (uint32_t number = 0; number < terminator.numberOfEdges; ++number)
            functor(m_code.at(terminator.edge(number)));
        if (EdgeID handler = m_code.handlerOf(id))
            functor(m_code.at(handler));
    }

    // What a handler reads is alive up to the end of the blocks that lead to it.
    void noteRead(Candidate& candidate, BlockID block, uint32_t position)
    {
        candidate.reads.append({ block, position });
        Vector<BlockID, 16> pending { block };
        while (!pending.isEmpty()) {
            BlockID reached = pending.takeLast();
            if (reached == candidate.home || candidate.isAliveAtHead.set(reached.index()))
                continue;
            for (BlockID predecessor : m_predecessors[reached.index()]) {
                candidate.isAliveAtEnd.set(predecessor.index());
                pending.append(predecessor);
            }
        }
    }

    void noteLetGo(Candidate& candidate, BlockID from, BlockID to)
    {
        if (m_code.at(from).frequency == BlockFrequency::Normal && m_code.at(to).frequency == BlockFrequency::Normal)
            candidate.placesWhereLetGo.append({ from, to });
    }

    // A loop that makes a value and lets the object go loses by it, which is made up for by a loop inside.
    bool isWorthIt(std::span<Candidate* const> group) const
    {
        Vector<BlockID, 4> gains;
        Vector<const BitVector*, 4> losses;
        for (const Candidate* candidate : group) {
            if (candidate->isRefused)
                return false;
            size_t place = m_innermostLoops[candidate->home.index()];
            if (candidate->isParameter() || place == notFound || m_code.at(candidate->home).frequency != BlockFrequency::Normal)
                continue;
            const BitVector& loop = m_loops[place];
            bool letsGo = std::ranges::any_of(group, [&](const Candidate* other) {
                return std::ranges::any_of(other->placesWhereLetGo, [&](auto& way) {
                    return loop.get(way.first.index()) && loop.get(way.second.index());
                });
            });
            if (letsGo)
                losses.append(&loop);
            else if (group.size() > 1 || !candidate->isAliveAtHead.isEmpty())
                gains.append(candidate->home);
        }
        return !gains.isEmpty() && std::ranges::all_of(losses, [&](const BitVector* loop) {
            return std::ranges::any_of(gains, [&](BlockID block) { return loop->get(block.index()); });
        });
    }

    static bool isAliveBehind(const Candidate& candidate, BlockID block, uint32_t position)
    {
        if (candidate.home == block ? candidate.position > position : !candidate.isAliveAtHead.get(block.index()))
            return false;
        return candidate.isAliveAtEnd.get(block.index()) || std::ranges::any_of(candidate.reads, [&](auto& read) {
            return read.first == block && read.second > position;
        });
    }

    static bool takeTurns(std::span<Candidate* const> group)
    {
        return std::ranges::none_of(group, [&](const Candidate* candidate) {
            return std::ranges::any_of(group, [&](const Candidate* other) {
                return other != candidate && isAliveBehind(*other, candidate->home, candidate->position);
            });
        });
    }

    const Code& m_code;
    const DecimalRuns& m_runs;
    std::span<const BlockID> m_order;
    Vector<Candidate, 16> m_candidates;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_places;
    Vector<Vector<BlockID, 2>> m_predecessors;
    Vector<BitVector, 4> m_loops;
    Vector<size_t> m_innermostLoops;
};

} // anonymous namespace

ReusedDecimals::ReusedDecimals(const Code& code, const DecimalRuns& runs, std::span<const BlockID> order, const ScopedLambda<bool(BlockID, BlockID)>& dominates)
{
    FinderOfReusedDecimals finder(code, runs, order);
    if (!finder.findCandidates())
        return;
    finder.findLoops(dominates);
    finder.findReads();
    finder.findGroups();
    finder.forEachGroupWorthIt([&](auto group) {
        for (auto* candidate : group) {
            m_values.set(candidate->value.index());
            m_objects.append({ candidate->value, m_numberOfObjects });
        }
        ++m_numberOfObjects;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
