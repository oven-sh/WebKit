/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    return walker.constant(PrimitiveKnownToFolding::boolean(value));
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

static std::optional<int64_t> onlyPairOfTextForStringComparisons(Walker& walker, StringID text)
{
    Vector<char16_t> buffer;
    StringView view = walker.types().viewOf(text, buffer);
    if (view.length() != 2 || !U16_IS_LEAD(view[0]) || !U16_IS_TRAIL(view[1]))
        return std::nullopt;
    return static_cast<int64_t>(U16_GET_SUPPLEMENTARY(view[0], view[1]));
}

static Reference compareCharacterWithTextForStringComparisons(Walker& walker, Opcode opcode, Reference character, Reference codeUnit, StringID text, bool characterIsOnLeft, Origin origin)
{
    bool mayBeUndefined = walker.types().at(walker.typeOf(rootForStrings(walker, character))).primitives & Primitive::Undefined;
    std::optional<char16_t> unit = onlyCodeUnitOfText(walker, text);
    if (allEqualities.contains(opcode)) {
        if (unit)
            return buildOperatorForStrings(walker, Opcode::StrictEq, codeUnit, integerForStrings(walker, *unit), origin);
        if (std::optional<int64_t> pair = onlyPairOfTextForStringComparisons(walker, text))
            return buildOperatorForStrings(walker, Opcode::StrictEq, codeUnit, integerForStrings(walker, *pair), origin);
        if (!lengthOfTextForStrings(walker, text) && !mayBeUndefined)
            return buildOperatorForStrings(walker, Opcode::StrictEq, codeUnit, integerForStrings(walker, -1), origin);
        return booleanForStringComparisons(walker, false);
    }
    if (!unit)
        return { };
    if (*unit >= 0xD800 && walker.types().at(walker.typeOf(codeUnit)).integers.maximum > 0xFFFF)
        return { };
    bool holdsOfMinusOne = asksForGreaterOnLeftForStringComparisons(opcode) != characterIsOnLeft;
    if (holdsOfMinusOne && mayBeUndefined)
        codeUnit = buildOperatorForStrings(walker, Opcode::URShift, codeUnit, integerForStrings(walker, 0), origin);
    return compareInOrderForStringComparisons(walker, opcode, codeUnit, integerForStrings(walker, *unit), characterIsOnLeft, origin);
}

static ALWAYS_INLINE Reference compareByCodeUnitForStringComparisons(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    for (uint32_t side = 0; side < 2; ++side) {
        Reference subject = side ? right : left;
        Reference other = side ? left : right;
        if (Reference codeUnit = codeUnitOfCharCodeInSight(walker, subject)) {
            if (Reference answer = compareCharCodeForStringComparisons(walker, opcode, codeUnit, other, !side, origin))
                return answer;
            continue;
        }
        Reference codeUnit = codePointOfCharacterInSight(walker, subject);
        if (!codeUnit)
            continue;
        PrimitiveKnownToFolding known = primitiveKnownForStrings(walker, other);
        if (!known.isText())
            continue;
        if (Reference answer = compareCharacterWithTextForStringComparisons(walker, opcode, subject, codeUnit, known.asText(), !side, origin))
            return answer;
    }
    return { };
}

static Reference compareCharactersForStringComparisons(Walker& walker, Reference left, Reference right, Origin origin)
{
    Reference leftUnit = codePointOfCharacterInSight(walker, left);
    Reference rightUnit = leftUnit ? codePointOfCharacterInSight(walker, right) : Reference();
    if (!rightUnit)
        return { };
    TypeArena& types = walker.types();
    bool bothMayLieOutside = types.at(walker.typeOf(leftUnit)).integers.minimum < 0 && types.at(walker.typeOf(rightUnit)).integers.minimum < 0;
    uint16_t differing = types.at(walker.typeOf(rootForStrings(walker, left))).primitives ^ types.at(walker.typeOf(rootForStrings(walker, right))).primitives;
    if (bothMayLieOutside && (differing & Primitive::Undefined))
        return { };
    return buildOperatorForStrings(walker, Opcode::StrictEq, leftUnit, rightUnit, origin);
}

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

static ALWAYS_INLINE std::optional<int64_t> integerKnownForStringComparisons(const Walker& walker, Reference reference)
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
    return { };
}

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

namespace {

struct SubjectForStringComparisons {
    ValueID value;
    PropertyKeyID key;

    explicit operator bool() const { return !!value; }
    friend bool operator==(const SubjectForStringComparisons&, const SubjectForStringComparisons&) = default;
};

} // anonymous namespace

static ValueID valueBeforeNarrowings(const BodyIndex& index, ValueID value)
{
    for (value = index.stripCasts(value); value && index.isParameter(value);) {
        ValueID narrowed = index.valueNarrowedInto(value);
        if (!narrowed)
            break;
        value = index.stripCasts(narrowed);
    }
    return value;
}

static SubjectForStringComparisons findSubjectComparedWithText(const BodyIndex& index, ValueID test)
{
    if (!test || index.isParameter(test) || (index.opcodeOf(test) != Opcode::StrictEq && index.opcodeOf(test) != Opcode::Eq))
        return { };
    BodyValue comparison = index.value(test);
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID written = index.stripCasts(comparison.operand(1 - side));
        if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::String)
            continue;
        ValueID subject = valueBeforeNarrowings(index, comparison.operand(side));
        if (subject && !index.isParameter(subject) && index.opcodeOf(subject) == Opcode::PropGet && !index.value(subject).isFreeName())
            return { valueBeforeNarrowings(index, index.value(subject).objectOperand()), index.value(subject).key() };
        return { subject, { } };
    }
    return { };
}

static ValueID branchConditionTest(const BodyIndex& index, BlockID block)
{
    BodyTerminator terminator = index.terminator(block);
    if (terminator.kind() != TerminatorKind::Branch)
        return { };
    ValueID test = terminator.operand(0);
    while (test && !index.isParameter(test) && (index.opcodeOf(test) == Opcode::LogicalNot || index.opcodeOf(test) == Opcode::ToBoolean))
        test = index.value(test).operand(0);
    return test;
}

static bool isInComparisonChain(const BodyIndex& index, ValueID comparison)
{
    static constexpr unsigned minComparisons = 3;
    BlockID block = index.blockOf(comparison);
    SubjectForStringComparisons subject = findSubjectComparedWithText(index, comparison);
    if (!subject || branchConditionTest(index, block) != comparison)
        return false;
    unsigned comparisons = 1;
    for (BlockID behind = block; comparisons < minComparisons;) {
        std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(behind);
        if (!edge || !(findSubjectComparedWithText(index, branchConditionTest(index, edge->from)) == subject))
            break;
        behind = edge->from;
        ++comparisons;
    }
    for (BlockID inFront = block; inFront && comparisons < minComparisons;) {
        BlockID next;
        for (BodyTarget target : index.terminator(inFront).targets()) {
            std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(target.block());
            if (edge && edge->from == inFront && findSubjectComparedWithText(index, branchConditionTest(index, target.block())) == subject)
                next = target.block();
        }
        comparisons += !!next;
        inFront = next;
    }
    return comparisons >= minComparisons;
}

static bool compareSubjectWithTextForStringComparisons(Walker& walker, const BodyValue& value, uint32_t placeOfSubject, StringID text, std::optional<uint32_t> narrowed, Origin origin)
{
    TypeArena& types = walker.types();
    Reference subject = walker.referenceOf(value.operand(placeOfSubject));
    Type type = types.at(walker.typeOf(subject));
    bool narrowsNothing = narrowed && !*narrowed;

    bool subjectIsString = isProvenString(walker, walker.typeOf(subject));
    if (narrowsNothing && (subjectIsString || (value.opcode() == Opcode::StrictEq && lengthOfTextForStrings(walker, text)))) {
        std::optional<PieceOfString> piece = pieceReadInLoop(walker, subject, origin);
        if (!piece)
            piece = pieceInSight(walker, subject, !subjectIsString);
        if (piece) {
            if (Reference answer = comparePieceWithTextForStringComparisons(walker, *piece, text, origin)) {
                walker.bind(value.id(), answer);
                return true;
            }
            if (walker.isUnreachable())
                return true;
        }
    }

    if (!narrowed || (*narrowed & ~(placeOfSubject ? 0u : 1u)))
        return false;
    if (!subject.isValue() || !type.isJavaScriptValue() || !(type.primitives & Primitive::AnyString))
        return false;
    bool isString = isProvenString(walker, walker.typeOf(subject));
    if (!isString && value.opcode() != Opcode::StrictEq)
        return false;
    if (!lengthOfTextForStrings(walker, text) || lengthOfTextForStrings(walker, text) > maxLengthOfTextComparedInPlace)
        return false;

    Reference flat = isString ? flatStringInSight(walker, subject) : Reference();
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    bool isAllocatedInIteration = loop && !walker.standsInFrontOfOpenLoop(*loop, subject);
    bool sharesLength = walker.isInLoop() && !(isAllocatedInIteration && isInComparisonChain(walker.currentBodyIndex(), value.id()));
    Reference length = sharesLength ? lengthSharedByComparisonsForStringComparisons(walker, value, subject, text, origin) : Reference();
    if (walker.isUnreachable())
        return true;
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
    if (!flat && !length) {
        if (!(walker.comparisonWithTextLeavesLoop(comparison)))
            return false;
        walker.count(StatisticID::StringLiteralComparisonsHoisted);
        comparison.setHasLeftLoop(true);
    }
    if (Reference answer = walker.build(WTF::move(comparison)))
        walker.bind(value.id(), answer);
    return true;
}

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

static bool secondRelationStandsBehindBranchForStringComparisons(Walker& walker, const BodyValue& value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<BodyUser> user = index.onlyUser(value.id());
    if (!user || user->kind != BodyUseKind::TerminatorOperand || index.terminator(user->block).kind() != TerminatorKind::Branch)
        return false;
    ValueID left = index.stripCasts(value.operand(0));
    ValueID right = index.stripCasts(value.operand(1));
    for (BodyTarget target : index.terminator(user->block).targets()) {
        if (index.numberOfIncomingEdges(target.block()) != 1)
            continue;
        BodyBlock block = index.block(target.block());
        for (uint32_t place = block.firstValue().index(); place < block.endOfValues().index(); ++place) {
            ValueID id(place);
            if (index.isParameter(id) || !allRelations.contains(index.opcodeOf(id)))
                continue;
            ValueID first = index.stripCasts(index.value(id).operand(0));
            ValueID second = index.stripCasts(index.value(id).operand(1));
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

    for (uint32_t placeOfText = 0; placeOfText < 2 && walker.isInLoop(); ++placeOfText) {
        StringID text = textKnownForStrings(walker, placeOfText ? right : left);
        std::optional<char16_t> unit = text ? onlyCodeUnitOfText(walker, text) : std::nullopt;
        if (!unit || textKnownForStrings(walker, placeOfText ? left : right))
            continue;
        Draft order(Opcode::StringOrderToCharacter, origin);
        order.setStringOperand(placeOfText ? left : right);
        order.setCodeUnit(*unit);
        if (Reference built = walker.build(WTF::move(order)))
            walker.bind(value.id(), buildOperatorForStrings(walker, placeOfText ? opcode : relationWithOperandsSwappedForStrings(opcode), built, zero, origin));
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
    walker.bind(value.id(), buildOperatorForStrings(walker, isSwapped ? relationWithOperandsSwappedForStrings(opcode) : opcode, order, zero, origin));
    return true;
}

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
