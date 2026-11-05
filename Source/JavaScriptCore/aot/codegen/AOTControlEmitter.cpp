/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTShapeTestEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

void ControlEmitter::generateJump(const TerminatorContext& terminator)
{
    generator().jumpTo(terminator, 0);
}

void ControlEmitter::generateBranch(const TerminatorContext& terminator)
{
    if (terminator.generatesCondition()) {
        generateTestAndBranch(terminator);
        return;
    }
    BranchTargets targets = branchTargetsOf(terminator);
    branch(in<Condition>(terminator->conditionOperand()), targets);
}

void ControlEmitter::generateTestAndBranch(const TerminatorContext& terminator)
{
    generator().generateBranchCondition(terminator, terminator->conditionOperand(), branchTargetsOf(terminator));
}

BranchTargets ControlEmitter::branchTargetsOf(const TerminatorContext& terminator)
{
    return { generator().edgeTo(terminator, 0), generator().edgeTo(terminator, 1) };
}

void ControlEmitter::generateSwitch(const TerminatorContext& terminator)
{
    RELEASE_ASSERT(terminator.numberOfTargets() == terminator->cases().size() + 1);
    ValueID subject = terminator->subjectOperand();
    BlockEdge otherwise = generator().edgeTo(terminator, static_cast<uint32_t>(terminator->cases().size()));
    switch (representationOf(subject)) {
    case Representation::Int32:
        switchOnInteger(terminator, in<Word32>(subject).value(), Machine::Int32, otherwise);
        return;
    case Representation::Int64:
        switchOnInteger(terminator, in<Integer>(subject).value(), Machine::Int64, otherwise);
        return;
    case Representation::Double: {
        Number number = in<Number>(subject);
        Integer truncated = emitter<ConversionEmitter>().toInteger(number);
        LBasicBlock isIntegral = newBlock();
        branch(equal(toNumber(truncated), number), { normalEdge(isIntegral), otherwise });
        m_out.appendTo(isIntegral);
        switchOnInteger(terminator, truncated.value(), Machine::Int64, otherwise);
        return;
    }
    case Representation::JSValue:
        switchOnBoxedInteger(terminator, in<Boxed>(subject), otherwise);
        return;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::RawWord:
        fail(terminator, InternalErrorKind::OpcodeWithoutCode);
        m_out.unreachable();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == Machine::Int32 || type == Machine::Int64));
    PayloadListView<int64_t> cases = terminator->cases();
    Vector<SwitchTarget, 8> targets;
    targets.reserveInitialCapacity(cases.size());
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if (type == Machine::Int32 && caseValue != static_cast<int32_t>(caseValue))
            continue;
        targets.append(SwitchTarget { caseValue, generator().edgeTo(terminator, index) });
    }
    if (targets.isEmpty()) {
        m_out.jump(otherwise);
        return;
    }
    m_out.switchOn(subject, targets.span(), otherwise);
}

void ControlEmitter::switchOnBoxedInteger(const TerminatorContext& terminator, Boxed subject, BlockEdge otherwise)
{
    const Type& subjectType = typeOf(terminator->subjectOperand());
    if (terminator->cases().isEmpty() || !(subjectType.primitives & Primitive::Number)) {
        m_out.jump(otherwise);
        return;
    }

    LBasicBlock isInt32Subject = newBlock();
    LBasicBlock isOtherSubject = newBlock();
    Join<Integer> join(m_out);
    branch(isInt32(subject), { normalEdge(isInt32Subject), unlikelyEdge(isOtherSubject) });

    m_out.appendTo(isInt32Subject);
    join.add(signExtended(unboxInt32(subject)));

    m_out.appendTo(isOtherSubject);
    if (!subjectType.isNumber()) {
        LBasicBlock isDoubleSubject = newBlock();
        branch(isNumber(subject), { normalEdge(isDoubleSubject), otherwise });
        m_out.appendTo(isDoubleSubject);
    }
    join.add(callLeaf<Integer>(RuntimeOperation::TryConvertToInteger, { subject }));

    switchOnInteger(terminator, join.finish().value(), Machine::Int64, otherwise);
}

void ControlEmitter::generateSwitchOnShape(const TerminatorContext& terminator)
{
    std::span<const LayoutSetID> cases = terminator->layoutSets();
    RELEASE_ASSERT(terminator.numberOfTargets() == cases.size() + 1);
    Vector<BlockEdge, 8> targets;
    targets.reserveInitialCapacity(cases.size());
    for (uint32_t index = 0; index < cases.size(); ++index)
        targets.append(generator().edgeTo(terminator, index));
    BlockEdge otherwise = generator().edgeTo(terminator, static_cast<uint32_t>(cases.size()));
    emitter<ShapeTestEmitter>().switchOnShape(in<CellPointer>(terminator->subjectOperand()), cases, targets.span(), otherwise);
}

void ControlEmitter::generateThrow(const TerminatorContext& terminator)
{
    emitter<ExceptionEmitter>().throwValue(terminator, in<Boxed>(terminator->valueOperand()));
}

void ControlEmitter::generateRethrow(const TerminatorContext& terminator)
{
    emitter<ExceptionEmitter>().rethrow(terminator, in<CellPointer>(terminator->exceptionOperand()));
}

void ControlEmitter::generateUnreachable(const TerminatorContext&)
{
    m_out.unreachable();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
