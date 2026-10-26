/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3FixSSA.h"

#if ENABLE(B3_JIT)

#include "B3BreakCriticalEdges.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "CompilerTimingScope.h"
#include <wtf/CommaPrinter.h>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/IndexSparseSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3FixSSAInternal {
static constexpr bool verbose = false;
}

// Many variables often have trivial local liveness (i.e. within a single block), which we can resolve
// without global SSA. Returns true if no variables remain.
bool fixSSALocally(Procedure& proc, IndexSet<Value*>& valuesToDelete)
{
    IndexSparseSet<KeyValuePair<unsigned, Value*>> mapping(proc.variables().size());
    IndexSet<Variable*> liveVariables;
    Vector<Value*> sets;
    for (BasicBlock* block : proc) {
        mapping.clear();

        for (Value* value : *block) {
            switch (value->opcode()) {
            case Get: {
                Variable* variable = value->as<VariableValue>()->variable();
                if (KeyValuePair<unsigned, Value*>* replacement = mapping.get(variable->index())) {
                    value->replaceWithIdentity(replacement->value);
                    valuesToDelete.add(value);
                } else
                    liveVariables.add(variable);
                break;
            }

            case Set: {
                Variable* variable = value->as<VariableValue>()->variable();
                mapping.set(variable->index(), value->child(0)->foldIdentity());
                sets.append(value);
                break;
            }

            default:
                break;
            }
        }
    }

    for (Value* set : sets) {
        if (!liveVariables.contains(set->as<VariableValue>()->variable()))
            set->replaceWithNop();
    }

    if (liveVariables.isEmpty()) {
        proc.deleteAllVariables();
        return true;
    }

    for (Variable* variable : proc.variables()) {
        if (!liveVariables.contains(variable))
            proc.deleteVariable(variable);
    }
    return false;
}

// Answers which joins are in the dominance frontier of a block from a list of the edges into joins, in space proportional
// to the edges. A list of joins per block takes blocks x joins entries where each block of a long chain in the dominator
// tree has an edge to a join of its own that the head of the chain does not dominate.
class FrontierEdges {
public:
    // Blocks are named by their numbers in a preorder walk of the dominator tree, from 1 on. The join is in the frontier
    // of a block that dominates `from` and has a number above `bound`: the number of the join's immediate dominator, or
    // that of the edge before this one into the same join, which then answers in place of this one.
    struct Edge {
        unsigned from;
        unsigned bound;
        unsigned previousIntoJoin;
        BasicBlock* join;
    };

    static constexpr unsigned noEdge = UINT_MAX;

    void clear() { m_edges.shrink(0); }
    unsigned size() const { return m_edges.size(); }
    const Edge& at(unsigned position) const LIFETIME_BOUND { return m_edges[position]; }

    // In ascending order of `from`.
    void append(const Edge& edge) { m_edges.append(edge); }

    void finish()
    {
        m_leafCount = std::bit_ceil(std::max<unsigned>(m_edges.size(), 1));
        m_leastBound.fill(UINT_MAX, 2 * m_leafCount);
        for (unsigned position = 0; position < m_edges.size(); ++position)
            m_leastBound[m_leafCount + position] = m_edges[position].bound;
        for (unsigned node = m_leafCount; --node;)
            m_leastBound[node] = std::min(m_leastBound[2 * node], m_leastBound[2 * node + 1]);
    }

    void remove(unsigned position)
    {
        unsigned node = m_leafCount + position;
        m_leastBound[node] = UINT_MAX;
        while (node >>= 1)
            m_leastBound[node] = std::min(m_leastBound[2 * node], m_leastBound[2 * node + 1]);
    }

    // Calls functor(position) once for each join in the frontier of the block that dominates the numbers first to last,
    // until it returns false. The functor may remove the edge that it is given.
    template<typename Functor>
    bool forEachJoinInFrontier(unsigned first, unsigned last, NOESCAPE const Functor& functor)
    {
        unsigned begin = std::ranges::lower_bound(m_edges, first, { }, &Edge::from) - m_edges.begin();
        unsigned end = std::ranges::upper_bound(m_edges, last, { }, &Edge::from) - m_edges.begin();
        return visit(1, 0, m_leafCount, begin, end, first, functor);
    }

private:
    template<typename Functor>
    bool visit(unsigned node, unsigned nodeBegin, unsigned nodeEnd, unsigned begin, unsigned end, unsigned limit, NOESCAPE const Functor& functor)
    {
        if (end <= nodeBegin || nodeEnd <= begin || m_leastBound[node] >= limit)
            return true;
        if (nodeEnd - nodeBegin == 1)
            return functor(nodeBegin);
        unsigned middle = nodeBegin + (nodeEnd - nodeBegin) / 2;
        return visit(2 * node, nodeBegin, middle, begin, end, limit, functor)
            && visit(2 * node + 1, middle, nodeEnd, begin, end, limit, functor);
    }

    Vector<Edge> m_edges;
    // A binary tree over the edges: node n has the children 2 n and 2 n + 1, and the edges are its last m_leafCount nodes.
    Vector<unsigned> m_leastBound;
    unsigned m_leafCount { 1 };
};

// Finds the blocks where a variable needs a Phi and the blocks whose last Set is read, one variable at a time. It keeps
// no live set per block, so a variable that stays live across a large procedure costs as much as its Sets, Gets and Phis.
class LivePhiFinder {
public:
    using BlockList = Vector<BasicBlock*, 2>;

    LivePhiFinder(Procedure& proc)
        : m_setBlocks(proc.variables().size())
        , m_getBlocks(proc.variables().size())
        , m_firstDominated(proc.size(), UINT_MAX)
        , m_lastDominated(proc.size(), UINT_MAX)
        , m_hasSet(proc.size(), 0)
        , m_isSetRead(proc.size(), 0)
        , m_isCandidate(proc.size(), 0)
        , m_isPhiRead(proc.size(), 0)
        , m_isLiveAtHead(proc.size(), 0)
        , m_isVisited(proc.size(), 0)
        , m_lastEdgeInto(proc.size(), FrontierEdges::noEdge)
    {
        for (BasicBlock* block : proc) {
            for (Value* value : *block) {
                if (value->opcode() != Get && value->opcode() != Set)
                    continue;
                auto& blocksByVariable = value->opcode() == Set ? m_setBlocks : m_getBlocks;
                BlockList& blocks = blocksByVariable[value->as<VariableValue>()->variable()->index()];
                if (blocks.isEmpty() || blocks.last() != block)
                    blocks.append(block);
            }
        }

        auto& dominators = proc.dominators();
        unsigned nextNumber = 1;
        dominators.forAllBlocksInDominatorTreePreOrder(
            [&](BasicBlock* block) {
                unsigned number = nextNumber++;
                m_firstDominated[block] = number;
                for (BasicBlock* join : block->successorBlocks()) {
                    if (join->numPredecessors() < 2)
                        continue;
                    BasicBlock* idom = dominators.idom(join);
                    unsigned bound = idom ? m_firstDominated[idom] : 0;
                    unsigned previous = std::exchange(m_lastEdgeInto[join], m_frontiers.size());
                    if (previous != FrontierEdges::noEdge)
                        bound = std::max(bound, m_frontiers.at(previous).from);
                    m_frontiers.append({ number, bound, previous, join });
                }
            },
            [&](BasicBlock* block) { m_lastDominated[block] = nextNumber - 1; });
        m_frontiers.finish();
    }

    // Calls functor(block) for each block where the variable needs a Phi.
    template<typename Functor>
    void forEachPhiBlock(Variable* variable, NOESCAPE const Functor& functor)
    {
        analyze(variable);
        FrontierEdges& frontiers = m_isLivenessComplete ? m_liveFrontiers : m_frontiers;
        if (m_isLivenessComplete && !collectLiveFrontiers())
            return;

        // Each Set creates work for us, so leave out the ones nothing reads.
        m_frontierWorklist.shrink(0);
        for (BasicBlock* block : m_setBlocks[variable->index()]) {
            if (m_isSetRead[block] == m_epoch)
                m_frontierWorklist.append(block);
        }
        while (!m_frontierWorklist.isEmpty()) {
            BasicBlock* block = m_frontierWorklist.takeLast();
            m_phiBlocks.shrink(0);
            frontiers.forEachJoinInFrontier(m_firstDominated[block], m_lastDominated[block], [&](unsigned position) {
                BasicBlock* join = frontiers.at(position).join;
                if (m_isLivenessComplete)
                    frontiers.remove(position);
                if (std::exchange(m_isVisited[join], m_epoch) != m_epoch && m_isLiveAtHead[join] == m_epoch)
                    m_phiBlocks.append(join);
                return true;
            });
            std::ranges::sort(m_phiBlocks, { }, &BasicBlock::index);
            for (BasicBlock* join : m_phiBlocks) {
                functor(join);
                m_frontierWorklist.append(join);
            }
        }
    }

private:
    static constexpr unsigned noSite = UINT_MAX;

    // Afterwards m_isSetRead is right for the blocks with a Set, and m_isLiveAtHead for the blocks in the iterated
    // dominance frontier of the Sets.
    void analyze(Variable* variable)
    {
        ++m_epoch;
        const BlockList& setBlocks = m_setBlocks[variable->index()];
        const BlockList& getBlocks = m_getBlocks[variable->index()];

        m_candidates.shrink(0);
        m_frontierWorklist.shrink(0);
        for (BasicBlock* block : setBlocks) {
            m_hasSet[block] = m_epoch;
            m_frontierWorklist.append(block);
        }
        m_liveWorklist.shrink(0);
        m_liveBlocks.shrink(0);
        for (BasicBlock* block : getBlocks) {
            m_isLiveAtHead[block] = m_epoch;
            m_liveWorklist.append(block);
            m_liveBlocks.append(block);
        }

        // Either search answers alone. The first is short when the variable is live in few blocks, the second when few
        // joins follow the Sets, so they take turns and the cost stays within a constant factor of the shorter one.
        for (unsigned budget = 64;; budget *= 2) {
            m_isLivenessComplete = findLiveBlocks(budget);
            if (m_isLivenessComplete)
                return;
            if (findCandidates(budget)) {
                findReadCandidates(setBlocks, getBlocks);
                return;
            }
        }
    }

    // Leaves in m_liveFrontiers the edges into the joins where the variable is live. Returns false if there are none.
    bool collectLiveFrontiers()
    {
        m_livePositions.shrink(0);
        for (BasicBlock* block : m_liveBlocks) {
            for (unsigned position = m_lastEdgeInto[block]; position != FrontierEdges::noEdge; position = m_frontiers.at(position).previousIntoJoin)
                m_livePositions.append(position);
        }
        if (m_livePositions.isEmpty())
            return false;
        std::ranges::sort(m_livePositions);
        m_liveFrontiers.clear();
        for (unsigned position : m_livePositions)
            m_liveFrontiers.append(m_frontiers.at(position));
        m_liveFrontiers.finish();
        return true;
    }

    // A block that defines the variable, by a Set or by a candidate Phi.
    struct Site {
        BasicBlock* block;
        unsigned parent; // The nearest site that dominates this one.
    };

    // From this dominator tree number up to the next boundary, `site` is the nearest dominating site.
    struct Boundary {
        unsigned number;
        unsigned site;
    };

    bool findCandidates(unsigned budget)
    {
        while (!m_frontierWorklist.isEmpty()) {
            if (!budget)
                return false;
            --budget;
            BasicBlock* block = m_frontierWorklist.takeLast();
            bool isFinished = m_frontiers.forEachJoinInFrontier(m_firstDominated[block], m_lastDominated[block], [&](unsigned position) {
                if (!budget)
                    return false;
                --budget;
                BasicBlock* join = m_frontiers.at(position).join;
                if (std::exchange(m_isCandidate[join], m_epoch) != m_epoch) {
                    m_candidates.append(join);
                    m_frontierWorklist.append(join);
                }
                return true;
            });
            if (!isFinished) {
                m_frontierWorklist.append(block);
                return false;
            }
        }
        return true;
    }

    bool findLiveBlocks(unsigned budget)
    {
        while (!m_liveWorklist.isEmpty()) {
            if (!budget)
                return false;
            BasicBlock* block = m_liveWorklist.takeLast();
            for (BasicBlock* predecessor : block->predecessors()) {
                if (m_hasSet[predecessor] == m_epoch) {
                    m_isSetRead[predecessor] = m_epoch;
                    continue;
                }
                if (m_isLiveAtHead[predecessor] == m_epoch)
                    continue;
                m_isLiveAtHead[predecessor] = m_epoch;
                m_liveWorklist.append(predecessor);
                m_liveBlocks.append(predecessor);
            }
            budget -= std::min<unsigned>(budget, block->numPredecessors() + 1);
        }
        return true;
    }

    // Follows each Get to the definition that reaches it, and each Phi found that way to the definitions that reach the
    // ends of its predecessors.
    void findReadCandidates(const BlockList& setBlocks, const BlockList& getBlocks)
    {
        m_sites.shrink(0);
        for (BasicBlock* block : setBlocks)
            m_sites.append({ block, noSite });
        for (BasicBlock* block : m_candidates) {
            if (m_hasSet[block] != m_epoch)
                m_sites.append({ block, noSite });
        }
        std::ranges::sort(m_sites, { }, [&](const Site& site) { return m_firstDominated[site.block]; });

        m_boundaries.shrink(0);
        auto closeSitesBefore = [&](unsigned number) {
            while (!m_openSites.isEmpty() && m_lastDominated[m_sites[m_openSites.last()].block] < number) {
                const Site& closed = m_sites[m_openSites.takeLast()];
                m_boundaries.append({ m_lastDominated[closed.block] + 1, closed.parent });
            }
        };
        for (unsigned index = 0; index < m_sites.size(); ++index) {
            unsigned number = m_firstDominated[m_sites[index].block];
            closeSitesBefore(number);
            if (!m_openSites.isEmpty())
                m_sites[index].parent = m_openSites.last();
            m_openSites.append(index);
            m_boundaries.append({ number, index });
        }
        closeSitesBefore(UINT_MAX);

        for (BasicBlock* block : getBlocks)
            readAtHead(block);
        while (!m_readPhis.isEmpty()) {
            for (BasicBlock* predecessor : m_readPhis.takeLast()->predecessors())
                readAtTail(findNearestSite(predecessor));
        }
    }

    unsigned findNearestSite(BasicBlock* block) const
    {
        auto boundary = std::ranges::upper_bound(m_boundaries, m_firstDominated[block], { }, &Boundary::number);
        return boundary == m_boundaries.begin() ? noSite : (boundary - 1)->site;
    }

    void readAtHead(BasicBlock* block)
    {
        if (m_isCandidate[block] == m_epoch) {
            readPhi(block);
            return;
        }
        unsigned site = findNearestSite(block);
        if (site != noSite && m_sites[site].block == block)
            site = m_sites[site].parent;
        readAtTail(site);
    }

    void readAtTail(unsigned site)
    {
        if (site == noSite)
            return;
        BasicBlock* block = m_sites[site].block;
        if (m_hasSet[block] == m_epoch)
            m_isSetRead[block] = m_epoch;
        else
            readPhi(block);
    }

    void readPhi(BasicBlock* block)
    {
        if (m_isPhiRead[block] == m_epoch)
            return;
        m_isPhiRead[block] = m_epoch;
        m_isLiveAtHead[block] = m_epoch;
        m_readPhis.append(block);
    }

    FrontierEdges m_frontiers;
    Vector<BlockList> m_setBlocks;
    Vector<BlockList> m_getBlocks;
    IndexMap<BasicBlock*, unsigned> m_firstDominated;
    IndexMap<BasicBlock*, unsigned> m_lastDominated;

    // A block has the property for the current variable when its entry equals m_epoch.
    unsigned m_epoch { 0 };
    IndexMap<BasicBlock*, unsigned> m_hasSet;
    IndexMap<BasicBlock*, unsigned> m_isSetRead;
    IndexMap<BasicBlock*, unsigned> m_isCandidate;
    IndexMap<BasicBlock*, unsigned> m_isPhiRead;
    IndexMap<BasicBlock*, unsigned> m_isLiveAtHead;
    IndexMap<BasicBlock*, unsigned> m_isVisited;
    bool m_isLivenessComplete { false };

    IndexMap<BasicBlock*, unsigned> m_lastEdgeInto;
    FrontierEdges m_liveFrontiers;
    Vector<unsigned, 16> m_livePositions;
    Vector<BasicBlock*, 16> m_liveBlocks;
    Vector<BasicBlock*, 16> m_phiBlocks;

    Vector<BasicBlock*, 16> m_frontierWorklist;
    Vector<BasicBlock*, 16> m_liveWorklist;
    Vector<BasicBlock*, 16> m_candidates;
    Vector<BasicBlock*, 16> m_readPhis;
    Vector<Site, 16> m_sites;
    Vector<unsigned, 16> m_openSites;
    Vector<Boundary, 32> m_boundaries;
};

void fixSSAGlobally(Procedure& proc, IndexSet<Value*>& valuesToDelete)
{
    struct PhiOfVariable {
        Variable* variable;
        Value* phi;
    };
    IndexMap<BasicBlock*, Vector<PhiOfVariable>> phisAtHead(proc.size());

    // Decide where Phis are to be inserted. This creates them but does not insert them.
    {
        CompilerTimingScope timingScope("B3"_s, "fixSSA: computePhis"_s);
        LivePhiFinder livePhiFinder(proc);
        for (Variable* variable : proc.variables()) {
            livePhiFinder.forEachPhiBlock(variable, [&](BasicBlock* block) {
                Value* phi = proc.add<Value>(Phi, variable->type(), block->at(0)->origin());
                if (proc.knownInductions().hasDemoted()) [[unlikely]]
                    proc.knownInductions().didCreatePhi(variable, block, phi);
                proc.knownProperties().didCreatePhi(variable, phi);
                if (B3FixSSAInternal::verbose) {
                    dataLog(
                        "Adding Phi for ", pointerDump(variable), " at ", *block, ": ",
                        deepDump(proc, phi), "\n");
                }
                phisAtHead[block].append({ variable, phi });
            });
        }
    }

    // Now perform the conversion.
    CompilerTimingScope timingScope("B3"_s, "fixSSA: convert"_s);
    InsertionSet insertionSet(proc);

    Vector<Value*> reachingDefs(FillWith { }, proc.variables().size(), nullptr);
    Vector<std::pair<unsigned, Value*>> undoLog;
    Vector<unsigned, 16> undoLogSizeAtEntry;
    auto setReachingDef = [&](Variable* variable, Value* value) {
        undoLog.append({ variable->index(), reachingDefs[variable->index()] });
        reachingDefs[variable->index()] = value;
    };

    proc.dominators().forAllBlocksInDominatorTreePreOrder(
        [&](BasicBlock* block) {
            undoLogSizeAtEntry.append(undoLog.size());

            auto reachingDef = [&](Variable* variable, unsigned valueIndex, Origin origin) -> Value* {
                if (Value* def = reachingDefs[variable->index()])
                    return def;
                Value* bottom = insertionSet.insertBottom(valueIndex, origin, variable->type());
                setReachingDef(variable, bottom);
                return bottom;
            };

            for (auto [variable, phi] : phisAtHead[block]) {
                insertionSet.insertValue(0, phi);
                setReachingDef(variable, phi);
            }

            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);
                value->performSubstitution();

                switch (value->opcode()) {
                case Get: {
                    Variable* variable = value->as<VariableValue>()->variable();
                    value->replaceWithIdentity(reachingDef(variable, valueIndex, value->origin()));
                    valuesToDelete.add(value);
                    break;
                }

                case Set: {
                    Variable* variable = value->as<VariableValue>()->variable();
                    setReachingDef(variable, value->child(0));
                    value->replaceWithNop();
                    break;
                }

                default:
                    break;
                }
            }

            unsigned upsilonInsertionPoint = block->size() - 1;
            Origin upsilonOrigin = block->last()->origin();
            for (BasicBlock* successorBlock : block->successorBlocks()) {
                for (auto [variable, phi] : phisAtHead[successorBlock]) {
                    Value* mappedValue = reachingDef(variable, upsilonInsertionPoint, upsilonOrigin);
                    if (B3FixSSAInternal::verbose) {
                        dataLog(
                            "Mapped value for ", *variable, " with successor Phi ", *phi,
                            " at end of ", *block, ": ", pointerDump(mappedValue), "\n");
                    }

                    insertionSet.insert<UpsilonValue>(
                        upsilonInsertionPoint, upsilonOrigin, mappedValue->foldIdentity(), phi);
                }
            }

            insertionSet.execute(block);
        },
        [&](BasicBlock* block) {
            unsigned undoLogSize = undoLogSizeAtEntry.takeLast();
            while (undoLog.size() > undoLogSize) {
                auto [index, previous] = undoLog.takeLast();
                reachingDefs[index] = previous;
            }

            // Now that we've visited every block these `Get`s dominate, and all their users have been
            // substituted, we can delete the `Get`. This isn't strictly necessary, but it leaves the IR
            // tidy for phases that do size estimates.
            block->values().removeAllMatching(
                [&](Value* value) -> bool {
                    if (!valuesToDelete.remove(value) && value->opcode() != Nop)
                        return false;

                    proc.deleteValue(value);
                    return true;
                });
        });

    if (B3FixSSAInternal::verbose) {
        dataLog("B3 after SSA conversion:\n");
        dataLog(proc);
    }
}

void forgetDemotedValues(Procedure& proc)
{
    proc.knownInductions().clearDemoted();
    proc.knownProperties().clearDemoted();
}

} // anonymous namespace

void demoteValues(Procedure& proc, const IndexSet<Value*>& values)
{
    UncheckedKeyHashMap<Value*, Variable*> map;
    UncheckedKeyHashMap<Value*, Variable*> phiMap;

    // Create stack slots.
    auto addVariableFor = [&](Value* value) {
        Variable* variable = proc.addVariable(value->type());
        proc.knownProperties().didDemote(value, variable);
        return variable;
    };
    for (Value* value : values.values(proc.values())) {
        map.add(value, addVariableFor(value));

        if (value->opcode() == Phi)
            phiMap.add(value, addVariableFor(value));
    }

    if (B3FixSSAInternal::verbose) {
        dataLog("Demoting values as follows:\n");
        dataLog("   map = ");
        CommaPrinter comma;
        for (auto& entry : map)
            dataLog(comma, *entry.key, "=>", *entry.value);
        dataLog("\n");
        dataLog("   phiMap = ");
        comma = CommaPrinter();
        for (auto& entry : phiMap)
            dataLog(comma, *entry.key, "=>", *entry.value);
        dataLog("\n");
    }

    // Change accesses to the values to accesses to the stack slots.
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        if (block->numPredecessors()) {
            // Deal with terminals that produce values (i.e. patchpoint terminals, like the ones we
            // generate for the allocation fast path).
            Value* value = block->predecessor(0)->last();
            Variable* variable = map.get(value);
            if (variable && block->predecessor(0)->successorBlock(0) == block) {
                RELEASE_ASSERT(block->numPredecessors() == 1); // Critical edges better be broken.
                insertionSet.insert<VariableValue>(0, Set, value->origin(), variable, value);
            }
        }
        
        for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
            Value* value = block->at(valueIndex);

            if (value->opcode() == Phi) {
                if (Variable* variable = phiMap.get(value)) {
                    value->replaceWithIdentity(
                        insertionSet.insert<VariableValue>(
                            valueIndex, Get, value->origin(), variable));
                }
            } else {
                for (Value*& child : value->children()) {
                    if (Variable* variable = map.get(child)) {
                        child = insertionSet.insert<VariableValue>(
                            valueIndex, Get, value->origin(), variable);
                    }
                }

                if (UpsilonValue* upsilon = value->as<UpsilonValue>()) {
                    if (Variable* variable = phiMap.get(upsilon->phi())) {
                        insertionSet.insert<VariableValue>(
                            valueIndex, Set, upsilon->origin(), variable, upsilon->child(0));
                        value->replaceWithNop();
                    }
                }
            }

            if (Variable* variable = map.get(value)) {
                if (valueIndex + 1 < block->size()) {
                    insertionSet.insert<VariableValue>(
                        valueIndex + 1, Set, value->origin(), variable, value);
                }
            }
        }
        insertionSet.execute(block);
    }
}

bool fixSSA(Procedure& proc)
{
    PhaseScope phaseScope(proc, "fixSSA"_s);

    if (proc.variables().isEmpty()) {
        forgetDemotedValues(proc);
        return false;
    }
    
    IndexSet<Value*> valuesToDelete;
    if (fixSSALocally(proc, valuesToDelete)) {
        // Users of a forwarded Get can be in any block, so substitute everywhere before deleting.
        for (Value* value : proc.values())
            value->performSubstitution();
        for (BasicBlock* block : proc) {
            block->values().removeAllMatching(
                [&](Value* value) -> bool {
                    if (!valuesToDelete.remove(value))
                        return false;

                    proc.deleteValue(value);
                    return true;
                });
        }
        forgetDemotedValues(proc);
        return false;
    }

    breakCriticalEdges(proc);

    fixSSAGlobally(proc, valuesToDelete);

    forgetDemotedValues(proc);

    proc.deleteAllVariables();

    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

