/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
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

// proveOrTrap() of walk/AOTWalkCasts.cpp: the value has the type, and nothing goes on where the test fails.
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
    const TypeTestPlan& plan = planFor(source, value->testedType());
    // A test that gives way to the road for any value may turn away a closure that has changed since it was made. One that traps may not: the target of a proxy is still what the trap is handed.
    bool& passesChangedClosures = emitter<ClosureTestEmitter>().passesChangedClosures();
    SetForScope passesChangedClosuresWhereFailureTraps(passesChangedClosures, passesChangedClosures || failingSideOfTypeTestTraps(generator().code(), value));
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    branchOnType(operand, plan, value.branchTargets());
}

// The record of the value has the type that is asked for, so the code of the test goes by what is known without it. A cell and its boxed form are one word: where the proof is false the word of a "cell" may be any value.
void CastEmitter::generateCheckProof(const ValueContext& value)
{
    Operand source = get(value->valueOperand());
    // The answer of a test that was proven to pass: Walker::checkProofThatSlotIsFilled(). What was found is nothing.
    bool isAnswerOfTest = source.representation == Representation::Boolean;
    Boxed found(isAnswerOfTest ? m_out.constInt64(JSValue::encode(JSValue())) : source.value);
    // As where the failing side of a TypeTest traps: a closure that has changed since it was made is still a closure of its function.
    SetForScope passesChangedClosures(emitter<ClosureTestEmitter>().passesChangedClosures(), true);
    // Walker::checkThatReadGivesValue(): nothing is claimed of the type.
    bool asksForType = value->checkedType() != value->typeWithoutProof();
    Condition holds = isAnswerOfTest ? Condition(source.value) : asksForType ? hasType(found, typeTestPlanFor(value->typeWithoutProof(), Representation::JSValue, value->checkedType())) : boolean(true);
    // Whatever is claimed of it, it is a value of the language. The pair of an accessor, an environment, a structure pass every test of the tag bits, and `anything` asks no other.
    if (!isAnswerOfTest) {
        holds = logicalAnd(holds, either<Condition>(logicalOr(logicalNot(isCell(found)), isEmpty(found)), [&] {
            return boolean(true);
        }, [&] {
            CellPointer cell = unboxCell(found);
            return logicalOr(hasCellTypeIn(cell, { StringType, SymbolType }), isObjectCell(cell));
        }));
    }
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holds.value(), normalEdge(continuation), rareEdge(stops));

    m_out.appendToArm(stops, fork);
    // operationAOTStopAtFalseProof() has a word for each.
    static_assert(numberOfEnumerators<ProofSource> == 10);
    FunctionID writtenIn = generator().code().inlineFrames()[value->inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(value, RuntimeOperation::StopAtFalseProof, { found, word32(static_cast<int32_t>(writtenIn.index())), word32(static_cast<int32_t>(value->position)), word32(static_cast<int32_t>(value->source())) });
    m_out.resumeDeclaredAccesses();
    m_out.unreachable();
    m_out.appendToArm(continuation, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
