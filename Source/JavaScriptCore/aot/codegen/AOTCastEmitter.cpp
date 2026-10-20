/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTEffectsDeclared.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(declaredBoundOf(RuntimeOperation::ReportMismatch).mayThrow && declaredBoundOf(Opcode::ReportMismatch).mayThrow, "the report reads the position of its call site, and a call has a site only if the operation and the callee declare throws");

CastEmitter::CastEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

const TypeTestPlan& CastEmitter::planFor(ValueID operand, TypeID to)
{
    return typeTestPlanFor(m_function.at(operand).type, representationOf(operand), to);
}

void CastEmitter::generateUpcast(const ValueContext& value)
{
    define(value, get(value->valueOperand()));
}

void CastEmitter::generateAssumeType(const ValueContext& value)
{
    define(value, get(value->valueOperand()));
}

void CastEmitter::generateClaim(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    requireType(value, operand, planFor(source, value->castType()), value->site());
    define(value, operand);
}

void CastEmitter::generateReportMismatch(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    const TypeTestPlan& plan = planFor(source, value->expectedType());
    if (plan.verdict == TypeTestVerdict::AlwaysPasses)
        return;
    LBasicBlock mismatch = newBlock();
    LBasicBlock continuation = newBlock();
    branchOnType(operand, plan, { normalEdge(continuation), rareEdge(mismatch) });

    m_out.appendTo(mismatch);
    Boxed found = emitter<ConversionEmitter>().box(value, operand, m_program.at(plan.from));
    callVoidOperation(value, RuntimeOperation::ReportMismatch, { found, word32(static_cast<int32_t>(plan.to.index())), word32(static_cast<int32_t>(value->site().index())) }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void CastEmitter::generateCheckInitialized(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    emitter<ThrowEmitter>().requireInitialized(value, operand, typeOf(source), value->name());
    define(value, operand);
}

void CastEmitter::generateRequireObjectCoercible(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    emitter<ThrowEmitter>().requireNotNullish(value, operand, typeOf(source), value->nullishAccess(), value->name());
    define(value, operand);
}

void CastEmitter::generateRequireObject(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    const TypeTestPlan& plan = planFor(source, TypeTable::object);
    if (plan.verdict != TypeTestVerdict::AlwaysPasses) {
        LBasicBlock isNotObject = newBlock();
        LBasicBlock continuation = newBlock();
        branchOnType(operand, plan, { normalEdge(continuation), rareEdge(isNotObject) });

        m_out.appendTo(isNotObject);
        callVoidOperation(value, RuntimeOperation::ThrowRuntimeError, { word32(static_cast<int32_t>(value->runtimeError())) }, { .isOnRarePath = true });
        m_out.unreachable();

        m_out.appendTo(continuation);
    }
    define(value, operand);
}

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    const TypeTestPlan& plan = planFor(source, value->testedType());
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    if (generator().shouldDropGuard(value)) {
        m_out.jump(value.branchTargets().taken.block());
        return;
    }
    branchOnType(operand, plan, value.branchTargets());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
