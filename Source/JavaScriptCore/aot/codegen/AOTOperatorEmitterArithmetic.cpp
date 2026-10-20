/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTPureOperations.h"
#include "B3StackmapGenerationParams.h"
#include "MathCommon.h"
#include "PureNaN.h"
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

static std::optional<int32_t> positiveInt32Of(double number)
{
    if (!(number >= 1 && number <= std::numeric_limits<int32_t>::max()))
        return std::nullopt;
    int32_t whole = static_cast<int32_t>(number);
    if (whole != number)
        return std::nullopt;
    return whole;
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

class IntegerArithmeticEmitter final : public OperationEmitter {
public:
    explicit IntegerArithmeticEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    template<typename T> Operand exactResult(const ValueContext&, ArithmeticOperator);
    template<typename T> Operand resultAsNumber(const ValueContext&, ArithmeticOperator);
    Number remainder(Number dividend, Number divisor);

private:
    template<typename T> T constant(int64_t);
    template<typename T> IntegerOperands<T> operandsOf(const ValueContext&);
    Integer widened(Word32 value) { return signExtended(value); }
    Integer widened(Integer value) { return value; }
    template<typename T> T remainderOfNonNegative(T dividend, T divisor);
    template<typename T> Number remainderAsNumber(T dividend, T divisor);
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
        }
    }
    return binary<T>(&FTL::Output::mod, dividend, divisor);
}

template<typename T>
Number IntegerArithmeticEmitter::remainderAsNumber(T dividend, T divisor)
{
    Condition dividendIsNegative = lessThan(dividend, constant<T>(0));
    // B3's Mod is undefined for a zero divisor and for the least integer modulo -1.
    Condition isDefinedInB3 = above(add(divisor, constant<T>(1)), constant<T>(1));
    return either<Number>(isDefinedInB3, [&] {
        T whole = binary<T>(&FTL::Output::mod, dividend, divisor);
        return select(logicalAnd(isZero(whole), dividendIsNegative), number(-0.0), toNumber(whole));
    }, [&] {
        return select(isZero(divisor), number(PNaN), select(dividendIsNegative, number(-0.0), number(0)));
    });
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
        return remainderAsNumber(operands.left, operands.right);
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

Number IntegerArithmeticEmitter::remainder(Number dividend, Number divisor)
{
    std::optional<double> constantDivisor = constantNumberOf(divisor.value());
    if (constantDivisor && std::abs(*constantDivisor) == 1) {
        Number fraction = sub(dividend, trunc(dividend));
        return select(equal(fraction, number(0)), mul(dividend, number(0)), fraction);
    }
    std::optional<int32_t> constantWholeDivisor = constantDivisor ? positiveInt32Of(*constantDivisor) : std::nullopt;
    if (constantDivisor && !constantWholeDivisor)
        return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });

    LBasicBlock genericPath = newBlock();
    Join<Number> join(m_out);
    Word32 wholeDividend = nonNegativeInt32OrElse(dividend, genericPath);
    Word32 wholeDivisor;
    if (constantWholeDivisor)
        wholeDivisor = word32(*constantWholeDivisor);
    else {
        wholeDivisor = nonNegativeInt32OrElse(divisor, genericPath);
        branchUnless(isNotZero(wholeDivisor), genericPath);
    }
    join.add(toNumber(remainderOfNonNegative(wholeDividend, wholeDivisor)));

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

Operand OperatorEmitter::arithmeticOnCheckedIntegers(const ValueContext& value, ArithmeticOperator op)
{
    switch (representationOf(firstOperandOf(value))) {
    case Representation::Int32:
        return IntegerArithmeticEmitter(generator()).resultAsNumber<Word32>(value, op);
    case Representation::Int64:
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
