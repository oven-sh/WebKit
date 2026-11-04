/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPureOperations.h"
#include "AOTRepresentationRules.h"
#include "AOTVector.h"
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
#include <wtf/Atomics.h>
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

namespace {

class InlineMathEmitter : public OperationEmitter {
protected:
    using OperationEmitter::OperationEmitter;

    // An invalid operation produces a NaN with the sign bit set on x86-64 and clear on ARM64. A C library function may pick either.
    static bool matchesHardwareNaN(double libraryNaN) { return std::signbit(libraryNaN) == isX86_64(); }

    // What the function answers when a program calls it. A C++ compiler that sees the callee and constant operands computes the result itself, and its NaN may have the other sign.
    template<typename Function>
    static double resultAtRunTime(Function* function, double left, double right) { return WTF::opaque(function)(left, right); }

    Number callLibraryIfNaN(Number inlineResult, RuntimeOperation function, Number left, Number right)
    {
        LBasicBlock isNaN = newBlock();
        Join<Number> join(m_out);
        branchUnless(equal(inlineResult, inlineResult), isNaN);
        join.add(inlineResult);

        m_out.appendTo(isNaN);
        join.add(callLeaf<Number>(function, { left, right }));
        return join.finish();
    }
};

class IntegerArithmeticEmitter final : public InlineMathEmitter {
public:
    explicit IntegerArithmeticEmitter(CodeGenerator& generator)
        : InlineMathEmitter(generator)
    {
    }

    template<typename T>
    Operand exactResult(const ValueContext& value, ArithmeticOperator op)
    {
        auto [left, right] = operandsOf<T>(value);
        switch (op) {
        case ArithmeticOperator::Add:
            return add(left, right);
        case ArithmeticOperator::Sub:
            return sub(left, right);
        case ArithmeticOperator::Mul:
            return mul(left, right);
        case ArithmeticOperator::Mod:
            return remainderOfNonNegative(left, right, typeOf(value->leftOperand()).integers.isSubsetOf(Range::uint32()));
        case ArithmeticOperator::Div:
        case ArithmeticOperator::Pow:
            break;
        }
        return emitter<OperatorEmitter>().failWithZero(value);
    }

    template<typename T>
    Operand resultAsNumber(const ValueContext& value, ArithmeticOperator op)
    {
        auto [left, right] = operandsOf<T>(value);
        switch (op) {
        case ArithmeticOperator::Add:
            return toNumber(add(widened(left), widened(right)));
        case ArithmeticOperator::Sub:
            return toNumber(sub(widened(left), widened(right)));
        case ArithmeticOperator::Mul: {
            Number x = toNumber(left);
            return mul(x, toNumber(right));
        }
        case ArithmeticOperator::Div: {
            Number x = toNumber(left);
            return div(x, toNumber(right));
        }
        case ArithmeticOperator::Mod:
        case ArithmeticOperator::Pow:
            break;
        }
        return emitter<OperatorEmitter>().failWithZero(value);
    }

    template<typename T>
    WholeRemainder remainderOfIntegers(const ValueContext& value)
    {
        auto [dividend, divisor] = operandsOf<T>(value);
        std::optional<int64_t> constantDivisor = constantIntegerOf(divisor.value());
        bool isNeverNaN = constantDivisor && *constantDivisor;
        uint64_t magnitude = isNeverNaN ? (*constantDivisor < 0 ? -static_cast<uint64_t>(*constantDivisor) : static_cast<uint64_t>(*constantDivisor)) : 0;

        WholeRemainder result;
        result.dividend = dividend;
        T whole;
        if (magnitude && hasOneBitSet(magnitude) && magnitude <= 1ull << 30) {
            whole = remainderByPowerOfTwo(dividend, magnitude);
            result.zeroWhereWholeIsZero = bitAnd(dividend, constant<T>(magnitude - 1));
            result.powerOfTwoDivisor = magnitude;
        } else if (std::is_same_v<T, Integer> && magnitude && magnitude <= 1ull << 62) {
            if constexpr (std::is_same_v<T, Integer>)
                whole = remainderByConstant(dividend, static_cast<int64_t>(magnitude));
        } else if (!constantDivisor) {
            Range dividends = typeOf(value->leftOperand()).integers;
            bool mayBeNegative = dividends.minimum < 0;
            bool mayOverflow = dividends.minimum <= (std::is_same_v<T, Word32> ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min());
            LBasicBlock divides = newBlock();
            LBasicBlock doesNotDivide = newBlock();
            LBasicBlock continuation = newBlock();
            Vector<ValueFromBlock, 4> wholes;
            Vector<ValueFromBlock, 4> numbers;
            auto answer = [&](T wholeAnswer, Number numberAnswer) {
                wholes.append(m_out.anchor(wholeAnswer.value()));
                numbers.append(m_out.anchor(numberAnswer.value()));
                m_out.jump(continuation);
            };
            auto zeroWithSignOfDividend = [&] {
                return mayBeNegative ? select(lessThan(dividend, constant<T>(0)), number(-0.0), number(0)) : number(0);
            };
            result.isNumber = isNotZero(divisor);
            Condition isOrdinary = mayOverflow ? above(add(divisor, constant<T>(1)), constant<T>(1)) : result.isNumber;
            m_out.branch(isOrdinary.value(), normalEdge(divides), rareEdge(doesNotDivide));

            m_out.appendTo(divides);
            T remainder = binary<T>(&FTL::Output::mod, dividend, divisor);
            if (mayBeNegative) {
                LBasicBlock hasSignOfItsOwn = newBlock();
                LBasicBlock isZero = newBlock();
                LBasicBlock isNegativeZero = newBlock();
                Join<Number> signedNumber(m_out);
                m_out.branch(isNotZero(remainder).value(), normalEdge(hasSignOfItsOwn), unlikelyEdge(isZero));
                m_out.appendTo(isZero);
                m_out.branch(lessThan(dividend, constant<T>(0)).value(), unlikelyEdge(isNegativeZero), normalEdge(hasSignOfItsOwn));
                m_out.appendTo(isNegativeZero);
                signedNumber.add(number(-0.0));
                m_out.appendTo(hasSignOfItsOwn);
                signedNumber.add(toNumber(remainder));
                answer(remainder, signedNumber.finish());
            } else
                answer(remainder, toNumber(remainder));

            m_out.appendTo(doesNotDivide);
            Number ofZero = std::signbit(remainderByZero(-1)) != std::signbit(remainderByZero(1)) && mayBeNegative ? select(lessThan(dividend, constant<T>(0)), number(remainderByZero(-1)), number(remainderByZero(1))) : number(remainderByZero(1));
            answer(constant<T>(0), mayOverflow ? select(result.isNumber, zeroWithSignOfDividend(), ofZero) : ofZero);

            m_out.appendTo(continuation);
            result.whole = T(m_out.phi(b3TypeOf(T::representation), wholes));
            result.zeroWhereWholeIsZero = result.whole;
            result.number = Number(m_out.phi(B3::Double, numbers));
            return result;
        } else
            whole = binary<T>(&FTL::Output::chillMod, dividend, divisor);
        result.whole = whole;
        if (!result.powerOfTwoDivisor)
            result.zeroWhereWholeIsZero = whole;
        if (!isNeverNaN)
            result.isNumber = isNotZero(divisor);
        result.number = remainderAsNumber(whole, dividend, divisor, !isNeverNaN);
        return result;
    }

    Condition testRemainder(Integer dividend, int32_t divisor, std::optional<int64_t> comparedWith)
    {
        uint64_t lowBits = static_cast<uint64_t>(divisor) - 1;
        int64_t constant = comparedWith.value_or(0);
        if (hasOneBitSet(static_cast<uint64_t>(divisor)) && static_cast<uint64_t>(constant < 0 ? -constant : constant) <= lowBits) {
            if (!constant) {
                Integer low = bitAnd(dividend, integer(static_cast<int64_t>(lowBits)));
                return comparedWith ? isZero(low) : isNotZero(low);
            }
            uint64_t sign = 1ull << 63;
            return equal(bitAnd(dividend, static_cast<int64_t>(sign | lowBits)), static_cast<int64_t>((constant < 0 ? sign : 0) | (static_cast<uint64_t>(constant) & lowBits)));
        }
        Integer remainder = remainderByConstant(dividend, divisor);
        return comparedWith ? equal(remainder, integer(constant)) : isNotZero(remainder);
    }

    Number remainder(Number dividend, Number divisor)
    {
        std::optional<double> constantDivisor = constantNumberOf(divisor.value());
        if (constantDivisor && std::abs(*constantDivisor) == 1) {
            Number fraction = sub(dividend, trunc(dividend));
            Condition isWhole = equal(fraction, 0);
            Number result = select(isWhole, mul(dividend, 0), fraction);
            constexpr double infinity = std::numeric_limits<double>::infinity();
            if (matchesHardwareNaN(resultAtRunTime(Math::fmodDouble, infinity, *constantDivisor)) && matchesHardwareNaN(resultAtRunTime(Math::fmodDouble, -infinity, *constantDivisor)))
                return result;
            return callLibraryIfNaN(result, RuntimeOperation::Fmod, dividend, divisor);
        }
        std::optional<int64_t> constantWholeDivisor = constantDivisor ? positiveWholeNumberOf(*constantDivisor) : std::nullopt;
        if (constantDivisor && !constantWholeDivisor) {
            bool hasFraction = *constantDivisor != std::trunc(*constantDivisor);
            if (std::optional<SplitDivisor> split = hasFraction ? splitDivisor(*constantDivisor) : std::nullopt)
                return remainderBySplitDivisor(dividend, divisor, *split);
            return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });
        }

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

private:
    enum class DividendSign : bool { Any, NonNegative };

    struct SplitDivisor {
        double magnitude;
        double high;
        double low;
        double reciprocal;
        double quotientLimit;
    };

    // A quotient below the limit times either half is exact, and the reciprocal is rounded up, so the quotient is the true one or one more.
    static std::optional<SplitDivisor> splitDivisor(double divisor)
    {
        constexpr unsigned precision = std::numeric_limits<double>::digits;
        constexpr unsigned lowBits = 27;
        double magnitude = std::abs(divisor);
        if (!(magnitude >= 0x1p-500 && magnitude <= 0x1p500))
            return std::nullopt;
        uint64_t bits = std::bit_cast<uint64_t>(magnitude);
        double high = std::bit_cast<double>(bits & ~((1ull << lowBits) - 1));
        double low = magnitude - high;
        unsigned significantBits = precision - std::countr_zero(bits | 1ull << (precision - 1));
        unsigned quotientBits = low ? precision - lowBits : std::min(precision - significantBits, 49u);
        return SplitDivisor { magnitude, high, low, std::nextafter(1 / magnitude, std::numeric_limits<double>::infinity()), std::ldexp(1.0, quotientBits) };
    }

    Number remainderBySplitDivisor(Number dividend, Number divisor, const SplitDivisor& split)
    {
        Number magnitude = abs(dividend);
        return selectByBranch<Number>(lessThan(magnitude, split.magnitude), [&] {
            return dividend;
        }, [&] {
            LBasicBlock slowPath = newBlock();
            Join<Number> join(m_out);
            Number quotient = trunc(mul(magnitude, split.reciprocal));
            branchUnless(lessThan(quotient, split.quotientLimit), slowPath);
            Number difference = sub(magnitude, mul(quotient, split.high));
            if (split.low)
                difference = sub(difference, mul(quotient, split.low));
            Number remainder = either<Number>(greaterThanOrEqual(difference, 0), [&] {
                return difference;
            }, [&] {
                return add(difference, split.magnitude);
            });
            join.add(Number(m_out.bitOr(remainder.value(), m_out.bitAnd(dividend.value(), m_out.constDouble(-0.0)))));

            m_out.appendTo(slowPath);
            join.add(callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor }));
            return join.finish();
        });
    }

    template<typename T>
    T constant(int64_t value)
    {
        if constexpr (std::is_same_v<T, Word32>)
            return word32(static_cast<int32_t>(value));
        else
            return integer(value);
    }

    template<typename T>
    std::pair<T, T> operandsOf(const ValueContext& value)
    {
        if (isStepByOne(value.opcode()))
            return { in<T>(value->valueOperand()), constant<T>(1) };
        return { in<T>(value->leftOperand()), in<T>(value->rightOperand()) };
    }

    Integer widened(Word32 value) { return signExtended(value); }
    Integer widened(Integer value) { return value; }

    template<typename T>
    T remainderOfNonNegative(T dividend, T divisor, bool isDividendUint32 = false)
    {
        std::optional<int64_t> constantDivisor = constantIntegerOf(divisor.value());
        if (constantDivisor && *constantDivisor > 0) {
            if (hasOneBitSet(static_cast<uint64_t>(*constantDivisor)))
                return bitAnd(dividend, constant<T>(*constantDivisor - 1));
            if constexpr (std::is_same_v<T, Word32>) {
                Word32 quotient(m_out.m_block->appendNew<B3::Value>(m_proc, B3::UDiv, m_out.origin(), dividend.value(), divisor.value()));
                return sub(dividend, mul(quotient, divisor));
            } else {
                if (isDividendUint32 && *constantDivisor <= std::numeric_limits<int32_t>::max())
                    return zeroExtended(remainderOfNonNegative(low32(dividend), word32(static_cast<int32_t>(*constantDivisor))));
                return remainderByConstant(dividend, *constantDivisor, DividendSign::NonNegative);
            }
        }
        return binary<T>(&FTL::Output::mod, dividend, divisor);
    }

    template<typename T>
    T remainderByPowerOfTwo(T dividend, uint64_t divisor)
    {
        constexpr unsigned numberOfBits = std::is_same_v<T, Word32> ? 32 : 64;
        unsigned shift = WTF::fastLog2(divisor);
        if (!shift)
            return constant<T>(0);
        T roundsTowardZero(m_out.lShr(m_out.aShr(dividend.value(), numberOfBits - 1), numberOfBits - shift));
        T rounded = add(dividend, roundsTowardZero);
        return sub(dividend, bitAnd(rounded, constant<T>(-static_cast<int64_t>(divisor))));
    }

    Integer remainderByConstant(Integer dividend, int64_t divisor, DividendSign sign = DividendSign::Any)
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

    static double remainderByZero(double dividend) { return AOT::withoutPayload(resultAtRunTime(Math::fmodDouble, dividend, 0)); }

    template<typename T>
    Number remainderAsNumber(T whole, T dividend, T divisor, bool mayBeNaN)
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
                    double ofNegative = remainderByZero(-1);
                    double ofNonNegative = remainderByZero(1);
                    if (std::signbit(ofNegative) != std::signbit(ofNonNegative)) {
                        CCallHelpers::Jump isNonNegative = branchOnTest(jit, CCallHelpers::PositiveOrZero, dividend);
                        produce(ofNegative);
                        isNonNegative.link(&jit);
                    }
                    produce(ofNonNegative);
                    isNumber.link(&jit);
                }
                branchOnTest(jit, CCallHelpers::PositiveOrZero, dividend).linkTo(done, &jit);
                produce(-0.0);
            });
        });
        return Number(patchpoint);
    }

    Word32 nonNegativeInt32OrElse(Number number, LBasicBlock otherwise)
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
        Integer bitsOfNarrow = conversions.bitsOf(toNumber(narrow));
        branchUnless(equal(bitsOfNarrow, conversions.bitsOf(number)), otherwise);
        branchUnless(greaterThanOrEqual(narrow, word32(0)), otherwise);
        return narrow;
    }

    Integer nonNegativeIntegerOrElse(Number number, LBasicBlock otherwise)
    {
        if (number.value()->opcode() == B3::IToD && number.value()->child(0)->type() == B3::Int32)
            return zeroExtended(nonNegativeInt32OrElse(number, otherwise));
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Integer whole = conversions.toInteger(number);
        Integer bitsOfWhole = conversions.bitsOf(toNumber(whole));
        branchUnless(equal(bitsOfWhole, conversions.bitsOf(number)), otherwise);
        branchUnless(greaterThanOrEqual(whole, integer(0)), otherwise);
        return whole;
    }
};

class PowerEmitter final : public InlineMathEmitter {
public:
    explicit PowerEmitter(CodeGenerator& generator)
        : InlineMathEmitter(generator)
    {
    }

    Number power(Number base, Number exponent)
    {
        if (std::optional<double> constantExponent = constantNumberOf(exponent.value())) {
            if (std::optional<Number> result = powerWithConstantExponent(base, *constantExponent))
                return *result;
        }
        if (std::optional<double> constantBase = constantNumberOf(base.value())) {
            if (std::optional<Number> result = powerWithConstantBase(*constantBase, exponent))
                return *result;
        }
        if (exponent.value()->opcode() == B3::IToD)
            return powerWithWholeExponent(base, exponent);
        return callLeaf<Number>(RuntimeOperation::Pow, { base, exponent });
    }

private:
    // What operationMathPow does for such an exponent, in the same order of multiplications.
    Number powerWithWholeExponent(Number base, Number exponent)
    {
        LBasicBlock test = newBlock();
        LBasicBlock step = newBlock();
        LBasicBlock genericPath = newBlock();
        LBasicBlock done = newBlock();
        Integer whole = wholeExponentOrElse(exponent, genericPath);
        branchUnless(belowOrEqual(whole, static_cast<int64_t>(maxExponentForIntegerMathPow)), genericPath);
        ValueFromBlock firstBits = m_out.anchor(whole.value());
        ValueFromBlock firstResult = m_out.anchor(number(1).value());
        ValueFromBlock firstSquare = m_out.anchor(base.value());
        m_out.jump(test);

        m_out.appendTo(test);
        LValue bits = m_out.phi(B3::Int64, firstBits);
        LValue result = m_out.phi(B3::Double, firstResult);
        LValue square = m_out.phi(B3::Double, firstSquare);
        ValueFromBlock finished = m_out.anchor(result);
        branch(Condition(m_out.notEqual(bits, m_out.constInt64(0))), { normalEdge(step), normalEdge(done) });

        m_out.appendTo(step);
        LValue isOdd = m_out.testNonZero64(bits, m_out.constInt64(1));
        m_out.addIncomingToPhi(result, m_out.anchor(m_out.select(isOdd, m_out.doubleMul(result, square), result)));
        m_out.addIncomingToPhi(square, m_out.anchor(m_out.doubleMul(square, square)));
        m_out.addIncomingToPhi(bits, m_out.anchor(m_out.lShr(bits, m_out.constInt32(1))));
        m_out.jump(test);

        m_out.appendTo(genericPath);
        ValueFromBlock generic = m_out.anchor(callLeaf<Number>(RuntimeOperation::Pow, { base, exponent }).value());
        m_out.jump(done);

        m_out.appendTo(done);
        return Number(m_out.phi(B3::Double, finished, generic));
    }

    std::optional<Number> powerWithConstantExponent(Number base, double exponent)
    {
        if (std::abs(exponent) == 0.5) {
            Number magnitude = abs(base);
            Number zero = number(0);
            Number infinity = number(std::numeric_limits<double>::infinity());
            Condition isZero = equal(magnitude, zero);
            Condition isInfinite = equal(magnitude, infinity);
            Number result;
            if (exponent > 0)
                result = select(isZero, zero, select(isInfinite, infinity, sqrt(base)));
            else {
                Number one = number(1);
                result = select(isZero, infinity, select(isInfinite, zero, div(one, sqrt(base))));
            }
            if (matchesHardwareNaN(resultAtRunTime(operationMathPow, -1, exponent)))
                return result;
            return callLibraryIfNaN(result, RuntimeOperation::Pow, base, number(exponent));
        }
        if (!(exponent >= 0 && exponent <= maxExponentForIntegerMathPow) || exponent != std::trunc(exponent))
            return std::nullopt;
        if (exponent == 1)
            return emitter<ConversionEmitter>().withoutPayload(base);
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

    Integer wholeExponentOrElse(Number exponent, LBasicBlock otherwise)
    {
        if (exponent.value()->opcode() == B3::IToD) {
            LValue whole = exponent.value()->child(0);
            return whole->type() == B3::Int32 ? signExtended(Word32(whole)) : Integer(whole);
        }
        Word32 narrow = low32(emitter<ConversionEmitter>().toInteger(exponent));
        branchUnless(equal(toNumber(narrow), exponent), otherwise);
        return signExtended(narrow);
    }

    Number powerOfTwo(Integer exponent)
    {
        constexpr int64_t exponentBias = 1023;
        constexpr int32_t numberOfFractionBits = 52;
        return Number(m_out.bitCast(m_out.shl(add(exponent, exponentBias).value(), numberOfFractionBits), B3::Double));
    }

    Number powerOfTen(Integer exponent)
    {
        Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(numbers.powersOfTen));
        return Number(m_out.loadDouble(m_out.baseIndex(m_heaps.AOTInstance_fixedFields, table.value(), exponent.value(), FTL::ScaleEight)));
    }

    std::optional<Number> powerWithConstantBase(double base, Number exponent)
    {
        if (base != 2 && base != 10)
            return std::nullopt;
        bool isBaseTwo = base == 2;
        Range handled = isBaseTwo ? Range { -1022, 1023 } : Range { 0, std::numeric_limits<double>::max_exponent10 };

        LBasicBlock genericPath = newBlock();
        Join<Number> join(m_out);
        Integer whole = wholeExponentOrElse(exponent, genericPath);
        branchUnless(belowOrEqual(sub(whole, handled.minimum), handled.maximum - handled.minimum), genericPath);
        join.add(isBaseTwo ? powerOfTwo(whole) : powerOfTen(whole));

        m_out.appendTo(genericPath);
        join.add(callLeaf<Number>(RuntimeOperation::Pow, { number(base), exponent }));
        return join.finish();
    }
};

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
    return select(emitter<ConversionEmitter>().isMarker(unboxed, nullMarker), 0, Number(unboxed.value));
}

Number OperatorEmitter::asNumericOperand(ValueID id)
{
    switch (representationOf(id)) {
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return in<Number>(id);
    case Representation::OptionalDouble:
        return asNumber(in<OptionalNumber>(id), typeOf(id));
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
    Integer x = int32Of(left);
    Integer y = int32Of(right);
    Integer exact = op == ArithmeticOperator::Add ? add(x, y) : sub(x, y);
    join.add(conversions.boxInteger(exact, { 2 * static_cast<int64_t>(INT32_MIN), 2 * static_cast<int64_t>(INT32_MAX) + 1 }));

    m_out.appendTo(numberCase);
    Number leftNumber = asNumber(left, leftType);
    join.add(conversions.boxNumberByStub(arithmeticOnNumbers(op, leftNumber, asNumber(right, rightType))));
    return join.finish();
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    return failWithZero(value);
}

Operand OperatorEmitter::arithmeticOnCheckedIntegers(const ValueContext& value, ArithmeticOperator op)
{
    IntegerArithmeticEmitter integers(generator());
    bool is32Bit = false;
    switch (representationOf(isStepByOne(value.opcode()) ? value->valueOperand() : value->leftOperand())) {
    case Representation::Int32:
        is32Bit = true;
        break;
    case Representation::Int64:
        break;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return failWithZero(value);
    }
    if (op != ArithmeticOperator::Mod)
        return is32Bit ? integers.resultAsNumber<Word32>(value, op) : integers.resultAsNumber<Integer>(value, op);

    WholeRemainder remainder = is32Bit ? integers.remainderOfIntegers<Word32>(value) : integers.remainderOfIntegers<Integer>(value);
    m_wholeRemainders.set(value.id().index(), remainder);
    Word32 word = is32Bit ? remainder.whole.as<Word32>() : low32(remainder.whole.as<Integer>());
    m_int32OfNumbers.add(value.id().index(), Int32sOfNumber()).iterator->value.conversions.append({ value.block(), word });
    return remainder.number;
}

std::optional<OperatorEmitter::RemainderLeftToTest> OperatorEmitter::findRemainderLeftToTest(ValueID id)
{
    const Value& remainder = m_function.at(id);
    if (remainder.opcode != Opcode::Mod || remainder.form() != OperatorForm::Numbers || (representationOf(id) != Representation::Double && !holdsAnyNaN(representationOf(id))) || generator().validatesOrChecksProofs())
        return std::nullopt;
    const Value& divisor = m_function.at(remainder.rightOperand());
    if (divisor.opcode != Opcode::Integer || divisor.integer() < 2 || divisor.integer() > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    std::optional<Use> use = analyses().useLists().singleUse(id);
    if (!use || use->kind != Use::Kind::Operand || use->block != analyses().useLists().locationOf(id).block)
        return std::nullopt;
    if (!findNarrowedParameters(m_function, analyses().useLists(), *use).isEmpty())
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
        IntegerArithmeticEmitter integers(generator());
        LBasicBlock isNotWhole = newBlock();
        Integer whole = emitter<ConversionEmitter>().toInteger(dividend);
        branchUnless(equal(toNumber(whole), dividend), isNotWhole);
        branch(integers.testRemainder(whole, remainder->divisor, remainder->comparedWith), targets);

        m_out.appendTo(isNotWhole);
        Number number = integers.remainder(dividend, this->number(remainder->divisor));
        branch(remainder->comparedWith ? equal(number, this->number(static_cast<double>(*remainder->comparedWith))) : isNeitherZeroNorNaN(number), targets);
    });
    return true;
}

const WholeRemainder* OperatorEmitter::findWholeRemainder(ValueID number)
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
        return IntegerArithmeticEmitter(generator()).remainder(left, right);
    case ArithmeticOperator::Pow:
        return pow(left, right);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::pow(Number base, Number exponent)
{
    return PowerEmitter(generator()).power(base, exponent);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
