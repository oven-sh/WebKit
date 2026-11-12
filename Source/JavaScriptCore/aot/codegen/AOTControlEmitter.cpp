/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
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
    case Representation::OptionalInt64:
        static_assert(maxTrackedInteger < undefinedInteger && maxTrackedInteger < nullInteger && maxTrackedInteger < absentInteger);
        switchOnInteger(terminator, in<OptionalInteger>(subject).value(), Machine::Int64, otherwise);
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
    if (cases.size() < Machine::minCasesForSwitchWindow)
        return false;

    const Code& code = generator().code();
    bool someEdgeBringsValues = false;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesBehindEmptyBlocks;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> blocksBehindAnswers;
    Vector<BlockID, 8> skippedBlocks;
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
        while (skippedBlocks.size() < code.numberOfBlocks()) {
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
            skippedBlocks.append(block);
            block = behind;
        }
        for (BlockID passed : skippedBlocks)
            placesBehindEmptyBlocks.set(passed.index(), block);
        skippedBlocks.shrink(0);
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
    if (someEdgeBringsValues || numberOfListedCases < Machine::minCasesForSwitchWindow)
        return false;
    uint64_t span = static_cast<uint64_t>(greatest) - static_cast<uint64_t>(least) + 1;
    constexpr uint64_t maxBytesOfTablePerCase = 64;
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

bool ControlEmitter::tryLowerToValueTables(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    const Code& code = generator().code();
    PayloadListView<int64_t> cases = terminator->cases();
    if (cases.size() < Machine::minCasesForSwitchWindow || generator().isLayingOutTurns() || generator().keepsValuesInHeapFrame())
        return false;
    for (uint32_t index = 0; index < cases.size(); ++index) {
        if (const Edge& edge = code.at(terminator->caseEdge(index)); edge.arguments().empty() && !code.valuesOf(edge.to).empty())
            return false;
    }

    static constexpr OpcodeSet opcodesInTables { Opcode::Integer, Opcode::Double, Opcode::Undefined, Opcode::Null, Opcode::Absent };
    static constexpr RepresentationSet representationsInTables { Representation::Int32, Representation::Int64, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble, Representation::JSValue };
    auto isInTables = [&](ValueID value) {
        return value && code.isConstant(value) && opcodesInTables.contains(code.at(value).opcode);
    };
    UncheckedKeyHashMap<uint32_t, EdgeID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> forwardedEdges;
    Vector<BlockID, 8> skippedBlocks;
    auto forwardedEdgeOf = [&](EdgeID id) {
        while (skippedBlocks.size() < code.numberOfBlocks()) {
            BlockID next = code.at(id).to;
            const Block& passed = code.at(next);
            if (!passed.parameters().empty() || !code.valuesOf(next).empty() || passed.terminator.kind != TerminatorKind::Jump || generator().endsLoopInBlock(next))
                break;
            if (auto known = forwardedEdges.find(next.index()); known != forwardedEdges.end()) {
                id = known->value;
                break;
            }
            skippedBlocks.append(next);
            id = passed.terminator.next();
        }
        for (BlockID passed : skippedBlocks)
            forwardedEdges.set(passed.index(), id);
        skippedBlocks.shrink(0);
        return id;
    };
    auto returnedAt = [&](BlockID block) -> std::optional<ValueID> {
        const Block& returning = code.at(block);
        if (!returning.parameters().empty() || !code.valuesOf(block).empty() || returning.terminator.kind != TerminatorKind::Return || generator().endsLoopInBlock(block))
            return std::nullopt;
        ValueID value = returning.terminator.valueOperand();
        return !value || isInTables(value) ? std::optional { value } : std::nullopt;
    };

    BlockID join = code.at(forwardedEdgeOf(terminator->caseEdge(0))).to;
    Vector<Representation, 4> forms;
    const bool returns = returnedAt(join).has_value();
    if (returns) {
        if (conventions().numberOfResults(m_unit) != 1)
            return false;
        forms.append(conventions().resultRepresentation(m_unit));
        join = BlockID();
    } else {
        const Block& blockAtJoin = code.at(join);
        if (blockAtJoin.parameters().empty() || blockAtJoin.isLoopHead || !generator().canBeValueTableJoin(join))
            return false;
        for (ValueID parameter : blockAtJoin.parameters()) {
            if (!generator().canLoadFromTable(parameter))
                return false;
            forms.append(code.isNeeded(parameter) ? generator().carriedRepresentationOf(parameter) : Representation::Void);
        }
    }
    using PassedValues = Vector<ValueID, 2>;
    auto valuesPassedAlong = [&](EdgeID id) -> std::optional<PassedValues> {
        const Edge& arrival = code.at(forwardedEdgeOf(id));
        if (!returns)
            return arrival.to == join ? std::optional { PassedValues(arrival.arguments()) } : std::nullopt;
        std::optional<ValueID> value = returnedAt(arrival.to);
        return value ? std::optional { PassedValues { *value } } : std::nullopt;
    };

    Vector<PassedValues, 16> valuesOfCases;
    int64_t least = std::numeric_limits<int64_t>::max();
    int64_t greatest = std::numeric_limits<int64_t>::min();
    for (uint32_t index = 0; index < cases.size(); ++index) {
        std::optional<PassedValues> passedValues = valuesPassedAlong(terminator->caseEdge(index));
        if (!passedValues || (type == Machine::Int32 && cases[index] != static_cast<int32_t>(cases[index])))
            return false;
        valuesOfCases.append(WTF::move(*passedValues));
        least = std::min(least, cases[index]);
        greatest = std::max(greatest, cases[index]);
    }
    std::optional<PassedValues> valuesOfDefault = valuesPassedAlong(terminator->otherwise());

    Vector<bool, 4> comesFromTable;
    for (size_t place = 0; place < forms.size(); ++place) {
        ValueID first = valuesOfCases.first()[place];
        bool isSharedByAll = forms[place] == Representation::Void || std::ranges::all_of(valuesOfCases, [&](const PassedValues& passedValues) { return passedValues[place] == first; });
        comesFromTable.append(!isSharedByAll);
        if (isSharedByAll) {
            if (forms[place] != Representation::Void && !code.isConstant(first) && !generator().canLoadFromTable(first))
                return false;
            if (forms[place] != Representation::Void && valuesOfDefault && (*valuesOfDefault)[place] != first)
                valuesOfDefault = std::nullopt;
            continue;
        }
        auto isTableConstant = [&](const PassedValues& passedValues) {
            return returns || isInTables(passedValues[place]);
        };
        if (!representationsInTables.contains(forms[place]) || !std::ranges::all_of(valuesOfCases, isTableConstant))
            return false;
        if (valuesOfDefault && !isTableConstant(*valuesOfDefault))
            valuesOfDefault = std::nullopt;
    }
    if (!comesFromTable.contains(true))
        return false;

    const uint64_t maxEntries = Machine::maxEntriesOfJumpTablePerCase * cases.size();
    if (valuesOfDefault) {
        const Range range = typeOf(terminator->subjectOperand()).integers;
        int64_t leastOfAll = std::min(least, range.minimum);
        int64_t greatestOfAll = std::max(greatest, range.maximum);
        if (range.minimum <= range.maximum && static_cast<uint64_t>(greatestOfAll) - static_cast<uint64_t>(leastOfAll) < maxEntries) {
            least = leastOfAll;
            greatest = greatestOfAll;
        }
    }
    uint64_t distance = static_cast<uint64_t>(greatest) - static_cast<uint64_t>(least);
    if (distance >= (valuesOfDefault ? maxEntries : cases.size()))
        return false;
    size_t numberOfEntries = static_cast<size_t>(distance) + 1;

    Vector<Vector<LValue, 16>, 2> tables;
    size_t sizeOfTables = 0;
    size_t mostDistinct = 0;
    for (size_t place = 0; place < forms.size(); ++place) {
        if (!comesFromTable[place])
            continue;
        auto constantOf = [&](const PassedValues& passedValues) {
            return passedValues[place] ? generator().getIn(passedValues[place], forms[place]).value : emitter<ConstantEmitter>().constantOf(jsUndefined(), forms[place]).value;
        };
        Vector<LValue, 16> table(FillWith { }, numberOfEntries, valuesOfDefault ? constantOf(*valuesOfDefault) : nullptr);
        for (uint32_t index = 0; index < cases.size(); ++index)
            table[static_cast<size_t>(static_cast<uint64_t>(cases[index]) - static_cast<uint64_t>(least))] = constantOf(valuesOfCases[index]);
        sizeOfTables += numberOfEntries * valueTableEntrySize(table.span());
        mostDistinct = std::max(mostDistinct, numberOfDistinctConstants(table.span()));
        tables.append(WTF::move(table));
    }
    static constexpr size_t bytesOfCase = 12;
    if (mostDistinct <= Machine::maxPlacesToldByBitsOfSwitchWindow || (!generator().isInLoop(terminator.block()) && sizeOfTables > bytesOfCase * cases.size()))
        return false;

    LBasicBlock isInTable = newBlock();
    Integer position;
    if (type == Machine::Int32) {
        Word32 word = least ? add(Word32(subject), word32(static_cast<int32_t>(0u - static_cast<uint32_t>(least)))) : Word32(subject);
        branch(below(word, word32(static_cast<int32_t>(numberOfEntries))), { normalEdge(isInTable), otherwise });
        m_out.appendTo(isInTable);
        position = zeroExtended(word);
    } else {
        position = least ? add(Integer(subject), integer(static_cast<int64_t>(uint64_t { 0 } - static_cast<uint64_t>(least)))) : Integer(subject);
        branch(below(position, integer(static_cast<int64_t>(numberOfEntries))), { normalEdge(isInTable), otherwise });
        m_out.appendTo(isInTable);
    }
    Vector<Operand, 4> passedValues;
    size_t numberOfTable = 0;
    for (size_t place = 0; place < forms.size(); ++place) {
        if (forms[place] == Representation::Void)
            passedValues.append(Operand());
        else if (comesFromTable[place])
            passedValues.append(Operand(forms[place], loadFromValueTable(tables[numberOfTable++].span(), position)));
        else
            passedValues.append(generator().getIn(valuesOfCases.first()[place], forms[place]));
    }
    if (returns)
        emitter<CallEmitter>().returnValues(passedValues.span());
    else
        generator().jumpToJoin(join, passedValues.span());
    build().statistics().count(returns ? StatisticID::SwitchesLoweredToReturnTables : StatisticID::SwitchesLoweredToValueTables);
    return true;
}

static bool fitsInBytes(std::span<const LValue> constants)
{
    return std::ranges::all_of(constants, [](LValue constant) {
        return constant->hasInt32() && constant->asInt32() == static_cast<uint8_t>(constant->asInt32());
    });
}

size_t ControlEmitter::valueTableEntrySize(std::span<const LValue> constants)
{
    if (constants.front()->type() != Machine::Int32)
        return sizeof(uint64_t);
    return fitsInBytes(constants) ? sizeof(uint8_t) : sizeof(int32_t);
}

static uint64_t bitsOfConstant(LValue constant)
{
    return constant->hasDouble() ? std::bit_cast<uint64_t>(constant->asDouble()) : static_cast<uint64_t>(constant->asInt());
}

size_t ControlEmitter::numberOfDistinctConstants(std::span<const LValue> constants)
{
    Vector<uint64_t, 16> bits;
    for (LValue constant : constants)
        bits.append(bitsOfConstant(constant));
    std::ranges::sort(bits);
    return std::ranges::unique(bits).begin() - bits.begin();
}

LValue ControlEmitter::loadFromValueTable(std::span<const LValue> constants, Integer position)
{
    LType type = constants.front()->type();
    Vector<uint8_t, 128> table;
    auto addressOfEntry = [&](Scale scale) {
        LValue start = m_out.relocatable(SymbolSpace::Constant, m_out.addConstantData(table.span(), ConstantKind::Unlabeled));
        return m_out.baseIndex(m_heaps.AOTImage_tables, start, position.value(), scale);
    };
    auto append = [&](auto entry) {
        table.append(asByteSpan(entry));
    };
    switch (valueTableEntrySize(constants)) {
    case sizeof(uint8_t):
        for (LValue constant : constants)
            table.append(static_cast<uint8_t>(constant->asInt32()));
        return m_out.load8ZeroExt32(addressOfEntry(ScaleOne));
    case sizeof(int32_t):
        for (LValue constant : constants)
            append(constant->asInt32());
        return m_out.load32(addressOfEntry(ScaleFour));
    default:
        for (LValue constant : constants)
            append(bitsOfConstant(constant));
        return type == Machine::Double ? m_out.loadDouble(addressOfEntry(ScaleEight)) : m_out.load64(addressOfEntry(ScaleEight));
    }
}

BlockEdge ControlEmitter::switchOnInteger(const TerminatorContext& terminator, LValue subject, LType type, BlockEdge otherwise)
{
    RELEASE_ASSERT(subject->type() == type && (type == Machine::Int32 || type == Machine::Int64));
    if (tryLowerToValueTables(terminator, subject, type, otherwise))
        return otherwise;
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
