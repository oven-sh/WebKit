/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSpecifiedValues.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeldOperands.h"
#include "AOTIRBuilder.h"
#include "AOTLayout.h"
#include "AOTMemoryLocation.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The result is the operand as it was found.
static bool givesOperandBack(const Value& user, uint32_t operandIndex)
{
    if (user.operandUses(operandIndex) != OperandUses { OperandUse::Returns })
        return false;
    return isCastWithoutTest(user.opcode) || familyOf(user.opcode) == OperationFamily::Representation || user.opcode == Opcode::Results;
}

// The variable that the word was read from gets it back.
static bool storesBackIntoVariable(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::PutModuleVariable)
        return holdsDeclaredTypeOnlyBehindEntry(program.at(user.variable()).flow);
    return user.opcode == Opcode::PutEnvironmentField && operandIndex == OperandIndex::PutEnvironmentField::value && holdsDeclaredTypeOnlyBehindEntry(flowOf(program, { user.layout(), user.field() }).location);
}

bool handsOperandOn(const Program& program, const Value& user, uint32_t operandIndex)
{
    return user.opcode == Opcode::ValidateSameValue || isHeldOperand(user, operandIndex) || storesBackIntoVariable(program, user, operandIndex) || givesOperandBack(user, operandIndex);
}

Value makeCastBehindFact(const Function& function, TypeArena& types, ValueID value, ValueID fact)
{
    Value cast = IRBuilder::make(Opcode::AssumeType);
    cast.becomeAssumeType(value, Proven::behindFact(types.intern(types.at(function.at(value).type).withoutPrimitives(Primitive::Unspecified))));
    cast.setFactOperand(fact);
    return cast;
}

namespace {

class FactNaming {
public:
    FactNaming(IRBuilder& builder, FunctionAnalyses& analyses)
        : m_builder(builder)
        , m_analyses(analyses)
        , m_program(builder.context().program)
        , m_function(builder.function())
        , m_types(builder.types())
    {
    }

    bool run()
    {
        bool hasSource = std::ranges::any_of(m_function.values, [&](const Value& value) {
            return value.type && m_types.at(value.type).mayBeUnspecified();
        });
        if (!hasSource)
            return false;

        widenTypes();
        Origin origin = m_builder.origin();
        for (BlockID block : m_analyses.cfg().reversePostOrder())
            collectOperandsToCastIn(block);
        chooseBlocksOfCasts();
        for (const OperandToCast& use : m_operandsToCast)
            castOperand(use);
        m_builder.applyInsertions();
        m_builder.setOrigin(origin);
        return m_didChange;
    }

private:
    bool mayBeUnspecified(ValueID id) const { return m_types.at(m_function.at(id).type).mayBeUnspecified(); }

    // From each word that may be anything to whatever it is handed to.
    void widenTypes()
    {
        Vector<ValueID, 16> worklist;
        for (uint32_t index = 0; index < m_function.values.size(); ++index) {
            if (m_function.values[index].type && mayBeUnspecified(ValueID(index)))
                worklist.append(ValueID(index));
        }
        const UseLists& useLists = m_analyses.useLists();
        auto widen = [&](ValueID id) {
            if (mayBeUnspecified(id))
                return;
            m_function.edit(id, ValueEdit::Type).type = m_types.intern(m_types.at(m_function.at(id).type).withPrimitives(Primitive::Unspecified));
            m_didChange = true;
            worklist.append(id);
        };
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            if (id.index() >= useLists.numberOfValues())
                continue;
            for (const Use& use : useLists.usesOf(id)) {
                if (use.isEdgeUse()) {
                    widen(m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex]);
                    continue;
                }
                if (use.kind != Use::Kind::Operand)
                    continue;
                const Value& user = m_function.at(use.user);
                if (user.opcode == Opcode::Results) {
                    for (const Use& read : useLists.usesOf(use.user)) {
                        if (read.kind == Use::Kind::Operand && m_function.at(read.user).opcode == Opcode::ResultAt && m_function.at(read.user).index() == use.operandIndex)
                            widen(read.user);
                    }
                    continue;
                }
                bool dropsUnspecified = user.opcode == Opcode::AssumeType && user.factOperand();
                if (!dropsUnspecified && user.operandUses(use.operandIndex).contains(OperandUse::Returns) && handsOperandOn(m_program, user, use.operandIndex))
                    widen(use.user);
            }
        }
    }

    // A fact lasts for ever: what has been filled where one use stands has been filled wherever that use has been passed. A block that has a handler may be left in front of the cast.
    // So a block in which an operand is used makes a cast of it unless a block without a handler that dominates it makes one. Of the blocks that make one and have no handler none dominates another.
    void chooseBlocksOfCasts()
    {
        for (const OperandToCast& use : m_operandsToCast) {
            if (m_casts.isEmpty() || m_casts.last().operand != use.operand || m_casts.last().block != use.block)
                m_casts.append({ use.operand, use.block, 0, ValueID() });
        }
        AOT::sort(m_casts, { }, &Cast::key);
        removeRepeatedElements(m_casts, [](const Cast& a, const Cast& b) { return a.key() == b.key(); });

        Vector<uint32_t, 16> inOrderOfTree;
        for (uint32_t first = 0, end; first < m_casts.size(); first = end) {
            for (end = first + 1; end < m_casts.size() && m_casts[end].operand == m_casts[first].operand; ++end) { }
            m_casts[first].maker = first;
            if (end - first == 1)
                continue;

            const Dominators& dominators = m_analyses.dominators();
            inOrderOfTree.shrink(0);
            for (uint32_t index = first; index < end; ++index)
                inOrderOfTree.append(index);
            AOT::sort(inOrderOfTree, { }, [&](uint32_t index) { return dominators.numberOf(m_casts[index].block); });
            std::optional<uint32_t> makerAbove;
            for (uint32_t index : inOrderOfTree) {
                BlockID block = m_casts[index].block;
                if (makerAbove && dominators.numberOf(block) <= dominators.lastNumberBelow(m_casts[*makerAbove].block)) {
                    m_casts[index].maker = *makerAbove;
                    continue;
                }
                m_casts[index].maker = index;
                makerAbove = m_function.at(block).handler ? std::nullopt : std::optional { index };
            }
        }
    }

    ValueID castAt(ProgramPoint point, Origin origin, ValueID operand)
    {
        auto castOfBlock = std::ranges::lower_bound(m_casts, Cast { operand, point.block, 0, ValueID() }.key(), { }, &Cast::key);
        RELEASE_ASSERT(castOfBlock != m_casts.end() && castOfBlock->operand == operand && castOfBlock->block == point.block);
        Cast& made = m_casts[castOfBlock->maker];
        if (made.result)
            return made.result;
        RELEASE_ASSERT(made.block == point.block);
        m_builder.setOrigin(origin);
        if (!m_factInBlock)
            m_factInBlock = m_builder.insertBefore(point, IRBuilder::make(Opcode::Specified));
        // A fact that some way has not made is no word to look at: that it has been made where the use stands is what the new fact says.
        bool isFact = m_types.at(m_function.at(operand).type).withoutPrimitives(Primitive::Unspecified).isOpaque(OpaqueKind::Fact);
        made.result = isFact ? m_factInBlock : m_builder.insertBefore(point, makeCastBehindFact(m_function, m_types, operand, m_factInBlock));
        m_didChange = true;
        return made.result;
    }

    // A result that is declared without `unspecified` is filled by every Return: what a Return hands over for it is a part of the variant that it returns.
    // While the worker is made its results are declared so, and those that a Return leaves out get their type behind this.
    bool fillsResultDeclaredFilled(ValueID user, uint32_t operandIndex)
    {
        const Value& results = m_function.at(user);
        if (results.opcode != Opcode::Results || m_function.at(results.operand(operandIndex)).opcode == Opcode::Unspecified || operandIndex >= m_function.results.size())
            return false;
        bool isReturned = std::ranges::any_of(m_analyses.useLists().usesOf(user), [&](const Use& use) {
            return use.kind == Use::Kind::TerminatorOperand && m_function.at(use.block).terminator.kind == TerminatorKind::Return;
        });
        return isReturned && !TypingContext { m_program, m_function, m_types }.typeOf(m_function.results[operandIndex]).mayBeUnspecified();
    }

    void collectOperandsToCastIn(BlockID id)
    {
        uint32_t numberOfValues = m_function.at(id).values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID user = m_function.at(id).values[index];
            for (uint32_t operandIndex = 0; operandIndex < m_function.at(user).numberOfOperands(); ++operandIndex) {
                ValueID operand = m_function.at(user).operand(operandIndex);
                if (!operand || !mayBeUnspecified(operand) || (handsOperandOn(m_program, m_function.at(user), operandIndex) && !fillsResultDeclaredFilled(user, operandIndex)))
                    continue;
                m_operandsToCast.append({ id, index, user, operandIndex, operand });
            }
        }

        const Terminator& terminator = m_function.at(id).terminator;
        if (terminator.kind == TerminatorKind::Return && m_function.results.size() == 1 && TypingContext { m_program, m_function, m_types }.typeOf(m_function.result()).mayBeUnspecified())
            return;
        for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
            ValueID operand = terminator.operand(operandIndex);
            if (!operand || !mayBeUnspecified(operand))
                continue;
            m_operandsToCast.append({ id, numberOfValues, ValueID(), operandIndex, operand });
        }
    }

    struct OperandToCast {
        BlockID block;
        uint32_t index;
        ValueID user;
        uint32_t operandIndex;
        ValueID operand;
    };

    void castOperand(const OperandToCast& use)
    {
        if (m_blockOfFact != use.block) {
            m_blockOfFact = use.block;
            m_factInBlock = { };
        }
        if (use.user) {
            ValueID cast = castAt({ use.block, use.index }, m_function.at(use.user).origin(), use.operand);
            m_function.edit(use.user).setOperand(use.operandIndex, cast);
            return;
        }
        ValueID cast = castAt({ use.block, use.index }, m_function.at(use.block).terminator.origin(), use.operand);
        m_function.edit(use.block, BlockEdit::Terminator).terminator.setOperand(use.operandIndex, cast);
    }

    struct Cast {
        uint64_t key() const { return static_cast<uint64_t>(operand.index()) << 32 | block.index(); }

        ValueID operand;
        BlockID block;
        uint32_t maker;
        ValueID result;
    };

    IRBuilder& m_builder;
    FunctionAnalyses& m_analyses;
    const Program& m_program;
    Function& m_function;
    TypeArena& m_types;
    BlockID m_blockOfFact;
    ValueID m_factInBlock;
    Vector<OperandToCast> m_operandsToCast;
    Vector<Cast> m_casts;
    bool m_didChange { false };
};

} // anonymous namespace

bool nameFactsWhereUnspecifiedValuesAreUsed(IRBuilder& builder, FunctionAnalyses& analyses)
{
    if (!FactNaming(builder, analyses).run())
        return false;
    analyses.didChangeValues();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
