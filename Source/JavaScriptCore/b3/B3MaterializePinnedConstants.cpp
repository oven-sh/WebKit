/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3MaterializePinnedConstants.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlock.h"
#include "B3ImmediateForms.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PhaseScope.h"
#include "B3Policy.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapValue.h"
#include "B3UseCounts.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

namespace {

class PinnedConstantMaterializer {
public:
    explicit PinnedConstantMaterializer(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
        , m_finder(proc.policy().addressing.findPinnedConstant)
    {
    }

    bool run()
    {
        if (!m_finder || !(m_proc.hasEverAdded(Const64) || m_proc.hasEverAdded(ConstDouble)))
            return false;

        m_proc.resetValueOwners();

        bool changed = false;
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* user = block->at(index);
                for (unsigned childIndex = 0; childIndex < user->numChildren(); ++childIndex) {
                    Value* constant = user->child(childIndex);
                    std::optional<PinnedConstant> pinned = find(constant);
                    if (!pinned)
                        continue;
                    RELEASE_ASSERT(m_proc.code().isPinned(pinned->reg));
                    if (!shouldReplace(user, childIndex, *pinned))
                        continue;
                    user->child(childIndex) = replacementFor(user, index, constant, *pinned);
                    changed = true;
                }
            }
            m_insertionSet.execute(block);
        }

        BasicBlock* root = m_proc.at(0);
        for (Value* value : m_newRegisterValues) {
            root->values().insert(0, value);
            value->owner = root;
        }
        return changed;
    }

private:
    std::optional<PinnedConstant> find(const Value* constant) const
    {
        if (constant->opcode() == Const64)
            return m_finder(static_cast<uint64_t>(constant->asInt64()));
        if (constant->opcode() == ConstDouble)
            return m_finder(std::bit_cast<uint64_t>(constant->asDouble()));
        return std::nullopt;
    }

    bool shouldReplace(Value* user, unsigned childIndex, const PinnedConstant& pinned)
    {
        auto* stackmap = user->as<StackmapValue>();
        if (stackmap && stackmap->constrainedChild(childIndex).rep().isAny())
            return false;

        Value* constant = user->child(childIndex);
        if (!isSelectedAsImmediate(user, constant))
            return true;
        if (!pinned.replacesImmediates)
            return false;
        return !pinned.offset || !hasFoldableLoadOperand(user, constant);
    }

    bool hasFoldableLoadOperand(Value* user, Value* constant)
    {
        if (user->numChildren() != 2)
            return false;

        Value* other = user->child(0) == constant ? user->child(1) : user->child(0);
        if (other->opcode() != Load || other->owner != user->owner)
            return false;

        if (!m_useCounts)
            m_useCounts.emplace(m_proc);
        return m_useCounts->numUses(other) == 1;
    }

    Value* registerValue(Reg reg, const Value* user, unsigned index)
    {
        for (auto& [knownReg, value] : m_registerValues) {
            if (knownReg == reg)
                return value;
        }

        BasicBlock* root = m_proc.at(0);
        size_t end = user->owner == root ? index : root->size();
        Value* result = nullptr;
        for (size_t i = 0; i < end && !result; ++i) {
            auto* argument = root->at(i)->as<ArgumentRegValue>();
            if (argument && argument->argumentReg() == reg)
                result = argument;
        }
        if (!result) {
            result = m_proc.add<ArgumentRegValue>(Origin(), reg);
            m_newRegisterValues.append(result);
        }
        m_registerValues.append({ reg, result });
        return result;
    }

    Value* replacementFor(Value* user, unsigned index, Value* constant, const PinnedConstant& pinned)
    {
        Value* base = registerValue(pinned.reg, user, index);
        if (!pinned.offset) {
            if (base->type() == constant->type())
                return base;
            return m_insertionSet.insert<Value>(index, BitwiseCast, user->origin(), base);
        }

        ASSERT(base->type() == pointerType());
        MemoryValue* load = m_insertionSet.insert<MemoryValue>(index, Load, constant->type(), user->origin(), base, *pinned.offset, HeapRange());
        load->setControlDependent(false);
        load->setReadsMutability(Mutability::Immutable);
        return load;
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    PinnedConstantFinder m_finder;
    Vector<std::pair<Reg, Value*>, 2> m_registerValues;
    Vector<Value*, 2> m_newRegisterValues;
    std::optional<UseCountsWithoutUsingInstructions> m_useCounts;
};

} // anonymous namespace

bool materializePinnedConstants(Procedure& proc)
{
    PhaseScope phaseScope(proc, "materializePinnedConstants"_s);
    return PinnedConstantMaterializer(proc).run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
