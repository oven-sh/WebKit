/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHoistTestsIntoPredecessors.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static BitVector singleIndex(size_t index)
{
    BitVector indexes;
    indexes.set(index);
    return indexes;
}

namespace {

using TypePredicate = bool (Type::*)() const;

struct TruthinessTest {
    BlockID mergePoint;
    uint32_t parameterIndex;
};

struct ParameterOperation {
    BlockID mergePoint;
    uint32_t valueIndex;
    uint32_t parameterIndex;
    uint32_t operandIndex;
};

class TestHoister {
public:
    explicit TestHoister(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
    }

    PassOutcome run()
    {
        collectCandidates();
        if (m_truthinessTests.isEmpty() && m_operations.isEmpty())
            return PassOutcome::Unchanged;

        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        ValueSubstitution substitution(m_function);
        for (const TruthinessTest& test : m_truthinessTests)
            copyIntoPredecessors(builder, test);
        for (const ParameterOperation& operation : m_operations)
            copyIntoPredecessors(builder, substitution, operation);
        builder.applyInsertions();

        Vector<ValueID, 4> retypedParameters;
        for (const TruthinessTest& test : m_truthinessTests)
            retypedParameters.append(removeFromMergePoint(builder, test));
        for (const ParameterOperation& operation : m_operations)
            removeFromMergePoint(builder, operation);
        substitution.apply(m_function, &m_analyses);
        m_analyses.didChangeValues();

        if (!retypedParameters.isEmpty())
            narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }, retypedParameters.span());
        return PassOutcome::Changed;
    }

private:
    const Type& typeOf(ValueID id) const { return m_context.program.typeOf(m_function, id); }

    void collectCandidates()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = m_function.at(id);
            if (id == cfg.root() || block.parameters.isEmpty() || block.values.isEmpty())
                continue;
            if (!collectTruthinessTest(id, block))
                collectOperation(id, block);
        }
    }

    bool collectTruthinessTest(BlockID id, const Block& block)
    {
        if (block.terminator.kind != TerminatorKind::Branch || block.values.size() != 1 || block.terminator.conditionOperand() != block.values[0])
            return false;
        ValueID testID = block.values[0];
        const Value& test = m_function.at(testID);
        if (test.opcode != Opcode::ToBoolean)
            return false;
        size_t parameterIndex = block.parameters.find(test.valueOperand());
        if (parameterIndex == notFound || !canHoistIntoPredecessors(id, parameterIndex, &Type::isBoolean))
            return false;
        const UseLists& useLists = m_analyses.useLists();
        if (useLists.useCount(test.valueOperand()) != 1 || useLists.useCount(testID) != 1)
            return false;
        if (isConditionOfBranchWithProofs(m_function, useLists, testID))
            return false;
        if (!m_analyses.isRemovableWhenUnused(testID))
            return false;
        m_truthinessTests.append({ id, static_cast<uint32_t>(parameterIndex) });
        return true;
    }

    // The constants of an operation stand before it.
    uint32_t indexOfFirstOperation(const Block& block) const
    {
        uint32_t index = 0;
        while (index < block.values.size() && isRematerializable(m_function.at(block.values[index])))
            ++index;
        return index;
    }

    std::optional<TypePredicate> findCheapOperandType(const Value& operation, const Type& parameter) const
    {
        if (relationalOperators.contains(operation.opcode) && !parameter.isNumber())
            return &Type::isNumber;
        if (operatorsOnInt32.contains(operation.opcode) && parameter.isNumber() && !parameter.isInteger())
            return &Type::isInteger;
        return std::nullopt;
    }

    void collectOperation(BlockID id, const Block& block)
    {
        uint32_t valueIndex = indexOfFirstOperation(block);
        if (valueIndex == block.values.size())
            return;
        ValueID operation = block.values[valueIndex];
        const Value& value = m_function.at(operation);
        if (!operatorsHoistedIntoPredecessors.contains(value.opcode))
            return;
        size_t leftIndex = block.parameters.find(value.operand(0));
        size_t rightIndex = block.parameters.find(value.operand(1));
        if ((leftIndex == notFound) == (rightIndex == notFound))
            return;
        uint32_t operandIndex = leftIndex == notFound ? 1 : 0;
        size_t parameterIndex = operandIndex ? rightIndex : leftIndex;
        ValueID parameter = value.operand(operandIndex);
        std::optional<TypePredicate> isCheap = findCheapOperandType(value, typeOf(parameter));
        if (!isCheap || !canHoistIntoPredecessors(id, parameterIndex, *isCheap))
            return;
        bool hasSameHandler = std::ranges::all_of(m_analyses.cfg().incomingEdges(id), [&](Edge edge) {
            return m_function.at(edge.from).handler == block.handler;
        });
        if (!hasSameHandler)
            return;
        const UseLists& useLists = m_analyses.useLists();
        if (useLists.useCount(parameter) != 1 || isConditionOfBranchWithProofs(m_function, useLists, operation))
            return;
        m_operations.append({ id, valueIndex, static_cast<uint32_t>(parameterIndex), operandIndex });
    }

    bool canHoistIntoPredecessors(BlockID mergePoint, size_t parameterIndex, TypePredicate isCheap)
    {
        if (m_function.at(mergePoint).isHandlerBlock())
            return false;
        bool hasCheapArgument = false;
        for (Edge edge : m_analyses.cfg().incomingEdges(mergePoint)) {
            if (edge.isHandlerEdge() || edge.from == mergePoint || m_function.at(edge.from).terminator.kind != TerminatorKind::Jump)
                return false;
            hasCheapArgument |= (typeOf(targetOf(m_function, edge).arguments[parameterIndex]).*isCheap)();
        }
        return hasCheapArgument && !m_analyses.isEnteredFromUnreachableBlock(mergePoint);
    }

    ValueID insertBeforeTerminator(IRBuilder& builder, BlockID block, Value&& value)
    {
        return builder.insertBefore(pointBeforeTerminatorOf(m_function, block), WTF::move(value));
    }

    void copyIntoPredecessors(IRBuilder& builder, const TruthinessTest& candidate)
    {
        ValueID test = m_function.at(candidate.mergePoint).values[0];
        builder.setOrigin(m_function.at(test).origin());
        for (Edge edge : m_analyses.cfg().incomingEdges(candidate.mergePoint)) {
            ValueID argument = targetOf(m_function, edge).arguments[candidate.parameterIndex];
            if (typeOf(argument).isBoolean())
                continue;
            ValueID copy = insertBeforeTerminator(builder, edge.from, IRBuilder::make(Opcode::ToBoolean, { argument }));
            editTarget(m_function, edge).arguments[candidate.parameterIndex] = copy;
        }
    }

    void copyIntoPredecessors(IRBuilder& builder, ValueSubstitution& substitution, const ParameterOperation& candidate)
    {
        const Block& mergePoint = m_function.at(candidate.mergePoint);
        ValueID operation = mergePoint.values[candidate.valueIndex];
        Opcode opcode = m_function.at(operation).opcode;
        std::array<ValueID, 2> operands { m_function.at(operation).operand(0), m_function.at(operation).operand(1) };
        uint32_t otherOperandIndex = 1 - candidate.operandIndex;
        ValueID otherOperand = operands[otherOperandIndex];
        bool otherOperandIsConstantOfMergePoint = std::ranges::contains(mergePoint.values.span().first(candidate.valueIndex), otherOperand);
        builder.setOrigin(m_function.at(operation).origin());
        ValueID result = builder.addParameter(candidate.mergePoint, m_function.at(operation).type);
        for (Edge edge : m_analyses.cfg().incomingEdges(candidate.mergePoint)) {
            operands[candidate.operandIndex] = targetOf(m_function, edge).arguments[candidate.parameterIndex];
            if (otherOperandIsConstantOfMergePoint)
                operands[otherOperandIndex] = insertBeforeTerminator(builder, edge.from, Value(m_function.at(otherOperand)));
            ValueID copy = insertBeforeTerminator(builder, edge.from, IRBuilder::make(opcode, { operands[0], operands[1] }));
            builder.appendArgument(edge, copy);
        }
        substitution.replace(operation, result);
    }

    ValueID removeFromMergePoint(IRBuilder& builder, const TruthinessTest& candidate)
    {
        Block& block = m_function.edit(candidate.mergePoint);
        ValueID parameter = block.parameters[candidate.parameterIndex];
        block.terminator.setConditionOperand(parameter);
        builder.eraseValues(candidate.mergePoint, singleIndex(0));
        return parameter;
    }

    void removeFromMergePoint(IRBuilder& builder, const ParameterOperation& candidate)
    {
        builder.eraseValues(candidate.mergePoint, singleIndex(candidate.valueIndex));
        builder.removeParameters(candidate.mergePoint, singleIndex(candidate.parameterIndex));
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<TruthinessTest, 4> m_truthinessTests;
    Vector<ParameterOperation, 4> m_operations;
};

} // anonymous namespace

PassOutcome hoistTestsIntoPredecessors(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    while (TestHoister(context).run() == PassOutcome::Changed)
        outcome = PassOutcome::Changed;
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
