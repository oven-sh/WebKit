/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: comparisons that need no string. A character and a char code are compared by the code unit that their read has left, a length is asked for zero by asking the cell, a piece is compared where it lies,
// two relations on two strings share one order, and a row of comparisons of one subject with written texts shares its length.
// Each looks back by records that dominate it and keeps nothing, so a suspension and a throw end nothing. A test whose edges hand on a narrowed operand keeps its opcode and its operands: it is refused here.
// Withheld (TypedOperations): every comparison is copied in its closed form.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "JSGlobalObjectFunctions.h"
#include <array>
#include <cmath>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static Reference booleanForStringComparisons(Walker& walker, bool value)
{
    return constantForStrings(walker, PrimitiveKnownToFolding::boolean(value));
}

static bool asksForGreaterOnLeftForStringComparisons(Opcode relation)
{
    return relation == Opcode::Greater || relation == Opcode::GreaterEq;
}

static bool relationHoldsBetweenForStringComparisons(Opcode relation, int64_t left, int64_t right)
{
    switch (relation) {
    case Opcode::Less:
        return left < right;
    case Opcode::LessEq:
        return left <= right;
    case Opcode::Greater:
        return left > right;
    case Opcode::GreaterEq:
        return left >= right;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Reference compareInOrderForStringComparisons(Walker& walker, Opcode opcode, Reference subject, Reference other, bool subjectIsOnLeft, Origin origin)
{
    return buildOperatorForStrings(walker, opcode, subjectIsOnLeft ? subject : other, subjectIsOnLeft ? other : subject, origin);
}

// ---- A character by its code unit

// Outside the string the read gave undefined, and the code unit is -1. Only for a character that nobody takes as a string: whoever does wants the test to leave him the string.
static Reference testCharacterForUndefinedForStringComparisons(Walker& walker, Reference character, bool passesForUndefined, Origin origin)
{
    Reference codeUnit = codeUnitOfCharacterInSight(walker, character);
    Reference root = rootForStrings(walker, character);
    if (!codeUnit || !(walker.types().at(walker.typeOf(root)).primitives & Primitive::Undefined) || !notesOfStrings(walker).charactersOnlyAskedAbout.contains(root.id().index()))
        return { };
    Reference zero = integerForStrings(walker, 0);
    return passesForUndefined ? buildOperatorForStrings(walker, Opcode::Less, codeUnit, zero, origin) : buildOperatorForStrings(walker, Opcode::LessEq, zero, codeUnit, origin);
}

// NaN equals nothing and stands in no relation. -1 does as NaN does where the other side cannot tell them apart, and as an unsigned word it lies above every int32.
static Reference compareCharCodeForStringComparisons(Walker& walker, Opcode opcode, Reference codeUnit, Reference other, bool numberIsOnLeft, Origin origin)
{
    Type typeOfOther = walker.types().at(walker.typeOf(other));
    if (!typeOfOther.isInt32())
        return { };
    bool isEquality = allEqualities.contains(opcode);
    bool charCodeIsLesserOperand = !isEquality && asksForGreaterOnLeftForStringComparisons(opcode) != numberIsOnLeft;
    if (charCodeIsLesserOperand)
        codeUnit = buildOperatorForStrings(walker, Opcode::URShift, codeUnit, integerForStrings(walker, 0), origin);
    else if (isEquality ? typeOfOther.integers.contains(-1) : typeOfOther.integers.minimum < 0)
        return { };
    return compareInOrderForStringComparisons(walker, isEquality ? Opcode::StrictEq : opcode, codeUnit, other, numberIsOnLeft, origin);
}

static Reference compareCharacterWithTextForStringComparisons(Walker& walker, Opcode opcode, Reference character, Reference codeUnit, StringID text, bool characterIsOnLeft, Origin origin)
{
    bool mayBeUndefined = walker.types().at(walker.typeOf(rootForStrings(walker, character))).primitives & Primitive::Undefined;
    std::optional<char16_t> unit = onlyCodeUnitOfText(walker, text);
    if (allEqualities.contains(opcode)) {
        if (unit)
            return buildOperatorForStrings(walker, Opcode::StrictEq, codeUnit, integerForStrings(walker, *unit), origin);
        // charAt() gives the empty string outside the string, and nowhere else.
        if (!lengthOfTextForStrings(walker, text) && !mayBeUndefined)
            return buildOperatorForStrings(walker, Opcode::StrictEq, codeUnit, integerForStrings(walker, -1), origin);
        return booleanForStringComparisons(walker, false);
    }
    if (!unit)
        return { };
    // The empty string lies below every character, as -1 does. undefined stands in no relation.
    bool holdsOfMinusOne = asksForGreaterOnLeftForStringComparisons(opcode) != characterIsOnLeft;
    if (holdsOfMinusOne && mayBeUndefined)
        codeUnit = buildOperatorForStrings(walker, Opcode::URShift, codeUnit, integerForStrings(walker, 0), origin);
    return compareInOrderForStringComparisons(walker, opcode, codeUnit, integerForStrings(walker, *unit), characterIsOnLeft, origin);
}

// Null: nothing was built.
static Reference compareByCodeUnitForStringComparisons(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    bool isEquality = allEqualities.contains(opcode);
    for (uint32_t side = 0; side < 2; ++side) {
        Reference subject = side ? right : left;
        Reference other = side ? left : right;
        if (Reference codeUnit = codeUnitOfCharCodeInSight(walker, subject)) {
            if (Reference answer = compareCharCodeForStringComparisons(walker, opcode, codeUnit, other, !side, origin))
                return answer;
            continue;
        }
        Reference codeUnit = codeUnitOfCharacterInSight(walker, subject);
        if (!codeUnit)
            continue;
        PrimitiveKnownToFolding known = primitiveKnownForStrings(walker, other);
        bool asksForUndefined = known.kind() == PrimitiveKnownToFolding::Kind::Undefined || (opcode == Opcode::Eq && known.kind() == PrimitiveKnownToFolding::Kind::Null);
        if (isEquality && asksForUndefined) {
            if (Reference answer = testCharacterForUndefinedForStringComparisons(walker, subject, true, origin))
                return answer;
            continue;
        }
        if (!known.isText())
            continue;
        if (Reference answer = compareCharacterWithTextForStringComparisons(walker, opcode, subject, codeUnit, known.asText(), !side, origin))
            return answer;
    }
    return { };
}

// `s[i] === t[j]`. Where both may lie outside their strings, both have to give the same there.
static Reference compareCharactersForStringComparisons(Walker& walker, Reference left, Reference right, Origin origin)
{
    Reference leftUnit = codeUnitOfCharacterInSight(walker, left);
    Reference rightUnit = leftUnit ? codeUnitOfCharacterInSight(walker, right) : Reference();
    if (!rightUnit)
        return { };
    TypeArena& types = walker.types();
    bool bothMayLieOutside = types.at(walker.typeOf(leftUnit)).integers.minimum < 0 && types.at(walker.typeOf(rightUnit)).integers.minimum < 0;
    uint16_t differing = types.at(walker.typeOf(rootForStrings(walker, left))).primitives ^ types.at(walker.typeOf(rootForStrings(walker, right))).primitives;
    if (bothMayLieOutside && (differing & Primitive::Undefined))
        return { };
    return buildOperatorForStrings(walker, Opcode::StrictEq, leftUnit, rightUnit, origin);
}

// ---- The length asked for zero

// The operand is the length of a string, and the comparison alone reads it. Null: it is not.
static Reference stringWhoseLengthIsAskedForStringComparisons(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (ValueID name = operand; ; name = index.value(name).operand(0)) {
        if (!index.hasOneUse(name))
            return { };
        if (index.isParameter(name) || !opcodesThatStateTypes.contains(index.opcodeOf(name)))
            break;
    }
    Reference length = rootForStrings(walker, walker.referenceOf(operand));
    if (!length.isValue() || walker.code().at(length.id()).opcode != Opcode::GetStringLength)
        return { };
    Reference string = referenceToRecordForStrings(walker, walker.code().at(length.id()).stringOperand());
    if (!isProvenString(walker, walker.typeOf(string)) || walker.representationOf(string) != Representation::Cell)
        return { };
    return string;
}

static Reference buildTestForEmptyStringForStringComparisons(Walker& walker, Reference string, bool passesForEmptyString, Origin origin)
{
    Reference isNotEmpty = buildOperatorForStrings(walker, Opcode::ToBoolean, string, { }, origin);
    return passesForEmptyString && isNotEmpty ? buildNegationForStrings(walker, isNotEmpty, origin) : isNotEmpty;
}

static std::optional<int64_t> integerKnownForStringComparisons(const Walker& walker, Reference reference)
{
    Type type = walker.types().at(walker.typeOf(reference));
    if (!type.isInteger() || !type.integers.isSingleValue())
        return std::nullopt;
    return type.integers.minimum;
}

static Reference askLengthForZeroForStringComparisons(Walker& walker, const BodyValue& value, Origin origin)
{
    bool isEquality = allEqualities.contains(value.opcode());
    for (uint32_t side = 0; side < 2; ++side) {
        std::optional<int64_t> bound = integerKnownForStringComparisons(walker, walker.referenceOf(value.operand(1 - side)));
        if (!bound)
            continue;
        auto holdsForLength = [&](int64_t length) {
            if (isEquality)
                return length == *bound;
            return relationHoldsBetweenForStringComparisons(value.opcode(), side ? *bound : length, side ? length : *bound);
        };
        if (isEquality ? !!*bound : holdsForLength(0) == holdsForLength(1))
            continue;
        if (Reference string = stringWhoseLengthIsAskedForStringComparisons(walker, value.operand(side)))
            return buildTestForEmptyStringForStringComparisons(walker, string, holdsForLength(0), origin);
    }
    return { };
}

// ---- A subject beside a written text

// Null: nothing was built.
static Reference comparePieceWithTextForStringComparisons(Walker& walker, const PieceOfString& piece, StringID text, Origin origin)
{
    unsigned length = lengthOfTextForStrings(walker, text);
    if (length > maxLengthOfTextComparedInPlace)
        return { };
    bool isInOrder = isRangeOfPieceInOrder(walker, piece);
    if (!length)
        return buildOperatorForStrings(walker, isInOrder ? Opcode::StrictEq : Opcode::LessEq, piece.end, piece.start, origin);
    if (isInOrder) {
        if (Reference answer = buildComparisonOfRangeWithText(walker, piece, text, origin))
            return answer;
    }

    return buildComparisonOfPieceAtEndOfStringWithText(walker, piece, text, origin);
}

// The record is a comparison of the subject with some written text.
static bool comparesSubjectWithTextForStringComparisons(const Walker& walker, const Operation& record, Reference rootOfSubject)
{
    auto isSubject = [&](ValueID operand) {
        return rootOfStringForStrings(walker, referenceToRecordForStrings(walker, operand)) == rootOfSubject;
    };
    switch (record.opcode) {
    case Opcode::StringEqualsLiteral:
        return isSubject(record.stringOperand());
    case Opcode::ValueEqualsLiteral:
        return isSubject(record.valueOperand());
    case Opcode::StrictEq:
    case Opcode::Eq:
        for (uint32_t side = 0; side < 2; ++side) {
            if (isSubject(record.operand(side)) && textKnownForStrings(walker, referenceToRecordForStrings(walker, record.operand(1 - side))))
                return true;
        }
        return false;
    default:
        return false;
    }
}

// `if (s === "get") .. else if (s === "put") .. else if (s === "post") ..`: from the second comparison on the length is read once, -1 where the subject is no string, and a text of another length costs one comparison of integers.
// `e.kind === "num" .. e.kind === "add"`: behind a text of the same length the length tells nothing apart, and the comparison stays the one of two strings.
// Null: no comparison of the subject has led here, or none that the length goes on from.
static Reference lengthSharedByComparisonsForStringComparisons(Walker& walker, const BodyValue& value, Reference subject, StringID text, Origin origin)
{
    std::optional<TestThatLedHere> led = testThatLedToBlockOf(walker, value);
    Reference earlier = led ? walker.referenceBoundTo(led->test) : Reference();
    if (!earlier.isValue())
        return { };
    const Operation& record = walker.code().at(earlier.id());
    if (!comparesSubjectWithTextForStringComparisons(walker, record, rootOfStringForStrings(walker, subject)))
        return { };
    if (record.opcode == Opcode::StringEqualsLiteral || record.opcode == Opcode::ValueEqualsLiteral) {
        if (ValueID length = record.lengthOperand())
            return referenceToRecordForStrings(walker, length);
    }
    StringID earlierText = record.opcode == Opcode::StringEqualsLiteral || record.opcode == Opcode::ValueEqualsLiteral ? record.literal() : StringID();
    for (uint32_t side = 0; side < 2 && !earlierText; ++side)
        earlierText = textKnownForStrings(walker, referenceToRecordForStrings(walker, record.operand(side)));
    if (earlierText && lengthOfTextForStrings(walker, earlierText) == lengthOfTextForStrings(walker, text))
        return { };

    if (isProvenString(walker, walker.typeOf(subject)))
        return buildLengthOfString(walker, subject, origin);
    Reference isString = buildTypeTestForStrings(walker, subject, TypeTable::string, origin);
    std::array<Reference, 1> length;
    bool isBuilt = buildArmsForStrings(walker, isString, BlockFrequency::Normal, [&](std::span<Reference> results) {
        results[0] = buildLengthOfString(walker, buildProofBehindTestForStrings(walker, subject, isString, OperandIndex::TypeTest::value, origin), origin);
    }, [&](std::span<Reference> results) {
        results[0] = integerForStrings(walker, -1);
    }, length);
    return isBuilt ? length[0] : Reference();
}

// True: the comparison is built and bound, or the walk stands nowhere.
static bool compareSubjectWithTextForStringComparisons(Walker& walker, const BodyValue& value, uint32_t placeOfSubject, StringID text, std::optional<uint32_t> narrowed, Origin origin)
{
    TypeArena& types = walker.types();
    Reference subject = walker.referenceOf(value.operand(placeOfSubject));
    Type type = types.at(walker.typeOf(subject));
    bool narrowsNothing = narrowed && !*narrowed;

    if (narrowsNothing && isProvenString(walker, walker.typeOf(subject))) {
        if (std::optional<PieceOfString> piece = pieceInSight(walker, subject)) {
            if (Reference answer = comparePieceWithTextForStringComparisons(walker, *piece, text, origin)) {
                walker.bind(value.id(), answer);
                return true;
            }
            if (walker.isUnreachable())
                return true;
        }
    }

    // `anything == "abc"`: no number, boolean or BigInt equals a text that converts to NaN, so only an object is converted.
    if (value.opcode() == Opcode::Eq && narrowsNothing && type.hasObjects() && type.isJavaScriptValue() && !type.mayBeUnspecified()) {
        Vector<char16_t> buffer;
        if (!std::isnan(jsToNumber(types.viewOf(text, buffer))))
            return false;
        Draft comparison(Opcode::ValueLooselyEqualsLiteral, origin);
        walker.chooseConservativeFields(comparison);
        comparison.setValueOperand(subject);
        comparison.setLiteral(text);
        if (Reference answer = walker.build(WTF::move(comparison)))
            walker.bind(value.id(), answer);
        return true;
    }

    // The outcome narrows operand 0 of the operations below, which is the subject.
    if (!narrowed || (*narrowed & ~(placeOfSubject ? 0u : 1u)))
        return false;
    // A subject that is held as one of the program's written texts is compared as a word.
    if (!subject.isValue() || walker.representationOf(subject) == Representation::LiteralString || !type.isJavaScriptValue() || !(type.primitives & Primitive::AnyString))
        return false;
    bool isString = isProvenString(walker, walker.typeOf(subject));
    if (!isString && value.opcode() != Opcode::StrictEq)
        return false;
    if (!lengthOfTextForStrings(walker, text) || lengthOfTextForStrings(walker, text) > maxLengthOfTextComparedInPlace)
        return false;

    Reference flat = isString ? flatStringInSight(walker, subject) : Reference();
    Reference length = walker.isInLoop() ? lengthSharedByComparisonsForStringComparisons(walker, value, subject, text, origin) : Reference();
    if (walker.isUnreachable())
        return true;
    if (!flat && !length)
        return false;
    if (length && !types.at(walker.typeOf(length)).integers.contains(lengthOfTextForStrings(walker, text))) {
        walker.bind(value.id(), booleanForStringComparisons(walker, false));
        return true;
    }

    Draft comparison(isString ? Opcode::StringEqualsLiteral : Opcode::ValueEqualsLiteral, origin);
    walker.chooseConservativeFields(comparison);
    comparison.setOperand(0, subject);
    if (length)
        comparison.setLengthOperand(length);
    if (flat)
        comparison.setFlatOperand(flat);
    comparison.setLiteral(text);
    if (Reference answer = walker.build(WTF::move(comparison)))
        walker.bind(value.id(), answer);
    return true;
}

// ---- The doors

bool buildEqualityOnStrings(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (inputs.areOnlyTypes() || walker.isUnreachable() || (opcode != Opcode::StrictEq && opcode != Opcode::Eq))
        return false;
    Reference left = walker.referenceOf(value.operand(0));
    Reference right = walker.referenceOf(value.operand(1));
    if (!left || !right || !left.isRecord() || !right.isRecord())
        return false;
    Origin origin = walker.originOf(value);
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTestForStrings(walker, value.id());

    if (narrowed && !*narrowed) {
        Reference answer = compareByCodeUnitForStringComparisons(walker, opcode, left, right, origin);
        if (!answer)
            answer = compareCharactersForStringComparisons(walker, left, right, origin);
        if (!answer)
            answer = askLengthForZeroForStringComparisons(walker, value, origin);
        if (answer) {
            walker.bind(value.id(), answer);
            return true;
        }
    }

    for (uint32_t side = 0; side < 2; ++side) {
        StringID text = textKnownForStrings(walker, side ? left : right);
        if (text && !textKnownForStrings(walker, side ? right : left))
            return compareSubjectWithTextForStringComparisons(walker, value, side, text, narrowed, origin);
    }
    return false;
}

// The order of the two strings that the test which led here has asked for. `isSwapped`: it was asked with the sides the other way round.
static Reference orderInSightForStringComparisons(Walker& walker, const BodyValue& value, Reference left, Reference right, bool& isSwapped)
{
    std::optional<TestThatLedHere> led = testThatLedToBlockOf(walker, value);
    Reference earlier = led ? walker.referenceBoundTo(led->test) : Reference();
    if (!earlier.isValue())
        return { };
    const Code& code = walker.code();
    const Operation& relation = code.at(earlier.id());
    if (!allRelations.contains(relation.opcode) || integerKnownForStringComparisons(walker, referenceToRecordForStrings(walker, relation.operand(1))) != 0)
        return { };
    Reference order = rootForStrings(walker, referenceToRecordForStrings(walker, relation.operand(0)));
    if (!order.isValue() || code.at(order.id()).opcode != Opcode::StringOrder)
        return { };
    Reference first = rootOfStringForStrings(walker, referenceToRecordForStrings(walker, code.at(order.id()).leftOperand()));
    Reference second = rootOfStringForStrings(walker, referenceToRecordForStrings(walker, code.at(order.id()).rightOperand()));
    Reference rootOfLeft = rootOfStringForStrings(walker, left);
    Reference rootOfRight = rootOfStringForStrings(walker, right);
    isSwapped = first == rootOfRight && second == rootOfLeft;
    return isSwapped || (first == rootOfLeft && second == rootOfRight) ? order : Reference();
}

// `a < b ? -1 : a > b ? 1 : 0`: a block that the Branch on this relation alone leads to holds a relation on the same two.
static bool secondRelationStandsBehindBranchForStringComparisons(Walker& walker, const BodyValue& value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<BodyUser> user = index.onlyUser(value.id());
    if (!user || user->kind != BodyUseKind::TerminatorOperand || index.terminator(user->block).kind() != TerminatorKind::Branch)
        return false;
    ValueID left = index.valueBehindCasts(value.operand(0));
    ValueID right = index.valueBehindCasts(value.operand(1));
    for (BodyTarget target : index.terminator(user->block).targets()) {
        if (index.numberOfIncomingEdges(target.block()) != 1)
            continue;
        BodyBlock block = index.block(target.block());
        for (uint32_t place = block.firstValue().index(); place < block.endOfValues().index(); ++place) {
            ValueID id(place);
            if (index.isParameter(id) || !allRelations.contains(index.opcodeOf(id)))
                continue;
            ValueID first = index.valueBehindCasts(index.value(id).operand(0));
            ValueID second = index.valueBehindCasts(index.value(id).operand(1));
            if ((first == left && second == right) || (first == right && second == left))
                return true;
        }
    }
    return false;
}

bool buildRelationOnStrings(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTestForStrings(walker, value.id());
    if (!narrowed || *narrowed)
        return false;
    Reference left = walker.referenceOf(value.operand(0));
    Reference right = walker.referenceOf(value.operand(1));
    if (!left || !right || !left.isRecord() || !right.isRecord())
        return false;
    Origin origin = walker.originOf(value);
    Reference zero = integerForStrings(walker, 0);

    Reference answer = askLengthForZeroForStringComparisons(walker, value, origin);
    if (!answer)
        answer = compareByCodeUnitForStringComparisons(walker, opcode, left, right, origin);
    if (answer) {
        walker.bind(value.id(), answer);
        return true;
    }
    if (!isProvenString(walker, walker.typeOf(left)) || !isProvenString(walker, walker.typeOf(right)))
        return false;

    // `s < "a"`: the first code unit and the length decide.
    for (uint32_t placeOfText = 0; placeOfText < 2 && walker.isInLoop(); ++placeOfText) {
        StringID text = textKnownForStrings(walker, placeOfText ? right : left);
        std::optional<char16_t> unit = text ? onlyCodeUnitOfText(walker, text) : std::nullopt;
        if (!unit || textKnownForStrings(walker, placeOfText ? left : right))
            continue;
        Draft order(Opcode::StringOrderToCharacter, origin);
        order.setStringOperand(placeOfText ? left : right);
        order.setCodeUnit(*unit);
        if (Reference built = walker.build(WTF::move(order)))
            walker.bind(value.id(), buildOperatorForStrings(walker, placeOfText ? opcode : relationWithSidesSwappedForStrings(opcode), built, zero, origin));
        return true;
    }

    bool isSwapped = false;
    Reference order = orderInSightForStringComparisons(walker, value, left, right, isSwapped);
    if (!order) {
        if (!secondRelationStandsBehindBranchForStringComparisons(walker, value))
            return false;
        Draft comparison(Opcode::StringOrder, origin);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        order = walker.build(WTF::move(comparison));
        if (!order)
            return true;
    }
    walker.bind(value.id(), buildOperatorForStrings(walker, isSwapped ? relationWithSidesSwappedForStrings(opcode) : opcode, order, zero, origin));
    return true;
}

// `if (s.length)`
bool buildTruthinessOfLengthOfString(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTestForStrings(walker, value.id());
    if (!narrowed || *narrowed)
        return false;
    Reference string = stringWhoseLengthIsAskedForStringComparisons(walker, value.operand(0));
    if (!string)
        return false;
    if (Reference answer = buildTestForEmptyStringForStringComparisons(walker, string, false, walker.originOf(value)))
        walker.bind(value.id(), answer);
    return true;
}

// `typeof ch === "string"`, a test for undefined: it tells every string from undefined, which is all that a character can be.
bool buildTestOfCharacterByCodeUnit(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTestForStrings(walker, value.id());
    if (!narrowed || *narrowed)
        return false;
    Type tested = walker.types().at(value.testedType());
    if (!tested.isJavaScriptValue())
        return false;
    bool passesForUndefined = tested.primitives & Primitive::Undefined;
    bool tellsStringsFromUndefined = passesForUndefined ? !(tested.primitives & Primitive::AnyString) : !!(tested.primitives & Primitive::String);
    if (!tellsStringsFromUndefined)
        return false;
    Reference answer = testCharacterForUndefinedForStringComparisons(walker, walker.referenceOf(value.operand(0)), passesForUndefined, walker.originOf(value));
    if (!answer)
        return false;
    walker.bind(value.id(), answer);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
