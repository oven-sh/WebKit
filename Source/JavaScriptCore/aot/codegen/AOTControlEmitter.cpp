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
#include "AOTStaticsEmitter.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

ControlEmitter::ControlEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ControlEmitter::generateJump(const TerminatorContext& terminator)
{
    generator().jumpTo(terminator, 0);
}

void ControlEmitter::generateBranch(const TerminatorContext& terminator)
{
    BranchTargets targets = branchTargetsOf(terminator);
    branch(in<Condition>(terminator->conditionOperand()), targets);
}

void ControlEmitter::generateTestAndBranch(const TerminatorContext& terminator)
{
    generator().generateBranchCondition(terminator, terminator->conditionOperand(), branchTargetsOf(terminator));
}

BranchTargets ControlEmitter::branchTargetsOf(const TerminatorContext& terminator)
{
    return { countedEdgeTo(terminator, 0), countedEdgeTo(terminator, 1) };
}

B3::FrequentedBlock ControlEmitter::countedEdgeTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    B3::FrequentedBlock edge = generator().edgeTo(terminator, targetIndex, declaredResults);
    if (inputs().branchCounters.isCounting())
        countBranch(terminator, targetIndex, edge);
    return edge;
}

void ControlEmitter::countBranch(const TerminatorContext& terminator, uint32_t targetIndex, B3::FrequentedBlock& edge)
{
    if (!m_countedBranches)
        m_countedBranches = findCountedBranches(m_program, m_function);
    auto* countedBranch = tryBinarySearch<CountedBranch, BlockID>(*m_countedBranches, m_countedBranches->size(), terminator.block(), [](const CountedBranch* candidate) {
        return candidate->block;
    });
    if (!countedBranch)
        return;
    uint32_t ordinal = static_cast<uint32_t>(countedBranch - m_countedBranches->begin());
    std::optional<uint32_t> number = inputs().branchCounters.counterFor(m_id, ordinal, counterSideOf(terminator.terminator(), targetIndex));
    if (!number)
        return;

    LBasicBlock branchingBlock = m_out.m_block;
    LBasicBlock countingBlock = newBlock();
    m_out.appendTo(countingBlock);
    TypedPointer counter = emitter<StaticsEmitter>().branchCounter(*number);
    m_out.store64(m_out.add(m_out.load64(counter), m_out.constInt64(1)), counter);
    m_out.jump(edge.block());
    m_out.appendTo(branchingBlock);
    edge.block() = countingBlock;
}

void ControlEmitter::generateSwitch(const TerminatorContext& terminator)
{
    RELEASE_ASSERT(terminator->targets.size() == terminator->cases().size() + 1);
    ValueID subject = terminator->subjectOperand();
    B3::FrequentedBlock otherwise = countedEdgeTo(terminator, static_cast<uint32_t>(terminator->cases().size()));
    switch (representationOf(subject)) {
    case Representation::Int32:
        switchOnInteger(terminator, in<Word32>(subject).value(), B3::Int32, otherwise);
        return;
    case Representation::Int64:
        switchOnInteger(terminator, in<Integer>(subject).value(), B3::Int64, otherwise);
        return;
    case Representation::Double: {
        Number number = in<Number>(subject);
        Integer truncated = emitter<ConversionEmitter>().toInteger(number);
        LBasicBlock isIntegral = newBlock();
        branch(equal(toNumber(truncated), number), { normalEdge(isIntegral), otherwise });
        m_out.appendTo(isIntegral);
        switchOnInteger(terminator, truncated.value(), B3::Int64, otherwise);
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
    case Representation::LiteralString:
    case Representation::RawWord:
        fail(terminator, InternalErrorKind::OpcodeWithoutCode);
        m_out.unreachable();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, B3::FrequentedBlock otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == B3::Int32 || type == B3::Int64));
    PayloadListView<int64_t> cases = terminator->cases();
    Vector<SwitchTarget, 8> targets;
    targets.reserveInitialCapacity(cases.size());
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if (type == B3::Int32 && caseValue != static_cast<int32_t>(caseValue))
            continue;
        targets.append(SwitchTarget { caseValue, countedEdgeTo(terminator, index) });
    }
    if (targets.isEmpty()) {
        m_out.jump(otherwise.block());
        return;
    }
    m_out.switchOn(subject, targets.span(), otherwise);
}

void ControlEmitter::switchOnBoxedInteger(const TerminatorContext& terminator, Boxed subject, B3::FrequentedBlock otherwise)
{
    if (terminator->cases().isEmpty() || !(typeOf(terminator->subjectOperand()).primitives & Primitive::Number)) {
        m_out.jump(otherwise.block());
        return;
    }

    LBasicBlock isInt32Subject = newBlock();
    LBasicBlock isOtherSubject = newBlock();
    Join<Integer> join(m_out);
    branch(isInt32(subject), { normalEdge(isInt32Subject), unlikelyEdge(isOtherSubject) });

    m_out.appendTo(isInt32Subject);
    join.add(signExtended(unboxInt32(subject)));

    m_out.appendTo(isOtherSubject);
    join.add(callLeaf<Integer>(RuntimeOperation::TryConvertToInteger, { subject }));

    switchOnInteger(terminator, join.finish().value(), B3::Int64, otherwise);
}

void ControlEmitter::generateSwitchOnShape(const TerminatorContext& terminator)
{
    std::span<const LayoutSetID> cases = terminator->layoutSets();
    RELEASE_ASSERT(terminator->targets.size() == cases.size() + 1);
    Vector<B3::FrequentedBlock, 8> targets;
    targets.reserveInitialCapacity(cases.size());
    for (uint32_t index = 0; index < cases.size(); ++index)
        targets.append(countedEdgeTo(terminator, index));
    B3::FrequentedBlock otherwise = countedEdgeTo(terminator, static_cast<uint32_t>(cases.size()));
    emitter<ShapeTestEmitter>().switchOnShape(in<CellPointer>(terminator->subjectOperand()), cases, targets.span(), otherwise);
}

void ControlEmitter::generateThrow(const TerminatorContext& terminator)
{
    emitter<ExceptionEmitter>().throwValue(terminator, in<Boxed>(terminator->valueOperand()));
}

void ControlEmitter::generateRethrow(const TerminatorContext& terminator)
{
    emitter<ExceptionEmitter>().rethrow(terminator, CellPointer(get(terminator->exceptionOperand()).value));
}

void ControlEmitter::generateUnreachable(const TerminatorContext&)
{
    m_out.unreachable();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
