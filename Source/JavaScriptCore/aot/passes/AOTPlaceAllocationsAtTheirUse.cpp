/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceAllocationsAtTheirUse.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedMove {
    ValueID original;
    ProgramPoint point;
    // Where the copy goes: an operand of a value that stays, or of the move with this index.
    ValueID user;
    std::optional<uint32_t> movedUser;
    uint32_t operandIndex { 0 };
};

// A list that one operation alone takes is made in front of that operation, and what it alone holds in front of it.
class AllocationPlacer {
public:
    explicit AllocationPlacer(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_isTakenByOneOperation(context.function.values.size())
        , m_isMoved(context.function.values.size())
    {
    }

    PassOutcome run()
    {
        Vector<ValueID, 8> candidates = collectCandidates();
        const UseLists& useLists = m_analyses.useLists();
        for (ValueID id : candidates) {
            const Use& use = useLists.usesOf(id)[0];
            if (!m_isTakenByOneOperation.quickGet(use.user.index()) && isApartFrom(id, use))
                plan(id, useLists.pointOf(use), use.user, std::nullopt, use.operandIndex);
        }
        if (m_moves.isEmpty())
            return PassOutcome::Unchanged;

        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        insertCopies(builder);
        eraseOriginals(builder);
        return PassOutcome::Changed;
    }

private:
    Vector<ValueID, 8> collectCandidates()
    {
        const UseLists& useLists = m_analyses.useLists();
        Vector<ValueID, 8> candidates;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (!allocationsPlacedAtTheirUse.contains(m_function.at(id).opcode) || !canStandAnywhere(id))
                    continue;
                std::span<const Use> uses = useLists.usesOf(id);
                if (uses.size() != 1 || uses[0].kind != Use::Kind::Operand)
                    continue;
                m_isTakenByOneOperation.quickSet(id.index());
                candidates.append(id);
            }
        }
        return candidates;
    }

    // Running out of memory is no event of the program, so it holds nothing in its place.
    bool canStandAnywhere(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::CreateUnmappedArguments && m_analyses.useLists().useCount(value.restOperand()) != 1)
            return false;
        Effects effects = m_analyses.effectsOf(id).onAnyEdge();
        if (!effects.isRemovableWhenUnused())
            return false;
        effects.mayThrow = false;
        return !effects.interferesWith(Effects::unknownCode());
    }

    bool mayMoveTo(ValueID id, BlockID target)
    {
        return m_analyses.useLists().blockOf(id) == target || !m_analyses.isInCycle(target);
    }

    bool isApartFrom(ValueID id, const Use& use)
    {
        const UseLists& useLists = m_analyses.useLists();
        if (!mayMoveTo(id, use.block))
            return false;
        ProgramPoint from = useLists.pointOf(id);
        ProgramPoint to = useLists.pointOf(use);
        if (from.block != to.block)
            return true;
        const BlockValues& values = m_function.at(from.block).values;
        for (uint32_t index = from.index + 1; index < to.index; ++index) {
            if (!m_isTakenByOneOperation.quickGet(values[index].index()))
                return true;
        }
        return false;
    }

    // What the object holds is planned behind it and made in front of it.
    void plan(ValueID id, ProgramPoint point, ValueID user, std::optional<uint32_t> movedUser, uint32_t operandIndex)
    {
        uint32_t move = m_moves.size();
        m_moves.append(PlannedMove { id, point, user, movedUser, operandIndex });
        m_isMoved.quickSet(id.index());
        m_definitionBlocks.appendIfNotContains(m_analyses.useLists().blockOf(id));
        m_function.at(id).forEachOperand([&](ValueID operand, uint32_t index) {
            if (m_isTakenByOneOperation.quickGet(operand.index()) && mayMoveTo(operand, point.block))
                plan(operand, point, id, move, index);
        });
    }

    void insertCopies(IRBuilder& builder)
    {
        Vector<Value> copies(m_moves.size(), [&](size_t index) {
            return Value(m_function.at(m_moves[index].original));
        });
        for (size_t index = m_moves.size(); index--;) {
            const PlannedMove& move = m_moves[index];
            builder.setOrigin(copies[index].origin());
            ValueID copy = builder.insertBefore(move.point, WTF::move(copies[index]));
            if (move.movedUser)
                copies[*move.movedUser].setOperand(move.operandIndex, copy);
            else
                m_function.edit(move.user, ValueEdit::Definition).setOperand(move.operandIndex, copy);
        }
        builder.applyInsertions();
    }

    void eraseOriginals(IRBuilder& builder)
    {
        for (BlockID block : m_definitionBlocks) {
            const BlockValues& values = m_function.at(block).values;
            BitVector indexes;
            for (size_t index = 0; index < values.size(); ++index) {
                if (m_isMoved.get(values[index].index()))
                    indexes.set(index);
            }
            builder.eraseValues(block, indexes);
        }
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    BitVector m_isTakenByOneOperation;
    BitVector m_isMoved;
    Vector<PlannedMove, 8> m_moves;
    Vector<BlockID, 4> m_definitionBlocks;
};

} // anonymous namespace

PassOutcome placeAllocationsAtTheirUse(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().intersects(allocationsPlacedAtTheirUse))
        return PassOutcome::Unchanged;
    return AllocationPlacer(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
