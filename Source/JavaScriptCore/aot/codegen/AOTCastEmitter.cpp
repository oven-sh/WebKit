/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
#include "AOTCompileUnit.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

void CastEmitter::generateCheckInitialized(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    emitter<ThrowEmitter>().requireInitialized(value, operand, typeOf(source), value->name());
    defineNarrowed(value, operand);
}

void CastEmitter::generateRequireObjectCoercible(const ValueContext& value)
{
    requireObjectCoercible(value);
    defineNarrowed(value, get(value->valueOperand()));
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
    if (plan.verdict == TypeTestVerdict::NeverPasses)
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FailsTest);
    else
        m_out.noteForm(Form::Open, plan.verdict == TypeTestVerdict::AlwaysPasses ? StateOfKnowledge::PassesTest : StateOfKnowledge::NeedsTest);
    if (plan.verdict != TypeTestVerdict::AlwaysPasses) {
        LBasicBlock isNotObject = newBlock();
        LBasicBlock continuation = newBlock();
        ScopeFork fork = m_out.forkScope();
        branchOnType(operand, plan, { normalEdge(continuation), rareEdge(isNotObject) });

        m_out.appendTo(isNotObject);
        callVoidOperation(value, RuntimeOperation::ThrowRuntimeError, { word32(static_cast<int32_t>(value->runtimeError())) }, { .isOnRarePath = true });
        m_out.unreachable();

        m_out.appendToJoin(continuation, fork);
    }
    defineNarrowed(value, operand);
}

static bool failingSideOfTypeTestTraps(const Code& code, const ValueContext& test)
{
    if (!test.isBranchCondition())
        return false;
    const Terminator& branch = code.at(test.block()).terminator;
    if (branch.kind != TerminatorKind::Branch)
        return false;
    BlockID failingSide = code.at(branch.notTaken()).to;
    return code.valuesOf(failingSide).empty() && code.at(failingSide).terminator.kind == TerminatorKind::Unreachable;
}

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    if (value->asksInFull())
        operand = Operand { Representation::JSValue, operand.value };
    const TypeTestPlan& plan = value->asksInFull() ? typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, value->testedType()) : planFor(source, value->testedType());
    std::optional<SetForScope<bool>> passesChangedClosuresWhereFailureTraps;
    if (!plan.cellTests.isEmpty()) {
        bool& passesChangedClosures = emitter<ClosureTestEmitter>().passesChangedClosures();
        passesChangedClosuresWhereFailureTraps.emplace(passesChangedClosures, passesChangedClosures || failingSideOfTypeTestTraps(generator().code(), value));
    }
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    branchOnType(operand, plan, value.branchTargets());
}

void CastEmitter::generateCheckProof(const ValueContext& value)
{
    Operand source = get(value->valueOperand());
    bool isAnswerOfTest = source.representation == Representation::Boolean;
    Boxed found(isAnswerOfTest ? m_out.constInt64(JSValue::encode(JSValue())) : source.value);
    SetForScope passesChangedClosures(emitter<ClosureTestEmitter>().passesChangedClosures(), true);
    bool asksForType = value->checkedType() != value->typeWithoutProof();
    Condition holds = isAnswerOfTest ? Condition(source.value) : asksForType ? hasType(found, typeTestPlanFor(value->typeWithoutProof(), Representation::JSValue, value->checkedType())) : boolean(true);
    if (!isAnswerOfTest && sourcesOfProofsCheckedUnder(build().options().testing)) {
        holds = logicalAnd(holds, either<Condition>(logicalOr(logicalNot(isCell(found)), isEmpty(found)), [&] {
            return boolean(true);
        }, [&] {
            CellPointer cell = unboxCell(found);
            return logicalOr(hasCellTypeIn(cell, { StringType, SymbolType }), isObjectCell(cell));
        }));
    }
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(value, holds, isAnswerOfTest ? std::nullopt : std::optional(found), value->source());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
