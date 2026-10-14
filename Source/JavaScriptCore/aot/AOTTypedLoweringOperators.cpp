/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3StackmapGenerationParams.h"
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

// A count does not pass 2^53: what is added to it or taken from it at a time.
static bool isStepOfCount(const Type& type)
{
    constexpr int64_t maxStep = 1024;
    return type.integers.least >= -maxStep && type.integers.most <= maxStep;
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

// The same, given what the operands are.
static Type guaranteeOfStub(Opcode opcode, const Type& left, const Type& right)
{
    Type guarantee = guaranteeOfStub(opcode);
    switch (opcode) {
    case Opcode::Add:
        // With a string the sum is a string, whatever the other becomes.
        if (left.isString() || right.isString())
            return Type::ofPrimitives(Primitive::String);
        break;
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        // A big integer and a number do not go together: that throws.
        if (!left.isNumber() && !right.isNumber())
            break;
        guarantee.primitives &= ~Primitive::BigInt;
        if (opcode != Opcode::BitAnd)
            break;
        for (const Type* mask : { &left, &right }) {
            if (mask->isInteger() && mask->integers.least >= 0 && mask->integers.most <= guarantee.integers.most)
                guarantee.integers = Range { 0, mask->integers.most };
        }
        break;
    default:
        break;
    }
    return guarantee;
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
    if (value.opcode == Opcode::Add && !left.hasObjects() && !right.hasObjects() && !isCompact(4) && !goesGenerallyWhereTextIsCold()) {
        bool isColdByText = !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id);
        if (left.isString() || right.isString()) {
            if (isColdByText) {
                CellPointer first = toStringCell(a);
                return ensure(concatenateByStub(first, toStringCell(b)), m_program.at(m_program.types.string), type);
            }
            StringPart first = stringPartOf(a);
            return ensure(concatenate(first, stringPartOf(b)).string, m_program.at(m_program.types.string), type);
        }
        if (isStringOrNil(left) && isStringOrNil(right)) {
            Boxed x = in<Boxed>(a);
            Boxed y = in<Boxed>(b);
            bool areWhole = !indexOf(x.value()) && !indexOf(y.value()) && !m_decoded.contains(x.value()) && !m_decoded.contains(y.value());
            Word32 areCells = areWhole ? Word32(m_out.testIsZero64(m_out.bitOr(x.value(), y.value()), m_out.constInt64(JSValue::OtherTag))) : logicalAnd(isCell(x, left), isCell(y, right));
            return eitherOperand(areCells, joinedAs(type), [&] {
                if (isColdByText)
                    return ensure(concatenateByStub(asCell(x), asCell(y)), m_program.at(m_program.types.string), type);
                return ensure(concatenate(stringPartOf(asCell(x)), stringPartOf(asCell(y))).string, m_program.at(m_program.types.string), type);
            }, [&] {
                return ensure(emitGenericArithmetic(Stub::Add, x, y, effects), guaranteeOfStub(Opcode::Add), type);
            });
        }
        // With a string the other, which is no object, becomes one.
        if (isStringOrNil(left) || isStringOrNil(right)) {
            bool isLeft = isStringOrNil(left);
            Boxed x = in<Boxed>(a);
            Boxed y = in<Boxed>(b);
            return eitherOperand(isCell(isLeft ? x : y, isLeft ? left : right), joinedAs(type), [&] {
                if (isColdByText) {
                    CellPointer string = asCell(isLeft ? x : y);
                    CellPointer other = toStringCell(isLeft ? b : a);
                    return ensure(isLeft ? concatenateByStub(string, other) : concatenateByStub(other, string), m_program.at(m_program.types.string), type);
                }
                StringPart string = stringPartOf(asCell(isLeft ? x : y));
                StringPart other = stringPartOf(isLeft ? b : a);
                return ensure((isLeft ? concatenate(string, other) : concatenate(other, string)).string, m_program.at(m_program.types.string), type);
            }, [&] {
                return ensure(emitGenericArithmetic(Stub::Add, x, y, effects), guaranteeOfStub(Opcode::Add), type);
            });
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
        // The engine's `>>>` yields the 32 bits as an int32: its bytecode makes them unsigned by an operation of its own.
        if (value.opcode == Opcode::URShift)
            return ensure(zeroExtended(low32(boxed)), guaranteeOfStub(value.opcode), type);
        Type guarantee = guaranteeOfStub(value.opcode, left, right);
        // Without a big integer what an operation on bits yields is an int32, and is boxed as one: no need to ask whether it is a double.
        if (guarantee.isInteger())
            return ensure(signExtended(low32(boxed)), guarantee, type);
        return ensure(boxed, guarantee, type);
    };

    if (goesGenerallyWhereTextIsCold())
        return generally();
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

Word32 Lowering::int32OrElse(Opcode opcode, Word32 x, Word32 y, LBasicBlock otherwise, const char* file, unsigned line)
{
    if (dropsGuardAt(file, line)) [[unlikely]]
        return opcode == Opcode::Add ? add(x, y) : opcode == Opcode::Sub ? Word32(m_out.sub(x.value(), y.value())) : mul(x, y);
    if (!m_isValidatingFact && isForcedForTesting(file, line)) [[unlikely]]
        Emitter::orElse(m_out.constInt32(0), otherwise);
    auto constantOf = [](LValue value) -> std::optional<int32_t> {
        if (value->hasInt32())
            return value->asInt32();
        if (value->opcode() == B3::Trunc && value->child(0)->hasInt64())
            return static_cast<int32_t>(value->child(0)->asInt64());
        return std::nullopt;
    };
    if (opcode == Opcode::Add && constantOf(x.value()))
        std::swap(x, y);
    std::optional<int32_t> constant = opcode == Opcode::Mul ? std::nullopt : constantOf(y.value());
    if (constant && (*constant <= -4096 || *constant >= 4096))
        constant = std::nullopt;
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* patchpoint = m_out.patchpoint(Int32);
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(x.value(), ValueRep::SomeRegister));
    if (!constant)
        patchpoint->append(ConstrainedValue(y.value(), opcode == Opcode::Sub ? ValueRep::SomeLateRegister : ValueRep::SomeRegister));
    m_out.appendSuccessor(FTL::rarely(otherwise));
    m_out.appendSuccessor(usually(continuation));
    patchpoint->setGenerator([opcode, constant](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg result = params[0].gpr();
        GPRReg left = params[1].gpr();
        RELEASE_ASSERT(opcode != Opcode::Sub || constant || result != params[2].gpr());
        CCallHelpers::Jump isNoInt32;
        if (opcode == Opcode::Add)
            isNoInt32 = constant ? jit.branchAdd32(CCallHelpers::Overflow, left, CCallHelpers::TrustedImm32(*constant), result) : jit.branchAdd32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        else if (opcode == Opcode::Sub)
            isNoInt32 = constant ? jit.branchSub32(CCallHelpers::Overflow, left, CCallHelpers::TrustedImm32(*constant), result) : jit.branchSub32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        else
            isNoInt32 = jit.branchMul32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        CCallHelpers::Jump isInt32;
        if (!params.fallsThroughToSuccessor(1))
            isInt32 = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            isNoInt32.linkTo(*labels[0], &jit);
            if (isInt32.isSet())
                isInt32.linkTo(*labels[1], &jit);
        });
    });
    m_out.appendTo(continuation);
    return Word32(patchpoint);
}

// Both operands are int32. What integers do not come out with the way the language has it goes to the block.
Operand Lowering::lowerBinaryOfInt32(Opcode opcode, ValueID a, ValueID b, const Type& type, LBasicBlock otherwise)
{
    bool isBoxed = repOf(type) == Rep::JSValue;
    switch (opcode) {
    case Opcode::Add:
        if (isBoxed)
            return ensure(signExtended(int32OrElse(opcode, low32(get(a).as<Integer>()), low32(get(b).as<Integer>()), otherwise)), m_program.at(m_program.types.int32), type);
        return yieldInt32OrElse(add(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Sub:
        if (isBoxed)
            return ensure(signExtended(int32OrElse(opcode, low32(get(a).as<Integer>()), low32(get(b).as<Integer>()), otherwise)), m_program.at(m_program.types.int32), type);
        return yieldInt32OrElse(sub(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Mul: {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        if (isBoxed) {
            Word32 product = int32OrElse(opcode, low32(x), low32(y), otherwise);
            LBasicBlock isZeroCase = newUnlikelyBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(isZero(product).value(), rarely(isZeroCase), usually(continuation));
            m_out.appendTo(isZeroCase);
            orElse(greaterThanOrEqual(bitOr(low32(x), low32(y)), word32(0)), otherwise);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return ensure(signExtended(product), m_program.at(m_program.types.int32), type);
        }
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
    case Opcode::BitAnd: {
        // A mask that is not negative bounds what comes out.
        Range range = Range::int32();
        for (const Type* operand : { &left, &right }) {
            if (operand->isInteger() && operand->integers.isWithin({ 0, INT32_MAX }))
                range = { 0, std::min(range.most, operand->integers.most) };
        }
        return yieldNumeric(lowerBitwise(opcode, a, b), Type::integer(range), type, typeIsFact);
    }
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
                sum = clampToMaximum(sum, maxExactInteger);
            if (type.integers.least == -maxExactInteger)
                sum = clampToMinimum(sum, -maxExactInteger);
            return sum;
        };
        // On integers where nothing is rounded and no zero comes out negative, going by the operands or by the result.
        std::optional<Integer> exact;
        switch (opcode) {
        case Opcode::Add:
            if (isExact(leastOfLeft + leastOfRight, mostOfLeft + mostOfRight))
                exact = add(x, y);
            else if (yieldsExactOrLimit)
                exact = isStepOfCount(left) || isStepOfCount(right) ? add(x, y) : withinLimits(add(x, y));
            break;
        case Opcode::Sub:
            if (isExact(leastOfLeft - mostOfRight, mostOfLeft - leastOfRight))
                exact = sub(x, y);
            else if (yieldsExactOrLimit)
                exact = isStepOfCount(right) ? sub(x, y) : withinLimits(sub(x, y));
            break;
        case Opcode::Mul:
            if (yieldsExact || (leastOfLeft >= 0 && leastOfRight >= 0 && isExact(0, mostOfLeft * mostOfRight)))
                exact = mul(x, y);
            break;
        case Opcode::Mod: {
            auto remainder = [&]() -> Number {
                Integer whole = mod(x, y);
                if (leastOfLeft >= 0 || yieldsInteger)
                    return convertToNumber(whole);
                // The remainder has the sign of what is divided even where it is zero.
                return select(logicalAnd(isZero(whole), lessThan(x, integer(0))), number(-0.0), convertToNumber(whole));
            };
            // There is no remainder of a division by zero.
            if (right.integers.contains(0)) {
                return ensure(either<Number>(isNotZero(y), remainder, [&] {
                    return number(PNaN);
                }), m_program.at(m_program.types.number), type);
            }
            if (leastOfLeft < 0 && !yieldsInteger)
                return ensure(remainder(), m_program.at(m_program.types.number), type);
            bool isMask = leastOfLeft >= 0 && leastOfRight == mostOfRight && leastOfRight > 0 && hasOneBitSet(static_cast<uint64_t>(right.integers.least));
            exact = isMask ? bitAnd(x, integer(right.integers.least - 1)) : mod(x, y);
            break;
        }
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
        computed = remainder(a, b);
        break;
    case Opcode::Pow:
        if (auto power = powerWithConstantExponent(x, b))
            computed = *power;
        else if (auto power = powerWithConstantBase(a, b))
            computed = *power;
        else
            computed = AOT_TYPED_OPERATION(Pow, x, y);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return yieldNumeric(computed, m_program.at(m_program.types.number), type, typeIsFact);
}

// The number as an int32 that is not negative. Whatever else it is, negative zero too: to the block.
Word32 Lowering::naturalInt32OrElse(Number number, LBasicBlock otherwise)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        // The conversion says whether it was exact, which that of negative zero is not. Where it was not: -1.
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->append(ConstrainedValue(number.value(), ValueRep::SomeRegister));
        patchpoint->effects = Effects::none();
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::NaturalInt32);
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            jit.m_assembler.csinv<32>(params[0].gpr(), params[0].gpr(), ARM64Registers::zr, ARM64Assembler::ConditionEQ);
        });
        Word32 converted(patchpoint);
        orElse(greaterThanOrEqual(converted, word32(0)), otherwise);
        return converted;
    }
#endif
    Word32 narrow = low32(truncateToInteger(number));
    // By the bits: negative zero is none, nor is what is not a number.
    orElse(equal(bitsOf(toNumber(narrow)), bitsOf(number)), otherwise);
    orElse(greaterThanOrEqual(narrow, word32(0)), otherwise);
    return narrow;
}

// fmod() looks at nothing and calls nobody. Around its call what the C ABI lets it change and is in use at the site is kept, and nothing else: no frame is needed for it.
Number Lowering::fmodOffTheUsualPath(Number x, Number y)
{
#if CPU(ARM64)
    PatchpointValue* patchpoint = m_out.patchpoint(Double);
    patchpoint->append(ConstrainedValue(x.value(), ValueRep::reg(FPRInfo::argumentFPR0)));
    patchpoint->append(ConstrainedValue(y.value(), ValueRep::reg(FPRInfo::argumentFPR1)));
    patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    patchpoint->effects = Effects::none();
    patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::Fmod);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        Vector<Reg, 8> kept;
        RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
            if (reg != Reg(FPRInfo::returnValueFPR))
                kept.append(reg);
        });
        unsigned size = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(kept.size() * sizeof(Register));
        jit.pushPair(GPRInfo::callFrameRegister, CCallHelpers::linkRegister);
        if (size)
            jit.subPtr(CCallHelpers::TrustedImm32(size), CCallHelpers::stackPointerRegister);
        for (unsigned i = 0; i < kept.size(); ++i) {
            CCallHelpers::Address slot(CCallHelpers::stackPointerRegister, i * sizeof(Register));
            kept[i].isGPR() ? jit.store64(kept[i].gpr(), slot) : jit.storeDouble(kept[i].fpr(), slot);
        }
        GPRReg target = jit.scratchRegister();
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfRuntimeTable()), target);
        jit.loadPtr(CCallHelpers::Address(target, static_cast<unsigned>(Entry::operationAOTTypedFmod) * sizeof(void*)), target);
        jit.call(target, OperationPtrTag);
        for (unsigned i = 0; i < kept.size(); ++i) {
            CCallHelpers::Address slot(CCallHelpers::stackPointerRegister, i * sizeof(Register));
            kept[i].isGPR() ? jit.load64(slot, kept[i].gpr()) : jit.loadDouble(slot, kept[i].fpr());
        }
        if (size)
            jit.addPtr(CCallHelpers::TrustedImm32(size), CCallHelpers::stackPointerRegister);
        jit.popPair(GPRInfo::callFrameRegister, CCallHelpers::linkRegister);
    });
    return Number(patchpoint);
#else
    return AOT_TYPED_OPERATION(Fmod, x, y);
#endif
}

// `x % y` on numbers. It has the sign of what is divided, also where it is zero.
Number Lowering::remainder(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    Number x = asNumericOperand(a);
    Number y = asNumericOperand(b);
    bool isConstant = right.isInteger() && right.integers.least == right.integers.most;
    if (isConstant && (right.integers.least == 1 || right.integers.least == -1)) {
        // The fraction: the difference is exact. Of an infinity it is no number, as the remainder is none. A zero has the sign of x.
        Number fraction = sub(x, trunc(x));
        return select(equal(fraction, number(0)), mul(x, number(0)), fraction);
    }
    // As a rule both are whole numbers, what is divided is not negative and what divides is positive.
    // Where the call stands at the site it is off the usual path. Elsewhere it is called as it was: the cold stub, which keeps every register, would cost the fractions more than they paid.
    LBasicBlock generally = isARM64() ? newColdBlock() : m_out.newBlock();
    Join join(m_out, Rep::Double, Double);
    auto naturalOrElse = [&](ValueID id, const Type& given, Number asNumber) {
        if (!given.isInteger() || !given.integers.isWithin(Range::int32()))
            return naturalInt32OrElse(asNumber, generally);
        Word32 whole = low32(get(id).as<Integer>());
        if (given.integers.least < 0)
            orElse(greaterThanOrEqual(whole, word32(0)), generally);
        return whole;
    };
    Word32 dividend = naturalOrElse(a, left, x);
    Word32 divisor = naturalOrElse(b, right, y);
    if (!right.isInteger() || right.integers.contains(0))
        orElse(Word32(m_out.notZero32(divisor.value())), generally);
    bool isMask = isConstant && right.integers.isWithin({ 1, INT32_MAX }) && hasOneBitSet(static_cast<uint64_t>(right.integers.least));
    join.add(toNumber(isMask ? bitAnd(dividend, word32(static_cast<int32_t>(right.integers.least - 1))) : Word32(m_out.mod(dividend.value(), divisor.value()))));
    m_out.appendTo(generally);
    join.add(fmodOffTheUsualPath(x, y));
    return join.finish().as<Number>();
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
        if (rep == Rep::JSValue) {
            orElse(Word32(m_out.notZero32(bitAnd(low32(x), word32(INT32_MAX)).value())), otherwise);
            return ensure(signExtended(Word32(m_out.neg(low32(x).value()))), m_program.at(m_program.types.int32), type);
        }
        orElse(isNotZero(x), otherwise);
        return yieldInt32OrElse(neg(x), type, otherwise);
    }, [&] {
        return lowerUnary(result, value);
    }, generally);
}

Operand Lowering::lowerStepByOne(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    ValueID a = value.arguments[0];
    bool isUp = which == InlineBuiltin::Increment;
    Rep rep = repOf(type);
    auto generally = [&]() -> Operand {
        Boxed operand = in<Boxed>(a);
        auto byOperation = [&] {
            return ensure(isUp ? AOT_TYPED_OPERATION(Increment, operand) : AOT_TYPED_OPERATION(Decrement, operand), guaranteeOfStub(Opcode::ToNumeric), type);
        };
        if (m_hasFailedInt32Test || rep != Rep::JSValue)
            return byOperation();
        LBasicBlock otherwise = m_out.newBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        orElse(isInt32(operand), otherwise);
        join.add(ensure(signExtended(int32OrElse(isUp ? Opcode::Add : Opcode::Sub, low32(operand), word32(1), otherwise)), m_program.at(m_program.types.int32), type));
        m_out.appendTo(otherwise);
        join.add(byOperation());
        return join.finish();
    };
    if (!areExpectedToBeNumbers({ a }))
        return generally();
    return ifNumbersAsExpected({ a }, rep, true, [&](LBasicBlock otherwise) {
        Integer x = get(a).as<Integer>();
        if (rep == Rep::JSValue)
            return ensure(signExtended(int32OrElse(isUp ? Opcode::Add : Opcode::Sub, low32(x), word32(1), otherwise)), m_program.at(m_program.types.int32), type);
        return ensure(isUp ? add(x, integer(1)) : sub(x, integer(1)), Type::integer(Range::full()), type);
    }, [&] {
        return ensure(add(asNumericOperand(a), number(isUp ? 1 : -1)), m_program.at(m_program.types.number), type);
    }, generally);
}

Word32 Lowering::lowerRelational(const Value& value)
{
    if (auto answer = compareWithIntegerConstant(value))
        return *answer;
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
    if (auto answer = lowerRelationalWithLikelyInteger(value))
        return *answer;
    if (auto answer = lowerRelationalWithIntegerBound(value))
        return *answer;
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
    if (isCompact(4))
        return generally();
    for (unsigned i = 0; i < 2; ++i) {
        auto text = constantASCIIText(value.child(i));
        ValueID subject = value.child(1 - i);
        if (!text || text->size() != 1 || !(typeOf(subject).primitives & Primitive::String) || !holdsCells(m_function.at(subject).rep))
            continue;
        Opcode opcode = value.opcode;
        if (!i)
            opcode = opcode == Opcode::Less ? Opcode::Greater : opcode == Opcode::Greater ? Opcode::Less : opcode == Opcode::LessEq ? Opcode::GreaterEq : Opcode::LessEq;
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        StringReceiver receiver = stringReceiver(subject);
        bool mayBeNoString = !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
        if (mayBeNoString) {
            receiver.flags = 0;
            if (!receiver.isString && get(subject).rep != Rep::Cell)
                receiver.isString = isCell(get(subject).as<Boxed>(), typeOf(subject));
        }
        if (receiver.isString)
            orElse(receiver.isString, otherwise);
        if (mayBeNoString)
            orElse(equal(typeOfCell(receiver.string), word32(StringType)), otherwise);
        Characters characters = charactersOf(receiver, otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        LBasicBlock isEmpty = m_out.newBlock();
        LBasicBlock hasCharacter = m_out.newBlock();
        m_out.branch(isZero(characters.length).value(), unsure(isEmpty), unsure(hasCharacter));
        m_out.appendTo(isEmpty);
        join.add(boolean(opcode == Opcode::Less || opcode == Opcode::LessEq));
        m_out.appendTo(hasCharacter);
        Word32 first(marked(m_out.load8ZeroExt32(m_out.address(m_heaps.characters8.atAnyIndex(), characters.data.value(), 0)), IsFixed));
        ensureStillAliveHere(characters.owner);
        Word32 character = word32((*text)[0]);
        Word32 isOneCharacterLong = equal(characters.length, integer(1));
        switch (opcode) {
        case Opcode::Less:
            join.add(below(first, character));
            break;
        case Opcode::GreaterEq:
            join.add(aboveOrEqual(first, character));
            break;
        case Opcode::LessEq:
            join.add(logicalOr(below(first, character), logicalAnd(equal(first, character), isOneCharacterLong)));
            break;
        default:
            join.add(logicalOr(below(character, first), logicalAnd(equal(first, character), logicalNot(isOneCharacterLong))));
            break;
        }
        m_out.appendTo(otherwise);
        join.add(generally());
        return join.finish().as<Word32>();
    }
    if (!areExpectedToBeNumbers({ a, b }))
        return generally();
    return ifNumbersAsExpected({ a, b }, Rep::Int32, true, [&](LBasicBlock) {
        return lowerRelational(value);
    }, [&] {
        return lowerRelational(value);
    }, generally).as<Word32>();
}

unsigned Lowering::numberOfComparisonsWithIntegers(ValueID id)
{
    if (m_numberOfComparisonsWithIntegers.isEmpty()) {
        m_numberOfComparisonsWithIntegers.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                auto& value = m_function.at(user);
                if (value.opcode != Opcode::StrictEq && value.opcode != Opcode::Eq)
                    continue;
                for (unsigned i = 0; i < 2; ++i) {
                    auto& count = m_numberOfComparisonsWithIntegers[value.child(i).index()];
                    if (m_function.at(value.child(i)).rep == Rep::Double && m_function.at(value.child(1 - i)).opcode == Opcode::Integer && count < UINT8_MAX)
                        ++count;
                }
            }
        }
    }
    return m_numberOfComparisonsWithIntegers[id.index()];
}

ValueID Lowering::valueBeingLowered() const
{
    if (!m_currentBlock || m_currentIndexInBlock >= m_currentBlock->values.size())
        return { };
    return m_currentBlock->values[m_currentIndexInBlock];
}

bool Lowering::isTakenAsInt32(ValueID id)
{
    if (!m_didFindWhatIsTakenAsInt32) {
        m_didFindWhatIsTakenAsInt32 = true;
        m_isTakenAsInt32.ensureSize(m_function.values.size());
        m_isTakenAsIndex.ensureSize(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                auto& value = m_function.at(user);
                switch (value.opcode) {
                case Opcode::BitAnd:
                case Opcode::BitOr:
                case Opcode::BitXor:
                case Opcode::LShift:
                case Opcode::RShift:
                case Opcode::URShift:
                    m_isTakenAsInt32.quickSet(value.child(1).index());
                    [[fallthrough]];
                case Opcode::BitNot:
                    m_isTakenAsInt32.quickSet(value.child(0).index());
                    break;
                case Opcode::GetByVal:
                case Opcode::PutByVal:
                    m_isTakenAsIndex.quickSet(value.child(1).index());
                    break;
                default:
                    break;
                }
            }
        }
    }
    return m_isTakenAsInt32.quickGet(id.index());
}

bool Lowering::isTakenAsIndex(ValueID id)
{
    isTakenAsInt32(id);
    return m_isTakenAsIndex.quickGet(id.index());
}

Word32 Lowering::toInt32InArm(Number value, bool isRare)
{
    if (value.value()->hasDouble())
        return word32(JSC::toInt32(value.value()->asDouble()));
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics())
        return toInt32(value);
#endif
    if (isRare)
        return AOT_TYPED_OPERATION(ToInt32, value);
    return toInt32(value);
}

Integer Lowering::exactIntegerInArm(Number value)
{
    if (value.value()->hasDouble()) {
        double constant = value.value()->asDouble();
        bool isExact = constant >= -9007199254740992.0 && constant <= 9007199254740992.0 && static_cast<double>(static_cast<int64_t>(constant)) == constant;
        return integer(isExact ? static_cast<int64_t>(constant) : INT64_MIN);
    }
    Integer truncated = truncateToInteger(value);
    return select(equal(convertToNumber(truncated), value), truncated, integer(INT64_MIN));
}

void Lowering::didDefineNumber(ValueID id, const Value& value, Number number)
{
    if (auto found = m_exactIntegersOfNumbers.find(number.value()); found != m_exactIntegersOfNumbers.end()) {
        m_integersOfNumbers.add(id.index(), IntegerOfNumber { number.value(), found->value });
        return;
    }
    // What was kept as an integer. `undefined` becomes what is not a number, `null` 0.
    // A read and a cast may have yielded a placeholder: see lowerFusedCast().
    if (value.opcode == Opcode::Cast && m_function.at(value.child(0)).opcode != Opcode::Cast && m_function.at(value.child(0)).opcode != Opcode::GetById) {
        Operand from = m_values[value.child(0).index()];
        auto least = leastIfIntegerOrNaN(id);
        if ((from.rep == Rep::Int64 || from.rep == Rep::OptionalInt64) && least) {
            Integer kept(from.value);
            if (from.rep == Rep::OptionalInt64) {
                auto known = m_integersOfNumbers.find(value.child(0).index());
                kept = known != m_integersOfNumbers.end() && known->value.least && known->value.number == from.value ? Integer(known->value.integer) : select(isMarker(from), integer(INT64_MIN), kept);
            }
            m_integersOfNumbers.add(id.index(), IntegerOfNumber { number.value(), kept.value(), least });
            return;
        }
    }
    static constexpr unsigned worthConverting = 3;
    if (numberOfComparisonsWithIntegers(id) < worthConverting)
        return;
    // By the value: -0 is 0, as `===` has it. What is not a number is equal to nothing.
    Integer truncated = truncateToInteger(number);
    Integer exact = select(equal(convertToNumber(truncated), number), truncated, integer(INT64_MIN));
    m_integersOfNumbers.add(id.index(), IntegerOfNumber { number.value(), exact.value() });
}

std::optional<Integer> Lowering::integerOfNumber(ValueID id)
{
    auto iterator = m_integersOfNumbers.find(id.index());
    // Where a test has shown more of the value it stands for something else.
    if (iterator == m_integersOfNumbers.end() || get(id).value != iterator->value.number)
        return std::nullopt;
    return Integer(iterator->value.integer);
}

Word32 Lowering::lowerStrictEq(ValueID a, ValueID b)
{
    if (!m_pinnedReads.isEmpty()) {
        for (auto [read, text] : { std::pair { a, b }, std::pair { b, a } }) {
            if (auto found = m_pinnedReads.find(read.index()); found != m_pinnedReads.end())
                return isPinnedTo(found->value, text);
        }
    }
    if (auto areEqual = areKeptCodeUnitsEqual(a, b))
        return *areEqual;
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (left.isInteger() && right.isInteger())
        return equal(get(a).as<Integer>(), get(b).as<Integer>());
    if (left.isBoolean() && right.isBoolean())
        return equal(get(a).as<Word32>(), get(b).as<Word32>());
    // Also of a number that may be `undefined`, which is equal to no integer, as what is no number.
    for (auto [number, constant] : { std::pair { a, b }, std::pair { b, a } }) {
        if (m_function.at(constant).opcode != Opcode::Integer)
            continue;
        if (auto asInteger = integerOfNumber(number))
            return equal(*asInteger, integer(m_function.at(constant).integer()));
    }
    for (auto [number, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(other).isInteger())
            continue;
        if (auto asInteger = integerOfNumber(number))
            return equal(*asInteger, get(other).as<Integer>());
    }
    auto kept = [&](ValueID id) -> const IntegerOfNumber* {
        auto found = m_integersOfNumbers.find(id.index());
        return found != m_integersOfNumbers.end() && get(id).value == found->value.number ? &found->value : nullptr;
    };
    if (auto* x = kept(a), *y = kept(b); x && y && left.isNumber() && right.isNumber()) {
        auto* integerOrNaN = x->least ? x : y->least ? y : nullptr;
        LBasicBlock isSame = m_out.newBlock();
        LBasicBlock isOther = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(equal(Integer(x->integer), Integer(y->integer)).value(), unsure(isSame), unsure(isOther));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        m_out.appendTo(isSame);
        if (integerOrNaN)
            join.add(*integerOrNaN->least >= 0 ? greaterThanOrEqual(Integer(integerOrNaN->integer), integer(0)) : notEqual(Integer(integerOrNaN->integer), integer(INT64_MIN)));
        else {
            join.add(either<Word32>(notEqual(Integer(x->integer), integer(INT64_MIN)), [&] {
                return boolean(true);
            }, [&] {
                return equal(in<Number>(a), in<Number>(b));
            }));
        }
        return join.finish().as<Word32>();
    }
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
        return haveEqualBits(in<Boxed>(a), in<Boxed>(b));
    if (isCompact(m_function.at(a).opcode == Opcode::String || m_function.at(b).opcode == Opcode::String ? 16 : 2)) {
        Boxed first = in<Boxed>(a);
        return emitGenericCompare(Stub::StrictEqual, first, in<Boxed>(b), OperationEffects { Writes::RuntimeState, true });
    }
    if (isStringOrNil(left) && isStringOrNil(right))
        return areStringsEqual(a, b);
    // A string is the same as nothing but a string.
    for (auto [string, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(string).isString())
            continue;
        return ifAsExpected({ { other, &m_program.at(m_program.types.string) } }, Rep::Int32, [&](LBasicBlock) {
            return lowerStrictEq(a, b);
        }, [&] {
            return boolean(false);
        }).as<Word32>();
    }
    // An integer is the same as nothing but a number: as a rule one that is kept as an int32.
    for (auto [integer, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(integer).isInteger() || !typeOf(integer).integers.isWithin(Range::int32()) || get(other).rep != Rep::JSValue)
            continue;
        Integer wanted = get(integer).as<Integer>();
        Boxed value = in<Boxed>(other);
        LBasicBlock hasSameBits = m_out.newBlock();
        LBasicBlock hasOtherBits = m_out.newBlock();
        LBasicBlock isOtherInt32 = m_out.newBlock();
        LBasicBlock isNoInt32 = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(equal(value, boxInt32(low32(wanted))).value(), unsure(hasSameBits), unsure(hasOtherBits));
        m_out.appendTo(hasSameBits);
        join.add(boolean(true));
        m_out.appendTo(hasOtherBits);
        m_out.branch(isInt32(value).value(), usually(isOtherInt32), rarely(isNoInt32));
        m_out.appendTo(isOtherInt32);
        join.add(boolean(false));
        m_out.appendTo(isNoInt32);
        // A whole number other than zero has one spelling among the doubles.
        auto& range = typeOf(integer).integers;
        if (range.least == range.most && range.least) {
            join.add(equal(value, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(range.least)))));
            return join.finish().as<Word32>();
        }
        LBasicBlock isDouble = newColdBlock();
        LBasicBlock isNoNumber = newColdBlock();
        m_out.branch(isNumber(value).value(), unsure(isDouble), unsure(isNoNumber));
        m_out.appendTo(isDouble);
        join.add(equal(unboxedDouble(value), convertToNumber(wanted)));
        m_out.appendTo(isNoNumber);
        join.add(boolean(false));
        return join.finish().as<Word32>();
    }
    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    // Whether two values are the same runs no code of the program. At most a rope is made into a string.
    auto generally = [&] {
        return emitGenericCompare(Stub::StrictEqual, x, y, OperationEffects { Writes::RuntimeState, true });
    };
    if (goesGenerallyWhereTextIsCold())
        return generally();
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
    // The same bits are the same value, unless it is a double, which may be NaN. Two cells with other bits are another value if one is an object. The rest is for the stub: in line it is bytes at every site.
    LBasicBlock hasSameBits = m_out.newBlock();
    LBasicBlock isSameNumber = m_out.newBlock();
    LBasicBlock hasOtherBits = m_out.newBlock();
    LBasicBlock areCells = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isOther = m_out.newBlock();
    LBasicBlock asksStub = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(equal(x, y).value(), unsure(hasSameBits), unsure(hasOtherBits));
    m_out.appendTo(hasSameBits);
    m_out.branch(isNumber(x).value(), unsure(isSameNumber), unsure(isSame));
    m_out.appendTo(isSameNumber);
    m_out.branch(isInt32(x).value(), usually(isSame), rarely(asksStub));
    m_out.appendTo(hasOtherBits);
    m_out.branch(isCell(Boxed(m_out.bitOr(x.value(), y.value()))).value(), usually(areCells), rarely(asksStub));
    m_out.appendTo(areCells);
    m_out.branch(aboveOrEqual(typeOfCell(asCell(x)), word32(ObjectType)).value(), usually(isOther), rarely(asksStub));
    m_out.appendTo(isSame);
    join.add(boolean(true));
    m_out.appendTo(isOther);
    join.add(boolean(false));
    m_out.appendTo(asksStub);
    join.add(generally());
    return join.finish().as<Word32>();
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
    if (goesGenerallyWhereTextIsCold())
        return throughStub();
    if (isCompact(3))
        return throughStub();
    // `undefined` and `null` are an answer like any other here.
    auto expected = predictionOf(id, 0);
    // Whoever asks this of a value that may be anything asks, as a rule, whether there is something, or for an answer that was given.
    if (!expected) {
        auto& type = typeOf(id);
        Type usual = Type::ofPrimitives(type.primitives & (Primitive::UndefinedOrNull | Primitive::Boolean));
        usual.anyObject = type.hasObjects();
        if (usual.isNever())
            return throughStub();
        expected = usual;
    }

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
            Word32 isTruthyObject = aboveOrEqual(cellType, word32(ObjectType));
            if (m_program.mayMeetMasquerader)
                isTruthyObject = logicalAnd(isTruthyObject, lacksBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined));
            when(isTruthyObject, [&] {
                if (!m_program.mayMeetMasquerader && Options::validateAOTTypedFacts()) [[unlikely]] {
                    LBasicBlock masquerades = newColdBlock();
                    LBasicBlock continuation = m_out.newBlock();
                    m_out.branch(lacksBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined).value(), usually(continuation), rarely(masquerades));
                    m_out.appendTo(masquerades);
                    AOT_TYPED_OPERATION(FactDoesNotHold, value, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
                    m_out.jump(continuation);
                    m_out.appendTo(continuation);
                }
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
    if (expected->primitives & (Primitive::UndefinedOrNull | Primitive::Boolean)) {
        static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined && JSValue::ValueUndefined < JSValue::DoubleEncodeOffset);
        when(equal(value, boxedConstant(jsBoolean(true))), [&] {
            return boolean(true);
        });
        when(belowOrEqual(value, undefined()), [&] {
            return boolean(false);
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
    bool makesStringGenerally = goesGenerallyWhereTextIsCold();
    if (operand.rep == Rep::Int64 && makesStringGenerally)
        return AOT_TYPED_OPERATION(DoubleToString, convertToNumber(operand.as<Integer>()));
    if (operand.rep == Rep::Double && makesStringGenerally)
        return AOT_TYPED_OPERATION(DoubleToString, operand.as<Number>());
    if (operand.rep == Rep::Int64)
        return integerToString(operand.as<Integer>(), type);
    // As a rule a number is an int32 where it is made a string of. Minus zero reads "0".
    if (operand.rep == Rep::Double) {
        LBasicBlock isOther = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        auto& int32Type = m_program.at(m_program.types.int32);
        join.add(integerToString(integralOrElse(operand.as<Number>(), Range::int32(), isOther), int32Type));
        m_out.appendTo(isOther);
        join.add(AOT_TYPED_OPERATION(DoubleToString, operand.as<Number>()));
        return join.finish().as<CellPointer>();
    }
    Boxed boxed = convertTo<Boxed>(operand, type);
    auto generally = [&] {
        // Only an object has a say in what string it becomes.
        return AOT_TYPED_OPERATION_WITH_EFFECTS(ToString, (OperationEffects { type.hasObjects() ? Writes::Heap : Writes::RuntimeState, true }), boxed);
    };
    if (type.primitives & Primitive::String && !(type.primitives & Primitive::Absent) && !isStringOrNil(type) && !predictionOf(id)) {
        bool everyCellIsString = !type.hasObjects() && !(type.primitives & (Primitive::Symbol | Primitive::BigInt));
        bool mayBeNumberByType = !type.hasObjects() && type.primitives & Primitive::Number;
        LBasicBlock isCellOfSomeKind = m_out.newBlock();
        LBasicBlock isString = m_out.newBlock();
        LBasicBlock isNoCell = m_out.newBlock();
        LBasicBlock isAnythingElse = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        m_out.branch(isCell(boxed).value(), unsure(isCellOfSomeKind), unsure(isNoCell));
        m_out.appendTo(isCellOfSomeKind);
        if (everyCellIsString)
            m_out.jump(isString);
        else
            m_out.branch(equal(typeOfCell(asCell(boxed)), word32(StringType)).value(), unsure(isString), unsure(isAnythingElse));
        m_out.appendTo(isString);
        join.add(asCell(boxed));
        m_out.appendTo(isNoCell);
        if (mayBeNumberByType) {
            LBasicBlock isWhole = m_out.newBlock();
            m_out.branch(isInt32(boxed).value(), unsure(isWhole), unsure(isAnythingElse));
            m_out.appendTo(isWhole);
            join.add(integerToString(signExtended(low32(boxed)), m_program.at(m_program.types.int32)));
        } else
            m_out.jump(isAnythingElse);
        m_out.appendTo(isAnythingElse);
        join.add(generally());
        return join.finish().as<CellPointer>();
    }
    if (isStringOrNil(type)) {
        return either<CellPointer>(isCell(boxed), [&] {
            return asCell(boxed);
        }, generally);
    }
    if (makesStringGenerally)
        return generally();
    auto prediction = predictionOf(id);
    bool isNumberOrNil = type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    if (!isNumberOrNil && (!prediction || !(prediction->isString() || prediction->isNumber())))
        return generally();
    // As a rule a number is an int32 where it is made a string of.
    auto& expected = m_program.at(!isNumberOrNil && prediction->isString() ? m_program.types.string : m_program.types.int32);
    return ifAsExpected({ { id, &expected } }, Rep::Cell, [&](LBasicBlock) {
        return toStringCell(id);
    }, generally).as<CellPointer>();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
