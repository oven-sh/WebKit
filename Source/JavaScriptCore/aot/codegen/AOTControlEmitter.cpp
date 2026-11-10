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
        Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(number);
        LBasicBlock isIntegral = newBlock();
        LBasicBlock isNoInteger = newBlock();
        branch(equal(toNumber(truncated), number), { normalEdge(isIntegral), BlockEdge(isNoInteger, otherwise.edgeClass()) });
        m_out.appendTo(isIntegral);
        BlockEdge rest = switchOnInteger(terminator, truncated.value(), Machine::Int64, otherwise);
        m_out.appendTo(isNoInteger);
        m_out.jump(rest);
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

bool ControlEmitter::testByteOfTableInPlaceOfSwitch(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise, SwitchesBehindSwitch& takenIn)
{
    PayloadListView<int64_t> cases = terminator->cases();
    if (cases.size() < Machine::minCasesForJumpTable)
        return false;

    const Code& code = generator().code();
    bool someEdgeBringsValues = false;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesBehindEmptyBlocks;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> blocksBehindAnswers;
    Vector<BlockID, 8> passedBy;
    auto blockBehindAnswerBroughtBy = [&](const Edge& bringing) {
        if (bringing.arguments().size() != 1 || !code.isConstant(bringing.arguments()[0]) || code.at(bringing.arguments()[0]).opcode != Opcode::Boolean)
            return BlockID();
        bool answer = code.at(bringing.arguments()[0]).boolean();
        return blocksBehindAnswers.ensure(bringing.to.index() * 2 + answer, [&] {
            std::span<const ValueID> parameters = code.at(bringing.to).parameters();
            if (parameters.size() != 1 || code.useCountOf(parameters[0]) != 1)
                return BlockID();
            BlockID block = bringing.to;
            for (uint32_t numberPassed = 0; numberPassed < code.numberOfBlocks(); ++numberPassed) {
                const Block& passed = code.at(block);
                if (!code.valuesOf(block).empty() || (numberPassed && !passed.parameters().empty()))
                    break;
                if (passed.terminator.kind == TerminatorKind::Branch) {
                    const Edge& chosen = code.at(answer ? passed.terminator.taken() : passed.terminator.notTaken());
                    return passed.terminator.conditionOperand() == parameters[0] && chosen.arguments().empty() ? chosen.to : BlockID();
                }
                if (passed.terminator.kind != TerminatorKind::Jump || !code.at(passed.terminator.next()).arguments().empty())
                    break;
                block = code.at(passed.terminator.next()).to;
            }
            return BlockID();
        }).iterator->value;
    };
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
            BlockID behind = next.arguments().empty() ? next.to : blockBehindAnswerBroughtBy(next);
            if (!behind)
                break;
            passedBy.append(block);
            block = behind;
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
        if (behind == rest) {
            takenIn.casesOfFirstThatLeadOn.set(index);
            continue;
        }
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
    while (place && !someEdgeBringsValues && !generator().isTakenIntoSwitchInFront(terminator.block()) && takenIn.casesThatLeadToPlace.size() < code.numberOfEdges()) {
        const Block& behindOtherwise = code.at(rest);
        const Terminator& next = behindOtherwise.terminator;
        if (!behindOtherwise.parameters().empty() || !code.valuesOf(rest).empty() || next.kind != TerminatorKind::Switch || next.subjectOperand() != terminator->subjectOperand() || &next == &terminator.terminator())
            break;
        std::optional<BlockEdge> otherwiseOfNext = generator().edgeThatBringsNothing(next.otherwise());
        if (!otherwiseOfNext)
            break;
        BlockID restOfNext = placeBehind(next.otherwise());
        PayloadListView<int64_t> casesOfNext = next.cases();
        size_t numberTakenInBefore = takenIn.casesThatLeadToPlace.size();
        for (uint32_t index = 0; index < casesOfNext.size() && !someEdgeBringsValues; ++index) {
            BlockID behind = placeBehind(next.caseEdge(index));
            if (behind == restOfNext || (type == Machine::Int32 && casesOfNext[index] != static_cast<int32_t>(casesOfNext[index])))
                continue;
            someEdgeBringsValues |= behind != place;
            takenIn.casesThatLeadToPlace.append(casesOfNext[index]);
        }
        if (someEdgeBringsValues) {
            someEdgeBringsValues = false;
            takenIn.casesThatLeadToPlace.shrink(numberTakenInBefore);
            break;
        }
        takenIn.otherwise = otherwiseOfNext;
        generator().noteSwitchTakenIntoSwitchInFront(rest);
        rest = restOfNext;
        build().statistics().count(StatisticID::SwitchesMergedIntoPredecessor);
    }
    if (takenIn.otherwise) {
        takenIn.firstCaseOfPlace = firstCaseOfPlace;
        otherwise = *takenIn.otherwise;
        for (int64_t caseValue : takenIn.casesThatLeadToPlace) {
            least = std::min(least, caseValue);
            greatest = std::max(greatest, caseValue);
            ++numberOfListedCases;
        }
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
    for (int64_t caseValue : takenIn.casesThatLeadToPlace)
        isListed[static_cast<size_t>(static_cast<uint64_t>(caseValue) - static_cast<uint64_t>(least))] |= leadsToPlace;
    for (uint8_t& entry : isListed)
        entry &= leadsToPlace;
    uint32_t constant = m_out.addConstantData(isListed.span(), ConstantKind::Unlabeled);
    build().statistics().count(StatisticID::SwitchesLoweredToByteTables);

    LBasicBlock isInTable = newBlock();
    Integer position;
    if (type == Machine::Int32) {
        Word32 word = add(Word32(subject), word32(static_cast<int32_t>(0u - static_cast<uint32_t>(least))));
        branch(below(word, word32(static_cast<int32_t>(span))), { normalEdge(isInTable), otherwise });
        m_out.appendTo(isInTable);
        position = zeroExtended(word);
    } else {
        position = add(Integer(subject), integer(static_cast<int64_t>(uint64_t { 0 } - static_cast<uint64_t>(least))));
        branch(below(position, integer(static_cast<int64_t>(span))), { normalEdge(isInTable), otherwise });
        m_out.appendTo(isInTable);
    }
    Pointer entry = offsetBy(Pointer(m_out.relocatable(SymbolSpace::Constant, constant)), position, 0);
    Word32 byte(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTImage_tables, entry.value())));
    branch(isNotZero(byte), { generator().edgeTo(terminator, firstCaseOfPlace), otherwise });
    return true;
}

BlockEdge ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == Machine::Int32 || type == Machine::Int64));
    SwitchesBehindSwitch takenIn;
    if (testByteOfTableInPlaceOfSwitch(terminator, subject, type, otherwise, takenIn))
        return takenIn.otherwise.value_or(otherwise);
    PayloadListView<int64_t> cases = terminator->cases();
    Vector<SwitchTarget, 8> targets;
    targets.reserveInitialCapacity(cases.size() + takenIn.casesThatLeadToPlace.size());
    for (uint32_t index = 0; index < cases.size(); ++index) {
        int64_t caseValue = cases[index];
        if ((type == Machine::Int32 && caseValue != static_cast<int32_t>(caseValue)) || (takenIn.otherwise && takenIn.casesOfFirstThatLeadOn.get(index)))
            continue;
        targets.append(SwitchTarget { caseValue, generator().edgeTo(terminator, index) });
    }
    if (takenIn.otherwise) {
        otherwise = *takenIn.otherwise;
        Vector<int64_t, 16> listed(targets.size(), [&](size_t index) { return targets[index].caseValue; });
        std::ranges::sort(listed);
        std::ranges::sort(takenIn.casesThatLeadToPlace);
        takenIn.casesThatLeadToPlace.shrink(std::ranges::unique(takenIn.casesThatLeadToPlace).begin() - takenIn.casesThatLeadToPlace.begin());
        for (int64_t caseValue : takenIn.casesThatLeadToPlace) {
            if (!std::ranges::binary_search(listed, caseValue))
                targets.append(SwitchTarget { caseValue, generator().edgeTo(terminator, takenIn.firstCaseOfPlace) });
        }
    }
    if (targets.isEmpty()) {
        m_out.jump(otherwise);
        return otherwise;
    }
    m_out.switchOn(subject, targets.span(), otherwise);
    return otherwise;
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
    LBasicBlock isNoNumber = nullptr;
    if (!subjectType.isNumber()) {
        LBasicBlock isDoubleSubject = newBlock();
        isNoNumber = newBlock();
        branch(isNumber(subject), { normalEdge(isDoubleSubject), BlockEdge(isNoNumber, otherwise.edgeClass()) });
        m_out.appendTo(isDoubleSubject);
    }
    join.add(callLeaf<Integer>(RuntimeOperation::TryConvertToInteger, { subject }));

    BlockEdge rest = switchOnInteger(terminator, join.finish().value(), Machine::Int64, otherwise);
    if (isNoNumber) {
        m_out.appendTo(isNoNumber);
        m_out.jump(rest);
    }
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
