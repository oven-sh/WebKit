/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilderRows.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include "AOTWalkJSONWrites.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

bool joinIsLikelyKeyInWalkTexts(Walker&, ValueID join);

static constexpr size_t mostPartsInTextBuilderRows = 32;
static constexpr size_t mostRecordsLookedAtBehindText = 32;
static constexpr size_t numberOfOperandsOfSumInTextBuilderRows = 2;

static constexpr OpcodeSet textsTakenApartInTextBuilderRows = operationsThatJoinTexts | OpcodeSet { Opcode::ToString };

static Reference referenceToRecordInTextBuilderRows(const Code& code, ValueID record)
{
    return code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

static bool hasOnlyInTextBuilderRows(const Walker& walker, Reference value, uint16_t primitives)
{
    TypeID id = value ? walker.typeOf(value) : TypeID();
    if (!id)
        return false;
    Type type = walker.types().at(id);
    return !type.isNever() && type.hasOnlyPrimitives(primitives);
}

static bool hasTextByItselfInTextBuilderRows(const Walker& walker, Reference value)
{
    TypeID id = value ? walker.typeOf(value) : TypeID();
    return id && !isNeverType(id) && hasTextByItself(walker.types().at(id));
}

bool isReadByNoRecordSoFar(const Walker& walker, Reference made)
{
    const Code& code = walker.code();
    Vector<ValueID, mostRecordsLookedAtBehindText> behind;
    for (ValueID id = code.lastOperationOf(walker.currentBlock()); id != made.id(); id = code.at(id).previous) {
        if (!id || behind.size() == mostRecordsLookedAtBehindText)
            return false;
        behind.append(id);
    }

    Vector<ValueID, 4> names { made.id() };
    for (size_t place = behind.size(); place--;) {
        const Operation& record = code.at(behind[place]);
        bool takesText = std::ranges::any_of(record.operands(), [&](ValueID operand) {
            return operand && names.contains(operand);
        });
        if (!takesText)
            continue;
        if (!opcodesThatGiveSameCell.contains(record.opcode))
            return false;
        names.append(behind[place]);
    }
    return true;
}

static bool isWrittenForOneReaderInTextBuilderRows(const BodyIndex& index, ValueID text)
{
    while (text && !index.isParameter(text) && index.hasOneUse(text)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(text)))
            return true;
        text = index.value(text).operand(0);
    }
    return false;
}

bool takePartsOfJoinReturnedByCopy(Walker& walker, ValueID operand, Vector<Reference, 8>& parts)
{
    Reference text = walker.isUnreachable() ? Reference() : walker.referenceOf(operand);
    if (!text || !text.isValue())
        return false;
    const Code& code = walker.code();
    ValueID root = rootOfCellInSweptCode(code, text.id());
    const Operation& made = code.at(root);
    if (!textsTakenApartInTextBuilderRows.contains(made.opcode) || made.block() != walker.currentBlock() || !made.traits().isRemovableWhenUnused())
        return false;
    if (made.opcode == Opcode::Strcat && made.isLikelyKey())
        return false;
    if (!hasOnlyInTextBuilderRows(walker, Reference::value(root), Primitive::AnyString))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID taker = index.onlyUsingValue(operand);
    if (!taker || !isWrittenForOneReaderInTextBuilderRows(index, operand))
        return false;
    size_t numberOfPartsAround = parts.size() + index.value(taker).variadicOperands().size() + numberOfOperandsOfSumInTextBuilderRows;
    if (numberOfPartsAround + made.numberOfOperands() > mostPartsInTextBuilderRows)
        return false;
    for (ValueID part : made.operands()) {
        if (!part || !hasTextByItselfInTextBuilderRows(walker, referenceToRecordInTextBuilderRows(code, part)))
            return false;
    }
    if (!isReadByNoRecordSoFar(walker, Reference::value(root)))
        return false;

    for (ValueID part : made.operands())
        parts.append(referenceToRecordInTextBuilderRows(code, part));
    return true;
}

bool holdsPartsTakenOutOfTexts(const Walker& walker, const BodyValue& join, std::span<const Reference> parts)
{
    size_t place = 0;
    bool standsAsWritten = true;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        standsAsWritten &= place < parts.size() && parts[place] == walker.referenceOf(operand);
        ++place;
    });
    return !standsAsWritten || place != parts.size();
}

static constexpr TypeID typeOfPartGuessedBesideString = typeOfPrimitives(Primitive::NumberLike | Primitive::String);
static constexpr uint32_t closedOperationsOfJoinBesideString = 2;

static ValueID findOperandBesideStringInTextBuilderRows(const Walker& walker, const BodyValue& sum)
{
    for (uint32_t side : { OperandIndex::Add::left, OperandIndex::Add::right }) {
        Reference operand = walker.referenceOf(sum.operand(side));
        Reference other = walker.referenceOf(sum.operand(1 - side));
        if (!operand || !other)
            return { };
        if (!hasTextByItselfInTextBuilderRows(walker, operand) && hasOnlyInTextBuilderRows(walker, other, Primitive::AnyString))
            return sum.operand(side);
    }
    return { };
}

static uint32_t limitOfStretchInTextBuilderRows(const Walker& walker, const BodyIndex& index, const BodyBlock& block)
{
    uint32_t limit = std::min(block.numberOfValues(), walker.endOfStretchBeingWalked());
    auto keepBehindStretch = [&](ValueID value) {
        if (value && value >= block.firstValue() && block.holds(value))
            limit = std::min(limit, value.index() - block.firstValue().index());
    };
    BodyTerminator end = block.terminator();
    if (end.isConditionalBranch()) {
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (!result.parameter || index.useCount(result.parameter))
                keepBehindStretch(result.test);
        });
    }
    keepBehindStretch(relationThatNarrowsBehind(walker, end));
    return limit;
}

static ValueID findLastJoinTypedByProofInTextBuilderRows(const Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID first, Reference guessed, uint32_t limit)
{
    uint32_t endOfStretch = block.firstValue().index() + limit;
    ValueID last = first;
    while (ValueID taker = index.onlyUsingValue(last)) {
        if (taker.index() >= endOfStretch || !block.holds(taker) || !operationsThatJoinTexts.contains(index.opcodeOf(taker)))
            break;
        bool isTypedByProof = true;
        for (uint32_t between = last.index() + 1; between < taker.index(); ++between)
            isTypedByProof &= primitiveConstants.contains(index.opcodeOf(ValueID(between)));
        index.value(taker).forEachOperand([&](ValueID operand, uint32_t) {
            if (!operand || operand == last || first < operand)
                return;
            Reference part = walker.referenceOf(operand);
            isTypedByProof &= part == guessed || hasTextByItselfInTextBuilderRows(walker, part);
        });
        if (!isTypedByProof)
            break;
        last = taker;
    }
    return last;
}

bool guessThatOperandBesideStringHasTextOfItsOpcodes(Walker& walker, const BodyValue& sum)
{
    bool isConversion = sum.opcode() == Opcode::ToString;
    if ((sum.opcode() != Opcode::Add && !isConversion) || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    ValueID guessed = isConversion ? sum.operand(OperandIndex::ToString::value) : findOperandBesideStringInTextBuilderRows(walker, sum);
    Reference value = guessed ? walker.referenceOf(guessed) : Reference();
    if (!value || !value.isValue() || (isConversion && hasTextByItselfInTextBuilderRows(walker, value)))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(sum.id()));
    ValueID joinOfTemplate = isConversion ? index.onlyUsingValue(sum.id()) : ValueID();
    bool handsPartOnToLikelyKey = joinOfTemplate && block.holds(joinOfTemplate) && index.opcodeOf(joinOfTemplate) == Opcode::Strcat && joinIsLikelyKeyInWalkTexts(walker, joinOfTemplate);
    if (isConversion && !handsPartOnToLikelyKey && !walker.hasTextsInStringBuilders())
        return false;
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = sum.id().index() - firstValue;
    uint32_t limit = limitOfStretchInTextBuilderRows(walker, index, block);
    if (begin >= limit)
        return false;
    ValueID last = findLastJoinTypedByProofInTextBuilderRows(walker, index, block, sum.id(), value, limit);

    TypeArena& types = walker.types();
    ExpectationOfValue expectation = findExpectation(walker, guessed);
    bool endsInTextInStringBuilder = last != sum.id() && findTextNamedBy(walker, last);
    handsPartOnToLikelyKey &= !endsInTextInStringBuilder && expectation.isDeclared;
    if (handsPartOnToLikelyKey)
        last = sum.id();
    if ((isConversion || (expectation.type && walker.isInLoop())) && !endsInTextInStringBuilder && !handsPartOnToLikelyKey)
        return false;
    TypeID expected = typeOfPartGuessedBesideString;
    if (expectation.type) {
        if (!hasTextByItself(types.at(expectation.type)))
            return false;
        expected = types.intersectionOf(expectation.type, expected);
    }
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(expected));
    TypeID proven = walker.typeOf(value);
    if (!tested || isNeverType(tested) || !types.at(proven).isJavaScriptValue() || types.isSubset(proven, tested) || !types.overlap(proven, tested))
        return false;
    if (isConversion && !partHasFormOfItsOwnInStringBuilder(types.at(tested)))
        return false;
    if (!shouldOpenGuess(walker, value, tested, expectation.isDeclared, closedOperationsOfJoinBesideString))
        return false;

    if (last == sum.id()) {
        if (ValueID test = findTestOfEndThatTakesJoinApart(walker, sum, value); test && test.index() < firstValue + limit)
            last = test;
    }
    uint32_t end = last.index() + 1 - firstValue;

    Vector<ValueID, 2> names;
    for (uint32_t id = sum.id().index(); id <= last.index(); ++id) {
        index.value(ValueID(id)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && operand < sum.id() && !names.contains(operand) && walker.referenceOf(operand) == value)
                names.append(operand);
        });
    }

    Origin origin = walker.originOf(sum);
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    Reference hasText = walker.build(WTF::move(test));
    if (!hasText)
        return true;

    BlockID joins = walker.addBlock();
    BlockID converts = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    walker.branch(hasText, joins, converts);
    walker.noteGuessOpened();

    struct Arm {
        BlockID last;
        Reference text;
    };
    Vector<Arm, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        arms.append(Arm { walker.currentBlock(), walker.referenceOf(last) });
    };

    walker.continueIn(converts);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), begin, end);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(joins);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(hasText);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    if (Reference part = walker.build(WTF::move(proof))) {
        Vector<Reference, 2> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, part);
        }
        if (handsPartOnToLikelyKey)
            walker.bind(sum.id(), part);
        else
            walker.walkStretch(block.id(), begin, end);
        endArm();
        for (size_t place = 0; place < names.size(); ++place)
            walker.bind(names[place], boundBefore[place]);
    }
    if (walker.hasFailed())
        return true;

    Reference met;
    if (arms.size() == 2 && arms[0].text && arms[1].text && arms[0].text != arms[1].text) {
        const Code& code = walker.code();
        TypeID type = joinOf(types, walker.facts().families(), code.at(arms[0].text.id()).type, code.at(arms[1].text.id()).type);
        Representation forms[] { walker.representationOf(arms[0].text), walker.representationOf(arms[1].text) };
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
    }
    for (const Arm& arm : arms) {
        walker.continueIn(arm.last);
        if (met)
            walker.jumpTo(meeting, singleElementSpan(arm.text));
        else
            walker.jumpTo(meeting);
        if (walker.hasFailed())
            return true;
    }
    walker.continueIn(meeting);
    if (met)
        walker.bind(last, met);
    else if (!arms.isEmpty() && arms[0].text)
        walker.bind(last, arms[0].text);
    return true;
}

bool guessThatConvertedPartOfTextInBuilderHasTextOfItsOpcodes(Walker& walker, const BodyValue& conversion)
{
    return guessThatOperandBesideStringHasTextOfItsOpcodes(walker, conversion);
}

bool buildCallOfBuiltinForTexts(Walker& walker, const BodyValue& call)
{
    if (writeJSONTextOfCall(walker, call)) {
        walker.count(StatisticID::OwnKeyWalksExpanded);
        return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
