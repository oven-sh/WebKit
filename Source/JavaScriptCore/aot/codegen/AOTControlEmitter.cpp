/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTAllocationEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTBitVector.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

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
    if (!inputs().branchCounters.isCounting())
        return edge;
    if (!m_countedBranches)
        m_countedBranches = findCountedBranches(m_program, m_function);
    auto* countedBranch = tryBinarySearch<CountedBranch, BlockID>(*m_countedBranches, m_countedBranches->size(), terminator.block(), [](const CountedBranch* candidate) {
        return candidate->block;
    });
    if (!countedBranch)
        return edge;
    uint32_t ordinal = static_cast<uint32_t>(countedBranch - m_countedBranches->begin());
    std::optional<uint32_t> number = inputs().branchCounters.counterFor(m_id, ordinal, counterSideOf(terminator.terminator(), targetIndex));
    if (!number)
        return edge;

    LBasicBlock branchingBlock = m_out.m_block;
    LBasicBlock countingBlock = newBlock();
    m_out.appendTo(countingBlock);
    TypedPointer counter = emitter<StaticsEmitter>().branchCounter(*number);
    m_out.store64(m_out.add(m_out.load64(counter), 1), counter);
    m_out.jump(edge.block());
    m_out.appendTo(branchingBlock);
    edge.block() = countingBlock;
    return edge;
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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

// A table that has an entry for every integer of the subject's type is never left, and the back end sees that only where each of them is listed. Fewer integers than a table needs are told apart by comparisons, and there the rest costs none.
static void listIntegersLeftToRest(const Type& subject, LType type, unsigned minCasesForJumpTable, Vector<SwitchTarget, 8>& targets, B3::FrequentedBlock otherwise)
{
    if (!subject.isInteger() || (type == B3::Int32 && !subject.integers.isSubsetOf(Range::int32())))
        return;
    Range integers = subject.integers;
    uint64_t numberOfIntegers = static_cast<uint64_t>(integers.maximum - integers.minimum) + 1;
    if (numberOfIntegers < minCasesForJumpTable || numberOfIntegers > 2 * static_cast<uint64_t>(targets.size()))
        return;
    BitVector isListed(numberOfIntegers);
    for (const SwitchTarget& target : targets) {
        if (!integers.contains(target.caseValue))
            return;
        isListed.quickSet(target.caseValue - integers.minimum);
    }
    for (uint64_t index = 0; index < numberOfIntegers; ++index) {
        if (!isListed.quickGet(index))
            targets.append(SwitchTarget { integers.minimum + static_cast<int64_t>(index), otherwise });
    }
}

void ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, B3::FrequentedBlock otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == B3::Int32 || type == B3::Int64));
    Vector<BlockID, 16> generatedBlocks = emitter<AllocationEmitter>().allocateByTable(terminator, subject);
    if (std::optional<SwitchWindow> window = generatedBlocks.isEmpty() ? findSwitchWindow(terminator.terminator()) : std::nullopt) {
        testBitsOfWindow(terminator, *window, subject, type, otherwise);
        return;
    }
    PayloadListView<int64_t> cases = terminator->cases();
    Vector<SwitchTarget, 8> targets;
    targets.reserveInitialCapacity(cases.size());
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if ((type == B3::Int32 && caseValue != static_cast<int32_t>(caseValue)) || generatedBlocks.contains(terminator->targets[index].block))
            continue;
        targets.append(SwitchTarget { caseValue, countedEdgeTo(terminator, index) });
    }
    if (targets.isEmpty()) {
        m_out.jump(otherwise.block());
        return;
    }
    if (generatedBlocks.isEmpty())
        listIntegersLeftToRest(typeOf(terminator->subjectOperand()), type, m_out.minCasesForJumpTable(), targets, otherwise);
    m_out.switchOn(subject, targets.span(), otherwise);
}

void ControlEmitter::testBitsOfWindow(const TerminatorContext& terminator, const SwitchWindow& window, LValue subject, LType type, B3::FrequentedBlock otherwise)
{
    auto constant = [&](int64_t value) {
        return type == B3::Int32 ? m_out.constInt32(static_cast<int32_t>(value)) : m_out.constInt64(value);
    };
    build().statistics().count(StatisticID::SwitchesTestedByWindow);
    LValue position = m_out.sub(subject, constant(window.first));
    Condition isInWindow(m_out.below(position, constant(window.length)));
    bool hasNoGap = window.hasNoGap();
    if (hasNoGap && window.places.size() == 1) {
        branch(isInWindow, { countedEdgeTo(terminator, window.places[0].firstCase), otherwise });
        return;
    }

    LBasicBlock inWindow = newBlock();
    branch(isInWindow, { normalEdge(inWindow), otherwise });
    m_out.appendTo(inWindow);
    LValue shift = type == B3::Int32 ? position : m_out.castToInt32(position);
    for (size_t index = 0; index < window.places.size(); ++index) {
        B3::FrequentedBlock place = countedEdgeTo(terminator, window.places[index].firstCase);
        bool isLast = index + 1 == window.places.size();
        if (isLast && hasNoGap) {
            m_out.jump(place.block());
            return;
        }
        Condition isListed(m_out.testNonZero64(m_out.lShr(m_out.constInt64(static_cast<int64_t>(window.places[index].values)), shift), 1));
        if (isLast) {
            branch(isListed, { place, otherwise });
            return;
        }
        LBasicBlock next = newBlock();
        branch(isListed, { place, normalEdge(next) });
        m_out.appendTo(next);
    }
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
