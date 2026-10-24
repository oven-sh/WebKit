/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTEffectsDeclared.h"
#include "AOTPromisedResults.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include "B3UpsilonValue.h"

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

void CastEmitter::generateNarrowByField(const ValueContext& value)
{
    define(value, get(value->valueOperand()));
}

void CastEmitter::generateAssumeType(const ValueContext& value)
{
    if (generator().checksProofs())
        emitter<RuntimeValidationEmitter>().checkAssumedType(value);
    define(value, get(value->valueOperand()));
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
    if (generator().nullishCheckBehindFailedTest(value.block()) != value.id())
        requireObjectCoercible(value);
    define(value, get(value->valueOperand()));
}

void CastEmitter::requireObjectCoercible(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    emitter<ThrowEmitter>().requireNotNullish(value, get(source), typeOf(source), value->nullishAccess(), value->name());
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
    if (value.isBranchCondition() && generator().nullishCheckBehindFailedTest(value.block()) == source) {
        testAheadOfNullishCheck(value, operand);
        return;
    }
    const TypeTestPlan& plan = planFor(source, value->testedType());
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan, value.shape()));
        return;
    }
    if (std::optional<CodeGenerator::GuardUnderTest> guard = generator().takeGuardUnderTest(value)) {
        branchOnType(operand, plan, generator().targetsOf(value, *guard), value.shape());
        return;
    }
    ValueID call = inputs().promisedResults.findCallThatStandsForTest(m_program, m_function, m_function.at(value.block()));
    if (std::optional<CodeGenerator::LandingOfMisfit> landing = generator().takeLandingOfMisfit(call)) {
        testAtLandingOfMisfit(value, call, *landing, plan);
        return;
    }
    branchOnType(operand, plan, value.branchTargets(), value.shape());
}

// What the callee returned has the type. Only what arrives at the landing is tested, and not even that where it failed the same test in the callee.
void CastEmitter::testAtLandingOfMisfit(const ValueContext& test, ValueID call, const CodeGenerator::LandingOfMisfit& landing, const TypeTestPlan& plan)
{
    FunctionID callee = m_function.at(call).function();
    const BranchTargets& targets = test.branchTargets();
    LBasicBlock here = m_out.m_block;

    m_out.appendTo(landing.landing);
    Boxed misfit(m_out.load64(mutableInstanceField(AOT_INSTANCE_OFFSET(scratch.misfit))));
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), misfit.value(), landing.result);
    bool failedSameTest = inputs().promisedResults.of(callee) == test->testedType() && inputs().promisedResults.misfitsAreNeverOfType(callee);
    LValue hasTestedType = failedSameTest ? m_out.constInt32(0) : hasType(misfit, plan, test.shape()).value();
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), hasTestedType, landing.hasTestedType);
    m_out.jump(landing.meeting);

    m_out.appendTo(here);
    if (!generator().validates()) {
        m_out.branch(landing.hasTestedType, targets);
        return;
    }
    LBasicBlock isSaidToHaveType = newBlock();
    LBasicBlock brokenPromise = newBlock();
    m_out.branch(landing.hasTestedType, normalEdge(isSaidToHaveType), targets.notTaken);
    m_out.appendTo(isSaidToHaveType);
    branchOnType(get(call), plan, { targets.taken, rareEdge(brokenPromise) }, test.shape());
    m_out.appendTo(brokenPromise);
    emitter<RuntimeValidationEmitter>().reportUnreachable(call);
    m_out.unreachable();
}

// The test meets what the check has not seen, so its plan is made for the type of the unchecked value.
void CastEmitter::testAheadOfNullishCheck(const ValueContext& test, Operand operand)
{
    ValueID check = test->valueOperand();
    const TypeTestPlan& plan = planFor(m_function.at(check).valueOperand(), test->testedType());
    const BranchTargets& targets = test.branchTargets();
    LBasicBlock failed = newBlock();
    branchOnType(operand, plan, { targets.taken, { failed, targets.notTaken.frequency() } }, test.shape());

    m_out.appendTo(failed);
    generator().generateNullishCheckBehindFailedTest(test.block(), check);
    m_out.jump(targets.notTaken.block());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
