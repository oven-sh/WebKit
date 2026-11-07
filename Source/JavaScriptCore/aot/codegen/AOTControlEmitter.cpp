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
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

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
    case Representation::OptionalDouble:
    case Representation::Double: {
        static_assert(undefinedDouble > PNaNAsBits && nullDouble > PNaNAsBits && absentDouble > PNaNAsBits);
        static_assert(undefinedDouble >> 52 == 0x7ff && nullDouble >> 52 == 0x7ff && absentDouble >> 52 == 0x7ff);
        Number number = representationOf(subject) == Representation::Double ? in<Number>(subject) : Number(in<OptionalNumber>(subject).value());
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
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::RawWord:
        fail(terminator, InternalErrorKind::OpcodeWithoutCode);
        m_out.unreachable();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ControlEmitter::testByteOfTableInPlaceOfSwitch(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    PayloadListView<int64_t> cases = terminator->cases();
    if (cases.size() < Machine::minCasesForJumpTable)
        return false;

    const Code& code = generator().code();
    bool someEdgeBringsValues = false;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesBehindEmptyBlocks;
    Vector<BlockID, 8> passedBy;
    auto placeBehind = [&](EdgeID id) {
        const Edge& edge = code.at(id);
        someEdgeBringsValues |= !edge.arguments().empty();
        BlockID block = edge.to;
        while (passedBy.size() < code.numberOfBlocks()) {
            if (auto known = placesBehindEmptyBlocks.find(block.index()); known != placesBehindEmptyBlocks.end()) {
                block = known->value;
                break;
            }
            const Block& passed = code.at(block);
            if (!passed.parameters().empty() || !code.valuesOf(block).empty() || passed.terminator.kind != TerminatorKind::Jump)
                break;
            const Edge& next = code.at(passed.terminator.next());
            if (!next.arguments().empty())
                break;
            passedBy.append(block);
            block = next.to;
        }
        for (BlockID passed : passedBy)
            placesBehindEmptyBlocks.set(passed.index(), block);
        passedBy.shrink(0);
        return block;
    };

    BlockID rest = placeBehind(terminator->otherwise());
    BlockID place;
    uint32_t firstCaseOfPlace = 0;
    uint64_t numberOfListedCases = 0;
    int64_t least = 0;
    int64_t greatest = 0;
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if (type == Machine::Int32 && caseValue != static_cast<int32_t>(caseValue))
            continue;
        BlockID behind = placeBehind(terminator->caseEdge(index));
        if (behind == rest)
            continue;
        if (!place) {
            place = behind;
            firstCaseOfPlace = index;
            least = greatest = caseValue;
        } else if (behind != place)
            return false;
        least = std::min(least, caseValue);
        greatest = std::max(greatest, caseValue);
        ++numberOfListedCases;
    }
    if (someEdgeBringsValues || numberOfListedCases < Machine::minCasesForJumpTable)
        return false;
    uint64_t span = static_cast<uint64_t>(greatest) - static_cast<uint64_t>(least) + 1;
    constexpr uint64_t maxBytesOfTablePerCase = Machine::maxEntriesOfJumpTablePerCase * sizeof(int32_t);
    if (span <= Machine::valuesInSwitchWindow || span > maxBytesOfTablePerCase * numberOfListedCases)
        return false;

    constexpr uint8_t leadsToPlace = 1;
    constexpr uint8_t leadsToRest = 2;
    Vector<uint8_t, 512> isListed(FillWith { }, static_cast<size_t>(span), static_cast<uint8_t>(0));
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if (caseValue < least || caseValue > greatest)
            continue;
        uint8_t& entry = isListed[static_cast<size_t>(static_cast<uint64_t>(caseValue) - static_cast<uint64_t>(least))];
        RELEASE_ASSERT(!entry);
        entry = placeBehind(terminator->caseEdge(index)) == place ? leadsToPlace : leadsToRest;
    }
    for (uint8_t& entry : isListed)
        entry &= leadsToPlace;
    uint32_t constant = m_out.addConstantData(isListed.span(), ConstantKind::Unlabeled);
    build().statistics().count(StatisticID::SwitchesAnsweredByByteOfTable);

    Integer position = type == Machine::Int32
        ? zeroExtended(add(Word32(subject), word32(static_cast<int32_t>(0u - static_cast<uint32_t>(least)))))
        : add(Integer(subject), integer(static_cast<int64_t>(uint64_t { 0 } - static_cast<uint64_t>(least))));
    LBasicBlock isInTable = newBlock();
    branch(below(position, integer(static_cast<int64_t>(span))), { normalEdge(isInTable), otherwise });
    m_out.appendTo(isInTable);
    Pointer entry = offsetBy(Pointer(m_out.relocatable(SymbolSpace::Constant, constant)), position, 0);
    Word32 byte(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTImage_tables, entry.value())));
    branch(isNotZero(byte), { generator().edgeTo(terminator, firstCaseOfPlace), otherwise });
    return true;
}

void ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == Machine::Int32 || type == Machine::Int64));
    if (testByteOfTableInPlaceOfSwitch(terminator, subject, type, otherwise))
        return;
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
