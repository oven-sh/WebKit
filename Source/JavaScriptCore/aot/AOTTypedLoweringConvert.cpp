/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Toward zero. Of what does not fit: the least or the greatest integer, depending on the processor.
Integer Lowering::truncateToInteger(Number value)
{
    // The back end does not look into a patchpoint.
    if (value.value()->hasDouble() && std::abs(value.value()->asDouble()) < static_cast<double>(maxKeptInteger))
        return integer(static_cast<int64_t>(value.value()->asDouble()));
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(value.value(), ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    m_asDouble.add(patchpoint, value.value());
    return Integer(patchpoint);
}

Number Lowering::toNumber(Integer value)
{
    if (LValue original = m_asDouble.get(value.value()))
        return Number(original);
    return convertToNumber(value);
}

Integer Lowering::bitsOf(Operand operand)
{
    if (b3TypeOf(operand.rep) == Double)
        return Integer(m_out.bitCast(operand.value, Int64));
    RELEASE_ASSERT(b3TypeOf(operand.rep) == Int64);
    return Integer(operand.value);
}

Operand Lowering::fromBits(Integer bits, Rep rep)
{
    if (b3TypeOf(rep) == Double)
        return Operand(rep, m_out.bitCast(bits.value(), Double));
    RELEASE_ASSERT(b3TypeOf(rep) == Int64);
    return Operand(rep, bits.value());
}

static Rep plain(Rep rep)
{
    if (rep == Rep::OptionalInt64)
        return Rep::Int64;
    if (rep == Rep::OptionalDouble)
        return Rep::Double;
    return rep;
}

static constexpr uint16_t markerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };

// What stands for each of markerKinds in a representation.
static std::array<int64_t, 3> markersIn(Rep rep)
{
    switch (rep) {
    case Rep::OptionalInt64:
        return { undefinedInteger, nullInteger, absentInteger };
    case Rep::OptionalDouble:
        return { static_cast<int64_t>(undefinedDouble), static_cast<int64_t>(nullDouble), static_cast<int64_t>(absentDouble) };
    case Rep::JSValue:
        return { JSValue::encode(jsUndefined()), JSValue::encode(jsNull()), JSValue::encode(JSValue()) };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

int64_t Lowering::bitsOfMarker(uint16_t kind, Rep rep)
{
    for (unsigned i = 0; i < std::size(markerKinds); ++i) {
        if (markerKinds[i] == kind)
            return markersIn(rep)[i];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether a value that may be a number is none.
Word32 Lowering::isMarker(Operand operand)
{
    RELEASE_ASSERT(plain(operand.rep) != operand.rep);
    return greaterThanOrEqual(bitsOf(operand), integer(bitsOfMarker(Primitive::Undefined, operand.rep)));
}

// Of what was read from a place: whether the place holds nothing.
Word32 Lowering::isAbsent(Operand raw)
{
    if (LValue index = indexOf(raw.value))
        return Word32(m_out.isZero64(index));
    return equal(bitsOf(raw), integer(bitsOfMarker(Primitive::Absent, raw.rep)));
}

// A double from anywhere as a double of the language. What stands for `undefined` is a double that arithmetic hands on as it is.
Number Lowering::purified(Number value)
{
    return select(equal(value, value), value, number(PNaN));
}

// As the engine boxes a number it has computed: as an int32 where it is one. So what is tried first on a value that is expected to be a
// number, that it is an int32, keeps passing for what a division or a rounding has come out with.
Boxed Lowering::boxNumber(Number value)
{
    Word32 low = low32(truncateToInteger(value));
    // By the bits: negative zero is no int32, nor is what is not a number.
    return select(equal(bitsOf(toNumber(low)), bitsOf(value)), boxInt32(low), box(purified(value)));
}

Word32 Lowering::isWithin(Integer value, Range range)
{
    return logicalAnd(greaterThanOrEqual(value, integer(range.least)), lessThanOrEqual(value, integer(range.most)));
}

// Between representations of which at least one has room for `undefined` and `null` beside a number.
Operand Lowering::convertOptional(Operand operand, const Type& type, Rep to)
{
    Rep from = operand.rep;
    std::optional<Operand> result;
    if (type.primitives & Primitive::Number) {
        // The number, as if it were one.
        Operand converted;
        if (from == Rep::OptionalDouble && to == Rep::JSValue)
            converted = boxNumber(assumedNumber(operand.as<OptionalNumber>()));
        else
            converted = convert(Operand(plain(from), operand.value), type.withoutPrimitives(static_cast<uint16_t>(~Primitive::Number)), plain(to));
        if (from == Rep::Double && to == Rep::OptionalDouble)
            converted = purified(converted.as<Number>());
        result = Operand(to, converted.value);
    }
    if (plain(to) == to && to != Rep::JSValue) {
        if (!result)
            return failWith("a conversion of what is not a number to a number"_s);
        return *result;
    }
    // A plain number has no room for what is not one, whatever the type it is converted for allows.
    bool mayBeMarker = plain(from) != from || from == Rep::JSValue;
    Integer bits;
    for (unsigned i = 0; i < std::size(markerKinds); ++i) {
        if (!(type.primitives & markerKinds[i]))
            continue;
        Operand marker = fromBits(integer(markersIn(to)[i]), to);
        if (!result) {
            result = marker;
            continue;
        }
        if (!mayBeMarker)
            continue;
        if (!bits)
            bits = bitsOf(operand);
        result = Operand(to, m_out.select(equal(bits, integer(markersIn(from)[i])).value(), marker.value, result->value));
    }
    // A value of no type never comes about.
    return result ? *result : fromBits(integer(0), to);
}

// The operand is of the type, in a representation that has room for every value of the type, as has the one asked for. So nothing is tested.
Operand Lowering::convert(Operand operand, const Type& type, Rep to)
{
    Rep from = operand.rep;
    RELEASE_ASSERT(from != Rep::Void && to != Rep::Void);
    if (from == to)
        return operand;
    if (plain(from) != from || plain(to) != to)
        return convertOptional(operand, type, to);
    // 32 bits that are no boolean were read from a field that keeps an integer in less than a word.
    if (from == Rep::Int32 && !(type.primitives & Primitive::Boolean)) {
        // What is not negative comes out the same either way, and a load has cleared the upper half already.
        bool isNotNegative = type.primitives & Primitive::Int && type.integers.least >= 0;
        operand = isNotNegative ? zeroExtended(operand.as<Word32>()) : signExtended(operand.as<Word32>());
        from = Rep::Int64;
        if (to == Rep::Int64)
            return operand;
    }
    switch (from) {
    case Rep::Cell:
        if (to == Rep::JSValue)
            return box(operand.as<CellPointer>());
        break;
    case Rep::Int32:
        if (to == Rep::JSValue)
            return Boxed(boxBoolean(operand.value));
        break;
    case Rep::Int64: {
        Integer value = operand.as<Integer>();
        if (to == Rep::Int32)
            return low32(value);
        if (to == Rep::Double)
            return toNumber(value);
        if (to == Rep::JSValue) {
            Word32 low = low32(value);
            if (type.primitives & Primitive::Int && type.integers.isWithin(Range::int32()))
                return boxInt32(low);
            return select(equal(signExtended(low), value), boxInt32(low), box(toNumber(value)));
        }
        break;
    }
    case Rep::Double: {
        Number value = operand.as<Number>();
        if (to == Rep::Int64)
            return truncateToInteger(value);
        if (to == Rep::Int32)
            return low32(truncateToInteger(value));
        if (to == Rep::JSValue)
            return boxNumber(value);
        break;
    }
    case Rep::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (to == Rep::Cell)
            return asCell(value);
        if (to == Rep::Int32 && type.primitives & Primitive::Boolean)
            return unboxedBoolean(value);
        if (to == Rep::Double)
            return select(isInt32(value), toNumber(low32(value)), unboxedDouble(value));
        Integer unboxed = select(isInt32(value), signExtended(low32(value)), truncateToInteger(unboxedDouble(value)));
        if (to == Rep::Int64)
            return unboxed;
        if (to == Rep::Int32)
            return low32(unboxed);
        break;
    }
    default:
        break;
    }
    return failWith("a conversion between these representations"_s);
}

// What `x | 0` yields.
Word32 Lowering::toInt32(Number value)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->append(ConstrainedValue(value.value(), ValueRep::SomeRegister));
        patchpoint->effects = Effects::none();
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        });
        return Word32(patchpoint);
    }
#endif
    // Of a number that fits in 64 bits the low half is the answer. Where it does not fit, truncation yields the least or the greatest integer.
    Integer truncated = truncateToInteger(value);
    Word32 fits = below(integer(1), add(truncated, integer(std::numeric_limits<int64_t>::min() + 1)));
    return either<Word32>(fits, [&] {
        return low32(truncated);
    }, [&] {
        return AOT_TYPED_OPERATION(ToInt32, value);
    });
}

// Of a value that may be anything. Where it is no number: to the block.
Word32 Lowering::toInt32OrElse(ValueID id, LBasicBlock otherwise)
{
    Operand operand = get(id);
    if (operand.rep != Rep::JSValue)
        return toInt32(id);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isInt32(boxed), [&] {
        return low32(boxed);
    }, [&] {
        orElse(isNumber(boxed), otherwise);
        return toInt32(unboxedDouble(boxed));
    });
}

// Of a number, `undefined`, `null` or a boolean.
Word32 Lowering::toInt32(ValueID id)
{
    // What is not a number becomes 0.
    if (typeOf(id).isUndefinedOrNull())
        return word32(0);
    Operand operand = get(id);
    switch (operand.rep) {
    case Rep::Int64:
        return low32(operand.as<Integer>());
    case Rep::Int32:
        return operand.as<Word32>();
    case Rep::OptionalInt64:
        return low32(select(isMarker(operand), integer(0), assumedInteger(operand.as<OptionalInteger>())));
    default:
        if (LValue phi = m_int32Phis.get(id.index()))
            return Word32(phi);
        if (auto ofSum = toInt32OfSum(id))
            return *ofSum;
        if (auto ofQuotient = toInt32OfQuotient(id))
            return *ofQuotient;
        if (isInlineBuiltin(id, InlineBuiltin::MathTrunc) && m_function.at(id).arguments.size() == 1 && isNumericOperand(typeOf(m_function.at(id).arguments[0])))
            return toInt32(m_function.at(id).arguments[0]);
        return toInt32(asNumericOperand(id));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
