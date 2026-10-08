/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// The type of a result is given. In TypeScript it is the checker's type of the expression, with the integers the checker has shown it to be
// in the place of `number` (src/sema/check/aot/integers.rs in Bun). For that the checker looks at more than the types of the operands:
// `i + 1` behind `i < n` is less than what `i` holds over its whole life would make it. In JavaScript, and for `any`, it is what the
// operator yields whatever it is given. Here the instructions are chosen, by how operands and result are kept.

// Whether integers and doubles alike stand for every number from the one to the other, so that nothing is rounded.
static bool isExact(Int128 least, Int128 most)
{
    return least >= -maxExactInteger && most <= maxExactInteger;
}

static bool isWithinInt32(const Type& type)
{
    return type.isInteger() && type.integers.isWithin(Range::int32());
}

static bool isStringOrNil(const Type& type)
{
    return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
}

// What the stub or the operation of the engine yields, whatever it is given.
static Type guaranteeOfStub(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
        return Type::ofPrimitives(Primitive::Number | Primitive::BigInt | Primitive::String);
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::BitNot: {
        Type type = Type::integer(Range::int32());
        type.primitives |= Primitive::BigInt;
        return type;
    }
    case Opcode::URShift:
        return Type::integer(Range::uint32());
    case Opcode::ToNumber:
        return Type::ofPrimitives(Primitive::Number);
    default:
        return Type::ofPrimitives(Primitive::Number | Primitive::BigInt);
    }
}

// As an operation on numbers takes it: `undefined` is not a number, `null` is zero.
Number Lowering::asNumericOperand(ValueID id)
{
    auto& type = typeOf(id);
    if (type.isNumber())
        return in<Number>(id);
    // What stands for `undefined` among doubles is itself not a number.
    OptionalNumber optional = in<OptionalNumber>(id);
    Number number = assumedNumber(optional);
    if (!(type.primitives & Primitive::Null))
        return number;
    return select(equal(bitsOf(optional), integer(bitsOfMarker(Primitive::Null, Rep::OptionalDouble))), this->number(0), number);
}

Operand Lowering::lowerBinary(ValueID result, const Value& value)
{
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    auto& type = typeOf(result);
    if (isNumericOperand(left) && isNumericOperand(right))
        return lowerNumericBinary(value.opcode, a, b, type);

    OperationEffects effects { writesOfArithmetic(left, &right), true };
    // An object is asked for a primitive first, which is not what it is asked for to become a string.
    if (value.opcode == Opcode::Add && !left.hasObjects() && !right.hasObjects()) {
        if (left.isString() || right.isString()) {
            StringPart first = stringPartOf(a);
            return ensure(concatenate(first, stringPartOf(b)).string, m_program.at(m_program.types.string), type);
        }
        if (isStringOrNil(left) && isStringOrNil(right)) {
            Boxed x = in<Boxed>(a);
            Boxed y = in<Boxed>(b);
            Boxed sum = either<Boxed>(logicalAnd(isCell(x), isCell(y)), [&] {
                return box(concatenate(stringPartOf(asCell(x)), stringPartOf(asCell(y))).string);
            }, [&] {
                return emitGenericArithmetic(Stub::Add, x, y, effects);
            });
            return ensure(sum, guaranteeOfStub(Opcode::Add), type);
        }
    }

    auto generally = [&] {
        Boxed x = in<Boxed>(a);
        Boxed y = in<Boxed>(b);
        Boxed boxed;
        switch (value.opcode) {
#define AOT_TYPED_GENERIC_BINARY(opcode) \
        case Opcode::opcode: \
            boxed = emitGenericArithmetic(Stub::opcode, x, y, effects); \
            break;
        AOT_TYPED_GENERIC_BINARY(Add)
        AOT_TYPED_GENERIC_BINARY(Sub)
        AOT_TYPED_GENERIC_BINARY(Mul)
        AOT_TYPED_GENERIC_BINARY(Div)
        AOT_TYPED_GENERIC_BINARY(Mod)
        AOT_TYPED_GENERIC_BINARY(BitAnd)
        AOT_TYPED_GENERIC_BINARY(BitOr)
        AOT_TYPED_GENERIC_BINARY(BitXor)
        AOT_TYPED_GENERIC_BINARY(LShift)
        AOT_TYPED_GENERIC_BINARY(RShift)
        AOT_TYPED_GENERIC_BINARY(URShift)
#undef AOT_TYPED_GENERIC_BINARY
        case Opcode::Pow:
            boxed = AOT_TYPED_OPERATION_WITH_EFFECTS(Exp, effects, x, y);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return ensure(boxed, guaranteeOfStub(value.opcode), type);
    };

    Rep rep = repOf(type);
    if (areExpectedToBeNumbers({ a, b })) {
        bool hasCodeForInt32 = value.opcode != Opcode::Div && value.opcode != Opcode::Pow;
        return ifNumbersAsExpected({ a, b }, rep, hasCodeForInt32, [&](LBasicBlock otherwise) {
            return lowerBinaryOfInt32(value.opcode, a, b, type, otherwise);
        }, [&] {
            // These often come out whole where the operands are.
            if ((value.opcode == Opcode::Div || value.opcode == Opcode::Mod || value.opcode == Opcode::Pow) && rep == Rep::JSValue) {
                auto& numberType = m_program.at(m_program.types.number);
                return ensure(boxNumber(lowerNumericBinary(value.opcode, a, b, numberType).as<Number>()), numberType, type);
            }
            return lowerNumericBinary(value.opcode, a, b, type);
        }, generally);
    }

    // A string and a string, or a string and a number, which as a rule is an int32 where it is made part of a string.
    if (value.opcode == Opcode::Add && (predictionOf(a) || predictionOf(b))) {
        auto whatToExpectOf = [&](ValueID id) -> const Type* {
            auto prediction = predictionOf(id);
            if (!prediction)
                return typeOf(id).isString() || typeOf(id).isNumber() ? &typeOf(id) : nullptr;
            if (prediction->isString())
                return &m_program.at(m_program.types.string);
            if (prediction->isNumber())
                return &m_program.at(m_program.types.int32);
            return nullptr;
        };
        const Type* first = whatToExpectOf(a);
        const Type* second = whatToExpectOf(b);
        if (first && second && (first->isString() || second->isString())) {
            return ifAsExpected({ { a, first }, { b, second } }, rep, [&](LBasicBlock) {
                return lowerBinary(result, value);
            }, generally);
        }
    }
    return generally();
}

// An integer that 64 bits have room for, as the result of an operation on int32. Where it is boxed and is no int32 itself: to the block,
// so that what goes on from here does not wait for the double it would be boxed as.
Operand Lowering::yieldInt32OrElse(Integer exact, const Type& type, LBasicBlock otherwise)
{
    if (repOf(type) != Rep::JSValue)
        return ensure(exact, Type::integer(Range::full()), type);
    orElse(equal(signExtended(low32(exact)), exact), otherwise);
    return ensure(exact, m_program.at(m_program.types.int32), type);
}

// Both operands are int32. What integers do not come out with the way the language has it goes to the block.
Operand Lowering::lowerBinaryOfInt32(Opcode opcode, ValueID a, ValueID b, const Type& type, LBasicBlock otherwise)
{
    switch (opcode) {
    case Opcode::Add:
        return yieldInt32OrElse(add(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Sub:
        return yieldInt32OrElse(sub(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Mul: {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        Integer product = mul(x, y);
        // Zero is negative where one of the factors is.
        orElse(logicalOr(isNotZero(product), greaterThanOrEqual(bitOr(x, y), integer(0))), otherwise);
        return yieldInt32OrElse(product, type, otherwise);
    }
    case Opcode::Mod: {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        // The remainder has the sign of what is divided even where it is zero, and there is none of a division by zero.
        orElse(logicalAnd(isNotZero(y), greaterThanOrEqual(x, integer(0))), otherwise);
        return ensure(mod(x, y), m_program.at(m_program.types.int32), type);
    }
    default:
        return lowerNumericBinary(opcode, a, b, type);
    }
}

// An int32, or for `>>>` a uint32, as an integer.
Integer Lowering::lowerBitwise(Opcode opcode, ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    // An int32 is kept with its sign extended, and these operations on all 64 bits leave it so.
    if (isWithinInt32(left) && isWithinInt32(right)) {
        switch (opcode) {
        case Opcode::BitAnd:
            return bitAnd(get(a).as<Integer>(), get(b).as<Integer>());
        case Opcode::BitOr:
            return bitOr(get(a).as<Integer>(), get(b).as<Integer>());
        case Opcode::BitXor:
            return bitXor(get(a).as<Integer>(), get(b).as<Integer>());
        default:
            break;
        }
    }
    // A mask that is not negative clears the upper half of whatever it meets.
    auto isMask = [](const Type& type) {
        return type.isInteger() && type.integers.isWithin({ 0, INT32_MAX });
    };
    if (opcode == Opcode::BitAnd && left.isInteger() && right.isInteger() && (isMask(left) || isMask(right)))
        return bitAnd(get(a).as<Integer>(), get(b).as<Integer>());
    // The back end takes the amount of a shift modulo the width of what is shifted.
    if (opcode == Opcode::RShift && isWithinInt32(left))
        return Integer(m_out.aShr(get(a).as<Integer>().value(), bitAnd(toInt32(b), word32(31)).value()));

    Word32 x = toInt32(a);
    Word32 y = toInt32(b);
    switch (opcode) {
    case Opcode::BitAnd:
        return signExtended(bitAnd(x, y));
    case Opcode::BitOr:
        return signExtended(bitOr(x, y));
    case Opcode::BitXor:
        return signExtended(bitXor(x, y));
    case Opcode::LShift:
        return signExtended(shiftLeft(x, y));
    case Opcode::RShift:
        return signExtended(shiftRight(x, y));
    case Opcode::URShift:
        return zeroExtended(shiftRightUnsigned(x, y));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A number, `undefined` or `null` by the type it is given, and not only where a test for what is expected of it has passed.
bool Lowering::isNumericByItsType(ValueID id) const
{
    return isNumericOperand(m_program.at(m_function.at(id).type));
}

// What an operation on numbers has computed, which by itself is of the type `guarantee`, as its result. Where the operands are numbers by
// their types the type of the result is a fact, also where it says more than the operation does by itself: nothing is tested. That is
// about which numbers come out. A type that has no number was given for other operands than are seen now: `s + 1`, with `null` for `s`.
Operand Lowering::yieldNumeric(Operand computed, const Type& guarantee, const Type& type, bool typeIsFact)
{
    if (!typeIsFact || !(type.primitives & Primitive::Number) || guarantee.isWithin(type, m_program))
        return ensure(computed, guarantee, type);
    return convert(computed, type, repOf(type));
}

// The operands are numbers, `undefined` or `null`.
Operand Lowering::lowerNumericBinary(Opcode opcode, ValueID a, ValueID b, const Type& type)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    bool typeIsFact = isNumericByItsType(a) && isNumericByItsType(b);
    switch (opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        return yieldNumeric(lowerBitwise(opcode, a, b), m_program.at(m_program.types.int32), type, typeIsFact);
    case Opcode::URShift:
        return yieldNumeric(lowerBitwise(opcode, a, b), Type::integer(Range::uint32()), type, typeIsFact);
    default:
        break;
    }

    if (left.isInteger() && right.isInteger()) {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        Int128 leastOfLeft = left.integers.least;
        Int128 mostOfLeft = left.integers.most;
        Int128 leastOfRight = right.integers.least;
        Int128 mostOfRight = right.integers.most;
        // The result is an integer for a fact, so no zero comes out negative. Up to 2^53 it is what integers come out with: had anything
        // been rounded, it would be beyond. At 2^53 itself one thing is rounded, 2^53 + 1, back to 2^53.
        bool yieldsInteger = typeIsFact && type.isInteger();
        bool yieldsExactOrLimit = yieldsInteger && isExact(type.integers.least, type.integers.most);
        bool yieldsExact = yieldsExactOrLimit && type.integers.least > -maxExactInteger && type.integers.most < maxExactInteger;
        auto withinLimits = [&](Integer sum) {
            if (type.integers.most == maxExactInteger)
                sum = select(greaterThan(sum, integer(maxExactInteger)), integer(maxExactInteger), sum);
            if (type.integers.least == -maxExactInteger)
                sum = select(lessThan(sum, integer(-maxExactInteger)), integer(-maxExactInteger), sum);
            return sum;
        };
        // On integers where nothing is rounded and no zero comes out negative, going by the operands or by the result.
        std::optional<Integer> exact;
        switch (opcode) {
        case Opcode::Add:
            if (isExact(leastOfLeft + leastOfRight, mostOfLeft + mostOfRight))
                exact = add(x, y);
            else if (yieldsExactOrLimit)
                exact = withinLimits(add(x, y));
            break;
        case Opcode::Sub:
            if (isExact(leastOfLeft - mostOfRight, mostOfLeft - leastOfRight))
                exact = sub(x, y);
            else if (yieldsExactOrLimit)
                exact = withinLimits(sub(x, y));
            break;
        case Opcode::Mul:
            if (yieldsExact || (leastOfLeft >= 0 && leastOfRight >= 0 && isExact(0, mostOfLeft * mostOfRight)))
                exact = mul(x, y);
            break;
        case Opcode::Mod:
            if (right.integers.contains(0))
                break;
            exact = mod(x, y);
            if (leastOfLeft < 0 && !yieldsInteger) {
                // The remainder has the sign of what is divided even where it is zero.
                Word32 isNegativeZero = logicalAnd(isZero(*exact), lessThan(x, integer(0)));
                return ensure(select(isNegativeZero, number(-0.0), convertToNumber(*exact)), m_program.at(m_program.types.number), type);
            }
            break;
        default:
            break;
        }
        if (exact)
            return convert(*exact, type, repOf(type));
    }

    Number x = asNumericOperand(a);
    Number y = asNumericOperand(b);
    Number computed;
    switch (opcode) {
    case Opcode::Add:
        computed = add(x, y);
        break;
    case Opcode::Sub:
        computed = sub(x, y);
        break;
    case Opcode::Mul:
        computed = mul(x, y);
        break;
    case Opcode::Div:
        computed = div(x, y);
        break;
    case Opcode::Mod:
        computed = AOT_TYPED_OPERATION(Fmod, x, y);
        break;
    case Opcode::Pow:
        computed = AOT_TYPED_OPERATION(Pow, x, y);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return yieldNumeric(computed, m_program.at(m_program.types.number), type, typeIsFact);
}

Operand Lowering::lowerUnary(ValueID result, const Value& value)
{
    ValueID a = value.child(0);
    auto& given = typeOf(a);
    auto& type = typeOf(result);
    auto& numberType = m_program.at(m_program.types.number);
    if (isNumericOperand(given)) {
        bool typeIsFact = isNumericByItsType(a);
        switch (value.opcode) {
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
            if (given.isNumber())
                return yieldNumeric(get(a), given, type, typeIsFact);
            return yieldNumeric(asNumericOperand(a), numberType, type, typeIsFact);
        case Opcode::Negate:
            // Zero negated is negative zero, which no integer stands for: where the result is an integer for a fact, it is not zero that is negated.
            if (given.isInteger() && (!given.integers.contains(0) || (typeIsFact && type.isInteger())))
                return convert(neg(get(a).as<Integer>()), type, repOf(type));
            return yieldNumeric(neg(asNumericOperand(a)), numberType, type, typeIsFact);
        case Opcode::BitNot: {
            Integer inverted = isWithinInt32(given) ? bitXor(get(a).as<Integer>(), integer(-1)) : signExtended(bitXor(toInt32(a), word32(-1)));
            return yieldNumeric(inverted, m_program.at(m_program.types.int32), type, typeIsFact);
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    Boxed operand = in<Boxed>(a);
    auto generally = [&] {
        OperationEffects effects { writesOfArithmetic(given, nullptr), true };
        Type guarantee = guaranteeOfStub(value.opcode);
        switch (value.opcode) {
        case Opcode::ToNumber:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(ToNumber, effects, operand), guarantee, type);
        case Opcode::ToNumeric:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(ToNumeric, effects, operand), guarantee, type);
        case Opcode::Negate:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(Neg, effects, operand), guarantee, type);
        case Opcode::BitNot:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(BitNot, effects, operand), guarantee, type);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    if (!areExpectedToBeNumbers({ a }))
        return generally();
    Rep rep = repOf(type);
    // A number is what comes of it, as it is.
    if ((value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric) && rep == Rep::JSValue) {
        return eitherOperand(isNumber(operand), rep, [&] {
            return ensure(operand, numberType, type);
        }, generally);
    }
    return ifNumbersAsExpected({ a }, rep, true, [&](LBasicBlock otherwise) {
        if (value.opcode != Opcode::Negate)
            return lowerUnary(result, value);
        // Zero negated is negative zero, which no integer stands for.
        Integer x = get(a).as<Integer>();
        orElse(isNotZero(x), otherwise);
        return yieldInt32OrElse(neg(x), type, otherwise);
    }, [&] {
        return lowerUnary(result, value);
    }, generally);
}

Word32 Lowering::lowerRelational(const Value& value)
{
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (left.isInteger() && right.isInteger()) {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        if (left.integers.least >= 0 && right.integers.least >= 0) {
            // The form that tests of an index against a length have, so that one of the two is seen to repeat the other.
            switch (value.opcode) {
            case Opcode::Less:
                return below(x, y);
            case Opcode::Greater:
                return below(y, x);
            case Opcode::LessEq:
                return belowOrEqual(x, y);
            default:
                return belowOrEqual(y, x);
            }
        }
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    }
    if (isNumericOperand(left) && isNumericOperand(right)) {
        Number x = asNumericOperand(a);
        Number y = asNumericOperand(b);
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    }
    auto generally = [&] {
        Boxed x = in<Boxed>(a);
        Boxed y = in<Boxed>(b);
        OperationEffects effects { writesOfArithmetic(left, &right), true };
        switch (value.opcode) {
        case Opcode::Less:
            return emitGenericCompare(Stub::Less, x, y, effects);
        case Opcode::Greater:
            return emitGenericCompare(Stub::Greater, x, y, effects);
        case Opcode::LessEq:
            return emitGenericCompare(Stub::LessEq, x, y, effects);
        default:
            return emitGenericCompare(Stub::GreaterEq, x, y, effects);
        }
    };
    if (!areExpectedToBeNumbers({ a, b }))
        return generally();
    return ifNumbersAsExpected({ a, b }, Rep::Int32, true, [&](LBasicBlock) {
        return lowerRelational(value);
    }, [&] {
        return lowerRelational(value);
    }, generally).as<Word32>();
}

Word32 Lowering::lowerStrictEq(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (left.isInteger() && right.isInteger())
        return equal(get(a).as<Integer>(), get(b).as<Integer>());
    if (left.isBoolean() && right.isBoolean())
        return equal(get(a).as<Word32>(), get(b).as<Word32>());
    if (left.isNumber() && right.isNumber())
        return equal(in<Number>(a), in<Number>(b));
    uint16_t either = left.primitives | right.primitives;
    uint16_t both = left.primitives & right.primitives;
    if (isNumericOperand(left) && isNumericOperand(right) && either & Primitive::Number) {
        if (!(either & Primitive::Double))
            return equal(in<OptionalInteger>(a), in<OptionalInteger>(b));
        OptionalNumber x = in<OptionalNumber>(a);
        OptionalNumber y = in<OptionalNumber>(b);
        Word32 hasSameBits = equal(bitsOf(x), bitsOf(y));
        if (!(both & Primitive::Number))
            return hasSameBits;
        Word32 isEqual = equal(assumedNumber(x), assumedNumber(y));
        if (!(both & Primitive::UndefinedOrNull))
            return isEqual;
        return logicalOr(isEqual, logicalAnd(hasSameBits, isMarker(x)));
    }
    // Equal bits are equal values and the other way round, unless both may be numbers, strings or big integers.
    if (!(both & (Primitive::Number | Primitive::String | Primitive::BigInt)))
        return equal(in<Boxed>(a), in<Boxed>(b));
    if (isStringOrNil(left) && isStringOrNil(right))
        return areStringsEqual(a, b);
    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    // Whether two values are the same runs no code of the program. At most a rope is made into a string.
    auto generally = [&] {
        return emitGenericCompare(Stub::StrictEqual, x, y, OperationEffects { Writes::RuntimeState, true });
    };
    auto inPlace = [&](LBasicBlock) {
        return lowerStrictEq(a, b);
    };
    if (areExpectedToBeNumbers({ a, b })) {
        return ifNumbersAsExpected({ a, b }, Rep::Int32, true, inPlace, [&] {
            return lowerStrictEq(a, b);
        }, generally).as<Word32>();
    }
    if (expectedTypeOf(a).isString() && expectedTypeOf(b).isString()) {
        auto& stringType = m_program.at(m_program.types.string);
        return ifAsExpected({ { a, &stringType }, { b, &stringType } }, Rep::Int32, inPlace, generally).as<Word32>();
    }
    // An object or a boolean is the same as nothing but itself, so it takes a look at one of the two.
    for (ValueID id : { a, b }) {
        auto prediction = predictionOf(id);
        if (!prediction || !(prediction->isObject() || prediction->isBoolean()))
            continue;
        auto& kind = m_program.at(prediction->isObject() ? m_program.types.object : m_program.types.boolean);
        return ifAsExpected({ { id, &kind } }, Rep::Int32, inPlace, generally).as<Word32>();
    }
    return generally();
}

Word32 Lowering::lowerLooseEq(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    // Between values of one kind nothing is converted.
    if ((left.isNumber() && right.isNumber()) || (left.isString() && right.isString()) || (left.isBoolean() && right.isBoolean()) || (left.isObject() && right.isObject()))
        return lowerStrictEq(a, b);
    auto generally = [&] {
        Boxed x = in<Boxed>(a);
        return emitGenericCompare(Stub::LooseEqual, x, in<Boxed>(b), OperationEffects { writesOfArithmetic(left, &right), true });
    };
    auto inPlace = [&](LBasicBlock) {
        return lowerStrictEq(a, b);
    };
    if (areExpectedToBeNumbers({ a, b }) && expectedTypeOf(a).isNumber() && expectedTypeOf(b).isNumber()) {
        return ifNumbersAsExpected({ a, b }, Rep::Int32, true, inPlace, [&] {
            return lowerStrictEq(a, b);
        }, generally).as<Word32>();
    }
    if (!predictionOf(a) && !predictionOf(b))
        return generally();
    Type first = expectedTypeOf(a);
    Type second = expectedTypeOf(b);
    for (TypeID kind : { m_program.types.string, m_program.types.boolean, m_program.types.object }) {
        auto& type = m_program.at(kind);
        if (first.isWithin(type, m_program) && second.isWithin(type, m_program))
            return ifAsExpected({ { a, &type }, { b, &type } }, Rep::Int32, inPlace, generally).as<Word32>();
    }
    return generally();
}

// Of a value whose type leaves open what decides. What is expected says what to look for, and in which order.
Word32 Lowering::toBooleanGenerally(ValueID id)
{
    Boxed value = in<Boxed>(id);
    auto throughStub = [&] {
        return Word32(callICStub(Stub::ToBoolean, Int32, { { value.value(), firstStubOperandGPR } }, std::nullopt, { Writes::RuntimeState, false }));
    };
    // `undefined` and `null` are an answer like any other here.
    auto expected = predictionOf(id, 0);
    if (!expected)
        return throughStub();

    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    // Goes on where the condition does not hold.
    auto when = [&](Word32 condition, const auto& answer) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer());
        m_out.appendTo(next);
    };
    if (expected->hasCells()) {
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isOtherCase = m_out.newBlock();
        m_out.branch(isCell(value).value(), unsure(isCellCase), unsure(isOtherCase));
        m_out.appendTo(isCellCase);
        CellPointer cell = asCell(value);
        Word32 cellType = typeOfCell(cell);
        if (expected->hasObjects()) {
            when(logicalAnd(aboveOrEqual(cellType, word32(ObjectType)), lacksBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined)), [&] {
                return boolean(true);
            });
        }
        // There is one empty string.
        if (expected->primitives & Primitive::String) {
            when(equal(cellType, word32(StringType)), [&] {
                return notEqual(cell, CellPointer(emptyString()));
            });
        }
        m_out.jump(otherwise);
        m_out.appendTo(isOtherCase);
    }
    if (expected->primitives & Primitive::Boolean) {
        when(isBoolean(value), [&] {
            return unboxedBoolean(value);
        });
    }
    if (expected->primitives & Primitive::Number) {
        when(isInt32(value), [&] {
            return notEqual(low32(value), word32(0));
        });
        // What is left of the numbers is kept as a double.
        when(isNumber(value), [&] {
            return greaterThan(abs(unboxedDouble(value)), number(0));
        });
    }
    if (expected->primitives & Primitive::UndefinedOrNull) {
        when(isUndefinedOrNull(value), [&] {
            return boolean(false);
        });
    }
    m_out.jump(otherwise);
    m_out.appendTo(otherwise);
    join.add(throughStub());
    return join.finish().as<Word32>();
}

CellPointer Lowering::toStringCell(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    if (type.isString())
        return operand.as<CellPointer>();
    if (operand.rep == Rep::Int64)
        return integerToString(operand.as<Integer>(), type);
    if (operand.rep == Rep::Double)
        return AOT_TYPED_OPERATION(DoubleToString, operand.as<Number>());
    Boxed boxed = convertTo<Boxed>(operand, type);
    auto generally = [&] {
        // Only an object has a say in what string it becomes.
        return AOT_TYPED_OPERATION_WITH_EFFECTS(ToString, (OperationEffects { type.hasObjects() ? Writes::Heap : Writes::RuntimeState, true }), boxed);
    };
    if (isStringOrNil(type)) {
        return either<CellPointer>(isCell(boxed), [&] {
            return asCell(boxed);
        }, generally);
    }
    auto prediction = predictionOf(id);
    if (!prediction || !(prediction->isString() || prediction->isNumber()))
        return generally();
    // As a rule a number is an int32 where it is made a string of.
    auto& expected = m_program.at(prediction->isString() ? m_program.types.string : m_program.types.int32);
    return ifAsExpected({ { id, &expected } }, Rep::Cell, [&](LBasicBlock) {
        return toStringCell(id);
    }, generally).as<CellPointer>();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
