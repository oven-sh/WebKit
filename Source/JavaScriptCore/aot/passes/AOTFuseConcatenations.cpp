/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet concatenations { Opcode::Add, Opcode::Strcat };

namespace {

struct AbsorbedValues {
    BlockID block;
    BitVector indexes;
};

class ConcatenationFuser {
public:
    explicit ConcatenationFuser(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
    {
    }

    PassOutcome run();

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    bool hasObjectOperand(const Value&) const;
    bool hasSymbolOperand(const Value&) const;
    bool isConcatenation(const Value&) const;
    bool hasObservableEffects(ValueID) const;

    std::optional<uint32_t> findAbsorbableOperand(ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts);
    void fuseOperands(ProgramPoint outerPoint, BitVector& absorbed);
    void fuseInBlock(BlockID);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    Vector<AbsorbedValues, 2> m_absorbedValues;
};

bool ConcatenationFuser::hasObjectOperand(const Value& value) const
{
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (typeOf(value.operand(operandIndex)).hasObjects())
            return true;
    }
    return false;
}

bool ConcatenationFuser::hasSymbolOperand(const Value& value) const
{
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (typeOf(value.operand(operandIndex)).primitives & Primitive::AnySymbol)
            return true;
    }
    return false;
}

bool ConcatenationFuser::isConcatenation(const Value& value) const
{
    if (!concatenations.contains(value.opcode) || !m_program.at(value.type).isString())
        return false;
    if (hasObjectOperand(value) || hasSymbolOperand(value))
        return false;
    return value.opcode == Opcode::Strcat || typeOf(value.leftOperand()).isAnyString() || typeOf(value.rightOperand()).isAnyString();
}

bool ConcatenationFuser::hasObservableEffects(ValueID id) const
{
    return !m_context.analyses.effectsOf(id).onAnyEdge().isRemovableWhenUnused();
}

std::optional<uint32_t> ConcatenationFuser::findAbsorbableOperand(ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts)
{
    const Value& inner = m_function.at(operand);
    if (!isConcatenation(inner))
        return std::nullopt;

    const UseLists& uses = m_context.analyses.useLists();
    if (uses.useCount(operand) != 1)
        return std::nullopt;

    ValueLocation location = uses.locationOf(operand);
    if (location.block != outerPoint.block || location.index >= limit)
        return std::nullopt;

    const Block& block = m_function.at(outerPoint.block);
    for (uint32_t index = limit; --index > location.index;) {
        if (!absorbed.get(index) && hasObservableEffects(block.values[index]))
            return std::nullopt;
    }
    if (m_context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), block.values[outerPoint.index]))
        return std::nullopt;
    return location.index;
}

void ConcatenationFuser::fuseOperands(ProgramPoint outerPoint, BitVector& absorbed)
{
    Value& outer = m_function.at(m_function.at(outerPoint.block).values[outerPoint.index]);
    Vector<ValueID, 16> reversedParts;
    uint32_t limit = outerPoint.index;
    for (uint32_t operandIndex = outer.numberOfOperands(); operandIndex--;) {
        ValueID operand = outer.operand(operandIndex);
        std::optional<uint32_t> found = findAbsorbableOperand(outerPoint, operand, limit, absorbed, reversedParts.size() + operandIndex);
        if (!found) {
            reversedParts.append(operand);
            continue;
        }
        const Value& inner = m_function.at(operand);
        for (uint32_t partIndex = inner.numberOfOperands(); partIndex--;)
            reversedParts.append(inner.operand(partIndex));
        absorbed.set(*found);
        limit = *found;
    }
    if (limit == outerPoint.index)
        return;

    reversedParts.reverse();
    bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(outer.opcode) && outer.isLikelyKey();
    outer.become(Opcode::Strcat);
    outer.setVariadicOperands(reversedParts.span());
    outer.setIsLikelyKey(isLikelyKey);
}

void ConcatenationFuser::fuseInBlock(BlockID id)
{
    const BlockValues& values = m_function.at(id).values;
    BitVector absorbed;
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (isConcatenation(m_function.at(values[index])))
            fuseOperands({ id, index }, absorbed);
    }
    if (!absorbed.isEmpty())
        m_absorbedValues.append(AbsorbedValues { id, WTF::move(absorbed) });
}

PassOutcome ConcatenationFuser::run()
{
    for (size_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex)
        fuseInBlock(BlockID(blockIndex));
    if (m_absorbedValues.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(m_context.analyses.context(), m_function, m_context.types, &m_context.analyses);
    size_t numberOfAbsorbedValues = 0;
    for (const AbsorbedValues& absorbed : m_absorbedValues) {
        builder.eraseValues(absorbed.block, absorbed.indexes);
        numberOfAbsorbedValues += absorbed.indexes.bitCount();
    }
    m_context.build.statistics().count(StatisticID::ConcatenationsFused, numberOfAbsorbedValues);
    return PassOutcome::Changed;
}

} // anonymous namespace

PassOutcome fuseConcatenations(FunctionPassContext& context)
{
    return ConcatenationFuser(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
