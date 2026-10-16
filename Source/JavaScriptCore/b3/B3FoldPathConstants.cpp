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
#include "B3Dominators.h"
#include "B3InsertionSet.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
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

#if USE(BUN_JSC_ADDITIONS)
    void run()
    {
        bool changed = false;

        if (B3FoldPathConstantsInternal::verbose)
            dataLog("B3 before folding path constants: \n", m_proc, "\n");

        Vector<std::pair<Value*, Override>, 32> overrides;

        auto addOverride = [&] (BasicBlock*, Value* value, const Override& override) {
            if (override.block->numPredecessors() != 1)
                return;
            overrides.append({ value, override });
        };

        for (BasicBlock* block : m_proc) {
            Value* branch = block->last();
            switch (branch->opcode()) {
            case Branch:
                if (block->successorBlock(0) == block->successorBlock(1))
                    continue;
                addOverride(block, branch->child(0), Override::nonZero(block->successorBlock(0)));
                addOverride(block, branch->child(0), Override::constant(block->successorBlock(1), 0));
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
                    addOverride(block, branch->child(0), Override::constant(switchCase.targetBlock(), switchCase.caseValue()));
                }
                break;
            }
            default:
                break;
            }
        }

        if (overrides.isEmpty())
            return;

        Dominators& dominators = m_proc.dominators();

        std::stable_sort(overrides.begin(), overrides.end(), [] (const auto& a, const auto& b) {
            return a.first->index() < b.first->index();
        });
        Vector<uint32_t, 512> firstOverridePlusOne;
        firstOverridePlusOne.fill(0, m_proc.values().size());
        for (unsigned i = overrides.size(); i--;)
            firstOverridePlusOne[overrides[i].first->index()] = i + 1;

        Vector<Override*, 16> overridesWithNewConstants;
        auto findOverride = [&] (BasicBlock* block, Value* value) -> Override* {
            unsigned index = value->index();
            if (index >= firstOverridePlusOne.size() || !firstOverridePlusOne[index])
                return nullptr;
            Override* result = nullptr;
            for (unsigned i = firstOverridePlusOne[index] - 1; i < overrides.size() && overrides[i].first == value; ++i) {
                Override& override = overrides[i].second;
                if (dominators.dominates(override.block, block) && override.isBetterThan(result ? *result : Override()))
                    result = &override;
            }
            return result;
        };
        auto getOverride = [&] (BasicBlock* block, Value* value) -> Override {
            Override* result = findOverride(block, value);
            return result ? *result : Override();
        };

        IndexSet<Value*> dependsOnOverride;

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
                    if (m_proc.positionIndependent() && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(value->child(1));
                    break;
                }

                default:
                    break;
                }

                for (Value*& child : value->children()) {
                    Override* override = findOverride(block, child);
                    if (!override || !override->hasValue)
                        continue;
                    if (!override->valueNode) {
                        override->valueNode = m_proc.addIntConstant(child->origin(), child->type(), override->value);
                        overridesWithNewConstants.append(override);
                    }
                    child = override->valueNode;
                    dependsOnOverride.add(value);
                }
            }
            m_insertionSet.execute(block);
        }
        for (Override* override : overridesWithNewConstants) {
            m_insertionSet.insertValue(0, override->valueNode);
            m_insertionSet.execute(override->block);
        }

        if (!dependsOnOverride.isEmpty())
            makeAccessesControlDependent(dependsOnOverride);

        if (changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
    }

    void makeAccessesControlDependent(IndexSet<Value*>& dependsOnOverride)
    {
        for (BasicBlock* block : m_proc.blocksInPreOrder()) {
            for (Value* value : *block) {
                bool depends = dependsOnOverride.contains(value);
                for (Value* child : value->children())
                    depends |= dependsOnOverride.contains(child);
                if (!depends)
                    continue;
                if (auto* memory = value->as<MemoryValue>())
                    memory->setControlDependent(true);
                else if (auto* patchpoint = value->as<PatchpointValue>())
                    patchpoint->effects.controlDependent = true;
                Effects effects = value->effects();
                if (value->opcode() == Phi || effects.mustExecute() || effects.controlDependent)
                    dependsOnOverride.remove(value);
                else
                    dependsOnOverride.add(value);
            }
        }
    }
#else
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
                    Override::nonZero(block->successorBlock(0)));
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
        Vector<Value*> overridden = copyToVector(overrides.keys());
        std::ranges::sort(overridden, { }, &Value::index);
        for (Value* value : overridden) {
            for (Override& override : overrides.find(value)->value) {
                if (!override.hasValue)
                    continue;
                override.valueNode =
                    m_insertionSet.insertIntConstant(0, value, override.value);
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
#endif
    
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

} // anonymous namespace

void foldPathConstants(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldPathConstants"_s);
    FoldPathConstants foldPathConstants(proc);
    foldPathConstants.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

