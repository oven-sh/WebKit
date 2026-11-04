/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding. An operation whose answer is known where the walk stands is bound to that answer. Nothing is built for it, but for a check that may still throw.
// At a suspension and at a throw it does nothing of its own: the state has lost what either may change by the time it is asked, and constants and proven types outlive both.
// It takes away only what neither throws nor runs code with the operands at hand. Withheld: every operation goes on down the chain, and every branch keeps both sides.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTStringTable.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include "AOTWalkStringFolds.h"
#include "AOTWalkKnown.h"
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

static constexpr size_t maxBytesOfTextMadeByFolding = 65536;

static constexpr OpcodeSet testsThatTypesDecideInFolding = opcodesWhere([](Opcode opcode) {
    Proof proof = provesOf(opcode);
    bool narrowsType = proof == Proof::TestedType || proof == Proof::ExactFunction || proof == Proof::Truthiness || proof == Proof::Equality || proof == Proof::IntegerBound;
    return narrowsType && hasRuleForFolding(opcode);
});
static_assert(testsThatTypesDecideInFolding == OpcodeSet { Opcode::TypeTest, Opcode::IsClosureOf, Opcode::ToBoolean, Opcode::Eq, Opcode::StrictEq, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq });

static constexpr OpcodeSet readsOfFolding { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::PropGet, Opcode::ElemGet };
static constexpr OpcodeSet storesThatStateFollowsForFolding { Opcode::PutModuleVariable, Opcode::PutEnvironmentField };
static constexpr OpcodeSet placesThatStateFollowsForFolding = storesThatStateFollowsForFolding | OpcodeSet { Opcode::GetModuleVariable, Opcode::GetEnvironmentField };
// An object is its own object, and a length is its own length.
static constexpr OpcodeSet conversionsToObjectsInFolding { Opcode::ToObject, Opcode::ToThis };
static constexpr OpcodeSet opcodesOfFolding = operationsOnPrimitivesInFolding | testsThatTypesDecideInFolding | castsOfFolding | readsOfFolding | storesThatStateFollowsForFolding | checksThatHandOnOrThrow
    | conversionsToObjectsInFolding | OpcodeSet { Opcode::TypeOf, Opcode::ToLength };

// ---- Known primitives

static PrimitiveKnownToFolding onlyValueOfTypeInFolding(const TypeArena& types, TypeID id)
{
    if (!id)
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

// No type tells two doubles apart, and none names a made text: those two are read off the constant.
static PrimitiveKnownToFolding primitiveHeldInFolding(Walker& walker, Reference reference)
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

// A number has one constant: an integer that integer types hold is an Integer, any other number a Double.
static Reference constantForFolding(Walker& walker, PrimitiveKnownToFolding primitive)
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
    return walker.constant(WTF::move(constant));
}

// ---- The rules of the rows that have hasKnownValueRule, over known primitives. None of them throws or runs code there.

// A text is not read as a number here.
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

// A NaN that an operand brings goes through an operation as it is. One that the processor makes has a sign on some processors and none on others,
// the program may be built on one and run on the other, and it can read the sign out of a Float64Array.
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
    // With such an exponent the power is made of multiplications alone. Any other goes to the C library, whose answer where the program is built need not be its answer where the program runs.
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

// As TypeArena::spellSameText().
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

// Null for a number, whose text the table need not hold.
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

// Each part as ToString spells it. A text of the table where that is the whole answer: it goes by its id like one that was written. Else a made text.
// Unknown for narrow parts beside wide ones: a piece of such a text that lies in an 8-bit part is an 8-bit string, a piece of a 16-bit literal is not.
// `maker` is null: unknown too where the text would have to be made.
static PrimitiveKnownToFolding joinTextsInFolding(const Program& program, TypeReader texts, TypeArena* maker, std::span<const PrimitiveKnownToFolding> parts)
{
    // Nothing here goes through the bytes of a made text: in `a + b + c + ..` each sum is a part of the next.
    struct Part {
        // Null: a number.
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

// `right` is unknown where the operation has one operand.
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

static Reference foldOverKnownPrimitivesInFolding(Walker& walker, const BodyValue& value)
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
    return constantForFolding(walker, evaluateInFolding(walker.program(), walker.types(), &walker.types(), value.opcode(), operands[0], operands[1]));
}

// ---- Without a walk

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

// As answerKnownToFolding() goes for operands that are constants: a cast is its operand, and foldOverKnownPrimitivesInFolding().
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

// ---- What proven types decide

// No rule asked here reads a position.
static TypingContext typingForFolding(Walker& walker)
{
    return { walker.program(), walker.types(), PositionTypes::knowingNothing() };
}

// Null where nothing may be concluded from the type: no run comes here, or the word may be anything.
static TypeID typeToJudgeByInFolding(Walker& walker, Reference reference)
{
    if (!reference)
        return { };
    TypeID type = walker.typeOf(reference);
    if (!type || isNeverType(type) || walker.types().at(type).mayBeUnspecified())
        return { };
    return type;
}

// `S.#count` asks whether S is the class object that declares the name. A class that is evaluated at most once has one class object in the one realm that runs the image.
static bool areOneClassObjectInFolding(Walker& walker, TypeID left, TypeID right)
{
    if (left != right || !walker.plan().hasOneInstance())
        return false;
    Type type = walker.types().at(left);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
    return classLayout && walker.facts().isEvaluatedAtMostOnce(classLayout);
}

// The side Taken narrows to what may pass, the side NotTaken takes away only what would surely have passed (ir/AOTTypeNarrowing.h). So an operand that one side leaves nothing of decides the test.
// Where that happens the operands are such that the test converts nothing: it neither throws nor runs code.
static std::optional<bool> answerThatTypesDecideInFolding(Walker& walker, const BodyValue& test)
{
    if (std::optional<bool> answer = answerOfRelationWithoutCompare(walker, test))
        return answer;
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

    // Of values of which nothing is said both answers are open: no edge leaves `never`.
    TypeArena& types = walker.types();
    if (types.saysNothing(typesOfOperands[0])) {
        if (test.opcode() == Opcode::ToBoolean || (test.opcode() == Opcode::TypeTest && !types.saysNothing(test.testedType())) || (numberOfOperands == 2 && types.saysNothing(typesOfOperands[1])))
            return std::nullopt;
    }

    if (test.opcode() == Opcode::StrictEq && areOneClassObjectInFolding(walker, typesOfOperands[0], typesOfOperands[1]))
        return true;

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

// CheckInitialized, RequireObjectCoercible, RequireObject: each throws for what it turns away, and its result rule gives the rest of its operand. Where that is all of the operand it turns nothing away.
static Reference operandOfCheckThatPassesInFolding(Walker& walker, const BodyValue& check)
{
    static_assert(!OperandIndex::CheckInitialized::value && !OperandIndex::RequireObjectCoercible::value && !OperandIndex::RequireObject::value);
    Reference checked = walker.referenceOf(check.operand(0));
    TypeID type = typeToJudgeByInFolding(walker, checked);
    if (!type)
        return { };
    // The rule goes by the opcode and the type of the operand.
    Draft draft(check.opcode());
    draft.setOperand(0, checked);
    TypeID passing = resultTypeOf(typingForFolding(walker), draft, OperandTypes { std::span<const TypeID>(&type, 1) });
    return passing == type ? checked : Reference();
}

// Not ToBoolean: the branch behind it narrows its operand by the record of the test, so a boolean too keeps that record.
static uint16_t primitivesThatConversionHandsOnInFolding(Opcode conversion)
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

static Reference operandThatConversionHandsOnInFolding(Walker& walker, const BodyValue& conversion)
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

// The texts are names of the built-ins' table, which every program holds.
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
    return answer ? constantForFolding(walker, PrimitiveKnownToFolding::text(answer)) : Reference();
}

// ---- Reads

// Link's tables may be shorter than the program's. What has no entry claims nothing.
static LinkedCell cellOfVariableInFolding(const Walker& walker, VariableID variable)
{
    const LinkTables& linked = walker.facts().linked();
    return variable.index() < linked.variables.size() ? linked.variable(variable) : LinkedCell { };
}

static LinkedCell cellOfFieldInFolding(const Walker& walker, FieldReference field)
{
    const LinkTables& linked = walker.facts().linked();
    const Vector<uint32_t>& first = linked.firstFieldOfLayout;
    size_t layout = field.layout.index();
    if (layout + 1 >= first.size() || field.field >= first[layout + 1] - first[layout])
        return { };
    return linked.field(field);
}

static LinkedCell cellOfCaptureInFolding(const Walker& walker, FunctionID function, uint32_t capture)
{
    const LinkTables& linked = walker.facts().linked();
    const Vector<uint32_t>& first = linked.firstCaptureOfFunction;
    size_t index = function.index();
    if (index + 1 >= first.size() || capture >= first[index + 1] - first[index])
        return { };
    return linked.capture(function, capture);
}

static bool isStoredBeforeEveryReadInFolding(const LocationFacts& location)
{
    return location.initialization != InitializationState::Unknown;
}

// LinkedCell::proven is the join of what the writers bring. A read in front of the first store finds the cell empty, and of that it says nothing.
static Reference onlyValueOfCellInFolding(Walker& walker, const LinkedCell& cell, bool isFilledAtEveryRead)
{
    if (!isFilledAtEveryRead || !cell.proven || !cell.everyWriterIsKnown)
        return { };
    // The read is built, so that what it finds can be held against the proof. Whoever takes its result goes by the type all the same.
    if (walker.checksProofs())
        return { };
    return constantForFolding(walker, onlyValueOfTypeInFolding(walker.types(), cell.proven));
}

// An empty slot and a hole are absent, and a read by name or by index then goes on above the object.
static Reference unlessAbsentInFolding(Walker& walker, Reference held)
{
    TypeID type = typeToJudgeByInFolding(walker, held);
    return type && !(walker.types().at(type).primitives & Primitive::Absent) ? held : Reference();
}

static bool holdsAccessorInFolding(Walker& walker, FieldReference field)
{
    FieldFacts facts = walker.facts().field(field);
    return fieldAt(walker.program(), field).form.contains(FieldForm::IsAccessor) || facts.formOfValue.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor);
}

// Every read of the field by name, of any object of the layout that is made, finds a value of the object's own. An object that this code is still filling is not made yet.
static bool isOwnValueForLifeInFolding(Walker& walker, Reference object, const Type& typeOfObject, LayoutID layout, FieldReference field)
{
    // Undefined and null throw. Only a field that the layout itself lists has a place in its objects.
    if (!typeOfObject.isJavaScriptValue() || typeOfObject.primitives || !typeOfObject.hasOnlyLayouts() || field.layout != layout)
        return false;
    const LinkTables& linked = walker.facts().linked();
    if (!isFoundByShapeTest(walker.program().at(layout)) || layout.index() >= linked.layouts.size() || walker.state().isFresh(object))
        return false;
    if (linked.layout(layout).possibleChanges.containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor }))
        return false;
    return walker.facts().field(field).occupancy == Occupancy::AlwaysFilled;
}

// Whoever noted the slot had the object in hand with the field in its slot, so the read finds an own data property and neither throws nor runs code.
static Reference answerOfNamedReadInFolding(Walker& walker, const BodyOperation<Opcode::PropGet>& read)
{
    if (read.isFreeName())
        return { };
    Reference object = walker.referenceOf(read.objectOperand());
    TypeID type = typeToJudgeByInFolding(walker, object);
    if (!type)
        return { };
    TypeArena& types = walker.types();
    Type typeOfObject = types.at(type);
    LayoutID layout = singleLayoutOf(types, typeOfObject);
    if (!layout)
        return { };
    std::optional<FieldReference> field = findField(walker.program(), layout, read.key());
    if (!field || holdsAccessorInFolding(walker, *field))
        return { };
    if (Reference held = walker.state().slot(object, *field))
        return unlessAbsentInFolding(walker, held);
    return onlyValueOfCellInFolding(walker, cellOfFieldInFolding(walker, *field), isOwnValueForLifeInFolding(walker, object, typeOfObject, layout, *field));
}

// An index that is a number is converted without code.
static Reference elementAnsweredByStateInFolding(Walker& walker, const BodyOperation<Opcode::ElemGet>& read)
{
    Reference index = walker.referenceOf(read.propertyOperand());
    TypeID typeOfIndex = typeToJudgeByInFolding(walker, index);
    if (!typeOfIndex || !walker.types().at(typeOfIndex).isNumber())
        return { };
    return unlessAbsentInFolding(walker, walker.state().element(walker.referenceOf(read.objectOperand()), index));
}

// A closure keeps the captures that it was made with, so the state has no place for them: they are read off the NewClosure.
static Reference captureOfClosureMadeHereInFolding(Walker& walker, const BodyOperation<Opcode::GetCapture>& read)
{
    Reference closure = walker.referenceOf(read.closureOperand());
    if (!closure || !closure.isRecord())
        return { };
    const Code& code = walker.code();
    const Operation& made = code.at(closure.id());
    if (made.opcode != Opcode::NewClosure || made.function() != read.function() || read.index() >= made.numberOfVariadicOperands())
        return { };
    ValueID capture = made.variadicOperands()[read.index()];
    Reference captured = code.isConstant(capture) ? Reference::constant(capture) : Reference::value(capture);
    return typeToJudgeByInFolding(walker, captured) ? captured : Reference();
}

static Reference answerOfReadInFolding(Walker& walker, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::GetModuleVariable: {
        VariableID variable = value.as<Opcode::GetModuleVariable>().variable();
        if (Reference held = walker.state().variable(variable))
            return held;
        if (Reference kept = valueOfStableBinding(walker, value))
            return kept;
        // What a variable starts with other than nothing is among what Link has joined.
        bool isFilled = walker.program().at(variable).initialization != VariableInitialization::Uninitialized || isStoredBeforeEveryReadInFolding(walker.facts().variable(variable).location);
        return onlyValueOfCellInFolding(walker, cellOfVariableInFolding(walker, variable), isFilled);
    }
    case Opcode::GetEnvironmentField: {
        auto read = value.as<Opcode::GetEnvironmentField>();
        FieldReference field { read.layout(), read.field() };
        if (Reference held = walker.state().environmentField(walker.referenceOf(read.environmentOperand()), field))
            return held;
        if (Reference kept = valueOfStableBinding(walker, value))
            return kept;
        return onlyValueOfCellInFolding(walker, cellOfFieldInFolding(walker, field), isStoredBeforeEveryReadInFolding(walker.facts().field(field).location));
    }
    case Opcode::GetCapture: {
        auto read = value.as<Opcode::GetCapture>();
        if (Reference captured = captureOfClosureMadeHereInFolding(walker, read))
            return captured;
        // A closure is made with its captures.
        return onlyValueOfCellInFolding(walker, cellOfCaptureInFolding(walker, read.function(), read.index()), true);
    }
    case Opcode::PropGet:
        if (Reference earlier = walker.tagReadInFrontOfTest(value))
            return earlier;
        return answerOfNamedReadInFolding(walker, value.as<Opcode::PropGet>());
    case Opcode::ElemGet:
        return elementAnsweredByStateInFolding(walker, value.as<Opcode::ElemGet>());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// The check is built as it was written. Whoever reads the place again while the state still has it finds what has passed, so a second check turns nothing away.
// `const debug = false`, read where its store may not have run yet: the cell is empty or holds the one value, so what passes is known.
static bool buildCheckOfCellInFolding(Walker& walker, const BodyValue& check)
{
    Reference checked = walker.referenceOf(check.operand(OperandIndex::CheckInitialized::value));
    if (!typeToJudgeByInFolding(walker, checked) || !checked.isValue())
        return false;

    LinkedCell cell;
    VariableID variable;
    Reference environment;
    FieldReference field;
    const Code& code = walker.code();
    const Operation& read = code.at(checked.id());
    if (read.opcode == Opcode::GetModuleVariable) {
        variable = read.variable();
        cell = cellOfVariableInFolding(walker, variable);
    } else if (read.opcode == Opcode::GetEnvironmentField) {
        ValueID holder = read.operand(OperandIndex::GetEnvironmentField::environment);
        environment = code.isConstant(holder) ? Reference::constant(holder) : Reference::value(holder);
        field = FieldReference { read.layout(), read.field() };
        cell = cellOfFieldInFolding(walker, field);
    }
    Reference known = onlyValueOfCellInFolding(walker, cell, true);

    Draft draft = walker.draftOf(check);
    walker.chooseConservativeFields(draft);
    Reference passed = walker.build(WTF::move(draft));
    if (known)
        passed = known;
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

// ---- The link of the chain

static Reference answerKnownToFolding(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (readsOfFolding.contains(opcode))
        return answerOfReadInFolding(walker, value);
    if (checksThatHandOnOrThrow.contains(opcode))
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
        if (std::optional<bool> answer = answerThatTypesDecideInFolding(walker, value))
            return constantForFolding(walker, PrimitiveKnownToFolding::boolean(*answer));
    }
    // The function behind the look answers for a list too. Where it has to be asked of what is left in any case, the look would only be one more: every value goes the way of the function.
    if (opcode == Opcode::TypeTest && isLookAtCellInFrontOfIsArray(walker, walker.currentBodyIndex(), value)) {
        Reference asked = walker.referenceOf(value.operand(0));
        if (!asked || !cellAnswersWhetherValueIsArray(walker, walker.typeOf(asked)))
            return constantForFolding(walker, PrimitiveKnownToFolding::boolean(false));
    }
    if (Reference answer = opcode == Opcode::StrictEq && !walker.isAskedByBranchOfItsBlock(value) ? walker.buildTestOfTagAsTestOfShape(value) : Reference())
        return answer;
    if (Reference answer = answerKnownOfTexts(walker, value))
        return answer;
    return operandThatConversionHandsOnInFolding(walker, value);
}

constinit const OpcodeSet opcodesThatFoldOperationTakes = opcodesOfFolding;

bool foldOperationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfFolding.contains(opcode))
        return false;
    if (Reference answer = storesThatStateFollowsForFolding.contains(opcode) ? Reference() : answerKnownToFolding(walker, value)) {
        walker.bind(value.id(), answer);
        return true;
    }
    if (opcode == Opcode::CheckInitialized)
        return buildCheckOfCellInFolding(walker, value);
    // The state answers a read only where somebody has told it what the place holds. buildTypedAccess() builds these four as they are written and tells it.
    // With TypedOperations on it is asked further down the chain.
    return placesThatStateFollowsForFolding.contains(opcode) && !walker.has(Mechanism::TypedOperations) && buildTypedAccess(walker, value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
