/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DFGDecimalPhiUnboxingPhase.h"

#if ENABLE(DFG_JIT)

#include "DFGBlockSet.h"
#include "DFGCFAPhase.h"
#include "DFGClobberize.h"
#include "DFGCombinedLiveness.h"
#include "DFGGraph.h"
#include "DFGInsertionSet.h"
#include "DFGLivenessAnalysisPhase.h"
#include "DFGOSRAvailabilityAnalysisPhase.h"
#include "DFGPhase.h"
#include "DecimalObject.h"
#include "JSCJSValueInlines.h"
#include <wtf/IndexMap.h>

namespace JSC { namespace DFG {

// A Decimal has an identity. An object that nothing has seen, and that only the Phi can reach from now on, can as well be made later: the
// Upsilon hands it over. Any other object goes along with its words in a third Phi, which is empty where there is no object yet.
class DecimalPhiUnboxingPhase : public Phase {
public:
    DecimalPhiUnboxingPhase(Graph& graph)
        : Phase(graph, "Decimal Phi unboxing"_s)
    {
    }

    bool run()
    {
        ASSERT(m_graph.m_form == SSA);
        m_structure = structureOfNewDecimalThatGoesIntoPhi();
        if (!m_structure)
            return false;
        // What else has an object can then not change its structure.
        m_mayWatchStructure = m_structure->dfgMayWatch();

        performGraphPackingAndLivenessAnalysis(m_graph);
        performCFA(m_graph);
        performOSRAvailabilityAnalysis(m_graph);
        m_liveness = CombinedLiveness(m_graph);

        findPhis();
        findUpsilons();
        do {
            findUses();
            choosePhis();
        } while (removeUnfitPhis());
        m_phis.removeAllMatching([&](Node* phi) { return !candidateFor(phi)->isChosen(); });
        if (m_phis.isEmpty())
            return false;

        if (m_mayWatchStructure)
            m_graph.watch(m_structure.get());
        removeChecks();
        convertPhis();
        convertUpsilons();
        return true;
    }

private:
    struct Input {
        Node* upsilon;
        BasicBlock* block;
        bool isHandedOver { false };

        Node* value() const { return upsilon->child1().node(); }
    };

    struct Candidate {
        BasicBlock* block { nullptr };
        bool structureIsProved { false };
        Vector<Input, 2> inputs { };
        bool getsNewObject { false };
        bool hasWordsRead { false };
        bool hasStructureChecked { false };
        bool mayHaveObject { false };

        bool isChosen() const { return getsNewObject && hasWordsRead; }
    };

    RegisteredStructure structureOfNewDecimalThatGoesIntoPhi()
    {
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            for (Node* node : *block) {
                if (node->op() == Upsilon && node->child1()->op() == NewDecimal)
                    return node->child1()->structure();
            }
        }
        return { };
    }

    static unsigned sizeOfHead(BasicBlock* block)
    {
        unsigned size = 0;
        while ((block->at(size)->op() == Phi || block->at(size)->op() == MovHint) && !block->at(size)->origin.exitOK)
            ++size;
        return size;
    }

    void findPhis()
    {
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            if (block->cfaStructureClobberStateAtHead != StructuresAreWatched)
                continue;
            bool hasUsualPredecessors = std::ranges::all_of(block->predecessors, [](BasicBlock* predecessor) {
                return predecessor->numSuccessors() == 1 && predecessor->cfaHasVisited && predecessor->cfaDidFinish;
            });
            if (!hasUsualPredecessors)
                continue;

            auto head = block->begin();
            auto endOfHead = head + sizeOfHead(block);
            for (auto& entry : block->ssa->valuesAtHead) {
                Node* phi = entry.node.node();
                if (entry.node.kind() != NodeFlowProjection::Shadow || std::find(head, endOfHead, phi) == endOfHead)
                    continue;
                bool structureIsProved = entry.value.isType(SpecObjectOther) && entry.value.m_structure.onlyStructure() == m_structure;
                if (!structureIsProved && phi->prediction() != SpecObjectOther)
                    continue;
                m_phis.append(phi);
                m_candidates.add(phi, Candidate { block, structureIsProved });
            }
        }
    }

    Candidate* candidateFor(Node* node)
    {
        auto iterator = m_candidates.find(node);
        return iterator == m_candidates.end() ? nullptr : &iterator->value;
    }

    void findUpsilons()
    {
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            for (Node* node : *block) {
                if (node->op() == NewDecimal && node->structure() == m_structure)
                    m_owners.add(node, block);
                if (Candidate* candidate = node->op() == Upsilon ? candidateFor(node->phi()) : nullptr)
                    candidate->inputs.append({ node, block });
            }
        }

        m_phis.removeAllMatching([&](Node* phi) {
            Candidate& candidate = *candidateFor(phi);
            bool hasOneForEachPredecessor = candidate.inputs.size() == candidate.block->predecessors.size() && std::ranges::all_of(candidate.block->predecessors, [&](BasicBlock* predecessor) {
                return candidate.inputs.containsIf([&](const Input& input) { return input.block == predecessor; });
            });
            if (hasOneForEachPredecessor) {
                m_owners.add(phi, candidate.block);
                return false;
            }
            m_candidates.remove(phi);
            return true;
        });
    }

    bool holdsOfDecimal(Node* check, StructureClobberState state)
    {
        switch (check->op()) {
        case AssertNotEmpty:
        case CheckNotEmpty:
            return true;
        case CheckJSCast:
            return check->classInfo() == DecimalObject::info();
        case CheckStructure:
            return m_mayWatchStructure && state == StructuresAreWatched && check->structureSet().onlyStructure() == m_structure;
        default:
            return false;
        }
    }

    // As in allocation sinking.
    bool letsEscape(Node* node, Edge edge)
    {
        switch (node->op()) {
        case GetDecimalWord:
        case MovHint:
        case FilterCallLinkStatus:
        case FilterGetByStatus:
        case FilterPutByStatus:
        case FilterInByStatus:
        case FilterDeleteByStatus:
        case FilterCheckPrivateBrandStatus:
        case FilterSetPrivateBrandStatus:
            return false;
        case Check:
        case CheckVarargs:
            return edge.willHaveCheck() && !alreadyChecked(edge.useKind(), SpecObject);
        default:
            return true;
        }
    }

    void findUses()
    {
        HashMap<Node*, BlockSet> escapedAtTail;
        Vector<std::pair<Node*, BasicBlock*>> worklist;
        auto escape = [&](Node* value, BasicBlock* block) {
            if (escapedAtTail.add(value, BlockSet()).iterator->value.add(block))
                worklist.append({ value, block });
        };

        m_checks.shrink(0);
        for (Node* phi : m_phis) {
            Candidate& candidate = *candidateFor(phi);
            candidate = { candidate.block, candidate.structureIsProved, WTF::move(candidate.inputs) };
        }

        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            NodeSet goesIntoPhi;
            StructureClobberState state = block->cfaStructureClobberStateAtHead;
            for (Node* node : *block) {
                if (Candidate* candidate = holdsOfDecimal(node, state) ? candidateFor(node->child1().node()) : nullptr) {
                    candidate->hasStructureChecked |= node->op() == CheckStructure;
                    m_checks.append(node);
                    continue;
                }
                if (node->op() == InvalidationPoint)
                    state = StructuresAreWatched;
                else if (writesOverlap(m_graph, node, Watchpoint_fire))
                    state = StructuresAreClobbered;

                m_graph.doToChildren(node, [&](Edge edge) {
                    Node* value = edge.node();
                    if (!m_owners.contains(value))
                        return;
                    if (node->op() == Upsilon) {
                        if (!goesIntoPhi.add(value).isNewEntry || m_liveness.liveAtTail[block].contains(value))
                            escape(value, block);
                        return;
                    }
                    if (letsEscape(node, edge))
                        escape(value, block);
                    else if (Candidate* candidate = node->op() == GetDecimalWord ? candidateFor(value) : nullptr)
                        candidate->hasWordsRead = true;
                });
            }
        }

        while (!worklist.isEmpty()) {
            auto [value, block] = worklist.takeLast();
            for (BasicBlock* successor : block->successors()) {
                if (successor != m_owners.get(value) && m_liveness.liveAtHead[successor].contains(value))
                    escape(value, successor);
            }
        }

        for (Node* phi : m_phis) {
            for (Input& input : candidateFor(phi)->inputs) {
                auto iterator = escapedAtTail.find(input.value());
                input.isHandedOver = m_owners.contains(input.value()) && (iterator == escapedAtTail.end() || !iterator->value.contains(input.block));
            }
        }
    }

    // Until it answers false for all.
    void forInputsUntilSettled(NOESCAPE const Invocable<bool(Candidate&, Input&)> auto& functor)
    {
        for (bool changed = true; std::exchange(changed, false);) {
            for (Node* phi : m_phis) {
                Candidate& candidate = *candidateFor(phi);
                for (Input& input : candidate.inputs)
                    changed |= functor(candidate, input);
            }
        }
    }

    void choosePhis()
    {
        auto set = [](bool& flag, bool value) { return value && !std::exchange(flag, true); };
        forInputsUntilSettled([&](Candidate& candidate, Input& input) {
            if (!input.isHandedOver)
                return false;
            Candidate* source = candidateFor(input.value());
            if (!source)
                return set(candidate.getsNewObject, true);
            bool changed = set(candidate.getsNewObject, source->getsNewObject);
            changed |= set(source->hasWordsRead, candidate.hasWordsRead);
            changed |= set(source->hasStructureChecked, candidate.hasStructureChecked);
            return changed;
        });
        forInputsUntilSettled([&](Candidate& candidate, Input& input) {
            Candidate* source = candidateFor(input.value());
            return set(candidate.mayHaveObject, !input.isHandedOver || (source && source->mayHaveObject));
        });
    }

    bool hasWords(Node* value)
    {
        Candidate* candidate = candidateFor(value);
        return candidate ? candidate->isChosen() : m_owners.contains(value);
    }

    bool mayCheckStructureOf(const Input& input)
    {
        NodeOrigin origin = input.upsilon->origin;
        return origin.exitOK && input.value()->op() != NewDecimal && std::ranges::none_of(std::array { TDZFailure, BadType, BadCache, BadConstantCache }, [&](ExitKind kind) {
            return m_graph.hasExitSite(origin.semantic, kind);
        });
    }

    bool removeUnfitPhis()
    {
        auto isFit = [&](Candidate& candidate) {
            if (candidate.mayHaveObject && !m_mayWatchStructure)
                return false;
            return candidate.structureIsProved || std::ranges::all_of(candidate.inputs, [&](const Input& input) {
                return hasWords(input.value()) || (candidate.hasStructureChecked && mayCheckStructureOf(input));
            });
        };
        return m_phis.removeAllMatching([&](Node* phi) {
            Candidate& candidate = *candidateFor(phi);
            if (!candidate.isChosen() || isFit(candidate))
                return false;
            m_candidates.remove(phi);
            m_owners.remove(phi);
            return true;
        });
    }

    void removeChecks()
    {
        for (Node* check : m_checks) {
            if (candidateFor(check->child1().node())->isChosen())
                check->removeWithoutChecks();
        }
    }

    void convertPhis()
    {
        IndexMap<BasicBlock*, Vector<Node*>> phisOfBlocks(m_graph.numBlocks());
        for (Node* phi : m_phis)
            phisOfBlocks[candidateFor(phi)->block].append(phi);

        InsertionSet insertionSet(m_graph);
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            if (phisOfBlocks[block].isEmpty())
                continue;
            unsigned size = sizeOfHead(block);
            for (Node* phi : phisOfBlocks[block]) {
                auto newPhi = [&](SpeculatedType type, NodeFlags result) {
                    Node* node = insertionSet.insertNode(0, type, Phi, phi->origin);
                    node->setResult(result);
                    return node;
                };
                Node* low = newPhi(SpecOther, NodeResultStorage);
                Node* upper = newPhi(SpecOther, NodeResultStorage);
                phi->convertPhiToNewDecimal(m_structure, low, upper, candidateFor(phi)->mayHaveObject ? newPhi(SpecObjectOther | SpecEmpty, NodeResultJS) : nullptr);
            }
            std::stable_partition(block->begin(), block->begin() + size, [](Node* node) { return node->op() == Phi; });
            insertionSet.execute(block);
        }
    }

    void convertUpsilons()
    {
        HashMap<Node*, bool> upsilons;
        for (Node* phi : m_phis) {
            for (Input& input : candidateFor(phi)->inputs)
                upsilons.add(input.upsilon, input.isHandedOver);
        }

        InsertionSet insertionSet(m_graph);
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Node* node = block->at(index);
                auto iterator = upsilons.find(node);
                if (iterator == upsilons.end())
                    continue;

                Node* target = node->phi();
                Node* value = node->child1().node();
                bool isNew = hasWords(value);
                if (!isNew && !candidateFor(target)->structureIsProved) {
                    insertionSet.insertNode(index + 1, SpecNone, CheckNotEmpty, node->origin, Edge(value));
                    insertionSet.insertNode(index + 1, SpecNone, CheckStructure, node->origin, OpInfo(m_graph.addStructureSet(m_structure)), Edge(value, CellUse));
                }
                auto insertUpsilon = [&](Edge phi, Node* incoming) {
                    insertionSet.insertNode(index + 1, SpecNone, Upsilon, node->origin, OpInfo(phi.node()), Edge(incoming, phi.useKind()));
                };
                for (unsigned word = 0; word < 2; ++word)
                    insertUpsilon(target->children.child(word), isNew ? value->children.child(word).node() : insertionSet.insertNode(index + 1, SpecOther, GetDecimalWord, node->origin, OpInfo(word), Edge(value, KnownCellUse)));
                if (target->child3()) {
                    if (isNew && iterator->value)
                        value = value->child3() ? value->child3().node() : insertionSet.insertConstant(index + 1, node->origin, JSValue());
                    insertUpsilon(target->child3(), value);
                }
                node->remove(m_graph);
            }
            insertionSet.execute(block);
        }
    }

    RegisteredStructure m_structure;
    bool m_mayWatchStructure { false };
    CombinedLiveness m_liveness;
    Vector<Node*> m_checks;
    Vector<Node*> m_phis;
    HashMap<Node*, Candidate> m_candidates;
    HashMap<Node*, BasicBlock*> m_owners;
};

bool performDecimalPhiUnboxing(Graph& graph)
{
    return runPhase<DecimalPhiUnboxingPhase>(graph);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
