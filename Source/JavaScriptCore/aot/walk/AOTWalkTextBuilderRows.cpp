/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, of rows of joins: what AOTWalkTypedTexts.cpp leaves out. A text that was made for one join alone gives that join its parts (the `${n}` of a template, the join that a copied callee returns),
// an operator on proven numbers may stand between two links, `"id-" + x` is tested once for a part that has text by itself (Guesses), and join, concat and String.raw of what is in sight are joins (BuiltinCalls).
// Nothing is kept from one operation to the next. At a suspension: no row goes past one. At a throw: no part runs code, and a text that is dropped for its parts could not have thrown.
// Withheld: nobody asks.

#include "config.h"
#include "AOTWalkTextBuilderRows.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
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
#if __has_include("AOTWalkJSONWrites.h")
#include "AOTWalkJSONWrites.h"
#endif
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkTypedTexts.cpp
bool joinIsLikelyKeyInWalkTexts(Walker&, ValueID join);

using PartsInTextBuilderRows = Vector<Reference, 8>;

// mostPartsOfJoinedText of AOTWalkTypedTexts.cpp: the emitter of a Strcat keeps the text of every part live at once.
static constexpr size_t mostPartsInTextBuilderRows = 32;
// With a separator between every two of them.
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
        if (!opcodesThatGiveSameCell.contains(record.opcode))
            return false;
        names.append(behind[place]);
    }
    return true;
}

// ---- A text that was made for one join alone

// By the body, back through whatever only hands the text on. It decides what pays, never what is right.
static bool isWrittenForOneReaderInTextBuilderRows(const BodyIndex& index, ValueID text)
{
    while (text && !index.isParameter(text) && index.hasOneUse(text)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(text)))
            return true;
        text = index.value(text).operand(0);
    }
    return false;
}

// `"<" + tag(n) + ">"`, `${n}px`, `(a + n as string) + b`. The record has made its text of parts that have text by themselves, and nobody can tell whether it has run: it cannot throw, so its parts surely fit into a string.
// A primitive has one text whenever it is asked, so whatever stands between the record and the join changes nothing. The sweep drops the record. It builds nothing.
bool takePartsOfJoinReturnedByCopy(Walker& walker, ValueID operand, Vector<Reference, 8>& parts)
{
    constexpr bool takesApartTextsMadeForOneJoin = true;
    if (!takesApartTextsMadeForOneJoin)
        return false;
    Reference text = walker.isUnreachable() ? Reference() : walker.referenceOf(operand);
    if (!text || !text.isValue())
        return false;
    const Code& code = walker.code();
    ValueID root = rootOfCellInSweptCode(code, text.id());
    const Operation& made = code.at(root);
    if (!textsTakenApartInTextBuilderRows.contains(made.opcode) || made.block() != walker.currentBlock() || !made.traits().isRemovableWhenUnused())
        return false;
    // A text that was made as a key is a name that the run time has at hand.
    if (made.opcode == Opcode::Strcat && made.isLikelyKey())
        return false;
    if (!hasOnlyInTextBuilderRows(walker, Reference::value(root), Primitive::AnyString))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID taker = index.onlyUsingValue(operand);
    if (!taker || !isWrittenForOneReaderInTextBuilderRows(index, operand))
        return false;
    // Each operand of the taker brings a part at least. It is asked once an operand, so it counts none of them.
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

// ---- What may stand between two links

// What stands behind the link is not walked yet, and what it was bound to in another copy says nothing: only a number as it is written is known there.
static TypeID typeOfOperandInsideRowInTextBuilderRows(const Walker& walker, const BodyIndex& index, ValueID link, ValueID operand)
{
    if (!operand)
        return { };
    if (!(link < operand)) {
        Reference built = walker.referenceOf(operand);
        return built ? walker.typeOf(built) : TypeID();
    }
    BodyValue written = index.value(operand);
    switch (written.opcode()) {
    case Opcode::Integer:
        return walker.types().integer(Range::one(written.as<Opcode::Integer>().integer()));
    case Opcode::Double:
        return walker.types().number(written.as<Opcode::Double>().number());
    default:
        return { };
    }
}

bool mayStandInsideRowOfJoins(const Walker& walker, const BodyIndex& index, ValueID link, ValueID between)
{
    BodyValue operation = index.value(between);
    Opcode opcode = operation.opcode();
    if (!hasOperatorForm(opcode))
        return false;
    ValueID right = operation.operand(1);
    OperatorTypes operands { typeOfOperandInsideRowInTextBuilderRows(walker, index, link, operation.operand(0)), typeOfOperandInsideRowInTextBuilderRows(walker, index, link, right) };
    if (!operands.left || !!operands.right != !!right)
        return false;
    return chooseOperatorForm(walker.program(), walker.types(), opcode, operands).effects.isRemovableWhenUnused();
}

// ---- The operand beside a string

// No bigint: its text may be too long for a string, which throws where the conversion stands.
static constexpr TypeID typeOfPartGuessedBesideString = typeOfPrimitives(Primitive::NumberLike | Primitive::String);
// Closed, the sum goes to the run time, which asks both operands what they are, converts the one and joins the two.
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

// As limitOfStretchOfGuess() of AOTWalkGuesses.cpp: what the end of the block finds by its definition stays behind every stretch, and whoever walks a stretch has asked for no operation behind its end.
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

// The row as far as the one proof types it: behind written primitives alone, each join alone takes the one in front, and its other parts have text by themselves. A part that needs a test of its own ends the stretch,
// so no closed arm holds the rest of a row whose arms are yet to come.
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
    // `${source.id},${target.id}` that is looked up. The join of a likely key asks the cache of keys by their parts, which knows a number by its value and a string by its cell,
    // and the string of a number is made anew each time. So the number itself goes on to the join: its text is made there, or never. What is no number is converted here, in its turn.
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

    // In a loop the guess that goes by what is expected takes whatever leans on the proof, further than a row goes. Not where the row ends in a text that lies in a builder, `line += c + ","`:
    // the join that grows the text does not mention `c`, so that guess ends in front of it, and every turn makes a string of the part and the comma.
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
    // The conversion of a number finds its string kept, and the string is copied. The call that takes any primitive is dearer than the two, so the test has to buy the part a form of its own.
    if (isConversion && !partHasFormOfItsOwnInStringBuilder(types.at(tested)))
        return false;
    if (!shouldOpenGuess(walker, value, tested, expectation.isDeclared, closedOperationsOfJoinBesideString))
        return false;

    // `path.startsWith(root + "/")`: the test that alone takes the join is walked with it. In the arm where the part has text the join is never made, buildTestAgainstJoinOfTexts(); what the arms bring is the answer of the test.
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

    // Both arms of a growth of a text in a builder bring the builder.
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

// ---- Built-ins that join

static constexpr uint16_t primitivesJoinedWithoutCode = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt;
// join() takes undefined and null for nothing.
static constexpr uint16_t primitivesJoinedAsTheirText = primitivesJoinedWithoutCode & ~Primitive::UndefinedOrNull;

// As CallInWalkBuiltins::findCallee(), which has answered for the call already. Canonical. Null: nothing is sure.
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
        ValueID read = index.valueBehindCasts(call.calleeOperand());
        if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
            return { };
        Reference object = walker.referenceOf(index.value(read).objectOperand());
        named = object ? findConstantMemberReadOff(walker, object, index.value(read).key()) : BuiltinID();
    }
    return named ? walker.program().builtins().canonicalID(named) : BuiltinID();
}

// The object and its list of raw texts are frozen when they are made. So whoever else has seen the object, its texts are those of its source site.
static bool collectTextsOfTemplateObjectInTextBuilderRows(const Walker& walker, Reference object, Vector<TemplateString, 8>& texts)
{
    if (!object || !object.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(rootOfCellInSweptCode(code, object.id()));
    if (made.opcode != Opcode::NewTemplateObject)
        return false;
    for (TemplateString text : made.templateStrings())
        texts.append(text);
    return texts.size() <= mostElementsJoinedInTextBuilderRows;
}

static std::optional<Vector<Reference, 8>> elementsOfJoinedListInTextBuilderRows(Walker& walker, Reference list)
{
    if (isFreshListAsMade(walker, list)) {
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

// `[a, b].join("-")`. Each element is its own and no hole, so reading it runs no code.
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
    std::optional<Vector<Reference, 8>> elements = elementsOfJoinedListInTextBuilderRows(walker, list);
    if (!elements)
        return false;
    if (isLeftOut)
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

// String.raw`${a}/${b}`. A substitution behind the last text is never converted.
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

// `a.concat(b, c)`
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

bool joinOfWhatIsInSight(Walker& walker, const BodyValue& call)
{
    constexpr bool joinsWhatIsInSight = true;
    if (!joinsWhatIsInSight)
        return false;
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    BuiltinID builtin = findBuiltinCalledInTextBuilderRows(walker, call);
    if (!builtin)
        return false;

    const BuiltinTable& table = walker.program().builtins();
    PartsInTextBuilderRows parts;
    bool isJoin = false;
    if (builtin == table.canonicalID(Builtins::ArrayPrototypeJoin))
        isJoin = takePartsOfJoinedListInTextBuilderRows(walker, call, parts);
    else if (builtin == table.canonicalID(Builtins::StringRaw))
        isJoin = takePartsOfRawTemplateInTextBuilderRows(walker, call, parts);
    else if (builtin == table.canonicalID(Builtins::StringPrototypeConcat))
        isJoin = takePartsOfConcatInTextBuilderRows(walker, call, parts);
    // Of written parts alone the text is known, which is for whoever folds the call.
    if (!isJoin || std::ranges::all_of(parts, [](Reference part) { return part.isConstant(); }))
        return false;

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
        joined.setForm(formAllowedForTypedOperator(walker, joined));
    } else
        joined.setPartsOperands(parts.span());
    if (Reference text = walker.build(WTF::move(joined)))
        walker.bind(call.id(), text);
    return true;
}

bool buildCallOfBuiltinForTexts(Walker& walker, const BodyValue& call)
{
#if __has_include("AOTWalkJSONWrites.h")
    if (writeJSONTextOfCall(walker, call) || handJSONTextToBuiltin(walker, call))
        return true;
#endif
    return joinOfWhatIsInSight(walker, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
