/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static_assert(!OperandIndex::FinishObject::object && !OperandIndex::FinishClass::classObject && !OperandIndex::Predict::value && !OperandIndex::TruncateNewArray::array, "addSameValue() copies operand 0.");

void FlowSummaryBuilder::addBlockParameters(BlockID block)
{
    const Block& target = m_function.at(block);
    for (Edge edge : m_cfg.incomingEdges(block)) {
        for (uint32_t index = 0; index < target.parameters.size(); ++index)
            addIncomingValue(edge, index, target.parameters[index]);
    }
}

void FlowSummaryBuilder::addIncomingValue(Edge edge, uint32_t parameterIndex, ValueID parameter)
{
    if (!localOf(parameter))
        return;

    IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        m_assembler.copy(localOf(incoming.argument), localOf(parameter));
        return;
    case IncomingValue::Kind::EdgeResult:
        addEdgeResult(incoming.result, parameter);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The type as read bounds an operand by what the text says about the other one. Of a parameter, a field or a length only the solver knows the range.
FlowLocalID FlowSummaryBuilder::addOperandBoundedByValues(const EdgeResult& result)
{
    Vector<FlowLocalID, FlowNaturalEvaluator::maxOperandCount> operands;
    bool hasEveryLocal = true;
    m_function.at(result.test).forEachOperand([&](ValueID operand, uint32_t) {
        operands.append(localOf(operand));
        hasEveryLocal &= !!operands.last();
    });
    if (!hasEveryLocal)
        return localOf(result.subject);

    FlowLocalID bounded = m_assembler.addTemporary();
    m_assembler.add(FlowNaturalOperation {
        .origin = result.test,
        .result = bounded,
        .operands = m_assembler.addList(operands.span()),
        .kind = result.side == EdgeSide::Taken ? FlowNaturalKind::OperandOnTakenEdge : FlowNaturalKind::OperandOnNotTakenEdge,
        .narrowedOperand = static_cast<uint8_t>(result.operandIndex),
    });
    return bounded;
}

void FlowSummaryBuilder::addEdgeResult(const EdgeResultPosition& position, ValueID parameter)
{
    EdgeResult result = edgeResultAt(m_function, position);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand: {
        FlowLocalID subject = localOf(result.subject);
        FlowLocalID narrowed = localOf(parameter);
        if (result.test && m_function.at(result.test).opcode == Opcode::TypeTest) {
            filter(subject, narrowed, m_function.at(result.test).testedType(), result.side == EdgeSide::Taken ? FlowFilterKind::Inside : FlowFilterKind::Outside);
            return;
        }
        // A type that admits every object cannot say that one object is missing, and a set of objects can.
        if (result.test && m_function.at(result.test).opcode == Opcode::StrictEq && result.side == EdgeSide::NotTaken) {
            ValueID other = m_function.at(result.test).operand(result.operandIndex ? 0 : 1);
            if (subject && typeOf(other).isSingleValue()) {
                FlowLocalID unequal = m_assembler.addTemporary();
                filter(subject, unequal, m_function.at(other).type, FlowFilterKind::Outside);
                subject = unequal;
            }
        }
        if (subject && narrowed && result.proof == Proof::IntegerBound && typeOf(parameter).hasOnlyPrimitives(Primitive::Defined) && m_build.isEnabled(FlowResultKind::NarrowedTypes, m_id.index()))
            subject = addOperandBoundedByValues(result);
        filter(subject, narrowed, m_function.at(parameter).type);
        return;
    }
    case EdgeResultKind::DeclaredResult:
        seedTypeAsRead(parameter);
        return;
    case EdgeResultKind::Guard:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addSameValue(ValueID id, const Value& value)
{
    m_assembler.copy(localOf(value.operand(0)), localOf(id));
}

void FlowSummaryBuilder::addCast(ValueID id, const Value& value)
{
    filter(localOf(value.valueOperand()), localOf(id), value.type);
}

void FlowSummaryBuilder::addCaughtValue(ValueID id, const Value&)
{
    m_assembler.copy(caughtLocalOf(m_block), localOf(id));
}

void FlowSummaryBuilder::addPlainTerminator(const Terminator& terminator)
{
    addTerminatorOperandUses(terminator);
}

void FlowSummaryBuilder::addReturn(const Terminator& terminator)
{
    ValueID returned = terminator.valueOperand();
    if (!returned) {
        FlowLocalID result = m_assembler.result(0);
        if (!m_assembler.hasSeed(result))
            seedPrimitives(result, Primitive::Undefined);
        return;
    }

    const Value& producer = m_function.at(returned);
    uint32_t numberOfResults = m_function.results.size();
    if (producer.opcode == Opcode::Results) {
        RELEASE_ASSERT(producer.numberOfOperands() == numberOfResults);
        for (uint32_t index = 0; index < numberOfResults; ++index)
            m_assembler.copy(localOf(producer.operand(index)), m_assembler.result(index));
        return;
    }
    if (numberOfResults == 1) {
        m_assembler.copy(localOf(returned), m_assembler.result(0));
        return;
    }

    bool definesEveryResult = producer.opcode == Opcode::CallDirect;
    auto resultLocals = m_resultLocals.find(returned);
    bool hasResultLocals = resultLocals != m_resultLocals.end();
    RELEASE_ASSERT(hasResultLocals ? resultLocals->value.size() == numberOfResults : !definesEveryResult);
    for (uint32_t index = 0; index < numberOfResults; ++index) {
        FlowLocalID result = m_assembler.result(index);
        if (hasResultLocals)
            m_assembler.copy(resultLocals->value[index], result);
        if (!definesEveryResult && !m_assembler.hasSeed(result))
            m_assembler.seed(FlowSeed::typeAsRead(result, m_function.results[index].type));
    }
}

void FlowSummaryBuilder::addReturnCall(const Terminator& terminator)
{
    m_assembler.copy(localOf(terminator.callOperand()), m_assembler.result(0));
}

void FlowSummaryBuilder::addThrow(const Terminator& terminator)
{
    ValueID thrown = terminator.kind == TerminatorKind::Throw ? terminator.valueOperand() : terminator.exceptionOperand();
    m_assembler.copy(localOf(thrown), thrownTarget());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
