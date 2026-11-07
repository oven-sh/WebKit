/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
#include "AOTCompileUnit.h"
#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include "AOTStubsValues.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include <algorithm>

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
        static_assert(mayCollect(Opcode::RequireObject) || !declaredBoundOf(RuntimeOperation::ThrowRuntimeError).mayCollect);
        callVoidOperation(value, RuntimeOperation::ThrowRuntimeError, { word32(static_cast<int32_t>(value->runtimeError())) }, { .isOnRarePath = true });
        m_out.unreachable();

        m_out.appendToJoin(continuation, fork);
    }
    defineNarrowed(value, operand);
}

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    if (value->asksInFull())
        operand = Operand { Representation::JSValue, operand.value };
    TypeID whatMayCome = hasTestedForHole(operationAt(definitionOf(source))) ? TypeTable::anything : TypeTable::anythingOrAbsent;
    const TypeTestPlan& plan = value->asksInFull() ? typeTestPlanFor(whatMayCome, Representation::JSValue, value->testedType()) : planFor(source, value->testedType());
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    branchOnType(operand, plan, value.branchTargets());
}

auto CastEmitter::kindsAndClosureThatMakeUp(TypeID type) -> std::optional<KindsAndClosure>
{
    if (!types().at(type).isJavaScriptValue())
        return std::nullopt;
    const TypeTestPlan& plan = typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, type);
    if (plan.verdict == TypeTestVerdict::AlwaysPasses)
        return KindsAndClosure { KindsOfProof::all, std::nullopt };
    if (plan.verdict == TypeTestVerdict::NeverPasses || plan.integers || !plan.stringLiterals.isEmpty())
        return std::nullopt;
    uint16_t numbers = plan.passingPrimitives & Primitive::Number;
    if (numbers && numbers != Primitive::Number)
        return std::nullopt;
    uint32_t primitives = plan.passingPrimitives & KindsOfProof::primitives;
    if (plan.objects == ObjectsVerdict::NeedTest) {
        if (plan.cellTests.size() != 1 || plan.cellTests[0].kind != CellTestKind::IsClosureOf)
            return std::nullopt;
        std::optional<uint32_t> mention = emitter<ClosureTestEmitter>().mentionOfUpperHalfThatNames(plan.cellTests[0].function);
        if (!mention)
            return std::nullopt;
        return KindsAndClosure { primitives, mention };
    }
    for (const CellTest& test : plan.cellTests) {
        if (test.kind != CellTestKind::IsObject)
            return std::nullopt;
    }
    return KindsAndClosure { primitives | (plan.objects == ObjectsVerdict::AllPass ? KindsOfProof::everyObject : 0), std::nullopt };
}

void CastEmitter::generateCheckProof(const ValueContext& value)
{
    if (value->isLeftToTestBehind())
        return;
    Operand source = get(value->valueOperand());
    if (numberRepresentations.contains(source.representation) || optionalRepresentations.contains(source.representation)) {
        emitter<ThrowEmitter>().checkOrStopAtFalseProof(value, hasType(source, typeTestPlanFor(value->typeWithoutProof(), source.representation, value->checkedType())), value->source());
        return;
    }
    bool isAnswerOfTest = source.representation == Representation::Boolean;
    Boxed found(isAnswerOfTest ? m_out.constInt64(JSValue::encode(JSValue())) : source.value);
    if (isAnswerOfTest) {
        emitter<ThrowEmitter>().requireOrStopAtFalseProof(value, Condition(source.value), std::nullopt, value->source());
        return;
    }
    bool asksForType = value->checkedType() != value->typeWithoutProof();
    bool looksAtCell = sourcesOfProofsCheckedUnder(build().options().testing);
    if (!asksForType && !looksAtCell)
        return;
    bool asksStub = looksAtCell && !generator().isInitializingCell();
    if (asksStub) {
        if (std::optional<KindsAndClosure> asked = asksForType ? kindsAndClosureThatMakeUp(value->checkedType()) : std::optional(KindsAndClosure { KindsOfProof::all, std::nullopt })) {
            emitter<ThrowEmitter>().stopUnlessAmongKinds(value, found, asked->kinds, value->source(), asked->mentionOfUpperHalfOfClosureWord);
            return;
        }
    }
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    if (asksForType) {
        LBasicBlock hasCheckedType = looksAtCell ? newBlock() : continuation;
        branchOnType(found, typeTestPlanFor(value->typeWithoutProof(), Representation::JSValue, value->checkedType()), { normalEdge(hasCheckedType), rareEdge(stops) });
        if (looksAtCell)
            m_out.appendToJoin(hasCheckedType, fork);
    }
    if (asksStub) {
        emitter<ThrowEmitter>().stopUnlessAmongKinds(value, found, KindsOfProof::all, value->source());
        m_out.jump(continuation);
    } else if (looksAtCell) {
        LBasicBlock isCellToLookAt = newBlock();
        m_out.branch(logicalOr(logicalNot(isCell(found)), isEmpty(found)).value(), normalEdge(continuation), normalEdge(isCellToLookAt));
        m_out.appendToJoin(isCellToLookAt, fork);
        CellPointer cell = unboxCell(found);
        m_out.branch(logicalOr(hasCellTypeIn(cell, { StringType, SymbolType }), isObjectCell(cell)).value(), normalEdge(continuation), rareEdge(stops));
    }
    m_out.appendToJoin(stops, fork);
    emitter<ThrowEmitter>().stopAtFalseProof(value, found, value->source());
    m_out.appendToJoin(continuation, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
