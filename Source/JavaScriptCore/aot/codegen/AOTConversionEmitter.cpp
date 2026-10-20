/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversionEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPureOperations.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsStringLiterals.h"
#include "B3IntRange.h"
#include "B3StackmapGenerationParams.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <cmath>
#include <limits>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t markerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };
static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

static_assert(integerMarkers.undefined < integerMarkers.null && integerMarkers.undefined < integerMarkers.absent);
static_assert(doubleMarkers.undefined < doubleMarkers.null && doubleMarkers.undefined < doubleMarkers.absent);
static_assert(static_cast<int64_t>(integerMarkers.absent) > 0 && static_cast<int64_t>(doubleMarkers.absent) > 0);
static_assert(PNaNAsBits < doubleMarkers.undefined);
static_assert(boxedMarkers.absent < boxedMarkers.undefined && boxedMarkers.null < boxedMarkers.undefined);

static constexpr std::optional<SlotMarkers> markersOf(Representation representation)
{
    switch (representation) {
    case Representation::OptionalInt64:
        return integerMarkers;
    case Representation::OptionalDouble:
        return doubleMarkers;
    case Representation::Cell32:
        return cell32Markers;
    case Representation::JSValue:
    case Representation::LiteralString:
        return boxedMarkers;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static bool isValueWithoutBits(const Type& type)
{
    return type.isJavaScriptValue() && isLegalRepresentation(type, Representation::Void);
}

static constexpr bool isWithinInt32(Range range)
{
    return !range.isEmpty() && range.isSubsetOf(Range::int32());
}

static constexpr bool isNonNegative(Range range)
{
    return !range.isEmpty() && range.minimum >= 0;
}

static std::optional<Word32> convertedInt32Of(Number number)
{
    LValue value = number.value();
    if (value->opcode() != B3::IToD || value->child(0)->type() != B3::Int32)
        return std::nullopt;
    return Word32(value->child(0));
}

namespace {

class MarkerEmitter final : public OperationEmitter {
public:
    explicit MarkerEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Operand convertMarkers(Operand source, const Type&, Representation to, std::optional<Operand> unmarked);
    Integer integerPartOf(Operand, const Type&);
    Number numberPartOf(Operand, const Type&);
};

Operand MarkerEmitter::convertMarkers(Operand source, const Type& type, Representation to, std::optional<Operand> unmarked)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    bool sourceHasMarkers = markersOf(source.representation).has_value();
    std::optional<Operand> result = unmarked;
    for (uint16_t primitive : markerKinds) {
        if (!(type.primitives & primitive))
            continue;
        Operand marker = conversions.fromBits(integer(static_cast<int64_t>(ConversionEmitter::markerBits(primitive, to))), to);
        if (!result) {
            result = marker;
            continue;
        }
        if (!sourceHasMarkers)
            continue;
        Condition holdsMarker = conversions.isMarker(source, ConversionEmitter::markerBits(primitive, source.representation));
        result = Operand(to, m_out.select(holdsMarker.value(), marker.value, result->value));
    }
    return result ? *result : conversions.fromBits(integer(0), to);
}

Integer MarkerEmitter::integerPartOf(Operand operand, const Type& type)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (operand.representation) {
    case Representation::Int32:
        return conversions.widen(operand.as<Word32>(), type.integers);
    case Representation::Int64:
    case Representation::OptionalInt64:
        return Integer(operand.value);
    case Representation::Double:
    case Representation::OptionalDouble:
        return conversions.toInteger(Number(operand.value));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number MarkerEmitter::numberPartOf(Operand operand, const Type& type)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (operand.representation) {
    case Representation::Int32:
        return toNumber(operand.as<Word32>());
    case Representation::Int64:
    case Representation::OptionalInt64:
        return conversions.toNumber(Integer(operand.value), type.integers);
    case Representation::Double:
        return type.mayBeNaN() ? conversions.purified(operand.as<Number>()) : operand.as<Number>();
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // anonymous namespace

ConversionEmitter::ConversionEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Boxed ConversionEmitter::box(const OperationContext& context, Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Void:
        if (!isValueWithoutBits(type))
            break;
        return MarkerEmitter(generator()).convertMarkers(operand, type, Representation::JSValue, std::nullopt).as<Boxed>();
    case Representation::Boolean:
        return boxBoolean(operand.as<Condition>());
    case Representation::Int32:
        return boxInt32(operand.as<Word32>());
    case Representation::Int64:
        return boxInteger(operand.as<Integer>(), type.integers);
    case Representation::Double:
        return boxNumber(operand.as<Number>());
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return boxOptional(operand, type);
    case Representation::Cell:
        return boxCell(operand.as<CellPointer>());
    case Representation::Cell32:
        return decode(operand.as<NarrowReference>(), type, Representation::JSValue).as<Boxed>();
    case Representation::JSValue:
        return operand.as<Boxed>();
    case Representation::LiteralString:
        return boxLiteralString(context, operand.as<LiteralStringWord>(), type);
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return undefined();
}

Boxed ConversionEmitter::boxOptional(Operand operand, const Type& type)
{
    auto boxMarker = [&] {
        return MarkerEmitter(generator()).convertMarkers(operand, type, Representation::JSValue, std::nullopt).as<Boxed>();
    };
    auto boxUnmarked = [&] {
        return operand.is<OptionalInteger>() ? boxInteger(Integer(operand.value), type.integers) : boxNumber(Number(operand.value));
    };
    if (!(type.primitives & Primitive::Number))
        return boxMarker();
    if (!(type.primitives & markerPrimitives))
        return boxUnmarked();

    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isMarker(operand).value(), unlikelyEdge(markerCase), normalEdge(numberCase));
    m_out.appendTo(numberCase);
    join.add(boxUnmarked());
    m_out.appendTo(markerCase);
    join.add(boxMarker());
    return join.finish();
}

Operand ConversionEmitter::unbox(Boxed value, const Type& type, Representation to, NumberUnboxing numberUnboxing)
{
    switch (to) {
    case Representation::Void:
        return { };
    case Representation::Boolean:
        return unboxBoolean(value);
    // The engine boxes an element of a double array as a double even when it is an int32, and a type test passes it as an integer.
    case Representation::Int32:
        return either<Word32>(isInt32(value), [&] {
            return unboxInt32(value);
        }, [&] {
            return low32(toInteger(unboxDouble(value)));
        });
    case Representation::Int64:
        return either<Integer>(isInt32(value), [&] {
            return signExtended(unboxInt32(value));
        }, [&] {
            return toInteger(unboxDouble(value));
        });
    case Representation::Double:
        if (!(type.primitives & Primitive::Integer))
            return unboxDouble(value);
        if (numberUnboxing == NumberUnboxing::ComputesBothSides) {
            build().statistics().count(StatisticID::NumbersUnboxedWithoutBranch);
            return numberToDouble(value);
        }
        return selectByBranch<Number>(isInt32(value), [&] {
            return toNumber(unboxInt32(value));
        }, [&] {
            return unboxDouble(value);
        });
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return unboxOptional(value, type, to, numberUnboxing);
    case Representation::Cell:
        return unboxCell(value);
    case Representation::Cell32:
        return compress(value, type);
    case Representation::JSValue:
        return value;
    case Representation::LiteralString:
        return toLiteralString(value, type);
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::unboxOptional(Boxed value, const Type& type, Representation to, NumberUnboxing numberUnboxing)
{
    auto unboxMarker = [&] {
        return MarkerEmitter(generator()).convertMarkers(value, type, to, std::nullopt);
    };
    auto unboxNumber = [&] {
        return Operand(to, unbox(value, type, to == Representation::OptionalInt64 ? Representation::Int64 : Representation::Double, numberUnboxing).value);
    };
    if (!(type.primitives & Primitive::Number))
        return unboxMarker();
    if (!(type.primitives & markerPrimitives))
        return unboxNumber();

    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    OperandJoin join(m_out, to);
    m_out.branch(isNumber(value).value(), normalEdge(numberCase), unlikelyEdge(markerCase));
    m_out.appendTo(numberCase);
    join.add(unboxNumber());
    m_out.appendTo(markerCase);
    join.add(unboxMarker());
    return join.finish();
}

Operand ConversionEmitter::toOptional(Operand operand, const Type& type, Representation to)
{
    RELEASE_ASSERT(to == Representation::OptionalInt64 || to == Representation::OptionalDouble);
    if (operand.representation == to)
        return operand;
    if (operand.is<Boxed>())
        return unbox(operand.as<Boxed>(), type, to);
    RELEASE_ASSERT(operand.representation != Representation::Void || isValueWithoutBits(type));

    MarkerEmitter markers(generator());
    std::optional<Operand> number;
    if (type.primitives & Primitive::Number) {
        LValue converted = to == Representation::OptionalInt64 ? markers.integerPartOf(operand, type).value() : markers.numberPartOf(operand, type).value();
        number = Operand(to, converted);
    }
    return markers.convertMarkers(operand, type, to, number);
}

Operand ConversionEmitter::inRepresentation(const OperationContext& context, Operand produced, const Type& producedType, Representation wanted)
{
    RELEASE_ASSERT(wanted != Representation::Unchosen);
    if (produced.representation == wanted)
        return produced;
    if (produced.is<Boxed>() && wanted != Representation::RawWord)
        return unbox(produced.as<Boxed>(), producedType, wanted);

    bool isNumber = numberRepresentations.contains(produced.representation);
    auto producedInteger = [&] {
        if (produced.is<Word32>())
            return signExtended(produced.as<Word32>());
        return produced.is<Integer>() ? produced.as<Integer>() : toInteger(produced.as<Number>());
    };

    switch (wanted) {
    case Representation::Void:
        return { };
    case Representation::JSValue:
        return box(context, produced, producedType);
    case Representation::Int32:
        if (isNumber)
            return narrow(producedInteger());
        break;
    case Representation::Int64:
        if (isNumber)
            return producedInteger();
        break;
    case Representation::Double:
        if (produced.is<Word32>())
            return toNumber(produced.as<Word32>());
        if (produced.is<Integer>())
            return toNumber(produced.as<Integer>(), producedType.integers);
        break;
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        if (isNumber || optionalRepresentations.contains(produced.representation))
            return toOptional(produced, producedType, wanted);
        break;
    case Representation::Cell32:
        if (produced.is<CellPointer>())
            return compress(produced, producedType);
        break;
    case Representation::LiteralString:
        if (produced.is<CellPointer>())
            return toLiteralString(produced.as<CellPointer>());
        break;
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return emitter<ConstantEmitter>().zeroOf(wanted);
}

Boxed ConversionEmitter::boxNumber(Number value)
{
    if (value.value()->hasDouble())
        return boxedConstant(jsNumber(purifyNaN(value.value()->asDouble())));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return boxInt32(*converted);

    Word32 low = low32(toInteger(value));
    LBasicBlock int32Case = newBlock();
    LBasicBlock doubleCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(equal(bitsOf(toNumber(low)), bitsOf(value)).value(), normalEdge(int32Case), normalEdge(doubleCase));
    m_out.appendTo(int32Case);
    join.add(boxInt32(low));
    m_out.appendTo(doubleCase);
    join.add(boxDouble(purified(value)));
    return join.finish();
}

Boxed ConversionEmitter::boxInteger(Integer value, Range range)
{
    Word32 low = low32(value);
    if (isWithinInt32(range))
        return boxInt32(low);

    Condition fitsInt32 = isNonNegative(range) ? belowOrEqual(value, integer(INT32_MAX)) : equal(signExtended(low), value);
    LBasicBlock int32Case = newBlock();
    LBasicBlock doubleCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(fitsInt32.value(), normalEdge(int32Case), unlikelyEdge(doubleCase));
    m_out.appendTo(int32Case);
    join.add(boxInt32(low));
    m_out.appendTo(doubleCase);
    join.add(boxDouble(toNumber(value)));
    return join.finish();
}

Number ConversionEmitter::toNumber(Integer value, Range range)
{
    if (isWithinInt32(range))
        return toNumber(low32(value));
    return toNumber(value);
}

Integer ConversionEmitter::toInteger(Number value)
{
    if (value.value()->hasDouble() && std::abs(value.value()->asDouble()) < static_cast<double>(maxTrackedInteger))
        return integer(static_cast<int64_t>(value.value()->asDouble()));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return signExtended(*converted);
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Int64);
    patchpoint->append(value.value(), B3::ValueRep::SomeRegister);
    patchpoint->effects = B3::Effects::none();
    patchpoint->pureFunction = pureFunction(PureOperation::TruncateToInt64, 0, true);
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    return Integer(patchpoint);
}

struct ConversionEmitter::ExactInteger {
    Integer value;
    Range range;
    bool mayBeRounded { false };
    int64_t largestSumOfOtherTerms { 0 };
    std::optional<Word32> word { };
};

// What is added to a product that may be rounded stays far below the 2^53 from which on it is.
static constexpr Range neverRounded { -(1ll << 51), 1ll << 51 };
// The product of two int32, with room for those terms within int64.
static constexpr Range productsBehindNumbers { -(1ll << 62), 1ll << 62 };

static std::optional<Range> rangeOfProduct(Range a, Range b)
{
    Range products = Range::empty();
    for (int64_t left : { a.minimum, a.maximum }) {
        for (int64_t right : { b.minimum, b.maximum }) {
            CheckedInt64 product = CheckedInt64(left) * right;
            if (product.hasOverflowed())
                return std::nullopt;
            products = products.unionWith(Range::one(product.value()));
        }
    }
    return products;
}

static int64_t largestMagnitudeIn(Range range)
{
    return std::max(std::abs(range.minimum), std::abs(range.maximum));
}

std::optional<ConversionEmitter::ExactInteger> ConversionEmitter::findExactIntegerBehind(LValue number)
{
    switch (number->opcode()) {
    case B3::ConstDouble: {
        double constant = number->asDouble();
        if (std::trunc(constant) != constant || std::abs(constant) > static_cast<double>(neverRounded.maximum) || (!constant && std::signbit(constant)))
            return std::nullopt;
        int64_t whole = static_cast<int64_t>(constant);
        return ExactInteger { integer(whole), Range::one(whole) };
    }
    case B3::IToD: {
        LValue source = number->child(0);
        B3::IntRange known = B3::rangeFor(m_proc, source);
        Range range { known.min(), known.max() };
        if (!range.isSubsetOf(neverRounded))
            return std::nullopt;
        if (source->type() != B3::Int32)
            return ExactInteger { Integer(source), range };
        return ExactInteger { .value = widen(Word32(source), range), .range = range, .word = Word32(source) };
    }
    case B3::Mul: {
        std::optional<ExactInteger> left = findExactIntegerBehind(number->child(0));
        std::optional<ExactInteger> right = left ? findExactIntegerBehind(number->child(1)) : std::nullopt;
        if (!right || left->mayBeRounded || right->mayBeRounded)
            return std::nullopt;
        std::optional<Range> range = rangeOfProduct(left->range, right->range);
        if (!range || !range->isSubsetOf(productsBehindNumbers))
            return std::nullopt;
        // B3 selects a multiplication that branches on overflow for the product of two sign-extended words.
        auto factor = [&](const ExactInteger& operand) {
            return operand.word ? signExtended(*operand.word) : operand.value;
        };
        return ExactInteger { mul(factor(*left), factor(*right)), *range, !range->isSubsetOf(neverRounded) };
    }
    case B3::Add:
    case B3::Sub: {
        std::optional<ExactInteger> left = findExactIntegerBehind(number->child(0));
        std::optional<ExactInteger> right = left ? findExactIntegerBehind(number->child(1)) : std::nullopt;
        if (!right || (left->mayBeRounded && right->mayBeRounded))
            return std::nullopt;
        bool isSum = number->opcode() == B3::Add;
        ExactInteger result {
            .value = isSum ? add(left->value, right->value) : sub(left->value, right->value),
            .range = isSum ? Range { left->range.minimum + right->range.minimum, left->range.maximum + right->range.maximum } : Range { left->range.minimum - right->range.maximum, left->range.maximum - right->range.minimum },
            .mayBeRounded = left->mayBeRounded || right->mayBeRounded,
        };
        if (!result.mayBeRounded)
            return result.range.isSubsetOf(neverRounded) ? std::optional(result) : std::nullopt;
        const ExactInteger& rounded = left->mayBeRounded ? *left : *right;
        const ExactInteger& other = left->mayBeRounded ? *right : *left;
        result.largestSumOfOtherTerms = rounded.largestSumOfOtherTerms + largestMagnitudeIn(other.range);
        if (result.largestSumOfOtherTerms > neverRounded.maximum)
            return std::nullopt;
        return result;
    }
    default:
        return std::nullopt;
    }
}

std::optional<Integer> ConversionEmitter::exactIntegerBehind(Number number)
{
    std::optional<ExactInteger> exact = findExactIntegerBehind(number.value());
    if (!exact)
        return std::nullopt;
    build().statistics().count(StatisticID::ExactIntegersBehindNumbers);
    return exact->value;
}

Integer ConversionEmitter::widen(Word32 value, Range range)
{
    return isNonNegative(range) ? zeroExtended(value) : signExtended(value);
}

Word32 ConversionEmitter::narrow(Integer value)
{
    return low32(value);
}

Word32 ConversionEmitter::toInt32(Number value)
{
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return *converted;
    if (std::optional<ExactInteger> exact = findExactIntegerBehind(value.value())) {
        build().statistics().count(StatisticID::ExactIntegersBehindNumbers);
        if (!exact->mayBeRounded)
            return low32(exact->value);
        // An exact result that is an int32 was not rounded. Any other lies within int64, and so does the number.
        return either<Word32>(equal(signExtended(low32(exact->value)), exact->value), [&] {
            return low32(exact->value);
        }, [&] {
            return low32(toInteger(value));
        });
    }
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
        B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Int32);
        patchpoint->append(B3::ConstrainedValue(value.value(), B3::ValueRep::SomeRegister));
        patchpoint->effects = B3::Effects::none();
        patchpoint->pureFunction = pureFunction(PureOperation::ToInt32, 0, true);
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        });
        return Word32(patchpoint);
    }
#endif
    Integer truncated = toInteger(value);
    Condition fits = below(integer(1), add(truncated, integer(std::numeric_limits<int64_t>::min() + 1)));
    return either<Word32>(fits, [&] {
        return low32(truncated);
    }, [&] {
        return low32(callLeaf<Integer>(RuntimeOperation::ToInt32, { value }));
    });
}

Number ConversionEmitter::purified(Number value)
{
    return select(equal(value, value), value, number(PNaN));
}

Operand ConversionEmitter::decode(NarrowReference word, const Type& type, Representation to)
{
    switch (to) {
    case Representation::Cell:
        return decode(word);
    case Representation::Cell32:
        return word;
    case Representation::JSValue: {
        if (!(type.primitives & markerPrimitives))
            return boxCell(decode(word));
        Boxed marker(m_out.zeroExt(word.value(), B3::Int64));
        if (!type.hasCells())
            return marker;
        return select(isCellReference(word), boxCell(decode(word)), marker);
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NarrowReference ConversionEmitter::compress(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Void:
        RELEASE_ASSERT(isValueWithoutBits(type));
        return MarkerEmitter(generator()).convertMarkers(operand, type, Representation::Cell32, std::nullopt).as<NarrowReference>();
    case Representation::Cell:
        return compress(operand.as<CellPointer>());
    case Representation::Cell32:
        return operand.as<NarrowReference>();
    case Representation::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (!(type.primitives & markerPrimitives))
            return compress(unboxCell(value));
        NarrowReference marker(low32(value).value());
        if (!type.hasCells())
            return marker;
        // The empty value passes isCell.
        return select(above(value, undefined()), compress(unboxCell(value)), marker);
    }
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer ConversionEmitter::bitsOf(Operand operand)
{
    switch (operand.representation) {
    case Representation::Double:
    case Representation::OptionalDouble:
        return Integer(m_out.bitCast(operand.value, B3::Int64));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return Integer(operand.value);
    case Representation::Cell32:
        return Integer(m_out.zeroExt(operand.value, B3::Int64));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::fromBits(Integer bits, Representation representation)
{
    switch (representation) {
    case Representation::Double:
    case Representation::OptionalDouble:
        return Operand(representation, m_out.bitCast(bits.value(), B3::Double));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return Operand(representation, bits.value());
    case Representation::Cell32:
        return Operand(representation, m_out.castToInt32(bits.value()));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t ConversionEmitter::markerBits(uint16_t primitive, Representation representation)
{
    std::optional<SlotMarkers> markers = markersOf(representation);
    RELEASE_ASSERT(markers);
    switch (primitive) {
    case Primitive::Undefined:
        return markers->undefined;
    case Primitive::Null:
        return markers->null;
    case Primitive::Absent:
        return markers->absent;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition ConversionEmitter::isMarker(Operand operand)
{
    RELEASE_ASSERT(operand.is<OptionalInteger>() || operand.is<OptionalNumber>());
    return greaterThanOrEqual(bitsOf(operand), integer(static_cast<int64_t>(markerBits(Primitive::Undefined, operand.representation))));
}

Condition ConversionEmitter::isMarker(Operand operand, uint64_t marker)
{
    return equal(bitsOf(operand), integer(static_cast<int64_t>(marker)));
}

Condition ConversionEmitter::isAbsent(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::JSValue:
    case Representation::LiteralString:
        return isMarker(operand, markerBits(Primitive::Absent, operand.representation));
    case Representation::Cell32:
        static_assert(!cell32Markers.absent);
        return isZero(Word32(operand.value));
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
        return boolean(false);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::withoutMarkers(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
        return Integer(operand.value);
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return operand;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LiteralStringWord ConversionEmitter::literalStringConstant(StringID string)
{
    return LiteralStringWord(m_out.constInt64(literalStringWord(inputs().literalStrings.numberOf(string))));
}

LiteralStringWord ConversionEmitter::toLiteralString(CellPointer string)
{
    return toLiteralString(Operand(string), m_program.at(TypeTable::string));
}

LiteralStringWord ConversionEmitter::toLiteralString(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Void:
        RELEASE_ASSERT(isValueWithoutBits(type));
        return MarkerEmitter(generator()).convertMarkers(operand, type, Representation::LiteralString, std::nullopt).as<LiteralStringWord>();
    case Representation::Cell:
    case Representation::JSValue: {
        if (!(type.primitives & Primitive::AnyString))
            return LiteralStringWord(operand.value);
        CallOptions options { .pureFunction = pureFunction(PureOperation::ToLiteralString, 0, true) };
        // The stub takes every cell for a string.
        if (type.hasOnlyPrimitives(Primitive::AnyString | markerPrimitives))
            return LiteralStringWord(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::toLiteralString(), singleElementSpan(operand.value), options).first);
        return LiteralStringWord(callWithoutSite<Boxed>(RuntimeOperation::ToLiteralString, { Boxed(operand.value) }, options).value());
    }
    case Representation::LiteralString:
        return operand.as<LiteralStringWord>();
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed ConversionEmitter::boxLiteralString(const OperationContext& context, LiteralStringWord word, const Type& type)
{
    auto loadOrCreateCell = [&](LiteralStringWord literal, TypedPointer cellWord) {
        Boxed created(m_out.load64(cellWord));
        return either<Boxed>(isNotEmpty(created), [&] {
            return created;
        }, [&] {
            return boxCell(callOperation<CellPointer>(context, RuntimeOperation::LiteralStringToString, { literal }, { .pureFunction = pureFunction(PureOperation::LiteralStringToString, 0, true), .isOnRarePath = true }));
        });
    };

    std::optional<uint32_t> number;
    if (word.value()->hasInt64()) {
        uint64_t bits = word.value()->asInt64();
        if (!isLiteralStringWord(bits))
            return Boxed(word.value());
        number = literalStringIndex(static_cast<uint32_t>(bits));
    } else if (StringID literal = type.singleStringLiteral()) {
        // A literal without a number has no word, so the operand is its string cell.
        number = inputs().literalStrings.indexOf(literal);
        if (!number)
            return Boxed(word.value());
    }
    if (number) {
        RELEASE_ASSERT(*number < inputs().literalStrings.strings().size());
        if (!*number)
            return boxCell(emptyString());
        Pointer cells = loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.constants.literalStrings));
        TypedPointer cellWord = m_out.address(m_heaps.AOTInstance_stringCells, cells.value(), static_cast<ptrdiff_t>(*number) * sizeof(JSCell*));
        return loadOrCreateCell(LiteralStringWord(m_out.constInt64(literalStringWord(*number))), cellWord);
    }

    static_assert(2 << literalStringShift == sizeof(JSCell*));
    Integer halfOffset = sub(Integer(word.value()), integer(emptyLiteralStringWord));
    LBasicBlock literalCase = newBlock();
    LBasicBlock boxedCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(below(halfOffset, integer(minCell32 - emptyLiteralStringWord)).value(), normalEdge(literalCase), normalEdge(boxedCase));

    m_out.appendTo(boxedCase);
    join.add(Boxed(word.value()));

    m_out.appendTo(literalCase);
    Pointer cells = loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.constants.literalStrings));
    join.add(loadOrCreateCell(word, m_out.baseIndex(m_heaps.AOTInstance_stringCells, cells.value(), halfOffset.value(), FTL::ScaleTwo)));
    return join.finish();
}

Condition ConversionEmitter::isStringCell(LiteralStringWord word)
{
    return aboveOrEqual(word, LiteralStringWord(m_out.constInt64(minCell32)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
