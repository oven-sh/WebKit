/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilderRows.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
bool joinIsLikelyKeyInWalkTexts(Walker&, ValueID join);

using PartsInTextBuilderRows = Vector<Reference, 8>;

static constexpr size_t mostPartsInTextBuilderRows = 32;
static constexpr size_t mostElementsJoinedInTextBuilderRows = (mostPartsInTextBuilderRows + 1) / 2;
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

static Reference writtenTextInTextBuilderRows(Walker& walker, StringID text)
{
    Draft constant(Opcode::String);
    constant.setString(text);
    return walker.constant(WTF::move(constant));
}

static bool isEmptyTextInTextBuilderRows(const Walker& walker, Reference text)
{
    if (!text.isConstant())
        return false;
    const Operation& constant = walker.code().at(text.id());
    return constant.opcode == Opcode::String && constant.string() == walker.program().strings.emptyString();
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
        if (!cellPreservingOpcodes.contains(record.opcode))
            return false;
        names.append(behind[place]);
    }
    return true;
}

static ValueID findOperationWrittenForOneReaderInTextBuilderRows(const BodyIndex& index, ValueID text)
{
    while (text && !index.isParameter(text) && index.hasOneUse(text)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(text)))
            return text;
        text = index.value(text).operand(0);
    }
    return { };
}

static bool onlyRecordsThatNobodySeesStandBehindInTextBuilderRows(const Walker& walker, ValueID made)
{
    const Code& code = walker.code();
    for (ValueID id = code.lastOperationOf(walker.currentBlock()); id != made; id = code.at(id).previous) {
        if (!id || !code.at(id).traits().isRemovableWhenUnused())
            return false;
    }
    return true;
}

bool takePartsOfJoinMadeForOneReader(Walker& walker, ValueID operand, Vector<Reference, 8>& parts, Vector<ValueID, 2>* joinsThatFailWhereReaderFails)
{
    Reference text = walker.isUnreachable() ? Reference() : walker.referenceOf(operand);
    if (!text || !text.isValue())
        return false;
    const Code& code = walker.code();
    ValueID root = underlyingCellOf(code, text.id());
    const Operation& made = code.at(root);
    if (!textsTakenApartInTextBuilderRows.contains(made.opcode))
        return false;
    bool mayFailForItsLength = !made.traits().isRemovableWhenUnused();
    if (mayFailForItsLength && (!joinsThatFailWhereReaderFails || !operationsThatJoinTexts.contains(made.opcode) || !made.traits().withoutThrowByRule().isRemovableWhenUnused() || made.block() != walker.currentBlock()))
        return false;
    if (made.opcode == Opcode::Strcat && made.isLikelyKey())
        return false;
    if (!hasOnlyInTextBuilderRows(walker, Reference::value(root), Primitive::AnyString))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID taker = index.onlyUsingValue(operand);
    ValueID written = taker ? findOperationWrittenForOneReaderInTextBuilderRows(index, operand) : ValueID();
    if (!written || index.loopOf(index.blockOf(written)) != index.loopOf(index.blockOf(taker)))
        return false;
    if (std::optional<uint32_t> loop = walker.innermostOpenLoop(); loop && walker.standsInFrontOfOpenLoop(*loop, Reference::value(root)))
        return false;
    size_t numberOfPartsAround = parts.size() + index.value(taker).variadicOperands().size() + numberOfOperandsOfSumInTextBuilderRows;
    if (numberOfPartsAround + made.numberOfOperands() > mostPartsInTextBuilderRows)
        return false;
    for (ValueID part : made.operands()) {
        if (!part || !hasTextByItselfInTextBuilderRows(walker, referenceToRecordInTextBuilderRows(code, part)))
            return false;
    }
    bool oneOperandIsText = std::ranges::any_of(made.operands(), [&](ValueID part) {
        return hasOnlyInTextBuilderRows(walker, referenceToRecordInTextBuilderRows(code, part), Primitive::AnyString);
    });
    if (made.opcode == Opcode::Add && !oneOperandIsText)
        return false;
    if (made.block() == walker.currentBlock() && !isReadByNoRecordSoFar(walker, Reference::value(root)))
        return false;
    if (mayFailForItsLength) {
        if (!onlyRecordsThatNobodySeesStandBehindInTextBuilderRows(walker, root))
            return false;
        joinsThatFailWhereReaderFails->append(root);
    }

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

static ValueID findOperandOfAppendedSum(Walker& walker, const BodyValue& sum)
{
    if (sum.opcode() != Opcode::Add || !walker.isInLoop() || !walker.hasStringsInBuilders())
        return { };
    ValueID reader = walker.currentBodyIndex().onlyUsingValue(sum.id());
    if (!reader || !findStringInBuilder(walker, reader))
        return { };
    for (uint32_t side : { OperandIndex::Add::left, OperandIndex::Add::right }) {
        Reference operand = walker.referenceOf(sum.operand(side));
        if (!operand || !operand.isValue() || !walker.types().overlap(walker.typeOf(operand), TypeTable::string) || hasTextByItselfInTextBuilderRows(walker, operand))
            return { };
    }
    return sum.operand(OperandIndex::Add::left);
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

static bool isJoinedIntoStringInBuilder(Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID part)
{
    ValueID reader = index.onlyUsingValue(part);
    if (!reader || (index.opcodeOf(reader) != Opcode::Strcat && !findStringInBuilder(walker, reader)))
        return false;
    for (ValueID taker = reader; taker && block.holds(taker) && operationsThatJoinTexts.contains(index.opcodeOf(taker)); taker = index.onlyUsingValue(taker)) {
        if (findStringInBuilder(walker, taker))
            return true;
    }
    return false;
}

static bool keepNumberThatIsConvertedInFrontOfOtherParts(Walker& walker, const BodyValue& conversion, Reference value, const ExpectationOfValue& expectation)
{
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    if (!expectation.type || !types.at(expectation.type).isNumber() || !types.at(proven).isJavaScriptValue() || !types.overlap(proven, TypeTable::number))
        return false;
    if (!shouldOpenGuess(walker, value, TypeTable::number, expectation.isDeclared, closedOperationsOfJoinBesideString))
        return false;

    Origin origin = walker.originOf(conversion);
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(TypeTable::number);
    Reference isNumber = walker.build(WTF::move(test));
    if (!isNumber)
        return true;
    if (!isNumber.isValue())
        return false;

    BlockID keeps = walker.addBlock();
    BlockID converts = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Reference part = walker.addParameter(meeting, { }, Representation::JSValue);
    walker.branch(isNumber, keeps, converts);
    walker.recordSpeculation();

    walker.continueIn(keeps);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(isNumber);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    if (Reference number = walker.build(WTF::move(proof)))
        walker.jumpTo(meeting, singleElementSpan(number));

    walker.continueIn(converts);
    Draft asWritten = walker.draftOf(conversion);
    walker.chooseConservativeFields(asWritten);
    asWritten.setForm(operatorFormFor(walker, asWritten));
    if (Reference text = walker.build(WTF::move(asWritten)))
        walker.jumpTo(meeting, singleElementSpan(text));

    walker.continueIn(meeting);
    if (!walker.isUnreachable())
        walker.bind(conversion.id(), part);
    return true;
}

static bool joinStringAndArrayInTextBuilderRows(Walker&, const BodyValue& sum);

bool guessThatOperandBesideStringHasTextOfItsOpcodes(Walker& walker, const BodyValue& sum)
{
    bool isConversion = sum.opcode() == Opcode::ToString;
    if ((sum.opcode() != Opcode::Add && !isConversion) || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    if (!isConversion && joinStringAndArrayInTextBuilderRows(walker, sum))
        return true;
    ValueID guessed = isConversion ? sum.operand(OperandIndex::ToString::value) : findOperandBesideStringInTextBuilderRows(walker, sum);
    bool isAppendedSum = !isConversion && !guessed;
    if (isAppendedSum)
        guessed = findOperandOfAppendedSum(walker, sum);
    Reference value = guessed ? walker.referenceOf(guessed) : Reference();
    if (!value || !value.isValue() || (isConversion && hasTextByItselfInTextBuilderRows(walker, value)))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(sum.id()));
    ValueID joinOfTemplate = isConversion ? index.onlyUsingValue(sum.id()) : ValueID();
    bool handsPartOnToLikelyKey = joinOfTemplate && block.holds(joinOfTemplate) && index.opcodeOf(joinOfTemplate) == Opcode::Strcat && joinIsLikelyKeyInWalkTexts(walker, joinOfTemplate);
    if (isConversion && !handsPartOnToLikelyKey && !walker.hasStringsInBuilders())
        return false;
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = sum.id().index() - firstValue;
    uint32_t limit = limitOfStretchInTextBuilderRows(walker, index, block);
    if (begin >= limit)
        return false;
    ValueID last = findLastJoinTypedByProofInTextBuilderRows(walker, index, block, sum.id(), value, limit);

    TypeArena& types = walker.types();
    ExpectationOfValue expectation = findExpectation(walker, guessed);
    bool endsInStringInBuilder = last != sum.id() && findStringInBuilder(walker, last);
    handsPartOnToLikelyKey &= !endsInStringInBuilder && expectation.isDeclared;
    if (handsPartOnToLikelyKey)
        last = sum.id();
    if (isConversion && !endsInStringInBuilder && !handsPartOnToLikelyKey && isJoinedIntoStringInBuilder(walker, index, block, sum.id()))
        return keepNumberThatIsConvertedInFrontOfOtherParts(walker, sum, value, expectation);
    if ((isConversion || (expectation.type && walker.isInLoop())) && !endsInStringInBuilder && !handsPartOnToLikelyKey)
        return false;
    TypeID expected = isAppendedSum ? TypeID(TypeTable::string) : typeOfPartGuessedBesideString;
    if (isAppendedSum && !endsInStringInBuilder)
        return false;
    if (expectation.type) {
        TypeID withText = hasTextByItself(types.at(expectation.type)) ? expectation.type : walker.isInLoop() ? findPrimitivesExpectedBesideObjects(walker, guessed) : TypeID();
        if (!withText || types.at(withText).isUndefinedOrNull())
            return false;
        expected = types.intersectionOf(withText, expected);
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
    walker.recordSpeculation();
    if (isAppendedSum)
        walker.count(StatisticID::AppendedSumsOfUnknownValues);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet arms;
    walker.walkClosedArm(converts, stretch, arms);
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
            boundBefore.append(walker.nameLastBoundTo(name));
            walker.bind(name, part);
        }
        if (handsPartOnToLikelyKey)
            walker.bind(sum.id(), part);
        else
            walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, arms);
        for (size_t place = 0; place < names.size(); ++place)
            walker.bind(names[place], boundBefore[place]);
    }
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

bool guessThatConvertedPartOfTextInBuilderHasTextOfItsOpcodes(Walker& walker, const BodyValue& conversion)
{
    return guessThatOperandBesideStringHasTextOfItsOpcodes(walker, conversion);
}

static constexpr uint16_t primitivesJoinedWithoutCode = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt;
static constexpr uint16_t primitivesJoinedAsTheirText = primitivesJoinedWithoutCode & ~Primitive::UndefinedOrNull;

static BuiltinID findBuiltinCalledInTextBuilderRows(Walker& walker, const BodyValue& call)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(walker.typeOf(callee));
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.stripCasts(call.calleeOperand());
        if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
            return { };
        Reference object = walker.referenceOf(index.value(read).objectOperand());
        named = object ? findConstantMemberReadOff(walker, object, index.value(read).key()) : BuiltinID();
    }
    return named ? walker.program().builtins().canonicalID(named) : BuiltinID();
}

static bool collectTextsOfTemplateObjectInTextBuilderRows(const Walker& walker, Reference object, Vector<TemplateString, 8>& texts)
{
    if (!object || !object.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(underlyingCellOf(code, object.id()));
    if (made.opcode != Opcode::NewTemplateObject)
        return false;
    for (TemplateString text : made.templateStrings())
        texts.append(text);
    return texts.size() <= mostElementsJoinedInTextBuilderRows;
}

static std::optional<Vector<Reference, 8>> elementsOfJoinedListInTextBuilderRows(Walker& walker, Reference list)
{
    if (isFreshPristineArray(walker, list)) {
        std::optional<uint32_t> length = lengthOfFreshList(walker, list);
        if (!length || *length > mostElementsJoinedInTextBuilderRows)
            return std::nullopt;
        return elementsOfFreshList(walker, list);
    }
    Vector<TemplateString, 8> texts;
    if (!collectTextsOfTemplateObjectInTextBuilderRows(walker, list, texts))
        return std::nullopt;
    Vector<Reference, 8> elements;
    for (TemplateString text : texts)
        elements.append(text.cooked ? writtenTextInTextBuilderRows(walker, text.cooked) : walker.constant(Draft(Opcode::Undefined)));
    return elements;
}

static bool takeElementPartsInTextBuilderRows(Walker&, Reference list, Reference separator, PartsInTextBuilderRows&);

static bool takePartsOfJoinedListInTextBuilderRows(Walker& walker, const BodyValue& call, PartsInTextBuilderRows& parts)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    Reference list = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
    if (!list || arguments.size() > 1)
        return false;
    Reference separator = arguments.empty() ? Reference() : walker.referenceOf(arguments[0]);
    bool isLeftOut = arguments.empty() || hasOnlyInTextBuilderRows(walker, separator, Primitive::Undefined);
    if (!isLeftOut && !hasOnlyInTextBuilderRows(walker, separator, Primitive::AnyString))
        return false;
    return takeElementPartsInTextBuilderRows(walker, list, isLeftOut ? Reference() : separator, parts);
}

static bool isConvertedByJoinInTextBuilderRows(Walker& walker, Reference list)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (program.admitsReplacedBuiltins)
        return false;
    PropertyKeyID hinted = program.keyOf(table.nameOf(WellKnownSymbol::ToPrimitive));
    auto lacksHinted = [&](BuiltinID prototype) {
        return !findOwnMember(program, prototype, hinted) && listsEveryMemberUnder(program, prototype, hinted);
    };
    auto finds = [&](BuiltinID member) {
        PropertyKeyID key = program.keyOf(table.at(table.canonicalID(member)).name);
        BuiltinID found = key ? findConstantMemberReadOff(walker, list, key) : BuiltinID();
        return found && table.canonicalID(found) == table.canonicalID(member);
    };
    return hinted && lacksHinted(table.canonicalID(Builtins::ArrayPrototype)) && lacksHinted(table.canonicalID(Builtins::ObjectPrototype))
        && finds(Builtins::ObjectPrototypeValueOf) && finds(Builtins::ArrayPrototypeToString) && finds(Builtins::ArrayPrototypeJoin);
}

bool takePartsOfConvertedList(Walker& walker, Reference list, Vector<Reference, 8>& parts)
{
    if (!list || !list.isValue() || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable() || !walker.types().at(walker.typeOf(list)).isObject())
        return false;
    size_t numberOfParts = parts.size();
    if (!isFreshPristineArray(walker, list) || !isConvertedByJoinInTextBuilderRows(walker, list) || !takeElementPartsInTextBuilderRows(walker, list, { }, parts)) {
        parts.shrink(numberOfParts);
        return false;
    }
    if (parts.size() == numberOfParts)
        parts.append(writtenTextInTextBuilderRows(walker, walker.program().strings.emptyString()));
    return true;
}

static bool takeElementPartsInTextBuilderRows(Walker& walker, Reference list, Reference separator, PartsInTextBuilderRows& parts)
{
    std::optional<Vector<Reference, 8>> elements = elementsOfJoinedListInTextBuilderRows(walker, list);
    if (!elements)
        return false;
    if (!separator)
        separator = writtenTextInTextBuilderRows(walker, walker.program().strings.comma());

    bool separates = !isEmptyTextInTextBuilderRows(walker, separator);
    for (size_t place = 0; place < elements->size(); ++place) {
        if (place && separates)
            parts.append(separator);
        Reference element = elements->at(place);
        if (hasOnlyInTextBuilderRows(walker, element, Primitive::UndefinedOrNull))
            continue;
        if (!hasOnlyInTextBuilderRows(walker, element, primitivesJoinedAsTheirText))
            return false;
        parts.append(element);
    }
    return true;
}

static bool takePartsOfRawTemplateInTextBuilderRows(Walker& walker, const BodyValue& call, PartsInTextBuilderRows& parts)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    Vector<TemplateString, 8> texts;
    if (arguments.empty() || !collectTextsOfTemplateObjectInTextBuilderRows(walker, walker.referenceOf(arguments[0]), texts))
        return false;
    StringID emptyString = walker.program().strings.emptyString();
    for (size_t place = 0; place < texts.size(); ++place) {
        if (texts[place].raw != emptyString)
            parts.append(writtenTextInTextBuilderRows(walker, texts[place].raw));
        if (place + 1 == texts.size() || place + 1 >= arguments.size())
            continue;
        Reference substitution = walker.referenceOf(arguments[place + 1]);
        if (!hasOnlyInTextBuilderRows(walker, substitution, primitivesJoinedWithoutCode))
            return false;
        parts.append(substitution);
    }
    return true;
}

static bool takePartsOfConcatInTextBuilderRows(Walker& walker, const BodyValue& call, PartsInTextBuilderRows& parts)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    Reference receiver = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
    if (!hasOnlyInTextBuilderRows(walker, receiver, Primitive::AnyString) || arguments.size() >= mostPartsInTextBuilderRows)
        return false;
    parts.append(receiver);
    for (ValueID argument : arguments) {
        Reference part = walker.referenceOf(argument);
        if (!hasOnlyInTextBuilderRows(walker, part, primitivesJoinedWithoutCode))
            return false;
        parts.append(part);
    }
    return true;
}

static bool isKeyAlwaysAbsentInTextBuilderRows(const Program& program, BuiltinID start, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    if (!start || !key)
        return false;
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (members.findOwnMember(holder) || !listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder) || !table.hasLockedPrototypeLink(holder))
            return false;
        switch (table.at(holder).prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(table.at(holder).inheritsFrom);
            break;
        }
    }
}

static bool joinStringAndArrayInTextBuilderRows(Walker& walker, const BodyValue& sum)
{
    ValueID written = findOperandBesideStringInTextBuilderRows(walker, sum);
    Reference list = written ? walker.referenceOf(written) : Reference();
    if (!list || !isFreshPristineArray(walker, list))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    auto finds = [&](BuiltinName name, BuiltinID method) {
        PropertyKeyID key = program.keyOf(name);
        BuiltinID found = key ? findConstantMemberReadOff(walker, list, key) : BuiltinID();
        return found && table.canonicalID(found) == table.canonicalID(method);
    };
    if (!finds(BuiltinNames::valueOf, Builtins::ObjectPrototypeValueOf) || !finds(BuiltinNames::toString, Builtins::ArrayPrototypeToString) || !finds(BuiltinNames::join, Builtins::ArrayPrototypeJoin))
        return false;
    if (!isKeyAlwaysAbsentInTextBuilderRows(program, Builtins::ArrayPrototype, program.keyOf(table.nameOf(WellKnownSymbol::ToPrimitive))))
        return false;
    std::optional<Vector<Reference, 8>> elements = elementsOfJoinedListInTextBuilderRows(walker, list);
    if (!elements)
        return false;

    bool textIsLeft = written == sum.operand(OperandIndex::Add::right);
    Reference text = walker.referenceOf(sum.operand(textIsLeft ? OperandIndex::Add::left : OperandIndex::Add::right));
    PartsInTextBuilderRows parts;
    if (textIsLeft)
        parts.append(text);
    for (size_t place = 0; place < elements->size(); ++place) {
        if (place)
            parts.append(writtenTextInTextBuilderRows(walker, program.strings.comma()));
        Reference element = elements->at(place);
        if (hasOnlyInTextBuilderRows(walker, element, Primitive::UndefinedOrNull))
            continue;
        if (!hasOnlyInTextBuilderRows(walker, element, primitivesJoinedAsTheirText))
            return false;
        parts.append(element);
    }
    if (!textIsLeft)
        parts.append(text);
    if (parts.size() == 1) {
        walker.bind(sum.id(), text);
        return true;
    }
    Origin origin = walker.originOf(sum);
    origin.isTailCall = false;
    Draft joined(Opcode::Strcat, origin);
    walker.chooseConservativeFields(joined);
    joined.inputValueGiven = sum.id();
    joined.setPartsOperands(parts.span());
    if (Reference whole = walker.build(WTF::move(joined)))
        walker.bind(sum.id(), whole);
    return true;
}

bool joinOfWhatIsInSight(Walker& walker, const BodyValue& call, BuiltinID called)
{
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = called ? table.canonicalID(called) : findBuiltinCalledInTextBuilderRows(walker, call);
    if (!builtin)
        return false;

    PartsInTextBuilderRows parts;
    bool isJoin = false;
    if (builtin == table.canonicalID(Builtins::ArrayPrototypeJoin))
        isJoin = takePartsOfJoinedListInTextBuilderRows(walker, call, parts);
    else if (builtin == table.canonicalID(Builtins::StringRaw))
        isJoin = takePartsOfRawTemplateInTextBuilderRows(walker, call, parts);
    else if (builtin == table.canonicalID(Builtins::StringPrototypeConcat))
        isJoin = takePartsOfConcatInTextBuilderRows(walker, call, parts);
    bool isWrittenWhole = std::ranges::all_of(parts, [](Reference part) { return part.isConstant(); });
    if (!isJoin || (isWrittenWhole && builtin != table.canonicalID(Builtins::StringRaw)))
        return false;
    if (parts.isEmpty()) {
        walker.bind(call.id(), writtenTextInTextBuilderRows(walker, walker.program().strings.emptyString()));
        return true;
    }

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    if (parts.size() == 1 && hasOnlyInTextBuilderRows(walker, parts[0], Primitive::AnyString)) {
        walker.bind(call.id(), parts[0]);
        return true;
    }
    Draft joined(parts.size() == 1 ? Opcode::ToString : Opcode::Strcat, origin);
    walker.chooseConservativeFields(joined);
    joined.inputValueGiven = call.id();
    if (parts.size() == 1) {
        joined.setValueOperand(parts[0]);
        joined.setForm(operatorFormFor(walker, joined));
    } else
        joined.setPartsOperands(parts.span());
    if (Reference text = walker.build(WTF::move(joined)))
        walker.bind(call.id(), text);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
