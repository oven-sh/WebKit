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
#include "B3Limits.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3SSACalculator.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableLiveness.h"
#include "B3VariableValue.h"
#include "CompilerTimingScope.h"
#include <wtf/CommaPrinter.h>
#include <wtf/IndexSet.h>
#include <wtf/IndexSparseSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3FixSSAInternal {
static constexpr bool verbose = false;
}

struct Access {
    unsigned variable;
    bool isSet;
    BasicBlock* block;
    Value* value;
};

struct Accesses {
    Vector<Access, 64> list;
    Vector<unsigned, 32> firstAccessOfVariable;

    std::span<Access> of(unsigned variable) { return list.mutableSpan().subspan(firstAccessOfVariable[variable], firstAccessOfVariable[variable + 1] - firstAccessOfVariable[variable]); }
};

bool killDeadVariables(Procedure& proc, Accesses& accesses)
{
    IndexSet<Variable*> liveVariables;
    for (Access& access : accesses.list) {
        if (!access.isSet)
            liveVariables.add(proc.variables()[access.variable]);
    }

    for (Access& access : accesses.list) {
        if (access.isSet && !liveVariables.contains(proc.variables()[access.variable]))
            access.value->replaceWithNop();
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

void fixSSALocally(Procedure& proc, Accesses& accesses, IndexSet<Value*>& valuesToDelete)
{
    IndexSparseSet<KeyValuePair<unsigned, Value*>> mapping(proc.variables().size());
    for (BasicBlock* block : proc.cachedBlocksInPreOrder()) {
        mapping.clear();

        for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
            Value* value = block->at(valueIndex);

            switch (value->opcode()) {
            case Get: {
                VariableValue* variableValue = value->as<VariableValue>();
                Variable* variable = variableValue->variable();

                if (KeyValuePair<unsigned, Value*>* replacement = mapping.get(variable->index())) {
                    value->replaceWithIdentity(replacement->value);
                    valuesToDelete.add(value);
                } else
                    accesses.list.append(Access { variable->index(), false, block, value });
                break;
            }
                
            case Set: {
                VariableValue* variableValue = value->as<VariableValue>();
                Variable* variable = variableValue->variable();

                value->performSubstitution();
                mapping.set(variable->index(), value->child(0));
                accesses.list.append(Access { variable->index(), true, block, value });
                break;
            }

            default:
                break;
            }
        }
    }
}

class LivenessByVariable {
public:
    LivenessByVariable(Procedure& proc, Accesses& accesses)
        : m_proc(proc)
        , m_accesses(accesses)
    {
    }

    enum class Outcome : uint8_t { Complete, LimitReached };

    Outcome run()
    {
        unsigned numVariables = m_proc.variables().size();
        std::ranges::stable_sort(m_accesses.list, [](const Access& a, const Access& b) {
            if (a.variable != b.variable)
                return a.variable < b.variable;
            return a.block->index() < b.block->index();
        });
        m_accesses.firstAccessOfVariable.fill(0, numVariables + 1);
        for (Access& access : m_accesses.list)
            ++m_accesses.firstAccessOfVariable[access.variable + 1];
        for (unsigned i = 0; i < numVariables; ++i)
            m_accesses.firstAccessOfVariable[i + 1] += m_accesses.firstAccessOfVariable[i];

        m_isLiveAtHead.fill(0, m_proc.size());
        m_hasLiveVariableAtHead.fill(0, m_proc.size());
        m_isLiveAtTail.fill(0, m_proc.size());
        m_hasSet.fill(0, m_proc.size());
        m_firstLiveBlockOfVariable.fill(0, numVariables + 1);
        uint64_t budget = static_cast<uint64_t>(m_proc.size()) * m_proc.limit(Limit::MaxLivenessStepsPerBlock);
        Vector<BasicBlock*, 32> worklist;
        for (unsigned variable = 0; variable < numVariables; ++variable) {
            m_firstLiveBlockOfVariable[variable] = m_blocksLiveAtHead.size();
            if (!m_proc.variables()[variable])
                continue;
            unsigned epoch = variable + 1;
            auto accesses = m_accesses.of(variable);
            for (Access& access : accesses) {
                if (access.isSet)
                    m_hasSet[access.block->index()] = epoch;
            }
            auto add = [&](BasicBlock* block) {
                if (m_isLiveAtHead[block->index()] == epoch)
                    return;
                m_isLiveAtHead[block->index()] = epoch;
                m_hasLiveVariableAtHead[block->index()] = 1;
                m_blocksLiveAtHead.append(block->index());
                worklist.append(block);
            };
            for (Access& access : accesses) {
                if (!access.isSet)
                    add(access.block);
            }
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                if (budget < block->numPredecessors() + 1) {
                    m_proc.didReachLimit(Limit::MaxLivenessStepsPerBlock, block->at(0)->origin());
                    return Outcome::LimitReached;
                }
                budget -= block->numPredecessors() + 1;
                for (BasicBlock* predecessor : block->predecessors()) {
                    m_isLiveAtTail[predecessor->index()] = epoch;
                    if (m_hasSet[predecessor->index()] != epoch)
                        add(predecessor);
                }
            }
            for (unsigned i = 0; i < accesses.size(); ++i) {
                Access& access = accesses[i];
                if (!access.isSet)
                    continue;
                bool isLast = i + 1 == accesses.size() || accesses[i + 1].block != access.block;
                if (!isLast || m_isLiveAtTail[access.block->index()] != epoch)
                    access.value->replaceWithNop();
            }
        }
        m_firstLiveBlockOfVariable[numVariables] = m_blocksLiveAtHead.size();
        m_isLiveAtHead.fill(0);
        return Outcome::Complete;
    }

    void selectVariable(unsigned variable)
    {
        m_epoch = variable + 1;
        for (unsigned i = m_firstLiveBlockOfVariable[variable]; i < m_firstLiveBlockOfVariable[variable + 1]; ++i)
            m_isLiveAtHead[m_blocksLiveAtHead[i]] = m_epoch;
    }

    bool isLiveAtHead(BasicBlock* block) const { return m_isLiveAtHead[block->index()] == m_epoch; }
    bool hasLiveVariableAtHead(BasicBlock* block) const { return m_hasLiveVariableAtHead[block->index()]; }

private:
    Procedure& m_proc;
    Accesses& m_accesses;
    Vector<unsigned> m_isLiveAtHead;
    Vector<uint8_t> m_hasLiveVariableAtHead;
    Vector<unsigned> m_isLiveAtTail;
    Vector<unsigned> m_hasSet;
    Vector<unsigned> m_blocksLiveAtHead;
    Vector<unsigned, 32> m_firstLiveBlockOfVariable;
    unsigned m_epoch { 0 };
};

void fixSSAGlobally(Procedure& proc, Accesses& accesses, IndexSet<Value*>& valuesToDelete)
{
    LivenessByVariable livenessByVariable(proc, accesses);
    bool usesPerVariableLiveness = livenessByVariable.run() == LivenessByVariable::Outcome::Complete;
    std::optional<VariableLiveness> dense;
    std::optional<VariableLiveness::LiveAtHead> denseLiveAtHead;
    if (!usesPerVariableLiveness) {
        dense.emplace(proc);
        VariableLiveness& liveness = *dense;
    
    // Kill any dead Set's. Each Set creates work for us, so this is profitable.
    for (BasicBlock* block : proc) {
        VariableLiveness::LocalCalc localCalc(liveness, block);
        for (unsigned valueIndex = block->size(); valueIndex--;) {
            Value* value = block->at(valueIndex);
            if (value->opcode() == Set && !localCalc.isLive(value->as<VariableValue>()->variable()))
                value->replaceWithNop();
            localCalc.execute(valueIndex);
        }
    }
    
        denseLiveAtHead.emplace(liveness.liveAtHead());
    }
    
    SSACalculator ssa(proc);

    // Create a SSACalculator::Variable ("calcVar") for every variable.
    Vector<Variable*> calcVarToVariable;
    IndexMap<Variable*, SSACalculator::Variable*> variableToCalcVar(proc.variables().size());

    for (Variable* variable : proc.variables()) {
        SSACalculator::Variable* calcVar = ssa.newVariable();
        RELEASE_ASSERT(calcVar->index() == calcVarToVariable.size());
        calcVarToVariable.append(variable);
        variableToCalcVar[variable] = calcVar;
    }

    if (usesPerVariableLiveness) {
        for (Access& access : accesses.list) {
            if (access.value->opcode() != Set)
                continue;
            if (SSACalculator::Variable* calcVar = variableToCalcVar[proc.variables()[access.variable]])
                ssa.newDefBlock(calcVar, access.block);
        }
    } else
    for (BasicBlock* block : proc) {
        for (Value* value : *block) {
            if (value->opcode() != Set)
                continue;

            Variable* variable = value->as<VariableValue>()->variable();

            if (SSACalculator::Variable* calcVar = variableToCalcVar[variable])
                ssa.newDefBlock(calcVar, block);
        }
    }

    // Decide where Phis are to be inserted. This creates them but does not insert them.
    {
        CompilerTimingScope timingScope("B3"_s, "fixSSA: computePhis"_s);
        ssa.computePhis(
            [&] (BasicBlock* block) {
                return !usesPerVariableLiveness || livenessByVariable.hasLiveVariableAtHead(block);
            },
            [&] (SSACalculator::Variable* calcVar) {
                if (usesPerVariableLiveness)
                    livenessByVariable.selectVariable(calcVarToVariable[calcVar->index()]->index());
            },
            [&] (SSACalculator::Variable* calcVar, BasicBlock* block) -> Value* {
                Variable* variable = calcVarToVariable[calcVar->index()];
                if (!(usesPerVariableLiveness ? livenessByVariable.isLiveAtHead(block) : denseLiveAtHead->isLiveAtHead(block, variable)))
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
    Vector<uint8_t, 256> hasNoRemovals;
    hasNoRemovals.fill(0, proc.size());

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
            bool hasRemovals = false;

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
                for (Value*& child : value->children()) {
                    if (child->opcode() == Identity)
                        child = child->foldIdentity();
                }

                switch (value->opcode()) {
                case Get: {
                    Variable* variable = value->as<VariableValue>()->variable();
                    value->replaceWithIdentity(reachingDef(variable, valueIndex, value->origin()));
                    valuesToDelete.add(value);
                    hasRemovals = true;
                    break;
                }

                case Set: {
                    Variable* variable = value->as<VariableValue>()->variable();
                    setReachingDef(variable, value->child(0));
                    value->replaceWithNop();
                    hasRemovals = true;
                    break;
                }

                case Identity:
                case Nop:
                    hasRemovals = true;
                    break;

                default:
                    break;
                }
            }
            hasNoRemovals[block->index()] = !hasRemovals;

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
        [&](BasicBlock*) {
            unsigned undoLogSize = undoLogSizeAtEntry.takeLast();
            while (undoLog.size() > undoLogSize) {
                auto [index, previous] = undoLog.takeLast();
                reachingDefs[index] = previous;
            }
        });

    // This is isn't strictly necessary, but it leaves the IR nice and tidy, which is particularly
    // useful for phases that do size estimates.
    for (BasicBlock* block : proc) {
        if (hasNoRemovals[block->index()])
            continue;
        block->values().removeAllMatching(
            [&] (Value* value) -> bool {
                if (value->opcode() != Nop && (value->opcode() != Identity || !valuesToDelete.contains(value)))
                    return false;
                
                proc.deleteValue(value);
                return true;
            });
    }

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
    struct VariableByValue {
        void add(Value* value, Variable* variable) { table.add(value, variable); }
        Variable* get(Value* value) const { return isDemoted[value->index()] ? table.get(value) : nullptr; }
        auto begin() const { return table.begin(); }
        auto end() const { return table.end(); }

        const uint8_t* isDemoted;
        UncheckedKeyHashMap<Value*, Variable*> table { };
    };
    Vector<uint8_t, 1024> isDemoted;
    isDemoted.fill(0, proc.values().size());
    for (Value* value : values.values(proc.values()))
        isDemoted[value->index()] = 1;
    VariableByValue map { isDemoted.span().data() };
    VariableByValue phiMap { isDemoted.span().data() };

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
    
    // Lots of variables have trivial local liveness. We can allocate those without any
    // trouble.
    Accesses accesses;
    IndexSet<Value*> valuesToDelete;
    fixSSALocally(proc, accesses, valuesToDelete);
    if (killDeadVariables(proc, accesses)) {
        // Users of a forwarded Get can be in any block, so substitute everywhere before deleting.
        for (BasicBlock* block : proc.cachedBlocksInPreOrder()) {
            for (Value* value : *block)
                value->performSubstitution();
        }
        for (BasicBlock* block : proc.cachedBlocksInPreOrder()) {
            block->values().removeAllMatching(
                [&](Value* value) -> bool {
                    if (value->opcode() != Identity || !valuesToDelete.contains(value))
                        return false;

                    proc.deleteValue(value);
                    return true;
                });
        }
        forgetDemotedValues(proc);
        return false;
    }

    breakCriticalEdges(proc);

    fixSSAGlobally(proc, accesses, valuesToDelete);

    forgetDemotedValues(proc);
    proc.deleteAllVariables();

    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

