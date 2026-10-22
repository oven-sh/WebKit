/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPureOperations.h"
#include "AOTRepresentationRules.h"
#include "B3ComputeDivisionMagic.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include "MathCommon.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t quietNaNBits = 0x7ff8ull << 48;
static_assert((undefinedDouble & quietNaNBits) == quietNaNBits);

static std::optional<int64_t> constantIntegerOf(LValue value)
{
    if (value->hasInt())
        return value->asInt();
    if (value->opcode() == B3::SExt32 && value->child(0)->hasInt32())
        return value->child(0)->asInt32();
    return std::nullopt;
}

static std::optional<double> constantNumberOf(LValue value)
{
    if (value->hasDouble())
        return value->asDouble();
    if (value->opcode() != B3::IToD)
        return std::nullopt;
    if (std::optional<int64_t> integer = constantIntegerOf(value->child(0)))
        return static_cast<double>(*integer);
    return std::nullopt;
}

static std::optional<int64_t> positiveWholeNumberOf(double number)
{
    if (!(number >= 1 && number <= static_cast<double>(1ll << 53)) || number != std::trunc(number))
        return std::nullopt;
    return static_cast<int64_t>(number);
}

static bool isStepByOne(Opcode opcode)
{
    return opcode == Opcode::Increment || opcode == Opcode::Decrement;
}

static ValueID firstOperandOf(const ValueContext& value)
{
    return isStepByOne(value.opcode()) ? value->valueOperand() : value->leftOperand();
}

namespace {

template<typename T>
struct IntegerOperands {
    T left;
    T right;
};

template<typename T>
struct RemainderOfIntegers {
    Number number;
    T whole;
    T zeroWhereWholeIsZero;
    Condition isNumber;
    T dividend;
    uint64_t powerOfTwoDivisor { 0 };
};

class IntegerArithmeticEmitter final : public OperationEmitter {
public:
    explicit IntegerArithmeticEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    template<typename T> Operand exactResult(const ValueContext&, ArithmeticOperator);
    template<typename T> Operand resultAsNumber(const ValueContext&, ArithmeticOperator);
    template<typename T> RemainderOfIntegers<T> remainderOfIntegers(const ValueContext&);
    Condition testRemainder(Integer dividend, int32_t divisor, std::optional<int64_t> comparedWith);
    Number remainder(Number dividend, Number divisor);

private:
    template<typename T> T constant(int64_t);
    template<typename T> IntegerOperands<T> operandsOf(const ValueContext&);
    Integer widened(Word32 value) { return signExtended(value); }
    Integer widened(Integer value) { return value; }
    template<typename T> T remainderOfNonNegative(T dividend, T divisor);
    template<typename T> T remainderByPowerOfTwo(T dividend, uint64_t divisor);
    enum class DividendSign : bool { Any, NonNegative };
    Integer remainderByConstant(Integer dividend, int64_t divisor, DividendSign = DividendSign::Any);
    Integer nonNegativeIntegerOrElse(Number, LBasicBlock otherwise);
    template<typename T> Number remainderAsNumber(T whole, T dividend, T divisor, bool mayBeNaN);
    Word32 nonNegativeInt32OrElse(Number, LBasicBlock otherwise);
};

template<typename T>
T IntegerArithmeticEmitter::constant(int64_t value)
{
    if constexpr (std::is_same_v<T, Word32>)
        return word32(static_cast<int32_t>(value));
    else
        return integer(value);
}

template<typename T>
IntegerOperands<T> IntegerArithmeticEmitter::operandsOf(const ValueContext& value)
{
    if (isStepByOne(value.opcode()))
        return { in<T>(value->valueOperand()), constant<T>(1) };
    return { in<T>(value->leftOperand()), in<T>(value->rightOperand()) };
}

template<typename T>
T IntegerArithmeticEmitter::remainderOfNonNegative(T dividend, T divisor)
{
    std::optional<int64_t> constantDivisor = constantIntegerOf(divisor.value());
    if (constantDivisor && *constantDivisor > 0) {
        if (hasOneBitSet(static_cast<uint64_t>(*constantDivisor)))
            return bitAnd(dividend, constant<T>(*constantDivisor - 1));
        if constexpr (std::is_same_v<T, Word32>) {
            Word32 quotient(m_out.m_block->appendNew<B3::Value>(m_proc, B3::UDiv, m_out.origin(), dividend.value(), divisor.value()));
            return sub(dividend, mul(quotient, divisor));
        } else
            return remainderByConstant(dividend, *constantDivisor, DividendSign::NonNegative);
    }
    return binary<T>(&FTL::Output::mod, dividend, divisor);
}

template<typename T>
T IntegerArithmeticEmitter::remainderByPowerOfTwo(T dividend, uint64_t divisor)
{
    constexpr unsigned numberOfBits = std::is_same_v<T, Word32> ? 32 : 64;
    unsigned shift = WTF::fastLog2(divisor);
    if (!shift)
        return constant<T>(0);
    T roundsTowardZero(m_out.lShr(m_out.aShr(dividend.value(), m_out.constInt32(numberOfBits - 1)), m_out.constInt32(numberOfBits - shift)));
    return sub(dividend, bitAnd(add(dividend, roundsTowardZero), constant<T>(-static_cast<int64_t>(divisor))));
}

Integer IntegerArithmeticEmitter::remainderByConstant(Integer dividend, int64_t divisor, DividendSign sign)
{
    B3::DivisionMagic<int64_t> magic = B3::computeSignedDivisionMagic(divisor);
    Integer quotient(m_out.m_block->appendNew<B3::Value>(m_proc, B3::MulHigh, m_out.origin(), dividend.value(), m_out.constInt64(magic.magicMultiplier)));
    if (magic.magicMultiplier < 0)
        quotient = add(quotient, dividend);
    if (magic.shift)
        quotient = Integer(m_out.aShr(quotient.value(), m_out.constInt32(magic.shift)));
    if (sign == DividendSign::Any)
        quotient = add(quotient, Integer(m_out.lShr(quotient.value(), m_out.constInt32(63))));
    return sub(dividend, mul(quotient, integer(divisor)));
}

Condition IntegerArithmeticEmitter::testRemainder(Integer dividend, int32_t divisor, std::optional<int64_t> comparedWith)
{
    uint64_t lowBits = static_cast<uint64_t>(divisor) - 1;
    int64_t constant = comparedWith.value_or(0);
    if (hasOneBitSet(static_cast<uint64_t>(divisor)) && static_cast<uint64_t>(constant < 0 ? -constant : constant) <= lowBits) {
        if (!constant) {
            Integer low = bitAnd(dividend, integer(static_cast<int64_t>(lowBits)));
            return comparedWith ? isZero(low) : isNotZero(low);
        }
        uint64_t sign = 1ull << 63;
        return equal(bitAnd(dividend, integer(static_cast<int64_t>(sign | lowBits))), integer(static_cast<int64_t>((constant < 0 ? sign : 0) | (static_cast<uint64_t>(constant) & lowBits))));
    }
    Integer remainder = remainderByConstant(dividend, divisor);
    return comparedWith ? equal(remainder, integer(constant)) : isNotZero(remainder);
}

template<typename T>
Number IntegerArithmeticEmitter::remainderAsNumber(T whole, T dividend, T divisor, bool mayBeNaN)
{
    constexpr bool is32Bit = std::is_same_v<T, Word32>;
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Double);
    patchpoint->append(whole.value(), B3::ValueRep::SomeRegister);
    patchpoint->append(dividend.value(), B3::ValueRep::SomeRegister);
    if (mayBeNaN)
        patchpoint->append(divisor.value(), B3::ValueRep::SomeRegister);
    patchpoint->effects = B3::Effects::none();
    patchpoint->pureFunction = Typed::remainderAsNumber(is32Bit, mayBeNaN);
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([mayBeNaN](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        FPRReg result = params[0].fpr();
        GPRReg whole = params[1].gpr();
        GPRReg dividend = params[2].gpr();
        GPRReg divisor = mayBeNaN ? params[3].gpr() : InvalidGPRReg;
        auto branchOnTest = [](CCallHelpers& jit, CCallHelpers::ResultCondition condition, GPRReg tested) {
            return is32Bit ? jit.branchTest32(condition, tested) : jit.branchTest64(condition, tested);
        };
        if constexpr (is32Bit)
            jit.convertInt32ToDouble(whole, result);
        else
            jit.convertInt64ToDouble(whole, result);
        CCallHelpers::Jump isZero = branchOnTest(jit, CCallHelpers::Zero, whole);
        CCallHelpers::Label done = jit.label();
        params.addLatePath([=](CCallHelpers& jit) {
            auto produce = [&](double number) {
                jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(number)), whole);
                jit.move64ToDouble(whole, result);
                jit.move(CCallHelpers::TrustedImm32(0), whole);
                jit.jump().linkTo(done, &jit);
            };
            isZero.link(&jit);
            if (mayBeNaN) {
                CCallHelpers::Jump isNumber = branchOnTest(jit, CCallHelpers::NonZero, divisor);
                produce(PNaN);
                isNumber.link(&jit);
            }
            branchOnTest(jit, CCallHelpers::PositiveOrZero, dividend).linkTo(done, &jit);
            produce(-0.0);
        });
    });
    return Number(patchpoint);
}

template<typename T>
RemainderOfIntegers<T> IntegerArithmeticEmitter::remainderOfIntegers(const ValueContext& value)
{
    auto [dividend, divisor] = operandsOf<T>(value);
    std::optional<int64_t> constantDivisor = constantIntegerOf(divisor.value());
    bool isNeverNaN = constantDivisor && *constantDivisor;
    uint64_t magnitude = isNeverNaN ? (*constantDivisor < 0 ? -static_cast<uint64_t>(*constantDivisor) : static_cast<uint64_t>(*constantDivisor)) : 0;

    RemainderOfIntegers<T> result;
    result.dividend = dividend;
    if (magnitude && hasOneBitSet(magnitude) && magnitude <= 1ull << 30) {
        result.whole = remainderByPowerOfTwo(dividend, magnitude);
        result.zeroWhereWholeIsZero = bitAnd(dividend, constant<T>(magnitude - 1));
        result.powerOfTwoDivisor = magnitude;
    } else if (std::is_same_v<T, Integer> && magnitude && magnitude <= 1ull << 62) {
        if constexpr (std::is_same_v<T, Integer>)
            result.whole = remainderByConstant(dividend, static_cast<int64_t>(magnitude));
        result.zeroWhereWholeIsZero = result.whole;
    } else {
        result.whole = binary<T>(&FTL::Output::chillMod, dividend, divisor);
        result.zeroWhereWholeIsZero = result.whole;
    }
    if (!isNeverNaN)
        result.isNumber = isNotZero(divisor);
    result.number = remainderAsNumber(result.whole, dividend, divisor, !isNeverNaN);
    return result;
}

template<typename T>
Operand IntegerArithmeticEmitter::exactResult(const ValueContext& value, ArithmeticOperator op)
{
    IntegerOperands<T> operands = operandsOf<T>(value);
    switch (op) {
    case ArithmeticOperator::Add:
        return add(operands.left, operands.right);
    case ArithmeticOperator::Sub:
        return sub(operands.left, operands.right);
    case ArithmeticOperator::Mul:
        return mul(operands.left, operands.right);
    case ArithmeticOperator::Mod:
        return remainderOfNonNegative(operands.left, operands.right);
    case ArithmeticOperator::Div:
    case ArithmeticOperator::Pow:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

template<typename T>
Operand IntegerArithmeticEmitter::resultAsNumber(const ValueContext& value, ArithmeticOperator op)
{
    IntegerOperands<T> operands = operandsOf<T>(value);
    switch (op) {
    case ArithmeticOperator::Add:
        return toNumber(add(widened(operands.left), widened(operands.right)));
    case ArithmeticOperator::Sub:
        return toNumber(sub(widened(operands.left), widened(operands.right)));
    case ArithmeticOperator::Mul:
        return mul(toNumber(operands.left), toNumber(operands.right));
    case ArithmeticOperator::Div:
        return div(toNumber(operands.left), toNumber(operands.right));
    case ArithmeticOperator::Mod:
        return remainderOfIntegers<T>(value).number;
    case ArithmeticOperator::Pow:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Word32 IntegerArithmeticEmitter::nonNegativeInt32OrElse(Number number, LBasicBlock otherwise)
{
    if (number.value()->opcode() == B3::IToD && number.value()->child(0)->type() == B3::Int32) {
        Word32 whole(number.value()->child(0));
        branchUnless(greaterThanOrEqual(whole, word32(0)), otherwise);
        return whole;
    }
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
        B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Int32);
        patchpoint->append(B3::ConstrainedValue(number.value(), B3::ValueRep::SomeRegister));
        patchpoint->effects = B3::Effects::none();
        patchpoint->pureFunction = pureFunction(PureOperation::NonNegativeInt32, 0, true);
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            jit.m_assembler.csinv<32>(params[0].gpr(), params[0].gpr(), ARM64Registers::zr, ARM64Assembler::ConditionEQ);
        });
        Word32 converted(patchpoint);
        branchUnless(greaterThanOrEqual(converted, word32(0)), otherwise);
        return converted;
    }
#endif
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Word32 narrow = low32(conversions.toInteger(number));
    branchUnless(equal(conversions.bitsOf(toNumber(narrow)), conversions.bitsOf(number)), otherwise);
    branchUnless(greaterThanOrEqual(narrow, word32(0)), otherwise);
    return narrow;
}

Integer IntegerArithmeticEmitter::nonNegativeIntegerOrElse(Number number, LBasicBlock otherwise)
{
    if (number.value()->opcode() == B3::IToD && number.value()->child(0)->type() == B3::Int32)
        return zeroExtended(nonNegativeInt32OrElse(number, otherwise));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Integer whole = conversions.toInteger(number);
    branchUnless(equal(conversions.bitsOf(toNumber(whole)), conversions.bitsOf(number)), otherwise);
    branchUnless(greaterThanOrEqual(whole, integer(0)), otherwise);
    return whole;
}

Number IntegerArithmeticEmitter::remainder(Number dividend, Number divisor)
{
    std::optional<double> constantDivisor = constantNumberOf(divisor.value());
    if (constantDivisor && std::abs(*constantDivisor) == 1) {
        Number fraction = sub(dividend, trunc(dividend));
        return select(equal(fraction, number(0)), mul(dividend, number(0)), fraction);
    }
    std::optional<int64_t> constantWholeDivisor = constantDivisor ? positiveWholeNumberOf(*constantDivisor) : std::nullopt;
    if (constantDivisor && !constantWholeDivisor)
        return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });

    LBasicBlock genericPath = newBlock();
    Join<Number> join(m_out);
    if (constantWholeDivisor)
        join.add(toNumber(remainderOfNonNegative(nonNegativeIntegerOrElse(dividend, genericPath), integer(*constantWholeDivisor))));
    else {
        Word32 wholeDividend = nonNegativeInt32OrElse(dividend, genericPath);
        Word32 wholeDivisor = nonNegativeInt32OrElse(divisor, genericPath);
        branchUnless(isNotZero(wholeDivisor), genericPath);
        join.add(toNumber(remainderOfNonNegative(wholeDividend, wholeDivisor)));
    }

    m_out.appendTo(genericPath);
    join.add(callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor }));
    return join.finish();
}

class PowerEmitter final : public OperationEmitter {
public:
    explicit PowerEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Number power(Number base, Number exponent);

private:
    std::optional<Number> powerWithConstantExponent(Number base, double exponent);
    std::optional<Number> powerWithConstantBase(double base, Number exponent);
    Integer wholeExponentOrElse(Number exponent, LBasicBlock otherwise);
    Number powerOfTwo(Integer exponent);
    Number powerOfTen(Integer exponent);
};

std::optional<Number> PowerEmitter::powerWithConstantExponent(Number base, double exponent)
{
    if (std::abs(exponent) == 0.5) {
        Number magnitude = abs(base);
        Number zero = number(0);
        Number infinity = number(std::numeric_limits<double>::infinity());
        if (exponent > 0)
            return select(equal(magnitude, zero), zero, select(equal(magnitude, infinity), infinity, sqrt(base)));
        return select(equal(magnitude, zero), infinity, select(equal(magnitude, infinity), zero, div(number(1), sqrt(base))));
    }
    if (!(exponent >= 0 && exponent <= maxExponentForIntegerMathPow) || exponent != std::trunc(exponent))
        return std::nullopt;
    if (exponent == 1)
        return emitter<ConversionEmitter>().purified(base);
    Number result;
    Number square = base;
    for (unsigned bits = static_cast<unsigned>(exponent); bits; bits >>= 1) {
        if (bits & 1)
            result = result ? mul(result, square) : square;
        if (bits >> 1)
            square = mul(square, square);
    }
    return result ? result : number(1);
}

Integer PowerEmitter::wholeExponentOrElse(Number exponent, LBasicBlock otherwise)
{
    if (exponent.value()->opcode() == B3::IToD) {
        LValue whole = exponent.value()->child(0);
        return whole->type() == B3::Int32 ? signExtended(Word32(whole)) : Integer(whole);
    }
    Word32 narrow = low32(emitter<ConversionEmitter>().toInteger(exponent));
    branchUnless(equal(toNumber(narrow), exponent), otherwise);
    return signExtended(narrow);
}

Number PowerEmitter::powerOfTwo(Integer exponent)
{
    constexpr int64_t exponentBias = 1023;
    constexpr int32_t numberOfFractionBits = 52;
    return Number(m_out.bitCast(m_out.shl(add(exponent, integer(exponentBias)).value(), m_out.constInt32(numberOfFractionBits)), B3::Double));
}

Number PowerEmitter::powerOfTen(Integer exponent)
{
    Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(numbers.powersOfTen));
    return Number(m_out.loadDouble(m_out.baseIndex(m_heaps.AOTInstance_fixedFields, table.value(), exponent.value(), FTL::ScaleEight)));
}

std::optional<Number> PowerEmitter::powerWithConstantBase(double base, Number exponent)
{
    if (base != 2 && base != 10)
        return std::nullopt;
    bool isBaseTwo = base == 2;
    Range handled = isBaseTwo ? Range { -1022, 1023 } : Range { 0, std::numeric_limits<double>::max_exponent10 };

    LBasicBlock genericPath = newBlock();
    Join<Number> join(m_out);
    Integer whole = wholeExponentOrElse(exponent, genericPath);
    branchUnless(belowOrEqual(sub(whole, integer(handled.minimum)), integer(handled.maximum - handled.minimum)), genericPath);
    join.add(isBaseTwo ? powerOfTwo(whole) : powerOfTen(whole));

    m_out.appendTo(genericPath);
    join.add(callLeaf<Number>(RuntimeOperation::Pow, { number(base), exponent }));
    return join.finish();
}

Number PowerEmitter::power(Number base, Number exponent)
{
    if (std::optional<double> constantExponent = constantNumberOf(exponent.value())) {
        if (std::optional<Number> result = powerWithConstantExponent(base, *constantExponent))
            return *result;
    }
    if (std::optional<double> constantBase = constantNumberOf(base.value())) {
        if (std::optional<Number> result = powerWithConstantBase(*constantBase, exponent))
            return *result;
    }
    return callLeaf<Number>(RuntimeOperation::Pow, { base, exponent });
}

} // anonymous namespace

Number OperatorEmitter::asNumber(Operand operand, const Type& type)
{
    if (operand.is<Word32>())
        return toNumber(operand.as<Word32>());
    Operand unboxed = operand;
    if (operand.is<Boxed>())
        unboxed = emitter<ConversionEmitter>().unbox(operand.as<Boxed>(), type, type.isNumber() ? Representation::Double : Representation::OptionalDouble);
    RELEASE_ASSERT(unboxed.is<Number>() || unboxed.is<OptionalNumber>());
    if (unboxed.is<Number>() || !(type.primitives & Primitive::Null))
        return Number(unboxed.value);
    uint64_t nullMarker = ConversionEmitter::markerBits(Primitive::Null, Representation::OptionalDouble);
    return select(emitter<ConversionEmitter>().isMarker(unboxed, nullMarker), number(0), Number(unboxed.value));
}

Boxed OperatorEmitter::sumOfBoxedNumbers(ArithmeticOperator op, Operand left, const Type& leftType, Operand right, const Type& rightType)
{
    RELEASE_ASSERT(op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub);
    RELEASE_ASSERT(left.is<Boxed>() || right.is<Boxed>());
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto int32Of = [&](Operand operand) {
        return signExtended(operand.is<Boxed>() ? low32(operand.as<Boxed>()) : operand.as<Word32>());
    };
    Boxed tested = left.is<Boxed>() ? left.as<Boxed>() : right.as<Boxed>();
    if (left.is<Boxed>() && right.is<Boxed>())
        tested = Boxed(m_out.bitAnd(left.value, right.value));

    LBasicBlock int32Case = newBlock();
    LBasicBlock numberCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isInt32(tested).value(), normalEdge(int32Case), unlikelyEdge(numberCase));

    m_out.appendTo(int32Case);
    Integer exact = op == ArithmeticOperator::Add ? add(int32Of(left), int32Of(right)) : sub(int32Of(left), int32Of(right));
    join.add(conversions.boxInteger(exact, { 2 * static_cast<int64_t>(INT32_MIN), 2 * static_cast<int64_t>(INT32_MAX) + 1 }));

    m_out.appendTo(numberCase);
    join.add(conversions.boxNumberByStub(arithmeticOnNumbers(op, asNumber(left, leftType), asNumber(right, rightType))));
    return join.finish();
}

Number OperatorEmitter::asNumericOperand(ValueID id)
{
    switch (representationOf(id)) {
    case Representation::Double:
        return in<Number>(id);
    case Representation::OptionalDouble: {
        OptionalNumber optional = in<OptionalNumber>(id);
        Number value(optional.value());
        if (!(typeOf(id).primitives & Primitive::Null))
            return value;
        uint64_t nullMarker = ConversionEmitter::markerBits(Primitive::Null, Representation::OptionalDouble);
        return select(emitter<ConversionEmitter>().isMarker(optional, nullMarker), number(0), value);
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::arithmeticOnIntegers(const ValueContext& value, ArithmeticOperator op)
{
    switch (value.representation()) {
    case Representation::Int32:
        return IntegerArithmeticEmitter(generator()).exactResult<Word32>(value, op);
    case Representation::Int64:
        return IntegerArithmeticEmitter(generator()).exactResult<Integer>(value, op);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

template<typename T>
Number OperatorEmitter::remainderOfCheckedIntegers(const ValueContext& value)
{
    RemainderOfIntegers<T> remainder = IntegerArithmeticEmitter(generator()).remainderOfIntegers<T>(value);
    m_wholeRemainders.set(value.id().index(), WholeRemainder { remainder.whole, remainder.zeroWhereWholeIsZero, remainder.isNumber, remainder.dividend, remainder.powerOfTwoDivisor });
    Word32 word;
    if constexpr (std::is_same_v<T, Word32>)
        word = remainder.whole;
    else
        word = low32(remainder.whole);
    m_int32OfNumbers.add(value.id().index(), Vector<Int32OfNumber, 1>()).iterator->value.append({ value.block(), word });
    return remainder.number;
}

std::optional<OperatorEmitter::RemainderLeftToTest> OperatorEmitter::findRemainderLeftToTest(ValueID id)
{
    const Value& remainder = m_function.at(id);
    if (remainder.opcode != Opcode::Mod || remainder.form() != OperatorForm::Numbers || representationOf(id) != Representation::Double || generator().validatesOrChecksProofs())
        return std::nullopt;
    const Value& divisor = m_function.at(remainder.rightOperand());
    if (divisor.opcode != Opcode::Integer || divisor.integer() < 2 || divisor.integer() > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    std::optional<Use> use = analyses().useLists().singleUse(id);
    if (!use || use->kind != Use::Kind::Operand || use->block != analyses().useLists().locationOf(id).block)
        return std::nullopt;

    const Terminator& terminator = m_function.at(use->block).terminator;
    bool handsOnRemainder = terminator.isConditionalBranch() && terminator.conditionOperand() == use->user && std::ranges::any_of(terminator.targets, [](const Target& target) {
        return !target.proofs.isEmpty();
    });
    if (handsOnRemainder)
        return std::nullopt;

    RemainderLeftToTest result { remainder.leftOperand(), static_cast<int32_t>(divisor.integer()), std::nullopt };
    const Value& test = m_function.at(use->user);
    if (test.opcode == Opcode::ToBoolean)
        return test.form() == OperatorForm::Numbers ? std::optional(result) : std::nullopt;
    if (!isEqualityOfNumbers(m_program, m_function, test))
        return std::nullopt;
    const Value& other = m_function.at(test.leftOperand() == id ? test.rightOperand() : test.leftOperand());
    if (other.opcode != Opcode::Integer)
        return std::nullopt;
    result.comparedWith = other.integer();
    return result;
}

bool OperatorEmitter::tryGenerateTestOfRemainder(const ValueContext& test, ValueID id)
{
    std::optional<RemainderLeftToTest> remainder = findRemainderLeftToTest(id);
    if (!remainder)
        return false;
    generator().takeOperandsOf(id);
    Number dividend = asNumericOperand(remainder->dividend);
    defineTestByBranching(test, [&](BranchTargets targets) {
        LBasicBlock isNotWhole = newBlock();
        Integer whole = emitter<ConversionEmitter>().toInteger(dividend);
        branchUnless(equal(toNumber(whole), dividend), isNotWhole);
        branch(IntegerArithmeticEmitter(generator()).testRemainder(whole, remainder->divisor, remainder->comparedWith), targets);

        m_out.appendTo(isNotWhole);
        Number number = fmod(dividend, this->number(remainder->divisor));
        branch(remainder->comparedWith ? equal(number, this->number(static_cast<double>(*remainder->comparedWith))) : isNeitherZeroNorNaN(number), targets);
    });
    return true;
}

const OperatorEmitter::WholeRemainder* OperatorEmitter::findWholeRemainder(ValueID number)
{
    auto entry = m_wholeRemainders.find(number.index());
    return entry == m_wholeRemainders.end() ? nullptr : &entry->value;
}

std::optional<Integer> OperatorEmitter::wholeRemainderAsIndex(ValueID position, LBasicBlock isNotAnIndex)
{
    const WholeRemainder* remainder = findWholeRemainder(position);
    if (!remainder)
        return std::nullopt;
    if (remainder->isNumber && isNotAnIndex)
        branchUnless(remainder->isNumber, isNotAnIndex);
    return remainder->whole.is<Word32>() ? signExtended(remainder->whole.as<Word32>()) : remainder->whole.as<Integer>();
}

Number OperatorEmitter::asTermOfSum(ValueID term, ValueID otherTerm)
{
    const WholeRemainder* remainder = findWholeRemainder(term);
    if (!remainder || remainder->isNumber || !typeOf(otherTerm).isInteger())
        return asNumericOperand(term);
    return remainder->whole.is<Word32>() ? toNumber(remainder->whole.as<Word32>()) : toNumber(remainder->whole.as<Integer>());
}

Operand OperatorEmitter::arithmeticOnCheckedIntegers(const ValueContext& value, ArithmeticOperator op)
{
    switch (representationOf(firstOperandOf(value))) {
    case Representation::Int32:
        if (op == ArithmeticOperator::Mod)
            return remainderOfCheckedIntegers<Word32>(value);
        return IntegerArithmeticEmitter(generator()).resultAsNumber<Word32>(value, op);
    case Representation::Int64:
        if (op == ArithmeticOperator::Mod)
            return remainderOfCheckedIntegers<Integer>(value);
        return IntegerArithmeticEmitter(generator()).resultAsNumber<Integer>(value, op);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Number OperatorEmitter::arithmeticOnNumbers(ArithmeticOperator op, Number left, Number right)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return add(left, right);
    case ArithmeticOperator::Sub:
        return sub(left, right);
    case ArithmeticOperator::Mul:
        return mul(left, right);
    case ArithmeticOperator::Div:
        return div(left, right);
    case ArithmeticOperator::Mod:
        return fmod(left, right);
    case ArithmeticOperator::Pow:
        return pow(left, right);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::fmod(Number dividend, Number divisor)
{
    return IntegerArithmeticEmitter(generator()).remainder(dividend, divisor);
}

Number OperatorEmitter::pow(Number base, Number exponent)
{
    return PowerEmitter(generator()).power(base, exponent);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
