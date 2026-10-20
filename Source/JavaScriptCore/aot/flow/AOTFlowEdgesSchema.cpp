/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsOutsideOperands = heapsHoldingValues - perObjectHeaps;

static constexpr OperandUses inspectsOrReturns { OperandUse::Inspects, OperandUse::Returns };

#define LIST_FLOW_EDGE_RULE(opcode, rule) Opcode::opcode,
static constexpr OpcodeSet operationsWithRule { FOR_EACH_FLOW_EDGE_RULE(LIST_FLOW_EDGE_RULE) };
#undef LIST_FLOW_EDGE_RULE

static constexpr bool takesSchemaRule(Opcode opcode)
{
    return isLegalIn(opcode, IRForm::AsRead) && removedByOf(opcode) != PassID::TypeValues && !operationsWithRule.contains(opcode);
}

static constexpr bool accessesOnlyOperands(Opcode opcode)
{
    Effects bound = declaredBoundOf(opcode);
    return bound.mayCallUnknownCode || !(bound.reads.containsAny(heapsOutsideOperands) || bound.writes.containsAny(heapsOutsideOperands));
}

#define CHECK_SCHEMA_RULE(opcode) \
    static_assert(!takesSchemaRule(Opcode::opcode) || accessesOnlyOperands(Opcode::opcode), "An operation that accesses values outside its operands needs a rule of its own.");
FOR_EACH_AOT_OPCODE(CHECK_SCHEMA_RULE)
#undef CHECK_SCHEMA_RULE

static bool isOnlyInspectedOrReturned(const OperandInfo& info)
{
    return info.uses.containsOnly(inspectsOrReturns) && info.protocols.isEmpty();
}

static bool answersUnknownObject(ResultRule rule)
{
    switch (rule) {
    case ResultRule::Declared:
    case ResultRule::Unknown:
    case ResultRule::Results:
        return true;
    case ResultRule::None:
    case ResultRule::Constant:
    case ResultRule::Natural:
    case ResultRule::Allocation:
    case ResultRule::Cast:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addFromSchema(ValueID id, const Value& value)
{
    // A read of contents is only recorded, so such an operation would answer the unknown object for a tracked object it read. It needs a rule of its own.
    if (answersUnknownObject(resultRuleOf(value.opcode))) {
        value.forEachOperand([&](ValueID, uint32_t operandIndex) {
            RELEASE_ASSERT(!operandUsesOf(value.opcode, operandIndex).contains(OperandUse::ReadsContents));
        });
    }

    addOperandUsesFromSchema(id, value, BitVector());
    addResultFromSchema(id, value);
}

void FlowSummaryBuilder::addResultFromSchema(ValueID id, const Value& value)
{
    switch (resultRuleOf(value.opcode)) {
    case ResultRule::None:
    case ResultRule::Results:
        return;
    case ResultRule::Constant:
    case ResultRule::Declared:
    case ResultRule::Unknown:
        seedTypeAsRead(id);
        return;
    case ResultRule::Natural:
        addNaturalOperation(id, value);
        return;
    case ResultRule::Allocation:
        addSiteFor(id, { .kind = FlowSiteKind::Opaque, .origin = FlowSiteOrigin::Operation });
        return;
    case ResultRule::Cast:
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns))
                filter(localOf(operand), localOf(id), value.type);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addOperandUse(ValueID origin, uint32_t operandIndex, ValueID operand, const OperandInfo& info)
{
    FlowLocalID operandLocal = localOf(operand);
    if (info.uses.contains(OperandUse::Returns))
        m_assembler.copy(operandLocal, localOf(origin));

    if (isOnlyInspectedOrReturned(info)) {
        if (info.uses.contains(OperandUse::Inspects))
            m_assembler.add(FlowOperandUse { .origin = origin, .operandIndex = operandIndex, .operand = operandLocal });
        return;
    }

    const Value& operation = m_function.at(origin);
    FlowLocalID result = mayHoldReference(origin) ? localOf(origin) : FlowLocalID();
    FlowLocalID retainer;
    switch (info.retainedIn) {
    case RetainedIn::UnknownMemory:
        break;
    case RetainedIn::Result:
        retainer = result;
        // An opaque value without a local carries no value of the program, so nothing reads the operand back out of it.
        if (!retainer && operation.type && m_program.at(operation.type).opaque)
            retainer = m_assembler.addTemporary();
        break;
    case RetainedIn::Operand:
        retainer = localOf(operation.operand(*info.retainingOperand));
        break;
    }
    m_assembler.add(FlowOperandUse { origin, operandIndex, operandLocal, retainer, result, thrownTarget(), false });
}

void FlowSummaryBuilder::addOperandUsesFromSchema(ValueID id, const Value& value, const BitVector& coveredOperands)
{
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (coveredOperands.get(operandIndex) || !mayHoldReference(operand))
            return;
        addOperandUse(id, operandIndex, operand, operandInfoOf(value.opcode, operandIndex));
    });
}

void FlowSummaryBuilder::addNaturalOperation(ValueID id, const Value& value)
{
    FlowLocalID result = localOf(id);
    if (!result)
        return;

    uint32_t numberOfOperands = value.numberOfOperands();
    bool canEvaluate = numberOfOperands && numberOfOperands <= FlowNaturalEvaluator::maxOperandCount && typeOf(id).hasOnlyPrimitives(Primitive::Defined);
    Vector<FlowLocalID, FlowNaturalEvaluator::maxOperandCount> operands;
    for (uint32_t operandIndex = 0; canEvaluate && operandIndex < numberOfOperands; ++operandIndex) {
        FlowLocalID operand = localOf(value.operand(operandIndex));
        canEvaluate = !!operand;
        operands.append(operand);
    }
    if (!canEvaluate) {
        seedTypeAsRead(id);
        return;
    }
    m_assembler.add(FlowNaturalOperation { id, result, m_assembler.addList(operands.span()) });
}

void FlowSummaryBuilder::addTerminatorOperandUses(const Terminator& terminator)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!mayHoldReference(operand))
            return;
        const OperandInfo& info = operandInfoOf(terminator.kind, operandIndex);
        bool isOnlyRecorded = isOnlyInspectedOrReturned(info);
        if (isOnlyRecorded && !info.uses.contains(OperandUse::Inspects))
            return;
        m_assembler.add(FlowOperandUse {
            .operandIndex = operandIndex,
            .operand = localOf(operand),
            .thrown = isOnlyRecorded ? FlowLocalID() : thrownTarget(),
            .isTerminatorOperand = true,
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
