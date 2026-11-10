/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTStringTable.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include "AOTWalkResultsInParts.h"
#include "AOTWalkStringFolds.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "MathCommon.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <optional>
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxBytesOfTextMadeByFolding = mostBytesOfTextMadeByStringFolds;

static constexpr OpcodeSet testsThatTypesDecideInFolding = opcodesWhere([](Opcode opcode) {
    Proof proof = provesOf(opcode);
    bool narrowsType = proof == Proof::TestedType || proof == Proof::ExactFunction || proof == Proof::Truthiness || proof == Proof::Equality || proof == Proof::IntegerBound;
    return narrowsType && hasRuleForFolding(opcode);
});
static_assert(testsThatTypesDecideInFolding == OpcodeSet { Opcode::TypeTest, Opcode::IsClosureOf, Opcode::ToBoolean, Opcode::Eq, Opcode::StrictEq, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq });

static constexpr OpcodeSet readsOfFolding { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet, Opcode::ElemGet };
static constexpr OpcodeSet storesThatStateFollowsForFolding { Opcode::PutModuleVariable, Opcode::PutEnvironmentField };
static constexpr OpcodeSet placesThatStateFollowsForFolding = storesThatStateFollowsForFolding | OpcodeSet { Opcode::GetModuleVariable, Opcode::GetEnvironmentField };
static constexpr OpcodeSet conversionsToObjectsInFolding { Opcode::ToObject, Opcode::ToThis };
static constexpr OpcodeSet opcodesOfFolding = operationsOnPrimitivesInFolding | testsThatTypesDecideInFolding | castsOfFolding | readsOfFolding | storesThatStateFollowsForFolding | passThroughChecks
    | conversionsToObjectsInFolding | OpcodeSet { Opcode::TypeOf, Opcode::ToLength };

static ALWAYS_INLINE PrimitiveKnownToFolding onlyValueOfTypeInFolding(const TypeArena& types, TypeID id)
{
    if (!id)
        return { };
    if (holdsOnlyPrimitives(id) ? !std::has_single_bit(primitivesInID(id)) || (isIntegerType(id) && !integersInID(id).isSingleValue()) : types.recordOf(id).hasObjects())
        return { };
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };

    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

static ALWAYS_INLINE PrimitiveKnownToFolding primitiveHeldInFolding(Walker& walker, Reference reference)
{
    if (!reference)
        return { };
    if (reference.isRecord()) {
        const Operation& record = walker.code().at(reference.id());
        if (record.opcode == Opcode::Double)
            return PrimitiveKnownToFolding::number(record.number());
        if (record.opcode == Opcode::String)
            return PrimitiveKnownToFolding::text(record.string());
    }
    return onlyValueOfTypeInFolding(walker.types(), walker.typeOf(reference));
}

Reference Walker::constant(PrimitiveKnownToFolding primitive)
{
    Draft constant(Opcode::Undefined);
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        break;
    case PrimitiveKnownToFolding::Kind::Null:
        constant.become(Opcode::Null);
        break;
    case PrimitiveKnownToFolding::Kind::Boolean:
        constant.become(Opcode::Boolean);
        constant.setBoolean(primitive.asBoolean());
        break;
    case PrimitiveKnownToFolding::Kind::Number:
        if (isTrackedInteger(primitive.asNumber())) {
            constant.become(Opcode::Integer);
            constant.setInteger(static_cast<int64_t>(primitive.asNumber()));
        } else {
            constant.become(Opcode::Double);
            constant.setNumber(primitive.asNumber());
        }
        break;
    case PrimitiveKnownToFolding::Kind::Text:
        constant.become(Opcode::String);
        constant.setString(primitive.asText());
        break;
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    return this->constant(WTF::move(constant));
}

static std::optional<double> toNumberInFolding(PrimitiveKnownToFolding primitive)
{
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return PNaN;
    case PrimitiveKnownToFolding::Kind::Null:
        return 0.0;
    case PrimitiveKnownToFolding::Kind::Boolean:
        return primitive.asBoolean() ? 1.0 : 0.0;
    case PrimitiveKnownToFolding::Kind::Number:
        return primitive.asNumber();
    case PrimitiveKnownToFolding::Kind::Unknown:
    case PrimitiveKnownToFolding::Kind::Text:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isTruthyInFolding(TypeReader texts, PrimitiveKnownToFolding primitive)
{
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
    case PrimitiveKnownToFolding::Kind::Null:
        return false;
    case PrimitiveKnownToFolding::Kind::Boolean:
        return primitive.asBoolean();
    case PrimitiveKnownToFolding::Kind::Number:
        return primitive.asNumber() && !std::isnan(primitive.asNumber());
    case PrimitiveKnownToFolding::Kind::Text:
        return !texts.bytesOf(primitive.asText()).empty();
    case PrimitiveKnownToFolding::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PrimitiveKnownToFolding numberComputedInFolding(double result, double left, double right)
{
    if (std::isnan(result) && ((!std::isnan(left) && !std::isnan(right)) || std::signbit(result)))
        return { };
    return PrimitiveKnownToFolding::number(result);
}

static PrimitiveKnownToFolding powerInFolding(double base, double exponent)
{
    if (std::isnan(exponent) || (std::abs(base) == 1 && std::isinf(exponent)))
        return PrimitiveKnownToFolding::number(PNaN);
    if (!(exponent >= 0 && exponent <= maxExponentForIntegerMathPow) || exponent != std::trunc(exponent))
        return { };
    return numberComputedInFolding(operationMathPow(base, exponent), base, exponent);
}

static PrimitiveKnownToFolding binaryOperatorInFolding(Opcode opcode, double left, double right)
{
    switch (opcode) {
    case Opcode::Add:
        return numberComputedInFolding(left + right, left, right);
    case Opcode::Sub:
        return numberComputedInFolding(left - right, left, right);
    case Opcode::Mul:
        return numberComputedInFolding(left * right, left, right);
    case Opcode::Div:
        return numberComputedInFolding(left / right, left, right);
    case Opcode::Mod:
        return numberComputedInFolding(std::fmod(left, right), left, right);
    case Opcode::Pow:
        return powerInFolding(left, right);
    case Opcode::BitAnd:
        return PrimitiveKnownToFolding::number(toInt32(left) & toInt32(right));
    case Opcode::BitOr:
        return PrimitiveKnownToFolding::number(toInt32(left) | toInt32(right));
    case Opcode::BitXor:
        return PrimitiveKnownToFolding::number(toInt32(left) ^ toInt32(right));
    case Opcode::LShift:
        return PrimitiveKnownToFolding::number(static_cast<int32_t>(toUInt32(left) << (toUInt32(right) & 31)));
    case Opcode::RShift:
        return PrimitiveKnownToFolding::number(toInt32(left) >> (toUInt32(right) & 31));
    case Opcode::URShift:
        return PrimitiveKnownToFolding::number(toUInt32(left) >> (toUInt32(right) & 31));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static PrimitiveKnownToFolding unaryOperatorInFolding(Opcode opcode, double operand)
{
    switch (opcode) {
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return PrimitiveKnownToFolding::number(operand);
    case Opcode::Negate:
        return numberComputedInFolding(-operand, operand, operand);
    case Opcode::Increment:
        return numberComputedInFolding(operand + 1, operand, operand);
    case Opcode::Decrement:
        return numberComputedInFolding(operand - 1, operand, operand);
    case Opcode::BitNot:
        return PrimitiveKnownToFolding::number(~toInt32(operand));
    case Opcode::ToInt32:
        return PrimitiveKnownToFolding::number(toInt32(operand));
    case Opcode::ToUint32:
        return PrimitiveKnownToFolding::number(toUInt32(operand));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool relationHoldsInFolding(Opcode relation, double left, double right)
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

static PrimitiveKnownToFolding relationInFolding(TypeReader texts, Opcode relation, PrimitiveKnownToFolding left, PrimitiveKnownToFolding right)
{
    if (left.isText() && right.isText()) {
        std::strong_ordering order = compareProgramStrings(texts.bytesOf(left.asText()), texts.bytesOf(right.asText()));
        return PrimitiveKnownToFolding::boolean(relationHoldsInFolding(relation, order < 0 ? -1 : order > 0 ? 1 : 0, 0));
    }
    std::optional<double> leftNumber = toNumberInFolding(left);
    std::optional<double> rightNumber = toNumberInFolding(right);
    if (!leftNumber || !rightNumber)
        return { };
    return PrimitiveKnownToFolding::boolean(relationHoldsInFolding(relation, *leftNumber, *rightNumber));
}

static bool spellSameTextInFolding(TypeReader texts, StringID a, StringID b)
{
    return a == b || (liesInThread(a) != liesInThread(b) && std::ranges::equal(texts.bytesOf(a), texts.bytesOf(b)));
}

static PrimitiveKnownToFolding equalityInFolding(TypeReader texts, PrimitiveKnownToFolding left, PrimitiveKnownToFolding right, bool isStrict)
{
    if (left.isNullish() || right.isNullish())
        return PrimitiveKnownToFolding::boolean(isStrict ? left.kind() == right.kind() : left.isNullish() && right.isNullish());
    if (left.isText() && right.isText())
        return PrimitiveKnownToFolding::boolean(spellSameTextInFolding(texts, left.asText(), right.asText()));
    if (left.isText() || right.isText())
        return isStrict ? PrimitiveKnownToFolding::boolean(false) : PrimitiveKnownToFolding();
    if (isStrict && left.kind() != right.kind())
        return PrimitiveKnownToFolding::boolean(false);
    return PrimitiveKnownToFolding::boolean(*toNumberInFolding(left) == *toNumberInFolding(right));
}

static StringID textInEveryProgramForFolding(const StringTable& strings, PrimitiveKnownToFolding primitive)
{
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return strings.textOfUndefined();
    case PrimitiveKnownToFolding::Kind::Null:
        return strings.textOfNull();
    case PrimitiveKnownToFolding::Kind::Boolean:
        return strings.textOf(primitive.asBoolean());
    case PrimitiveKnownToFolding::Kind::Text:
        return primitive.asText();
    case PrimitiveKnownToFolding::Kind::Number:
        return { };
    case PrimitiveKnownToFolding::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PrimitiveKnownToFolding joinTextsInFolding(const Program& program, TypeReader texts, TypeArena* maker, std::span<const PrimitiveKnownToFolding> parts)
{
    struct Part {
        StringID text;
        double number { 0 };
    };
    Vector<Part, 2> partsWithBytes;
    size_t numberOfBytes = 0;
    bool hasNarrowPart = false;
    bool hasWidePart = false;
    for (PrimitiveKnownToFolding part : parts) {
        StringID text = textInEveryProgramForFolding(program.strings, part);
        bool isMade = liesInThread(text);
        RELEASE_ASSERT(!isMade || maker);
        size_t numberOfBytesOfPart = isMade ? maker->numberOfBytesOf(text) : text ? texts.bytesOf(text).size() : String::number(part.asNumber()).length();
        numberOfBytes += numberOfBytesOfPart;
        if (numberOfBytes > maxBytesOfTextMadeByFolding)
            return { };
        if (!numberOfBytesOfPart)
            continue;
        partsWithBytes.append(Part { text, text ? 0 : part.asNumber() });
        bool isWide = isMade ? maker->isWideText(text) : text && isWideProgramString(texts.bytesOf(text));
        (isWide ? hasWidePart : hasNarrowPart) = true;
    }

    if (partsWithBytes.isEmpty())
        return PrimitiveKnownToFolding::text(program.strings.emptyString());
    if (partsWithBytes.size() == 1 && partsWithBytes[0].text)
        return PrimitiveKnownToFolding::text(partsWithBytes[0].text);
    if ((hasNarrowPart && hasWidePart) || !maker)
        return { };
    StringID joined;
    for (const Part& part : partsWithBytes) {
        StringID text = part.text ? part.text : maker->internText(byteCast<uint8_t>(String::number(part.number).span8()));
        joined = joined ? maker->internJoinedTexts(joined, text) : text;
    }
    return PrimitiveKnownToFolding::text(joined);
}

static PrimitiveKnownToFolding evaluateInFolding(const Program& program, TypeReader texts, TypeArena* maker, Opcode opcode, PrimitiveKnownToFolding left, PrimitiveKnownToFolding right)
{
    switch (opcode) {
    case Opcode::ToBoolean:
        return PrimitiveKnownToFolding::boolean(isTruthyInFolding(texts, left));
    case Opcode::LogicalNot:
        return left.kind() == PrimitiveKnownToFolding::Kind::Boolean ? PrimitiveKnownToFolding::boolean(!left.asBoolean()) : PrimitiveKnownToFolding();
    case Opcode::StrictEq:
    case Opcode::Eq:
        return equalityInFolding(texts, left, right, opcode == Opcode::StrictEq);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return relationInFolding(texts, opcode, left, right);
    case Opcode::Add:
        if (left.isText() || right.isText())
            return joinTextsInFolding(program, texts, maker, std::array { left, right });
        [[fallthrough]];
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift: {
        std::optional<double> leftNumber = toNumberInFolding(left);
        std::optional<double> rightNumber = toNumberInFolding(right);
        if (!leftNumber || !rightNumber)
            return { };
        return binaryOperatorInFolding(opcode, *leftNumber, *rightNumber);
    }
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::Negate:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::BitNot:
    case Opcode::ToInt32:
    case Opcode::ToUint32: {
        std::optional<double> number = toNumberInFolding(left);
        return number ? unaryOperatorInFolding(opcode, *number) : PrimitiveKnownToFolding();
    }
    case Opcode::ToString:
    case Opcode::ToPropertyKey:
        return joinTextsInFolding(program, texts, maker, std::array { left });
    case Opcode::GetStringLength: {
        if (!left.isText())
            return { };
        Vector<char16_t> buffer;
        return PrimitiveKnownToFolding::number(viewOfProgramString(texts.bytesOf(left.asText()), buffer).length());
    }
    default:
        return { };
    }
}

static ALWAYS_INLINE Reference foldOverKnownPrimitivesInFolding(Walker& walker, const BodyValue& value)
{
    std::array<PrimitiveKnownToFolding, 2> operands;
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        ValueID operand = value.operand(operandIndex);
        if (!operand)
            break;
        operands[operandIndex] = primitiveHeldInFolding(walker, walker.referenceOf(operand));
        if (!operands[operandIndex])
            return { };
    }
    if (!operands[0])
        return { };
    return walker.constant(evaluateInFolding(walker.program(), walker.types(), &walker.types(), value.opcode(), operands[0], operands[1]));
}

static_assert(primitivesWrittenForFolding == OpcodeSet { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String });

static PrimitiveKnownToFolding primitiveWrittenByInFolding(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Opcode::Null:
        return PrimitiveKnownToFolding::null();
    case Opcode::Boolean:
        return PrimitiveKnownToFolding::boolean(value.as<Opcode::Boolean>().boolean());
    case Opcode::Integer:
        return PrimitiveKnownToFolding::integer(value.as<Opcode::Integer>().integer());
    case Opcode::Double:
        return PrimitiveKnownToFolding::number(value.as<Opcode::Double>().number());
    case Opcode::String:
        return PrimitiveKnownToFolding::text(value.as<Opcode::String>().string());
    default:
        return { };
    }
}

PrimitiveKnownToFolding primitiveMadeOfConstants(const Program& program, const BodyValue& value, NOESCAPE const ScopedLambda<PrimitiveKnownToFolding(ValueID)>& heldBy)
{
    Opcode opcode = value.opcode();
    if (castsOfFolding.contains(opcode))
        return value.operand(0) ? heldBy(value.operand(0)) : PrimitiveKnownToFolding();
    if (!operationsOnPrimitivesInFolding.contains(opcode))
        return primitiveWrittenByInFolding(value);

    std::array<PrimitiveKnownToFolding, 2> operands;
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        ValueID operand = value.operand(operandIndex);
        if (!operand)
            break;
        operands[operandIndex] = heldBy(operand);
        if (!operands[operandIndex])
            return { };
    }
    if (!operands[0])
        return { };
    return evaluateInFolding(program, program, nullptr, opcode, operands[0], operands[1]);
}

std::optional<uint32_t> onlyTargetWith(TypeReader texts, const BodyTerminator& terminator, PrimitiveKnownToFolding operand)
{
    if (!operand)
        return std::nullopt;
    switch (terminator.kind()) {
    case TerminatorKind::Branch:
        if (operand.kind() != PrimitiveKnownToFolding::Kind::Boolean)
            return std::nullopt;
        return operand.asBoolean() ? 0 : 1;
    case TerminatorKind::Switch: {
        uint32_t target = 0;
        for (int64_t written : terminator.as<TerminatorKind::Switch>().cases()) {
            PrimitiveKnownToFolding label = PrimitiveKnownToFolding::integer(written);
            if (!label)
                return std::nullopt;
            if (equalityInFolding(texts, operand, label, true).asBoolean())
                return target;
            ++target;
        }
        return target;
    }
    case TerminatorKind::SwitchString: {
        uint32_t target = 0;
        for (StringID written : terminator.as<TerminatorKind::SwitchString>().strings()) {
            if (equalityInFolding(texts, operand, PrimitiveKnownToFolding::text(written), true).asBoolean())
                return target;
            ++target;
        }
        return target;
    }
    default:
        return std::nullopt;
    }
}

std::optional<uint32_t> onlyTargetKnownToFolding(Walker& walker, const BodyTerminator& terminator)
{
    if (!goesByOneOperand(terminator.kind()))
        return std::nullopt;
    Reference operand = walker.referenceOf(terminator.operand(0));
    if (std::optional<uint32_t> decided = onlyTargetDecidedOnWay(walker, terminator))
        return decided;
    if (!operand || !isKnownForTerminatorOfFolding(walker, operand))
        return std::nullopt;
    return onlyTargetWith(walker.types(), terminator, primitiveHeldInFolding(walker, operand));
}

static TypingContext typingForFolding(Walker& walker)
{
    return { walker.program(), walker.types(), PositionTypes::knowingNothing() };
}

static ALWAYS_INLINE TypeID typeToJudgeByInFolding(Walker& walker, Reference reference)
{
    if (!reference)
        return { };
    TypeID type = walker.typeOf(reference);
    if (!type || isNeverType(type) || ((holdsOnlyPrimitives(type) ? primitivesInID(type) : walker.types().recordOf(type).primitives) & Primitive::Unspecified))
        return { };
    return type;
}

static bool areOneClassObjectInFolding(Walker& walker, TypeID left, TypeID right)
{
    if (left != right || !walker.plan().hasOneInstance())
        return false;
    Type type = walker.types().at(left);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
    return classLayout && walker.facts().isEvaluatedAtMostOnce(classLayout);
}

struct PartsJudgedInFolding {
    uint16_t primitives;
    bool hasObjects;
    bool isJavaScriptValue;
    Range integers;
};

static ALWAYS_INLINE PartsJudgedInFolding partsJudgedInFolding(const TypeArena& types, TypeID id)
{
    if (holdsOnlyPrimitives(id))
        return { primitivesInID(id), false, true, integersInID(id) };
    const Type& record = types.recordOf(id);
    return { record.primitives, record.hasObjects(), record.isJavaScriptValue(), record.integers };
}

static bool surelyOverlapInFolding(const PartsJudgedInFolding& a, const PartsJudgedInFolding& b)
{
    constexpr uint16_t primitivesKeptWhereBothHoldThem = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::WholeNumber | Primitive::BigInt | Primitive::Absent | Primitive::String | Primitive::Symbol;
    uint16_t shared = a.primitives & b.primitives;
    return (shared & primitivesKeptWhereBothHoldThem) || ((shared & Primitive::Integer) && !a.integers.intersectWith(b.integers).isEmpty());
}

static bool surelyLeavesSomethingInFolding(const PartsJudgedInFolding& value, const PartsJudgedInFolding& removed)
{
    if (value.hasObjects && !removed.hasObjects)
        return true;
    return (value.primitives & ~removed.primitives & ~primitivesWithLists) || ((value.primitives & Primitive::Integer) && !value.integers.isSubsetOf(removed.integers));
}

static bool isSurelySeveralValuesInFolding(const PartsJudgedInFolding& type)
{
    constexpr uint16_t primitivesThatMayBeOneValue = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Absent | Primitive::Integer | primitivesWithLists;
    if (type.hasObjects)
        return type.primitives;
    if (!std::has_single_bit(type.primitives) || !(type.primitives & primitivesThatMayBeOneValue))
        return true;
    return type.primitives == Primitive::Integer && !type.integers.isSingleValue();
}

static_assert(provesOf(Opcode::ToBoolean) == Proof::Truthiness && provesOf(Opcode::TypeTest) == Proof::TestedType && provesOf(Opcode::StrictEq) == Proof::Equality);
static_assert(provesOf(Opcode::Less) == Proof::IntegerBound && provesOf(Opcode::LessEq) == Proof::IntegerBound && provesOf(Opcode::Greater) == Proof::IntegerBound && provesOf(Opcode::GreaterEq) == Proof::IntegerBound);

static bool neitherSideOfTestIsRuledOutInFolding(const TypeArena& types, const BodyValue& test, std::span<const TypeID> typesOfOperands)
{
    if (typesOfOperands.empty())
        return false;
    PartsJudgedInFolding first = partsJudgedInFolding(types, typesOfOperands[0]);
    if (!first.isJavaScriptValue)
        return false;
    Opcode opcode = test.opcode();
    if (opcode == Opcode::ToBoolean) {
        constexpr uint16_t primitivesThatMayAllBeFalsy = Primitive::UndefinedOrNull | Primitive::False | Primitive::Integer | Primitive::LiteralString;
        constexpr uint16_t primitivesThatMayAllBeTruthy = Primitive::True | Primitive::FractionalNumber | Primitive::AnySymbol | Primitive::Integer | Primitive::LiteralString;
        return (first.hasObjects || (first.primitives & ~primitivesThatMayAllBeFalsy)) && (first.primitives & ~primitivesThatMayAllBeTruthy);
    }
    if (opcode == Opcode::TypeTest) {
        TypeID tested = test.testedType();
        if (!tested || !holdsOnlyPrimitives(tested))
            return false;
        PartsJudgedInFolding asked = partsJudgedInFolding(types, tested);
        return surelyOverlapInFolding(first, asked) && surelyLeavesSomethingInFolding(first, asked);
    }
    if (typesOfOperands.size() != 2)
        return false;
    PartsJudgedInFolding second = partsJudgedInFolding(types, typesOfOperands[1]);
    if (!second.isJavaScriptValue)
        return false;
    if (opcode == Opcode::StrictEq) {
        return surelyOverlapInFolding(first, second)
            && (isSurelySeveralValuesInFolding(second) || surelyLeavesSomethingInFolding(first, second))
            && (isSurelySeveralValuesInFolding(first) || surelyLeavesSomethingInFolding(second, first));
    }
    if (opcode != Opcode::Less && opcode != Opcode::LessEq && opcode != Opcode::Greater && opcode != Opcode::GreaterEq)
        return false;
    auto mayBeInteger = [](const PartsJudgedInFolding& type) {
        return !type.hasObjects && (type.primitives & ~Primitive::Undefined) == Primitive::Integer;
    };
    auto mayLieWithinBounds = [](const PartsJudgedInFolding& type) {
        return !type.hasObjects && !(type.primitives & ~(Primitive::Undefined | Primitive::Integer | Primitive::FractionalNumber));
    };
    return !(mayBeInteger(first) && mayLieWithinBounds(second)) && !(mayBeInteger(second) && mayLieWithinBounds(first));
}

static ALWAYS_INLINE std::optional<bool> answerThatTypesDecideInFolding(Walker& walker, const BodyValue& test)
{
    std::array<TypeID, 2> typesOfOperands;
    uint32_t numberOfOperands = 0;
    for (; numberOfOperands < typesOfOperands.size(); ++numberOfOperands) {
        ValueID operand = test.operand(numberOfOperands);
        if (!operand)
            break;
        typesOfOperands[numberOfOperands] = typeToJudgeByInFolding(walker, walker.referenceOf(operand));
        if (!typesOfOperands[numberOfOperands])
            return std::nullopt;
    }

    TypeArena& types = walker.types();
    if (types.isUninformative(typesOfOperands[0])) {
        if (test.opcode() == Opcode::ToBoolean || (test.opcode() == Opcode::TypeTest && !types.isUninformative(test.testedType())) || (numberOfOperands == 2 && types.isUninformative(typesOfOperands[1])))
            return std::nullopt;
    }

    if (test.opcode() == Opcode::StrictEq && areOneClassObjectInFolding(walker, typesOfOperands[0], typesOfOperands[1]))
        return true;

    if (neitherSideOfTestIsRuledOutInFolding(types, test, std::span<const TypeID>(typesOfOperands.data(), numberOfOperands)))
        return std::nullopt;

    TypingContext context = typingForFolding(walker);
    OperandTypes operands { std::span<const TypeID>(typesOfOperands.data(), numberOfOperands) };
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (isNeverType(typeOfEdgeResult(context, test, operands, operandIndex, EdgeSide::Taken)))
            return false;
        if (isNeverType(typeOfEdgeResult(context, test, operands, operandIndex, EdgeSide::NotTaken)))
            return true;
    }
    return std::nullopt;
}

static NEVER_INLINE void checkRawNumbersThatAnswerRestsOnInFolding(Walker& walker, const BodyValue& answered)
{
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        ValueID operand = answered.operand(operandIndex);
        if (!operand)
            break;
        walker.checkRawNumberThatFoldRestsOn(walker.referenceOf(operand), walker.originOf(answered));
    }
}

static Reference operandOfCheckThatPassesInFolding(Walker& walker, const BodyValue& check)
{
    static_assert(!OperandIndex::CheckInitialized::value && !OperandIndex::RequireObjectCoercible::value && !OperandIndex::RequireObject::value);
    static_assert(passThroughChecks == OpcodeSet { Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject });
    static_assert(!genericOperationOf(Opcode::CheckInitialized) && !genericOperationOf(Opcode::RequireObjectCoercible) && !genericOperationOf(Opcode::RequireObject));
    Reference checked = walker.referenceOf(check.operand(0));
    TypeID type = typeToJudgeByInFolding(walker, checked);
    if (!type)
        return { };
    TypeArena& types = walker.types();
    Opcode opcode = check.opcode();
    TypeID passing = opcode == Opcode::CheckInitialized ? types.withoutPrimitives(type, Primitive::Absent) : opcode == Opcode::RequireObjectCoercible ? types.withoutNullish(type) : types.withOnlyObjects(type);
    return passing == type ? checked : Reference();
}

static constexpr uint16_t primitivesThatConversionHandsOnInFolding(Opcode conversion)
{
    switch (conversion) {
    case Opcode::ToBoolean:
        return Primitive::Boolean;
    case Opcode::ToNumber:
        return Primitive::Number;
    case Opcode::ToNumeric:
        return Primitive::Number | Primitive::BigInt;
    case Opcode::ToString:
        return Primitive::AnyString;
    case Opcode::ToPropertyKey:
        return Primitive::AnyString | Primitive::AnySymbol;
    default:
        return 0;
    }
}

static constexpr OpcodeSet conversionsThatMayHandOnInFolding = opcodesWhere([](Opcode opcode) {
    return primitivesThatConversionHandsOnInFolding(opcode) || conversionsToObjectsInFolding.contains(opcode) || opcode == Opcode::ToLength;
});

static ALWAYS_INLINE Reference operandThatConversionHandsOnInFolding(Walker& walker, const BodyValue& conversion)
{
    Opcode opcode = conversion.opcode();
    uint16_t handedOn = primitivesThatConversionHandsOnInFolding(opcode);
    if (!handedOn && !conversionsToObjectsInFolding.contains(opcode) && opcode != Opcode::ToLength)
        return { };
    Reference operand = walker.referenceOf(conversion.operand(0));
    TypeID id = typeToJudgeByInFolding(walker, operand);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (conversionsToObjectsInFolding.contains(opcode))
        return type.isJavaScriptValue() && type.isObject() ? operand : Reference();
    if (opcode == Opcode::ToLength)
        return type.isInteger() && type.integers.minimum >= 0 && type.integers.maximum <= maxSafeInteger() ? operand : Reference();
    return type.hasOnlyPrimitives(handedOn) ? operand : Reference();
}

static Reference foldTypeOfInFolding(Walker& walker, const BodyValue& typeOf)
{
    TypeID id = typeToJudgeByInFolding(walker, walker.referenceOf(typeOf.as<Opcode::TypeOf>().valueOperand()));
    if (!id)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || type.primitives & Primitive::Absent)
        return { };

    const Program& program = walker.program();
    StringID answer;
    if (type.hasObjects()) {
        if (!type.primitives && types.isSubset(id, TypeTable::function))
            answer = program.stringOf(BuiltinNames::function);
        else if (!(type.primitives & ~Primitive::Null) && !types.overlap(id, TypeTable::function))
            answer = program.stringOf(BuiltinNames::object);
    } else if (type.hasOnlyPrimitives(Primitive::Undefined))
        answer = program.stringOf(BuiltinNames::undefined);
    else if (type.hasOnlyPrimitives(Primitive::Null))
        answer = program.stringOf(BuiltinNames::object);
    else if (type.hasOnlyPrimitives(Primitive::Boolean))
        answer = program.stringOf(BuiltinNames::boolean);
    else if (type.hasOnlyPrimitives(Primitive::Number))
        answer = program.stringOf(BuiltinNames::number);
    else if (type.hasOnlyPrimitives(Primitive::AnyString))
        answer = program.stringOf(BuiltinNames::string);
    else if (type.hasOnlyPrimitives(Primitive::AnySymbol))
        answer = program.stringOf(BuiltinNames::symbol);
    else if (type.hasOnlyPrimitives(Primitive::BigInt))
        answer = program.stringOf(BuiltinNames::bigint);
    return answer ? walker.constant(PrimitiveKnownToFolding::text(answer)) : Reference();
}

static LinkedCell cellOfVariableInFolding(const Walker& walker, VariableID variable)
{
    const LinkTables& linked = walker.facts().linked();
    return variable.index() < linked.variables.size() ? linked.variable(variable) : LinkedCell { };
}

static bool isStoredBeforeEveryReadInFolding(const LocationFacts& location)
{
    return location.initialization != InitializationState::Unknown;
}

static Reference onlyValueOfCellInFolding(Walker& walker, const LinkedCell& cell, bool isFilledAtEveryRead)
{
    if (!isFilledAtEveryRead || !cell.proven || !cell.everyWriterIsKnown)
        return { };
    if (walker.checksProofs())
        return { };
    return walker.constant(onlyValueOfTypeInFolding(walker.types(), cell.proven));
}

static ALWAYS_INLINE Reference unlessAbsentInFolding(Walker& walker, Reference held)
{
    TypeID type = typeToJudgeByInFolding(walker, held);
    return type && !(walker.types().at(type).primitives & Primitive::Absent) ? held : Reference();
}

static bool holdsAccessorInFolding(Walker& walker, FieldReference field)
{
    FieldFacts facts = walker.facts().field(field);
    return fieldAt(walker.program(), field).form.contains(FieldForm::IsAccessor) || facts.formOfValue.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor);
}

static Reference answerOfNamedReadInFolding(Walker& walker, const BodyOperation<Opcode::PropGet>& read)
{
    if (read.isFreeName())
        return { };
    Reference object = walker.referenceOf(read.objectOperand());
    if (!walker.state().mayHoldPlaceOf(object))
        return propertyOfResultTakenInParts(walker, object, read.key());
    TypeID type = typeToJudgeByInFolding(walker, object);
    if (!type)
        return { };
    TypeArena& types = walker.types();
    LayoutID layout = singleLayoutOf(types, types.at(type));
    if (!layout)
        return propertyOfResultTakenInParts(walker, object, read.key());
    std::optional<FieldReference> field = findField(walker.program(), layout, read.key());
    if (!field || holdsAccessorInFolding(walker, *field))
        return { };
    Reference held = walker.state().slot(object, *field);
    return held && !isClosureLeftUnmade(walker, held) ? unlessAbsentInFolding(walker, held) : Reference();
}

static Reference elementAnsweredByStateInFolding(Walker& walker, const BodyValue& value)
{
    BodyOperation<Opcode::ElemGet> read = value.as<Opcode::ElemGet>();
    Reference index = walker.referenceOf(read.propertyOperand());
    TypeID typeOfIndex = typeToJudgeByInFolding(walker, index);
    if (!typeOfIndex || !walker.types().at(typeOfIndex).isNumber())
        return { };
    if (Reference part = partOfListThatIsOnlyTakenApart(walker, walker.referenceOf(read.objectOperand()), index))
        return part;
    Reference held = unlessAbsentInFolding(walker, walker.state().element(walker.referenceOf(read.objectOperand()), index));
    if (held && (walker.types().at(typeToJudgeByInFolding(walker, held)).primitives & Primitive::Undefined) && readLeavesTurnAtHoleBehindRow(walker, value))
        return { };
    return held;
}

static Reference answerOfReadInFolding(Walker& walker, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::GetModuleVariable: {
        VariableID variable = value.as<Opcode::GetModuleVariable>().variable();
        if (walker.plan().variable(variable).encoding == VariableEncoding::FunctionAsZero) {
            Draft read = walker.draftOf(value);
            walker.chooseConservativeFields(read);
            if (Reference closure = walker.cellThatRunTimeKeeps(read))
                return closure;
        }
        if (Reference held = walker.state().variable(variable))
            return held;
        if (Reference kept = valueOfStableBinding(walker, value))
            return kept;
        bool isFilled = walker.program().at(variable).initialization != VariableInitialization::Uninitialized || isStoredBeforeEveryReadInFolding(walker.facts().variable(variable).location);
        return onlyValueOfCellInFolding(walker, cellOfVariableInFolding(walker, variable), isFilled);
    }
    case Opcode::GetEnvironmentField: {
        auto read = value.as<Opcode::GetEnvironmentField>();
        FieldReference field { read.layout(), read.field() };
        Reference environment = walker.referenceOf(read.environmentOperand());
        if (Reference held = walker.state().environmentField(environment, field))
            return held;
        return valueOfStableBinding(walker, value);
    }
    case Opcode::PropGet:
        if (Reference earlier = walker.currentBodyIndex().opcodes().contains(Opcode::NarrowByField) ? walker.tagReadInFrontOfTest(value) : Reference())
            return earlier;
        return answerOfNamedReadInFolding(walker, value.as<Opcode::PropGet>());
    case Opcode::ElemGet:
        return elementAnsweredByStateInFolding(walker, value);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Reference tryHoistInitializationCheck(Walker& walker, const BodyValue& check, Reference checked)
{
    if (!walker.isInvariantInSpecializedVersion(checked) || isNeverType(walker.types().withoutPrimitives(walker.typeOf(checked), Primitive::Absent)))
        return { };
    Origin origin = walker.originOf(check);
    origin.isTailCall = false;
    Reference isEmpty;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true);
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(checked);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        isEmpty = walker.build(WTF::move(test));
    }
    if (!walker.tryBranchInPreheader(isEmpty, false))
        return { };
    walker.recordSpeculation();
    Walker::SpecializedPreheaderScope preheaderScope(walker, true);
    Draft filled(Opcode::Narrowed, origin);
    filled.setValueOperand(checked);
    filled.setTestOperand(isEmpty);
    filled.setOperandIndex(OperandIndex::TypeTest::value);
    filled.setIsOnTakenEdge(false);
    return walker.build(WTF::move(filled));
}

static bool buildCheckOfCellInFolding(Walker& walker, const BodyValue& check)
{
    Reference checked = walker.referenceOf(check.operand(OperandIndex::CheckInitialized::value));
    if (!typeToJudgeByInFolding(walker, checked) || !checked.isValue())
        return false;

    VariableID variable;
    Reference environment;
    FieldReference field;
    const Code& code = walker.code();
    const Operation& read = code.at(checked.id());
    if (read.opcode == Opcode::GetModuleVariable)
        variable = read.variable();
    else if (read.opcode == Opcode::GetEnvironmentField) {
        ValueID holder = read.operand(OperandIndex::GetEnvironmentField::environment);
        environment = code.isConstant(holder) ? Reference::constant(holder) : Reference::value(holder);
        field = FieldReference { read.layout(), read.field() };
    }

    Reference passed = tryHoistInitializationCheck(walker, check, checked);
    if (!passed) {
        Draft draft = walker.draftOf(check);
        walker.chooseConservativeFields(draft);
        passed = walker.build(WTF::move(draft));
    }
    walker.bind(check.id(), passed);
    if (!passed || walker.isUnreachable())
        return true;

    if (variable)
        noteValueOfStableBinding(walker, variable, passed);
    if (environment)
        noteValueOfStableBinding(walker, environment, field, passed);
    WalkState& state = walker.state();
    if (variable && state.variable(variable) == checked)
        state.noteVariable(variable, passed);
    if (environment && state.environmentField(environment, field) == checked)
        state.noteEnvironmentField(environment, field, passed);
    if (environment)
        state.noteEndOfDeadZone(environment, field);
    if (variable && leavesDeadZoneForGood(walker.program(), variable))
        state.noteEndOfDeadZone(variable);
    return true;
}

static ALWAYS_INLINE Reference answerKnownToFolding(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (readsOfFolding.contains(opcode))
        return answerOfReadInFolding(walker, value);
    if (passThroughChecks.contains(opcode))
        return operandOfCheckThatPassesInFolding(walker, value);
    if (castsOfFolding.contains(opcode)) {
        Reference operand = walker.referenceOf(value.operand(0));
        return primitiveHeldInFolding(walker, operand) ? operand : Reference();
    }
    if (opcode == Opcode::TypeOf)
        return foldTypeOfInFolding(walker, value);

    if (operationsOnPrimitivesInFolding.contains(opcode)) {
        if (Reference constant = foldOverKnownPrimitivesInFolding(walker, value))
            return constant;
    }
    if (testsThatTypesDecideInFolding.contains(opcode)) {
        if (std::optional<bool> answer = answerThatTypesDecideInFolding(walker, value)) {
            if (walker.checksProofs()) [[unlikely]]
                checkRawNumbersThatAnswerRestsOnInFolding(walker, value);
            return walker.constant(PrimitiveKnownToFolding::boolean(*answer));
        }
    }
    if (opcode == Opcode::TypeTest && isLookAtCellInFrontOfIsArray(walker, walker.currentBodyIndex(), value)) {
        Reference asked = walker.referenceOf(value.operand(0));
        if (!asked || !cellAnswersWhetherValueIsArray(walker, walker.typeOf(asked)))
            return walker.constant(PrimitiveKnownToFolding::boolean(false));
    }
    if (Reference answer = opcode == Opcode::StrictEq && !walker.isAskedByBranchOfItsBlock(value) ? walker.buildTestOfTagAsTestOfShape(value) : Reference())
        return answer;
    Reference handedOn = conversionsThatMayHandOnInFolding.contains(opcode) ? operandThatConversionHandsOnInFolding(walker, value) : Reference();
    if (handedOn && walker.checksProofs()) [[unlikely]]
        checkRawNumbersThatAnswerRestsOnInFolding(walker, value);
    return handedOn;
}

constinit const OpcodeSet opcodesThatFoldOperationTakes = opcodesOfFolding;
static_assert(opcodesThatFoldOperationTakesInChains == opcodesOfFolding);

static NEVER_INLINE bool buildWhereNoAnswerIsKnownToFolding(Walker& walker, const BodyValue& value)
{
    if (value.opcode() == Opcode::CheckInitialized)
        return buildCheckOfCellInFolding(walker, value);
    return !walker.has(Mechanism::TypedOperations) && buildTypedAccess(walker, value);
}

static NEVER_INLINE bool bindAnswerKnownToFolding(Walker& walker, const BodyValue& value, Reference answer)
{
    walker.bind(value.id(), answer);
    return true;
}

bool foldOperationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    ASSERT(opcodesOfFolding.contains(value.opcode()));
    if (Reference answer = storesThatStateFollowsForFolding.contains(value.opcode()) ? Reference() : answerKnownToFolding(walker, value))
        return bindAnswerKnownToFolding(walker, value, answer);
    if (value.opcode() != Opcode::CheckInitialized && !placesThatStateFollowsForFolding.contains(value.opcode()))
        return false;
    return buildWhereNoAnswerIsKnownToFolding(walker, value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
