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
#include "B3FoldPathConstants.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CaseCollectionInlines.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3InsertionSet.h"
#include "B3IntRange.h"
#include "B3PhaseScope.h"
#include "B3SwitchValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3FoldPathConstantsInternal {
static constexpr bool verbose = false;
}

class FoldPathConstants {
public:
    FoldPathConstants(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    void run()
    {
        bool changed = false;

        if (B3FoldPathConstantsInternal::verbose)
            dataLog("B3 before folding path constants: \n", m_proc, "\n");
        
        // Find all of the values that are the subject of a branch or switch. For any successor
        // that we dominate, install a value override at that block.

        UncheckedKeyHashMap<Value*, Vector<Override>> overrides;

        Dominators& dominators = m_proc.dominators();
        
        auto addOverride = [&] (
            BasicBlock* from, Value* value, const Override& override) {

            if (override.block->numPredecessors() != 1)
                return;
            ASSERT(override.block->predecessor(0) == from);

            Vector<Override>& forValue =
                overrides.add(value, Vector<Override>()).iterator->value;

            if (ASSERT_ENABLED) {
                for (const Override& otherOverride : forValue)
                    ASSERT_UNUSED(otherOverride, otherOverride.block != override.block);
            }

            if (B3FoldPathConstantsInternal::verbose)
                dataLog("Overriding ", *value, " from ", *from, ": ", override, "\n");
            
            forValue.append(override);
        };
        
        for (BasicBlock* block : m_proc) {
            Value* branch = block->last();
            switch (branch->opcode()) {
            case Branch:
                if (block->successorBlock(0) == block->successorBlock(1))
                    continue;
                addOverride(
                    block, branch->child(0),
                    branch->child(0)->returnsBool()
                        ? Override::constant(block->successorBlock(0), 1)
                        : Override::nonZero(block->successorBlock(0)));
                addOverride(
                    block, branch->child(0),
                    Override::constant(block->successorBlock(1), 0));
                break;
            case Switch: {
                SwitchValue* switchValue = branch->as<SwitchValue>();

                UncheckedKeyHashMap<BasicBlock*, unsigned> targetUses;
                for (SwitchCase switchCase : switchValue->cases(block))
                    targetUses.add(switchCase.targetBlock(), 0).iterator->value++;
                targetUses.add(switchValue->fallThrough(block), 0).iterator->value++;

                for (SwitchCase switchCase : switchValue->cases(block)) {
                    if (targetUses.find(switchCase.targetBlock())->value != 1)
                        continue;

                    addOverride(
                        block, branch->child(0),
                        Override::constant(switchCase.targetBlock(), switchCase.caseValue()));
                }
                break;
            }
            default:
                break;
            }
        }

        // Install the constants in the override blocks. We use one-shot insertion sets because
        // each block will get at most one thing inserted into it anyway.
        for (auto& entry : overrides) {
            for (Override& override : entry.value) {
                if (!override.hasValue)
                    continue;
                override.valueNode =
                    m_insertionSet.insertIntConstant(0, entry.key, override.value);
                m_insertionSet.execute(override.block);
            }
        }

        // Replace all uses of a value that has an override with that override, if appropriate.
        // Certain instructions get special treatment.
        auto getOverride = [&] (BasicBlock* block, Value* value) -> Override {
            auto iter = overrides.find(value);
            if (iter == overrides.end())
                return Override();

            Vector<Override>& forValue = iter->value;
            Override result;
            for (Override& override : forValue) {
                if (dominators.dominates(override.block, block)
                    && override.isBetterThan(result))
                    result = override;
            }

            if (B3FoldPathConstantsInternal::verbose)
                dataLog("In block ", *block, " getting override for ", *value, ": ", result, "\n");

            return result;
        };
        
        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);

                switch (value->opcode()) {
                case Branch: {
                    if (getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithJump(block, block->taken());
                        changed = true;
                    }
                    break;
                }

                case Equal: {
                    if (value->child(1)->isInt(0)
                        && getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithIdentity(
                            m_insertionSet.insertIntConstant(valueIndex, value, 0));
                    }
                    break;
                }

                case NotEqual: {
                    if (value->child(1)->isInt(0)
                        && getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithIdentity(
                            m_insertionSet.insertIntConstant(valueIndex, value, 1));
                    }
                    break;
                }

                default:
                    break;
                }

                for (Value*& child : value->children()) {
                    Override override = getOverride(block, child);
                    if (override.valueNode)
                        child = override.valueNode;
                }
            }
            m_insertionSet.execute(block);
        }

        if (changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
    }
    
private:
    struct Override {
        Override()
        {
        }

        static Override LEGACY_NODELETE constant(BasicBlock* block, int64_t value)
        {
            Override result;
            result.block = block;
            result.hasValue = true;
            result.value = value;
            if (value)
                result.isNonZero = true;
            return result;
        }

        static Override LEGACY_NODELETE nonZero(BasicBlock* block)
        {
            Override result;
            result.block = block;
            result.isNonZero = true;
            return result;
        }

        bool LEGACY_NODELETE isBetterThan(const Override& override)
        {
            if (hasValue && !override.hasValue)
                return true;
            if (isNonZero && !override.isNonZero)
                return true;
            return false;
        }

        void dump(PrintStream& out) const
        {
            out.print("{block = ", pointerDump(block), ", value = ");
            if (hasValue)
                out.print(value);
            else
                out.print("<none>");
            out.print(", isNonZero = ", isNonZero);
            if (valueNode)
                out.print(", valueNode = ", *valueNode);
            out.print("}");
        }

        BasicBlock* block { nullptr };
        bool hasValue { false };
        bool isNonZero { false };
        int64_t value { 0 };
        Value* valueNode { nullptr };
    };

    Procedure& m_proc;
    InsertionSet m_insertionSet;
};

class FoldPathConstantsInValueOrder {
public:
    FoldPathConstantsInValueOrder(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
        , m_foldsImpliedComparisons(proc.policy().optimization.foldsImpliedComparisons)
    {
    }

    void run()
    {
        if (B3FoldPathConstantsInternal::verbose)
            dataLog("B3 before folding path constants: \n", m_proc, "\n");

        collectOverrides();
        if (m_overrides.isEmpty())
            return;

        m_dominators = &m_proc.dominators();
        sortOverrides();

        bool changed = applyOverrides();

        if (changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
    }
    
private:
    struct Override {
        Override()
        {
        }

        static Override LEGACY_NODELETE constant(BasicBlock* block, int64_t value)
        {
            Override result;
            result.block = block;
            result.hasValue = true;
            result.value = value;
            if (value)
                result.isNonZero = true;
            return result;
        }

        static Override LEGACY_NODELETE nonZero(BasicBlock* block)
        {
            Override result;
            result.block = block;
            result.isNonZero = true;
            return result;
        }

        bool LEGACY_NODELETE isBetterThan(const Override& override)
        {
            if (hasValue && !override.hasValue)
                return true;
            if (isNonZero && !override.isNonZero)
                return true;
            return false;
        }

        void dump(PrintStream& out) const
        {
            out.print("{block = ", pointerDump(block), ", value = ");
            if (hasValue)
                out.print(value);
            else
                out.print("<none>");
            out.print(", isNonZero = ", isNonZero);
            if (valueNode)
                out.print(", valueNode = ", *valueNode);
            out.print("}");
        }

        BasicBlock* block { nullptr };
        bool hasValue { false };
        bool isNonZero { false };
        int64_t value { 0 };
        Value* valueNode { nullptr };
    };

    struct DecidedComparison {
        Value* comparison { nullptr };
        IntRange operandRange;
        Override falseOverride;
        Override trueOverride;
    };

    static bool canHoldOverrides(BasicBlock* block)
    {
        return block->numPredecessors() == 1;
    }

    void addOverride(Value* value, const Override& override)
    {
        if (value->isConstant() || !canHoldOverrides(override.block))
            return;
        m_overrides.append({ value, override });
    }

    void addDecidedComparison(BasicBlock* block, Value* condition)
    {
        if (!m_foldsImpliedComparisons || !isComparisonWithConstant(condition))
            return;
        for (bool holds : { true, false }) {
            BasicBlock* successor = block->successorBlock(holds ? 0 : 1);
            if (!canHoldOverrides(successor))
                continue;
            auto& decidedComparisons = m_decidedComparisons.add(condition->child(0), Vector<DecidedComparison>()).iterator->value;
            decidedComparisons.append(DecidedComparison { condition, rangeImpliedBy(condition, holds), Override::constant(successor, 0), Override::constant(successor, 1) });
        }
    }

    void collectOverrides()
    {
        for (BasicBlock* block : m_proc) {
            Value* branch = block->last();
            switch (branch->opcode()) {
            case Branch:
                if (block->successorBlock(0) == block->successorBlock(1))
                    continue;
                addOverride(branch->child(0), branch->child(0)->returnsBool() ? Override::constant(block->successorBlock(0), 1) : Override::nonZero(block->successorBlock(0)));
                addOverride(branch->child(0), Override::constant(block->successorBlock(1), 0));
                addDecidedComparison(block, branch->child(0));
                break;
            case Switch: {
                SwitchValue* switchValue = branch->as<SwitchValue>();

                UncheckedKeyHashMap<BasicBlock*, unsigned> targetUses;
                for (SwitchCase switchCase : switchValue->cases(block))
                    targetUses.add(switchCase.targetBlock(), 0).iterator->value++;
                targetUses.add(switchValue->fallThrough(block), 0).iterator->value++;

                for (SwitchCase switchCase : switchValue->cases(block)) {
                    if (targetUses.find(switchCase.targetBlock())->value != 1)
                        continue;
                    addOverride(branch->child(0), Override::constant(switchCase.targetBlock(), switchCase.caseValue()));
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void sortOverrides()
    {
        std::stable_sort(m_overrides.begin(), m_overrides.end(), [] (const auto& a, const auto& b) {
            return a.first->index() < b.first->index();
        });
        m_firstOverride.fill(std::nullopt, m_proc.values().size());
        for (unsigned i = m_overrides.size(); i--;)
            m_firstOverride[m_overrides[i].first->index()] = i;
    }

    Override* findOverride(BasicBlock* block, Value* value)
    {
        Override* result = nullptr;
        unsigned index = value->index();
        if (index < m_firstOverride.size() && m_firstOverride[index]) {
            for (unsigned i = *m_firstOverride[index]; i < m_overrides.size() && m_overrides[i].first == value; ++i) {
                Override& override = m_overrides[i].second;
                if (m_dominators->dominates(override.block, block) && override.isBetterThan(result ? *result : Override()))
                    result = &override;
            }
        }
        if (m_decidedComparisons.isEmpty() || !isComparisonWithConstant(value))
            return result;
        auto found = m_decidedComparisons.find(value->child(0));
        if (found == m_decidedComparisons.end())
            return result;
        for (DecidedComparison& decided : found->value) {
            if (decided.comparison == value || !m_dominators->dominates(decided.trueOverride.block, block))
                continue;
            TriState outcome = evaluateComparison(value->opcode(), decided.operandRange, value->child(1)->asInt());
            if (outcome == TriState::Indeterminate)
                continue;
            Override& override = outcome == TriState::True ? decided.trueOverride : decided.falseOverride;
            if (override.isBetterThan(result ? *result : Override()))
                result = &override;
        }
        return result;
    }

    Override getOverride(BasicBlock* block, Value* value)
    {
        Override* result = findOverride(block, value);
        return result ? *result : Override();
    }

    bool applyOverrides()
    {
        bool changed = false;
        Vector<Override*, 16> overridesWithNewConstants;

        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);

                switch (value->opcode()) {
                case Branch: {
                    if (getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithJump(block, block->taken());
                        changed = true;
                    }
                    break;
                }

                case Equal: {
                    if (value->child(1)->isInt(0) && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(m_insertionSet.insertIntConstant(valueIndex, value, 0));
                    break;
                }

                case NotEqual: {
                    if (value->child(1)->isInt(0) && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(m_insertionSet.insertIntConstant(valueIndex, value, 1));
                    break;
                }

                case Select: {
                    if (m_foldsImpliedComparisons && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(value->child(1));
                    break;
                }

                default:
                    break;
                }

                for (Value*& child : value->children()) {
                    if (m_foldsImpliedComparisons) {
                        bool isCast = child->opcode() == BitwiseCast;
                        Value* select = isCast ? child->child(0) : child;
                        if (select->opcode() == Select) {
                            Override decided = getOverride(block, select->child(0));
                            if (decided.isNonZero || decided.hasValue) {
                                Value* chosen = select->child(decided.isNonZero ? 1 : 2);
                                child = isCast ? m_insertionSet.insert<Value>(valueIndex, BitwiseCast, child->origin(), chosen) : chosen;
                            }
                        }
                    }
                    Override* override = findOverride(block, child);
                    if (!override || !override->hasValue)
                        continue;
                    if (!override->valueNode) {
                        override->valueNode = m_proc.addIntConstant(child->origin(), child->type(), override->value);
                        overridesWithNewConstants.append(override);
                    }
                    child = override->valueNode;
                }
            }
            m_insertionSet.execute(block);
        }
        for (Override* override : overridesWithNewConstants) {
            m_insertionSet.insertValue(0, override->valueNode);
            m_insertionSet.execute(override->block);
        }
        return changed;
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    const bool m_foldsImpliedComparisons;
    Dominators* m_dominators { nullptr };
    Vector<std::pair<Value*, Override>, 32> m_overrides;
    UncheckedKeyHashMap<Value*, Vector<DecidedComparison>> m_decidedComparisons;
    Vector<std::optional<unsigned>, 512> m_firstOverride;
};

} // anonymous namespace

void foldPathConstants(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldPathConstants"_s);
    if (proc.policy().optimization.foldsImpliedComparisons || proc.policy().addressing.relocationLowering) {
        FoldPathConstantsInValueOrder foldPathConstants(proc);
        foldPathConstants.run();
        return;
    }
    FoldPathConstants foldPathConstants(proc);
    foldPathConstants.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

