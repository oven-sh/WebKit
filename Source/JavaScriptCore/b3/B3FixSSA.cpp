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
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3SSACalculator.h"
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

// Finds the blocks where a variable needs a Phi and the blocks whose last Set is read, one variable at a time. It keeps
// no live set per block, so a variable that stays live across a large procedure costs as much as its Sets, Gets and Phis.
class LivePhiFinder {
public:
    using BlockList = Vector<BasicBlock*, 2>;

    LivePhiFinder(Procedure& proc, SSACalculator& ssa)
        : m_ssa(ssa)
        , m_setBlocks(proc.variables().size())
        , m_getBlocks(proc.variables().size())
        , m_firstDominated(proc.size(), UINT_MAX)
        , m_lastDominated(proc.size(), UINT_MAX)
        , m_hasSet(proc.size(), 0)
        , m_isSetRead(proc.size(), 0)
        , m_isCandidate(proc.size(), 0)
        , m_isPhiRead(proc.size(), 0)
        , m_isLiveAtHead(proc.size(), 0)
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

        unsigned nextNumber = 0;
        proc.dominators().forAllBlocksInDominatorTreePreOrder(
            [&](BasicBlock* block) { m_firstDominated[block] = nextNumber++; },
            [&](BasicBlock* block) { m_lastDominated[block] = nextNumber - 1; });

        ssa.computeDominanceFrontiers();
    }

    const BlockList& setBlocks(Variable* variable) const LIFETIME_BOUND { return m_setBlocks[variable->index()]; }

    // The answers below are about the variable given to the last call.
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
        for (BasicBlock* block : getBlocks) {
            m_isLiveAtHead[block] = m_epoch;
            m_liveWorklist.append(block);
        }

        // Either search answers alone. The first is short when few joins follow the Sets, the second when the variable
        // is live in few blocks, so they take turns and the cost stays within a constant factor of the shorter one.
        for (unsigned budget = 64;; budget *= 2) {
            if (findCandidates(budget)) {
                findReadCandidates(setBlocks, getBlocks);
                return;
            }
            if (findLiveBlocks(budget))
                return;
        }
    }

    // Only meaningful for blocks in the iterated dominance frontier of the Sets.
    bool needsPhi(BasicBlock* block) const { return m_isLiveAtHead[block] == m_epoch; }
    bool isSetRead(BasicBlock* block) const { return m_isSetRead[block] == m_epoch; }

private:
    static constexpr unsigned noSite = UINT_MAX;

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
            const auto& frontier = m_ssa.dominanceFrontierOf(m_frontierWorklist.takeLast());
            for (BasicBlock* block : frontier) {
                if (m_isCandidate[block] == m_epoch)
                    continue;
                m_isCandidate[block] = m_epoch;
                m_candidates.append(block);
                m_frontierWorklist.append(block);
            }
            budget -= std::min<unsigned>(budget, frontier.size() + 1);
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

    SSACalculator& m_ssa;
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
    SSACalculator ssa(proc);
    LivePhiFinder livePhiFinder(proc, ssa);

    // Create a SSACalculator::Variable ("calcVar") for every variable.
    Vector<Variable*> calcVarToVariable;

    for (Variable* variable : proc.variables()) {
        SSACalculator::Variable* calcVar = ssa.newVariable();
        RELEASE_ASSERT(calcVar->index() == calcVarToVariable.size());
        calcVarToVariable.append(variable);
    }

    // Decide where Phis are to be inserted. This creates them but does not insert them.
    {
        CompilerTimingScope timingScope("B3"_s, "fixSSA: computePhis"_s);
        ssa.computePhis(
            [&](SSACalculator::Variable* calcVar) {
                Variable* variable = calcVarToVariable[calcVar->index()];
                livePhiFinder.analyze(variable);
                // Each Set creates work for us, so leave out the ones nothing reads.
                for (BasicBlock* block : livePhiFinder.setBlocks(variable)) {
                    if (livePhiFinder.isSetRead(block))
                        ssa.newDefBlock(calcVar, block);
                }
            },
            [&](SSACalculator::Variable* calcVar, BasicBlock* block) -> Value* {
                Variable* variable = calcVarToVariable[calcVar->index()];
                if (!livePhiFinder.needsPhi(block))
                    return nullptr;

                Value* phi = proc.add<Value>(Phi, variable->type(), block->at(0)->origin());
                if (proc.knownInductions().hasDemoted()) [[unlikely]]
                    proc.knownInductions().didCreatePhi(variable, block, phi);
                proc.knownProperties().didCreatePhi(variable, phi);
                if (B3FixSSAInternal::verbose) {
                    dataLog(
                        "Adding Phi for ", pointerDump(variable), " at ", *block, ": ",
                        deepDump(proc, phi), "\n");
                }
                return phi;
            });
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

            for (SSACalculator::Def* phiDef : ssa.phisForBlock(block)) {
                Variable* variable = calcVarToVariable[phiDef->variable()->index()];

                insertionSet.insertValue(0, phiDef->value());
                setReachingDef(variable, phiDef->value());
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
                for (SSACalculator::Def* phiDef : ssa.phisForBlock(successorBlock)) {
                    Value* phi = phiDef->value();
                    Variable* variable = calcVarToVariable[phiDef->variable()->index()];

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

