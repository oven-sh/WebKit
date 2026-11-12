/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTPureOperations.h"
#include "AOTRepresentationRules.h"
#include "AOTThrowEmitter.h"
#include "AOTTypedArrayKinds.h"
#include "B3ComputeDivisionMagic.h"
#include "CCallHelpers.h"
#include "MathCommon.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>
#include <wtf/Atomics.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t quietNaNBits = 0x7ff8ull << 48;
static_assert((undefinedDouble & quietNaNBits) == quietNaNBits);

static std::optional<int64_t> constantIntegerOf(LValue value)
{
    if (value->hasInt())
        return value->asInt();
    if (value->opcode() == Machine::SExt32 && value->child(0)->hasInt32())
        return value->child(0)->asInt32();
    return std::nullopt;
}

static std::optional<int64_t> magnitudeOfConstantDivisor(LValue divisor)
{
    std::optional<int64_t> constantDivisor = constantIntegerOf(divisor);
    int64_t leastInteger = divisor->type() == Machine::Int32 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min();
    if (!constantDivisor || !*constantDivisor || *constantDivisor == leastInteger)
        return std::nullopt;
    return *constantDivisor < 0 ? -*constantDivisor : *constantDivisor;
}

static std::optional<double> constantNumberOf(LValue value)
{
    if (value->hasDouble())
        return value->asDouble();
    if (value->opcode() != Machine::IToD)
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

    static bool matchesHardwareNaN(double libraryNaN) { return std::signbit(libraryNaN) == isX86_64(); }

    template<typename Function>
    static double resultAtRunTime(Function* function, double left, double right) { return WTF::opaque(function)(left, right); }

    Number callLibraryIfNaN(Number inlineResult, RuntimeOperation function, Number left, Number right)
    {
        return either<Number>(equal(inlineResult, inlineResult), [&] {
            return inlineResult;
        }, [&] {
            return callLeaf<Number>(function, { left, right });
        });
    }
};

class IntegerArithmeticEmitter final : public InlineMathEmitter {
public:
    explicit IntegerArithmeticEmitter(CodeGenerator& generator)
        : InlineMathEmitter(generator)
    {
    }

    Representation widthOfOperands(const ValueContext& value)
    {
        bool fitInt32 = true;
        for (ValueID operand : value->operands()) {
            if (!constantsInFormOfUses.contains(opcodeOf(operand)))
                return representationOf(operand);
            fitInt32 = fitInt32 && typeOf(operand).isInt32();
        }
        return fitInt32 ? Representation::Int32 : Representation::Int64;
    }

    template<typename T>
    Operand exactResult(const ValueContext& value, ArithmeticOperator op)
    {
        auto [left, right] = operandsOf<T>(value);
        switch (op) {
        case ArithmeticOperator::Add:
            return heldToWhatNumbersGive(value, op, left, right, add(left, right));
        case ArithmeticOperator::Sub:
            return heldToWhatNumbersGive(value, op, left, right, sub(left, right));
        case ArithmeticOperator::Mul:
            return heldToWhatNumbersGive(value, op, left, right, mul(left, right));
        case ArithmeticOperator::Mod:
            if (checksProofs())
                emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(greaterThanOrEqual(left, constant<T>(0)), logicalAnd(isNotZero(right), lessThanOrEqual(left, constant<T>(typeOf(value->leftOperand()).integers.maximum)))), std::nullopt, ProofSource::OtherOperation);
            if (std::optional<SharedQuotient<T>> shared = typeOf(value->rightOperand()).integers.minimum > 0 ? findSharedQuotient(value, right) : std::nullopt) {
                T quotient = quotientOfOperands(value, *shared, left, right);
                if (shared->divisionBehind)
                    emitter<BuiltinCallEmitter>().noteQuotient(shared->divisionBehind, shared->whole);
                return sub(left, mul(quotient, right));
            }
            return remainderOfNonNegative(left, right, typeOf(value->leftOperand()).integers.maximum);
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
            Range multiplicands = typeOf(value->leftOperand()).integers;
            Range multipliers = typeOf(value->rightOperand()).integers;
            if (productIsRoundedOnceFromIntegers(multiplicands, multipliers)) {
                Integer exact = mul(widened(left, multiplicands), widened(right, multipliers));
                Number product = toNumber(exact);
                if (checksProofs()) {
                    Number x = toNumber(left);
                    Condition holds = logicalAnd(equal(mul(x, toNumber(right)), product), logicalOr(isNotZero(exact), greaterThanOrEqual(bitOr(left, right), constant<T>(0))));
                    emitter<ThrowEmitter>().requireOrCrash(value, holds, std::nullopt, ProofSource::OtherOperation);
                }
                return product;
            }
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
    Number remainderOfIntegers(const ValueContext& value)
    {
        auto [dividend, divisor] = operandsOf<T>(value);
        Range dividends = typeOf(value->leftOperand()).integers;
        bool mayBeNegative = dividends.minimum < 0;
        bool mayOverflow = dividends.minimum <= (std::is_same_v<T, Word32> ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min());
        if (checksProofs() && !mayOverflow)
            emitter<ThrowEmitter>().requireOrCrash(value, greaterThanOrEqual(dividend, constant<T>(dividends.minimum)), std::nullopt, ProofSource::OtherOperation);
        auto zeroWithSignOfDividend = [&] {
            return mayBeNegative ? select(lessThan(dividend, constant<T>(0)), number(-0.0), number(0)) : number(0);
        };
        auto remainderOfDivisionByZero = [&] {
            bool hasSignOfDividend = mayBeNegative && std::signbit(remainderByZero(-1)) != std::signbit(remainderByZero(1));
            return hasSignOfDividend ? select(lessThan(dividend, constant<T>(0)), number(remainderByZero(-1)), number(remainderByZero(1))) : number(remainderByZero(1));
        };

        if (std::optional<int64_t> constantDivisor = constantIntegerOf(divisor.value())) {
            if (!*constantDivisor)
                return remainderOfDivisionByZero();
            if (*constantDivisor == -1)
                return zeroWithSignOfDividend();
        }

        LBasicBlock divides = newBlock();
        LBasicBlock doesNotDivide = newBlock();
        Join<Number> join(m_out);
        Condition answerIsNumber = isNotZero(divisor);
        Condition isOrdinary = mayOverflow ? above(add(divisor, constant<T>(1)), constant<T>(1)) : answerIsNumber;
        std::optional<SharedQuotient<T>> shared = findSharedQuotient(value, divisor);
        Vector<ValueFromBlock, 2> quotients;
        ScopeFork fork = m_out.forkScope();
        m_out.branch(isOrdinary.value(), normalEdge(divides), rareEdge(doesNotDivide));

        m_out.appendToArm(divides, fork);
        T remainder = shared ? sub(dividend, mul(quotientOfOperands(value, *shared, dividend, divisor), divisor)) : signedRemainder(dividend, divisor);
        auto noteQuotientOfArm = [&](Integer ofArm) {
            quotients.append(m_out.anchor(ofArm.value()));
        };
        bool notesQuotient = shared && shared->divisionBehind;
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
            Number numberWithSign = signedNumber.finish();
            if (notesQuotient)
                noteQuotientOfArm(shared->whole);
            join.add(numberWithSign);
        } else {
            Number number = toNumber(remainder);
            if (notesQuotient)
                noteQuotientOfArm(shared->whole);
            join.add(number);
        }

        m_out.appendToArm(doesNotDivide, fork);
        Number ofZero = remainderOfDivisionByZero();
        Number answer = mayOverflow ? select(answerIsNumber, zeroWithSignOfDividend(), ofZero) : ofZero;
        if (notesQuotient)
            noteQuotientOfArm(mayOverflow ? select(answerIsNumber, sub(integer(0), widened(dividend)), integer(0)) : integer(0));
        join.add(answer);
        Number result = join.finishBehind(fork);
        if (notesQuotient)
            emitter<BuiltinCallEmitter>().noteQuotient(shared->divisionBehind, Integer(m_out.phi(machineTypeOf(Integer::representation), quotients)));
        if (isOrdinary.value()->hasInt() && isOrdinary.value()->asInt())
            emitter<ConversionEmitter>().noteIntegerBehind(result, remainder.value());
        return result;
    }

    template<typename T>
    struct SharedQuotient {
        std::optional<T> quotientAhead;
        ValueID divisionBehind;
        Integer whole;
    };

    template<typename T>
    std::optional<SharedQuotient<T>> findSharedQuotient(const ValueContext& value, T divisor)
    {
        if (std::optional<int64_t> magnitude = magnitudeOfConstantDivisor(divisor.value()); magnitude && hasOneBitSet(static_cast<uint64_t>(*magnitude)))
            return std::nullopt;
        auto found = emitter<BuiltinCallEmitter>().findDivisionOfSameOperands(value);
        if (!found)
            return std::nullopt;
        if (!found->isAhead)
            return SharedQuotient<T> { std::nullopt, found->division, { } };
        bool isWord = representationOf(found->division) == Representation::Int32;
        if constexpr (std::is_same_v<T, Word32>)
            return SharedQuotient<T> { isWord ? in<Word32>(found->division) : low32(in<Integer>(found->division)), { }, { } };
        else
            return SharedQuotient<T> { isWord ? widened(in<Word32>(found->division)) : in<Integer>(found->division), { }, { } };
    }

    template<typename T>
    T quotientOfOperands(const ValueContext& value, SharedQuotient<T>& shared, T dividend, T divisor)
    {
        if (shared.quotientAhead)
            return *shared.quotientAhead;
        shared.whole = emitter<BuiltinCallEmitter>().divide(widened(dividend), typeOf(value->leftOperand()).integers, widened(divisor), typeOf(value->rightOperand()).integers);
        if constexpr (std::is_same_v<T, Word32>)
            return low32(shared.whole);
        else
            return shared.whole;
    }

    Number remainder(const ValueContext* value, Number dividend, Number divisor)
    {
        static constexpr OpcodeSet remainders { Opcode::Mod };
        std::optional<double> constantDivisor = constantNumberOf(divisor.value());
        if (constantDivisor && std::abs(*constantDivisor) == 1) {
            m_out.noteFormOf(remainders, Form::Open, StateOfKnowledge::NumbersAndDivisorOne);
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
            if (std::optional<SplitDivisor> split = hasFraction ? splitDivisor(*constantDivisor) : std::nullopt) {
                m_out.noteFormOf(remainders, Form::Open, StateOfKnowledge::NumbersAndDivisorWithFraction);
                return remainderBySplitDivisor(dividend, divisor, *split);
            }
            m_out.noteFormOf(remainders, Form::Runtime, StateOfKnowledge::NumbersAndOtherDivisor);
            return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });
        }

        m_out.noteFormOf(remainders, Form::Open, constantWholeDivisor ? StateOfKnowledge::NumbersAndWholeDivisor : StateOfKnowledge::Numbers);
        if (value && constantWholeDivisor && isOnlyTruncatedBelow(*value, *constantWholeDivisor))
            return dividend;
        LBasicBlock genericPath = newBlock();
        Join<Number> join(m_out);
        IntegersOfRemainder integers;
        integers.areIndexes = value && mayBeKey(*value);
        integers.areInt32s = value && mayNeedInt32(*value);
        integers.areTruncated = value && isOnlyTruncated(*value);
        bool isKeyAlone = integers.areIndexes && isOnlyKeyOfRead(*value);
        auto addWhole = [&](Integer remainder) {
            integers.add(m_out, remainder, remainder);
            join.add(isKeyAlone ? dividend : toNumber(remainder));
        };
        std::optional<Integer> provenDividend = value ? provenNonNegativeInteger(*value, (*value)->leftOperand(), dividend) : std::nullopt;
        auto hasNoFraction = [&](ValueID operand) {
            return !checksProofs() && typeOf(operand).isNumberWithoutFraction();
        };
        if (constantWholeDivisor) {
            Integer wholeDividend = provenDividend ? *provenDividend : wholeDividendOrElse(dividend, join, integers, genericPath, value && hasNoFraction((*value)->leftOperand()));
            addWhole(remainderOfNonNegative(wholeDividend, integer(*constantWholeDivisor)));
        } else {
#if CPU(ARM64)
            if (!provenDividend && MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
                LBasicBlock widerPath = newBlock();
                Word32 narrowDividend = nonNegativeInt32OrElse(dividend, widerPath);
                Word32 narrowDivisor = nonNegativeInt32OrElse(divisor, widerPath);
                branchUnless(isNotZero(narrowDivisor), genericPath);
                addWhole(zeroExtended(remainderOfNonNegative(narrowDividend, narrowDivisor)));
                m_out.appendTo(widerPath);
            }
#endif
            Integer wholeDivisor = positiveIntegerOrElse(divisor, genericPath, value && hasNoFraction((*value)->rightOperand()));
            Integer wholeDividend = provenDividend ? *provenDividend : wholeDividendOrElse(dividend, join, integers, genericPath, value && hasNoFraction((*value)->leftOperand()));
            addWhole(remainderOfNonNegative(wholeDividend, wholeDivisor));
        }

        Number result = joinedWithAnyNumbers(join, integers, genericPath, dividend, divisor, constantWholeDivisor);
        if (isKeyAlone)
            emitter<ConversionEmitter>().noteHeldAsIndex(result);
        return result;
    }

    Number remainderByWholeDivisor(const ValueContext& value, Number dividend, Number divisor, Integer wholeDivisor)
    {
        if (checksProofs())
            emitter<ThrowEmitter>().requireOrCrash(value, logicalOr(equal(wholeDivisor, integer(0)), equal(toNumber(wholeDivisor), divisor)), std::nullopt, ProofSource::OtherOperation);
        LBasicBlock genericPath = newBlock();
        Join<Number> join(m_out);
        IntegersOfRemainder integers;
        std::optional<Integer> provenDividend = provenNonNegativeInteger(value, value->dividendOperand(), dividend);
        branchUnless(isNotZero(wholeDivisor), genericPath);
        integers.areIndexes = mayBeKey(value);
        integers.areInt32s = mayNeedInt32(value);
        integers.areTruncated = isOnlyTruncated(value);
        bool hasNoFraction = !checksProofs() && typeOf(value->dividendOperand()).isNumberWithoutFraction();
        Integer wholeDividend = provenDividend ? *provenDividend : wholeDividendOrElse(dividend, join, integers, genericPath, hasNoFraction);
        Integer remainder = remainderOfNonNegative(wholeDividend, wholeDivisor);
        integers.add(m_out, remainder, remainder);
        bool isKeyAlone = integers.areIndexes && isOnlyKeyOfRead(value);
        join.add(isKeyAlone ? dividend : toNumber(remainder));
        Number result = joinedWithAnyNumbers(join, integers, genericPath, dividend, divisor, std::nullopt);
        if (isKeyAlone)
            emitter<ConversionEmitter>().noteHeldAsIndex(result);
        return result;
    }

    void branchOnRemainder(Number dividend, int32_t divisor, std::optional<int32_t> comparedWith, BranchTargets targets)
    {
        BranchTargets congruenceTargets = comparedWith ? targets : BranchTargets { targets.notTaken, targets.taken };
        bool isInt32 = dividend.value()->opcode() == Machine::IToD && dividend.value()->child(0)->type() == Machine::Int32;
        LBasicBlock isNoInt32 = isInt32 ? nullptr : newBlock();
        Word32 whole = isInt32 ? Word32(dividend.value()->child(0)) : emitter<ConversionEmitter>().truncateToWord(dividend);
        if (!isInt32)
            branchUnless(equal(toNumber(whole), dividend), isNoInt32);
        branch(isCongruentTo(whole, divisor, comparedWith.value_or(0)), congruenceTargets);
        build().statistics().count(StatisticID::MultiplesTestedWithoutRemainder);
        if (isInt32)
            return;

        m_out.appendTo(isNoInt32);
        Number left = hasOneBitSet(static_cast<uint32_t>(divisor)) ? unsignedRemainderByPowerOfTwo(dividend, divisor) : remainder(nullptr, dividend, number(divisor));
        branch(comparedWith ? equal(left, static_cast<double>(*comparedWith)) : isNeitherZeroNorNaN(left), targets);
    }

private:
    Number unsignedRemainderByPowerOfTwo(Number dividend, double divisor)
    {
        Number quotient = trunc(mul(dividend, number(1 / divisor)));
        return sub(dividend, mul(quotient, number(divisor)));
    }

    bool isInlinePowerOfTwo(std::optional<int64_t> divisor)
    {
        constexpr double infinity = std::numeric_limits<double>::infinity();
        return divisor && hasOneBitSet(static_cast<uint64_t>(*divisor)) && matchesHardwareNaN(resultAtRunTime(Math::fmodDouble, infinity, static_cast<double>(*divisor))) && matchesHardwareNaN(resultAtRunTime(Math::fmodDouble, -infinity, static_cast<double>(*divisor)));
    }

    Number remainderOfAnyNumbers(Number dividend, Number divisor, std::optional<int64_t> constantWholeDivisor)
    {
        if (!isInlinePowerOfTwo(constantWholeDivisor))
            return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });
        Number remainder = unsignedRemainderByPowerOfTwo(dividend, static_cast<double>(*constantWholeDivisor));
        return select(equal(remainder, 0), mul(dividend, 0), remainder);
    }

    Condition isCongruentTo(Word32 dividend, int32_t divisor, int32_t remainder)
    {
        if (hasOneBitSet(static_cast<uint32_t>(divisor))) {
            if (!remainder)
                return isZero(bitAnd(dividend, word32(divisor - 1)));
            int32_t sign = std::numeric_limits<int32_t>::min();
            int32_t bits = (remainder < 0 ? sign : 0) | (remainder & (divisor - 1));
            return equal(bitAnd(dividend, word32(sign | (divisor - 1))), word32(bits));
        }

        unsigned shift = std::countr_zero(static_cast<uint32_t>(divisor));
        uint32_t oddPart = static_cast<uint32_t>(divisor) >> shift;
        uint32_t inverse = oddPart;
        while (inverse * oddPart != 1)
            inverse *= 2 - inverse * oddPart;
        uint32_t greatestInt32 = std::numeric_limits<int32_t>::max();
        uint32_t factor = remainder < 0 ? -inverse : inverse;
        uint32_t offset = -factor * static_cast<uint32_t>(remainder);
        uint32_t greatestQuotient = (remainder < 0 ? greatestInt32 + 1 + static_cast<uint32_t>(remainder) : greatestInt32 - static_cast<uint32_t>(remainder)) / static_cast<uint32_t>(divisor);
        if (!remainder) {
            offset = greatestInt32 / oddPart >> shift << shift;
            greatestQuotient = 2 * offset >> shift;
        }
        Word32 quotient = add(mul(dividend, word32(static_cast<int32_t>(factor))), word32(static_cast<int32_t>(offset)));
        if (shift)
            quotient = Word32(m_out.bitOr(m_out.lShr(quotient.value(), shift), m_out.shl(quotient.value(), 32 - shift)));
        return belowOrEqual(quotient, word32(static_cast<int32_t>(greatestQuotient)));
    }

    struct SplitDivisor {
        double magnitude;
        double high;
        double low;
        double reciprocal;
        double quotientLimit;
    };

    bool checksProofs() { return emittersCheckProofs(); }

    bool isOnlyKeyOfRead(const ValueContext& value)
    {
        const Code& code = generator().code();
        if (code.useCountOf(value.id()) != 1 || value.representation() != Representation::Double || generator().keepsValuesInHeapFrame() || generator().isLayingOutTurns())
            return false;
        ValueID user = code.onlyUserOf(value.id());
        if (!user || code.at(user).opcode != Opcode::GetArrayElement)
            return false;
        const Operation& read = code.at(user);
        return read.level() == KnowledgeLevel::Proven && read.positionOperand() == value.id() && !read.keyIsIntegral() && !read.isInBounds() && !read.isPresent();
    }

    bool mayBeKey(const ValueContext& value)
    {
        static constexpr OpcodeSet numberOnlyUsers = allNumericOperators | allRelations | allEqualities | allBitwiseOperators | OpcodeSet { Opcode::ToBoolean, Opcode::Box, Opcode::NumberToInteger };
        const Code& code = generator().code();
        if (code.useCountOf(value.id()) != 1)
            return code.useCountOf(value.id());
        ValueID user = code.onlyUserOf(value.id());
        return user && !numberOnlyUsers.contains(code.at(user).opcode);
    }

    bool mayNeedInt32(const ValueContext& value)
    {
        return value.representation() == Representation::Double && generator().code().useCountOf(value.id()) > 1;
    }

    bool isOnlyTruncatedBelow(const ValueContext& value, int64_t divisor)
    {
        if (divisor < 1 << 8 || (divisor & (divisor - 1)) || checksProofs() || !isOnlyTruncated(value))
            return false;
        const Operation& operation = generator().code().at(generator().code().onlyUserOf(value.id()));
        unsigned bitsTaken = operation.opcode == Opcode::PutTypedArrayElement ? 8 * elementSize(operation.typedArrayType()) : 32;
        return divisor >> bitsTaken;
    }

    bool isOnlyTruncated(const ValueContext& value)
    {
        static constexpr OpcodeSet truncations { Opcode::ToInt32, Opcode::ToUint32 };
        const Code& code = generator().code();
        ValueID user = value.representation() == Representation::Double && code.useCountOf(value.id()) == 1 ? code.onlyUserOf(value.id()) : ValueID();
        if (!user)
            return false;
        const Operation& operation = code.at(user);
        if (allBitwiseOperators.contains(operation.opcode))
            return operation.form() == OperatorForm::Numbers;
        if (operation.opcode == Opcode::PutTypedArrayElement)
            return operation.valueOperand() == value.id() && operation.positionOperand() != value.id() && operation.isInsideView() && hasIntegerElements(operation.typedArrayType());
        return truncations.contains(operation.opcode);
    }

    static constexpr int64_t noInt32 = 1ll << 62;

    struct IntegersOfRemainder {
        void add(Output& out, Integer index, Integer int32)
        {
            if (areTruncated)
                truncations.append(out.anchor(out.castToInt32(index.value())));
            if (areIndexes)
                indexes.append(out.anchor(index.value()));
            if (areInt32s)
                int32s.append(out.anchor(int32.value()));
        }

        Vector<ValueFromBlock, 3> indexes;
        Vector<ValueFromBlock, 3> int32s;
        Vector<ValueFromBlock, 3> truncations;
        bool areTruncated { false };
        bool areIndexes { false };
        bool areInt32s { false };
    };

    Integer wholeDividendOrElse(Number dividend, Join<Number>& join, IntegersOfRemainder& integers, LBasicBlock genericPath, bool hasNoFraction)
    {
        if (!isX86_64() || (dividend.value()->opcode() == Machine::IToD && dividend.value()->child(0)->type() == Machine::Int32))
            return nonNegativeIntegerOrElse(dividend, genericPath);
        LBasicBlock isNotPositive = newBlock();
        LBasicBlock isZero = newBlock();
        LBasicBlock continuation = newBlock();
        Integer whole = positiveIntegerOrElse(dividend, isNotPositive, hasNoFraction);
        m_out.jump(continuation);
        m_out.appendTo(isNotPositive);
        branch(equal(dividend, 0), { rareEdge(isZero), normalEdge(genericPath) });
        m_out.appendTo(isZero);
        integers.add(m_out, integer(0), integers.areInt32s ? select(equal(emitter<ConversionEmitter>().bitsOf(dividend), integer(0)), integer(0), integer(noInt32)) : integer(0));
        join.add(dividend);
        m_out.appendTo(continuation);
        return whole;
    }

    Number joinedWithAnyNumbers(Join<Number>& join, IntegersOfRemainder& integers, LBasicBlock genericPath, Number dividend, Number divisor, std::optional<int64_t> constantWholeDivisor)
    {
        m_out.appendTo(genericPath);
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Number ofAnyNumbers = remainderOfAnyNumbers(dividend, divisor, constantWholeDivisor);
        std::optional<Word32> truncation = integers.areTruncated ? std::optional(conversions.toInt32(ofAnyNumbers)) : std::nullopt;
        if (truncation)
            integers.truncations.append(m_out.anchor(truncation->value()));
        if (integers.areIndexes) {
            Integer whole = conversions.truncateUnchecked(ofAnyNumbers);
            integers.indexes.append(m_out.anchor(select(equal(toNumber(whole), ofAnyNumbers), whole, integer(-1)).value()));
        }
        if (integers.areInt32s) {
            Word32 word = conversions.truncateToWord(ofAnyNumbers);
            Condition isExact = equal(conversions.bitsOf(toNumber(word)), conversions.bitsOf(ofAnyNumbers));
            integers.int32s.append(m_out.anchor(select(isExact, signExtended(word), integer(noInt32)).value()));
        }
        join.add(ofAnyNumbers);
        Number result = join.finish();
        if (truncation)
            conversions.noteTruncationOfNumber(result, Word32(m_out.phi(machineTypeOf(Word32::representation), integers.truncations)));
        if (integers.areIndexes)
            conversions.noteIndexOfNumber(result, Integer(m_out.phi(machineTypeOf(Integer::representation), integers.indexes)));
        if (integers.areInt32s)
            conversions.noteInt32OfNumber(result, Integer(m_out.phi(machineTypeOf(Integer::representation), integers.int32s)));
        return result;
    }

    template<typename T>
    T heldToWhatNumbersGive(const ValueContext& value, ArithmeticOperator op, T left, T right, T result)
    {
        if (!checksProofs())
            return result;
        Number x = toNumber(left);
        Number y = toNumber(right);
        Number ofNumbers = op == ArithmeticOperator::Add ? add(x, y) : op == ArithmeticOperator::Sub ? sub(x, y) : mul(x, y);
        Condition holds = equal(ofNumbers, toNumber(result));
        if (op == ArithmeticOperator::Mul)
            holds = logicalAnd(holds, logicalOr(isNotZero(result), greaterThanOrEqual(bitOr(left, right), constant<T>(0))));
        emitter<ThrowEmitter>().requireOrCrash(value, holds, std::nullopt, ProofSource::OtherOperation);
        return result;
    }

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
            Number quotient = trunc(mul(magnitude, split.reciprocal));
            return either<Number>(lessThan(quotient, split.quotientLimit), [&] {
                Number difference = sub(magnitude, mul(quotient, split.high));
                if (split.low)
                    difference = sub(difference, mul(quotient, split.low));
                Number remainder = either<Number>(greaterThanOrEqual(difference, 0), [&] {
                    return difference;
                }, [&] {
                    return add(difference, split.magnitude);
                });
                ConversionEmitter& conversions = emitter<ConversionEmitter>();
                Integer signOfDividend = bitAnd(conversions.bitsOf(dividend), std::numeric_limits<int64_t>::min());
                return conversions.fromBits(bitOr(conversions.bitsOf(remainder), signOfDividend), Representation::Double).as<Number>();
            }, [&] {
                return callLeaf<Number>(RuntimeOperation::Fmod, { dividend, divisor });
            });
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
        if (value.opcode() == Opcode::Mod) {
            const Type& divisor = typeOf(value->rightOperand());
            if (divisor.isInteger() && divisor.integers.isSingleValue()) {
                build().statistics().count(StatisticID::RemaindersByProvenConstantDivisor);
                if (checksProofs())
                    emitter<ThrowEmitter>().requireOrCrash(value, equal(in<T>(value->rightOperand()), constant<T>(divisor.integers.minimum)), std::nullopt, ProofSource::OtherOperation);
                return { in<T>(value->leftOperand()), constant<T>(divisor.integers.minimum) };
            }
        }
        return { in<T>(value->leftOperand()), in<T>(value->rightOperand()) };
    }

    static bool productIsRoundedOnceFromIntegers(Range left, Range right)
    {
        if (left.isEmpty() || right.isEmpty())
            return false;
        if ((left.minimum < 0 && right.contains(0)) || (right.minimum < 0 && left.contains(0)))
            return false;
        for (int64_t multiplicand : { left.minimum, left.maximum }) {
            for (int64_t multiplier : { right.minimum, right.maximum }) {
                if ((CheckedInt64(multiplicand) * multiplier).hasOverflowed())
                    return false;
            }
        }
        return true;
    }

    Integer widened(Word32 value) { return signExtended(value); }
    Integer widened(Word32 value, Range range) { return emitter<ConversionEmitter>().widen(value, range); }
    Integer widened(Integer value, Range) { return value; }
    Integer widened(Integer value) { return value; }

    template<typename T>
    T remainderOfNonNegative(T dividend, T divisor, int64_t greatestDividend = std::numeric_limits<int64_t>::max())
    {
        if (std::optional<int64_t> magnitude = magnitudeOfConstantDivisor(divisor.value())) {
            if (hasOneBitSet(static_cast<uint64_t>(*magnitude)))
                return bitAnd(dividend, constant<T>(*magnitude - 1));
            if (greatestDividend >= 0 && greatestDividend - *magnitude < *magnitude) {
                build().statistics().count(StatisticID::RemaindersByConditionalSubtraction);
                T lessened = sub(dividend, constant<T>(*magnitude));
                return select(below(dividend, constant<T>(*magnitude)), dividend, lessened);
            }
            return remainderByConstant(dividend, *magnitude);
        }
        return binary<T>(&Output::mod, dividend, divisor);
    }

    template<typename T>
    T signedRemainder(T dividend, T divisor)
    {
        std::optional<int64_t> magnitude = magnitudeOfConstantDivisor(divisor.value());
        if (magnitude && *magnitude > 1) {
            if constexpr (std::is_same_v<T, Word32>)
                return signedRemainderByConstant(dividend, static_cast<int32_t>(*magnitude));
            else
                return signedRemainderByConstant(dividend, *magnitude);
        }
        return binary<T>(&Output::mod, dividend, divisor);
    }

    Word32 signedRemainderByConstant(Word32 dividend, int32_t divisor)
    {
        if (hasOneBitSet(static_cast<uint32_t>(divisor))) {
            unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisor));
            Word32 raised = add(dividend, Word32(m_out.lShr(m_out.aShr(dividend.value(), 31), 32 - shift)));
            return sub(dividend, bitAnd(raised, word32(-divisor)));
        }
        B3::DivisionMagic<int32_t> magic = B3::computeSignedDivisionMagic(divisor);
        Integer product = mul(signExtended(dividend), integer(static_cast<uint32_t>(magic.magicMultiplier)));
        Integer roundedDown(m_out.aShr(product.value(), 32 + magic.shift));
        Integer quotient = add(roundedDown, Integer(m_out.lShr(product.value(), 63)));
        return sub(dividend, mul(low32(quotient), word32(divisor)));
    }

    Integer signedRemainderByConstant(Integer dividend, int64_t divisor)
    {
        if (hasOneBitSet(static_cast<uint64_t>(divisor))) {
            unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisor));
            Integer raised = add(dividend, Integer(m_out.lShr(m_out.aShr(dividend.value(), 63), 64 - shift)));
            return sub(dividend, bitAnd(raised, integer(-divisor)));
        }
        Integer uncorrected = uncorrectedQuotient(dividend, divisor);
        Integer quotient = add(uncorrected, Integer(m_out.lShr(uncorrected.value(), 63)));
        return sub(dividend, mul(quotient, integer(divisor)));
    }

    Word32 remainderByConstant(Word32 dividend, int64_t divisor)
    {
        B3::DivisionMagic<int32_t> magic = B3::computeSignedDivisionMagic(static_cast<int32_t>(divisor));
        Integer product = mul(zeroExtended(dividend), integer(static_cast<uint32_t>(magic.magicMultiplier)));
        Word32 quotient = low32(Integer(m_out.lShr(product.value(), 32 + magic.shift)));
        return sub(dividend, mul(quotient, word32(static_cast<int32_t>(divisor))));
    }

    Integer remainderByConstant(Integer dividend, int64_t divisor)
    {
        Integer quotient = uncorrectedQuotient(dividend, divisor);
        return sub(dividend, mul(quotient, integer(divisor)));
    }

    Integer uncorrectedQuotient(Integer dividend, int64_t divisor)
    {
        B3::DivisionMagic<int64_t> magic = B3::computeSignedDivisionMagic(divisor);
        Integer quotient(m_out.mulHigh(dividend.value(), m_out.constInt64(magic.magicMultiplier)));
        if (magic.magicMultiplier < 0)
            quotient = add(quotient, dividend);
        if (magic.shift)
            quotient = Integer(m_out.aShr(quotient.value(), m_out.constInt32(magic.shift)));
        return quotient;
    }

    static double remainderByZero(double dividend) { return AOT::withoutPayload(resultAtRunTime(Math::fmodDouble, dividend, 0)); }

    Word32 nonNegativeInt32OrElse(Number number, LBasicBlock otherwise)
    {
        if (number.value()->opcode() == Machine::IToD && number.value()->child(0)->type() == Machine::Int32) {
            Word32 whole(number.value()->child(0));
            branchUnless(greaterThanOrEqual(whole, word32(0)), otherwise);
            return whole;
        }
#if CPU(ARM64)
        if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
            Machine::PureFunction function = pureFunction(PureOperation::NonNegativeInt32, 0, true);
            LValue operand = number.value();
            LValue conversion = m_out.findPurePatch(function, Machine::Int32, singleElementSpan(operand));
            if (!conversion) {
                Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Int32);
                patchpoint->appendSomeRegister(operand);
                patchpoint->effects = Machine::Effects::none();
                patchpoint->pureFunction = function;
                patchpoint->setRunsWithoutFrame(true);
                patchpoint->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
                    jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
                    jit.m_assembler.csinv<32>(params[0].gpr(), params[0].gpr(), ARM64Registers::zr, ARM64Assembler::ConditionEQ);
                });
                m_out.recordPurePatch(patchpoint);
                conversion = patchpoint;
            }
            Word32 converted(conversion);
            branchUnless(greaterThanOrEqual(converted, word32(0)), otherwise);
            return converted;
        }
#endif
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Word32 narrow = conversions.truncateToWord(number);
        Integer bitsOfNarrow = conversions.bitsOf(toNumber(narrow));
        branchUnless(equal(bitsOfNarrow, conversions.bitsOf(number)), otherwise);
        branchUnless(greaterThanOrEqual(narrow, word32(0)), otherwise);
        return narrow;
    }

    std::optional<Integer> provenNonNegativeInteger(const ValueContext& value, ValueID operand, Number number)
    {
        const Type& type = typeOf(operand);
        if (!type.isInteger() || type.integers.minimum < 0)
            return std::nullopt;
        Integer whole;
        if (LValue converted = number.value()->opcode() == Machine::IToD ? number.value()->child(0) : nullptr; converted && converted->type() == Machine::Int32)
            whole = zeroExtended(Word32(converted));
        else
            whole = emitter<ConversionEmitter>().toInteger(number, type.integers, value.block() && generator().usesLoopForms(value.block()));
        if (checksProofs()) {
            Condition isInRange = logicalAnd(greaterThanOrEqual(whole, integer(0)), lessThanOrEqual(whole, integer(type.integers.maximum)));
            emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(equal(toNumber(whole), number), isInRange), std::nullopt, ProofSource::OtherOperation);
        }
        return whole;
    }

    Integer positiveIntegerOrElse(Number number, LBasicBlock otherwise, bool hasNoFraction)
    {
        if (number.value()->opcode() == Machine::IToD && number.value()->child(0)->type() == Machine::Int32) {
            Word32 whole(number.value()->child(0));
            branchUnless(greaterThan(whole, word32(0)), otherwise);
            return zeroExtended(whole);
        }
        if (!isX86_64()) {
            Integer whole = nonNegativeIntegerOrElse(number, otherwise);
            branchUnless(isNotZero(whole), otherwise);
            return whole;
        }
        Integer whole = emitter<ConversionEmitter>().truncateUnchecked(number);
        branchUnless(greaterThan(whole, integer(0)), otherwise);
        bool isConvertedInteger = number.value()->opcode() == Machine::IToD && number.value()->child(0) == whole.value();
        if (!hasNoFraction && !isConvertedInteger)
            branchUnless(equalOrUnordered(toNumber(whole), number), otherwise);
        return whole;
    }

    Integer nonNegativeIntegerOrElse(Number number, LBasicBlock otherwise)
    {
        if (number.value()->opcode() == Machine::IToD && number.value()->child(0)->type() == Machine::Int32)
            return zeroExtended(nonNegativeInt32OrElse(number, otherwise));
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Integer whole = conversions.truncateUnchecked(number);
        Integer bitsOfWhole = conversions.bitsOf(toNumber(whole));
        branchUnless(equal(bitsOfWhole, conversions.bitsOf(number)), otherwise);
        branchUnless(isARM64() ? below(whole, integer(1ll << 62)) : greaterThanOrEqual(whole, integer(0)), otherwise);
        return whole;
    }
};

class PowerEmitter final : public InlineMathEmitter {
public:
    explicit PowerEmitter(CodeGenerator& generator)
        : InlineMathEmitter(generator)
    {
    }

    Number power(Number base, Number exponent, bool usesLoopForms)
    {
        static constexpr OpcodeSet powers { Opcode::Pow };
        if (std::optional<double> constantExponent = constantNumberOf(exponent.value())) {
            if (std::optional<Number> result = powerWithConstantExponent(base, *constantExponent)) {
                m_out.noteFormOf(powers, Form::Open, StateOfKnowledge::NumbersAndConstantExponent);
                return *result;
            }
        }
        if (std::optional<double> constantBase = constantNumberOf(base.value())) {
            if (std::optional<Number> result = powerWithConstantBase(*constantBase, exponent)) {
                m_out.noteFormOf(powers, Form::Open, StateOfKnowledge::NumbersAndConstantBase);
                return *result;
            }
        }
        if (usesLoopForms || exponent.value()->opcode() == Machine::IToD) {
            m_out.noteFormOf(powers, Form::Open, StateOfKnowledge::NumbersAndWholeExponent);
            return powerWithWholeExponent(base, exponent);
        }
        m_out.noteFormOf(powers, Form::Runtime, StateOfKnowledge::Numbers);
        return callLeaf<Number>(RuntimeOperation::Pow, { base, exponent });
    }

private:
    Number powerWithWholeExponent(Number base, Number exponent)
    {
        LBasicBlock test = newBlock();
        LBasicBlock step = newBlock();
        LBasicBlock finished = newBlock();
        LBasicBlock genericPath = newBlock();
        Join<Number> join(m_out);
        Integer whole = wholeExponentOrElse(exponent, genericPath);
        branchUnless(belowOrEqual(whole, static_cast<int64_t>(maxExponentForIntegerMathPow)), genericPath);
        ValueFromBlock firstBits = m_out.anchor(whole.value());
        ValueFromBlock firstResult = m_out.anchor(number(1).value());
        ValueFromBlock firstSquare = m_out.anchor(base.value());
        m_out.jump(test);

        m_out.appendTo(test);
        Integer bits(m_out.phi(Machine::Int64, firstBits));
        Number result(m_out.phi(Machine::Double, firstResult));
        Number square(m_out.phi(Machine::Double, firstSquare));
        branch(isNotZero(bits), { normalEdge(step), normalEdge(finished) });

        m_out.appendTo(step);
        Condition isOdd = hasBits(bits, 1);
        Number nextResult = select(isOdd, mul(result, square), result);
        Number nextSquare = mul(square, square);
        Integer nextBits(m_out.lShr(bits.value(), 1));
        m_out.addIncomingToPhi(result.value(), m_out.anchor(nextResult.value()));
        m_out.addIncomingToPhi(square.value(), m_out.anchor(nextSquare.value()));
        m_out.addIncomingToPhi(bits.value(), m_out.anchor(nextBits.value()));
        m_out.jump(test);

        m_out.appendTo(finished);
        join.add(result);

        m_out.appendTo(genericPath);
        join.add(callLeaf<Number>(RuntimeOperation::Pow, { base, exponent }));
        return join.finish();
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
        if (exponent.value()->opcode() == Machine::IToD) {
            LValue whole = exponent.value()->child(0);
            return whole->type() == Machine::Int32 ? signExtended(Word32(whole)) : Integer(whole);
        }
        Word32 narrow = low32(emitter<ConversionEmitter>().toInteger(exponent));
        branchUnless(equal(toNumber(narrow), exponent), otherwise);
        return signExtended(narrow);
    }

    Number powerOfTwo(Integer exponent)
    {
        constexpr int64_t exponentBias = 1023;
        constexpr int32_t numberOfFractionBits = 52;
        return Number(m_out.bitCast(m_out.shl(add(exponent, exponentBias).value(), numberOfFractionBits), Machine::Double));
    }

    Number powerOfTen(Integer exponent)
    {
        Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(numbers.powersOfTen));
        return Number(m_out.loadDouble(m_out.baseIndex(m_heaps.AOTInstance_fixedFields, table.value(), exponent.value(), ScaleEight)));
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

Number OperatorEmitter::asNumber(Operand operand, const Type& type, PayloadOfNaN payload)
{
    if (operand.is<Word32>())
        return toNumber(operand.as<Word32>());
    Operand unboxed = operand;
    if (operand.is<Boxed>())
        unboxed = emitter<ConversionEmitter>().unbox(operand.as<Boxed>(), type, type.isNumber() ? Representation::Double : Representation::OptionalDouble);
    RELEASE_ASSERT(unboxed.is<Number>() || unboxed.is<OptionalNumber>());
    if (unboxed.is<Number>())
        return unboxed.as<Number>();

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (std::optional<Number> whole = conversions.convertOptionalInteger(unboxed.value))
        return *whole;
    if (std::optional<Number> whole = conversions.convertIntegerOrNull(unboxed.value))
        return *whole;
    Number number(unboxed.value);
    if (payload == PayloadOfNaN::Goes && type.primitives & ~(Primitive::Number | Primitive::Null))
        number = conversions.withoutPayload(number);
    if (!(type.primitives & Primitive::Null))
        return number;
    uint64_t nullMarker = ConversionEmitter::markerBits(Primitive::Null, Representation::OptionalDouble);
    return select(conversions.isMarker(unboxed, nullMarker), 0, number);
}

Number OperatorEmitter::asNumericOperand(ValueID id, const OperationContext* user)
{
    if (const Type& type = typeOf(id); type.isInteger() && type.integers.isSingleValue()) {
        Number taken = number(static_cast<double>(type.integers.minimum));
        if (!emittersCheckProofs())
            return taken;
        if (user) {
            emitter<ThrowEmitter>().requireOrCrash(*user, equal(numericOperandAsItComes(id), taken), std::nullopt, ProofSource::OtherOperation);
            return taken;
        }
    }
    return numericOperandAsItComes(id);
}

Number OperatorEmitter::numericOperandAsItComes(ValueID id)
{
    Representation representation = representationOf(id);
    if (constantsInFormOfUses.contains(opcodeOf(id))) {
        const Type& type = typeOf(id);
        representation = type.isNumber() ? Representation::Double : type.isBoolean() ? Representation::Boolean : Representation::OptionalDouble;
    }
    switch (representation) {
    case Representation::Double:
        return in<Number>(id);
    case Representation::OptionalDouble:
        return asNumber(in<OptionalNumber>(id), typeOf(id));
    case Representation::Boolean:
        return toNumber(asWord32(in<Condition>(id)));
    case Representation::Int32:
        return toNumber(in<Word32>(id));
    case Representation::Int64:
        return toNumber(in<Integer>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::asNumericOperandWithAnyNaN(ValueID id)
{
    return generator().holdsAnyNaN(id) ? generator().getWithAnyNaN(id).as<Number>() : asNumericOperand(id);
}

Boxed OperatorEmitter::sumOfBoxedNumbers(ArithmeticOperator op, Operand left, const Type& leftType, Operand right, const Type& rightType, std::span<const CastEmitter::DeferredNumberCheck> deferredChecks)
{
    RELEASE_ASSERT(op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub);
    RELEASE_ASSERT(left.is<Boxed>() || right.is<Boxed>());
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto int32Of = [&](Operand operand) {
        return signExtended(operand.is<Boxed>() ? int32InBox(operand.as<Boxed>()) : operand.as<Word32>());
    };
    Boxed tested = left.is<Boxed>() ? left.as<Boxed>() : right.as<Boxed>();
    if (left.is<Boxed>() && right.is<Boxed>())
        tested = Boxed(m_out.bitAnd(left.value, right.value));

    LBasicBlock int32Case = newBlock();
    LBasicBlock numberCase = newBlock();
    Join<Boxed> join(m_out);
    Condition areInt32 = isInt32(tested);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(areInt32.value(), normalEdge(int32Case), unlikelyEdge(numberCase));

    m_out.appendToArm(int32Case, fork);
    Integer x = int32Of(left);
    Integer y = int32Of(right);
    Integer exact = op == ArithmeticOperator::Add ? add(x, y) : sub(x, y);
    join.add(conversions.boxInteger(exact, { 2 * static_cast<int64_t>(INT32_MIN), 2 * static_cast<int64_t>(INT32_MAX) + 1 }));

    m_out.appendToArm(numberCase, fork);
    for (const CastEmitter::DeferredNumberCheck& check : deferredChecks) {
        LBasicBlock holdsNumber = newBlock();
        m_out.branch(isNumber(check.value).value(), normalEdge(holdsNumber), check.failing);
        m_out.appendTo(holdsNumber);
    }
    bool oneAloneWasAsked = left.is<Boxed>() != right.is<Boxed>();
    auto numberIn = [&](Operand operand, const Type& type) {
        if (!oneAloneWasAsked || !operand.is<Boxed>())
            return asNumber(operand, type);
        if (type.isNumber())
            return unboxDouble(operand.as<Boxed>());
        Type rest = type;
        rest.primitives = (type.primitives & ~Primitive::Integer) | Primitive::Double;
        return asNumber(operand, rest);
    };
    Number leftNumber = numberIn(left, leftType);
    join.add(conversions.boxNumberByStub(arithmeticOnNumbers(op, leftNumber, numberIn(right, rightType))));
    return join.finishBehind(fork);
}

Boxed OperatorEmitter::boxedSum(ValueID sum)
{
    const Operation& operation = operationAt(sum);
    struct Term {
        Operand operand;
        ValueID typed;
    };
    auto termOf = [&](ValueID id) -> Term {
        const Operation& term = operationAt(id);
        if (term.opcode == Opcode::Integer)
            return { word32(static_cast<int32_t>(term.integer())), id };
        ValueID converted = term.valueOperand();
        if (term.opcode == Opcode::IntegerToNumber)
            return { in<Word32>(converted), converted };
        return { in<Boxed>(converted), converted };
    };
    bool adds = operation.opcode == Opcode::Add || operation.opcode == Opcode::Increment;
    ArithmeticOperator op = adds ? ArithmeticOperator::Add : ArithmeticOperator::Sub;
    if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement) {
        Term stepped = termOf(operation.valueOperand());
        return sumOfBoxedNumbers(op, stepped.operand, typeOf(stepped.typed), word32(1), typeOf(stepped.typed));
    }
    Term left = termOf(operation.leftOperand());
    Term right = termOf(operation.rightOperand());
    return sumOfBoxedNumbers(op, left.operand, typeOf(left.typed), right.operand, typeOf(right.typed));
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
    case Representation::RawWord:
        break;
    }
    return failWithZero(value);
}

Operand OperatorEmitter::arithmeticOnCheckedIntegers(const ValueContext& value, ArithmeticOperator op)
{
    IntegerArithmeticEmitter integers(generator());
    bool is32Bit = false;
    switch (integers.widthOfOperands(value)) {
    case Representation::Int32:
        is32Bit = true;
        break;
    case Representation::Int64:
        break;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        return failWithZero(value);
    }
    if (op != ArithmeticOperator::Mod)
        return is32Bit ? integers.resultAsNumber<Word32>(value, op) : integers.resultAsNumber<Integer>(value, op);

    return is32Bit ? integers.remainderOfIntegers<Word32>(value) : integers.remainderOfIntegers<Integer>(value);
}

std::optional<OperatorEmitter::RemainderTest> OperatorEmitter::findRemainderTest(ValueID test)
{
    static constexpr OpcodeSet tests { Opcode::StrictEq, Opcode::Eq, Opcode::ToBoolean };
    const Operation& operation = operationAt(test);
    BlockID block = m_code.blockOf(test);
    if (!tests.contains(operation.opcode) || operation.form() != OperatorForm::Numbers || !generator().usesLoopForms(block) || generator().keepsValuesInHeapFrame())
        return std::nullopt;

    auto isRemainder = [&](ValueID id) {
        const Operation& remainder = operationAt(id);
        return remainder.opcode == Opcode::Mod && remainder.form() == OperatorForm::Numbers && remainder.representation == Representation::Double && m_code.useCountOf(id) == 1 && m_code.blockOf(id) == block;
    };
    auto integerAt = [&](ValueID id) -> std::optional<int64_t> {
        const Operation& constant = operationAt(definitionOf(id));
        return constant.opcode == Opcode::Integer ? std::optional(constant.integer()) : std::nullopt;
    };
    RemainderTest found;
    if (operation.opcode == Opcode::ToBoolean)
        found.remainder = isRemainder(operation.valueOperand()) ? operation.valueOperand() : ValueID();
    else {
        bool isOnRight = isRemainder(operation.rightOperand());
        if (!isOnRight && !isRemainder(operation.leftOperand()))
            return std::nullopt;
        found.remainder = isOnRight ? operation.rightOperand() : operation.leftOperand();
        found.comparedWith = integerAt(isOnRight ? operation.leftOperand() : operation.rightOperand());
        if (!found.comparedWith)
            return std::nullopt;
    }
    std::optional<int64_t> divisor = found.remainder ? integerAt(operationAt(found.remainder).rightOperand()) : std::nullopt;
    if (!divisor || *divisor == std::numeric_limits<int64_t>::min())
        return std::nullopt;
    found.divisor = std::abs(*divisor);
    if (found.divisor < 2 || found.divisor > std::numeric_limits<int32_t>::max() || (found.comparedWith && (*found.comparedWith <= -found.divisor || *found.comparedWith >= found.divisor)))
        return std::nullopt;
    return found;
}

void OperatorEmitter::generateMod(const ValueContext& value)
{
    ValueID user = m_code.useCountOf(value.id()) == 1 ? m_code.onlyUserOf(value.id()) : ValueID();
    std::optional<RemainderTest> test = user ? findRemainderTest(user) : std::nullopt;
    if (test && test->remainder == value.id()) {
        define(value, number(0));
        return;
    }
    defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Mod));
}

bool OperatorEmitter::tryGenerateRemainderTest(const ValueContext& value)
{
    std::optional<RemainderTest> test = findRemainderTest(value.id());
    if (!test)
        return false;
    static constexpr OpcodeSet remainders { Opcode::Mod };
    m_out.noteFormOf(remainders, Form::Open, StateOfKnowledge::NumbersAndWholeDivisor);
    m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
    ValueID dividend = operationAt(test->remainder).leftOperand();
    bool mayBeMarker = representationOf(dividend) == Representation::OptionalDouble && !constantsInFormOfUses.contains(opcodeOf(dividend));
    Number number = mayBeMarker ? asNumber(in<OptionalNumber>(dividend), typeOf(dividend), PayloadOfNaN::Stays) : asNumericOperandWithAnyNaN(dividend);
    std::optional<int32_t> comparedWith = test->comparedWith ? std::optional(static_cast<int32_t>(*test->comparedWith)) : std::nullopt;
    defineTestByBranching(value, [&](BranchTargets targets) {
        IntegerArithmeticEmitter(generator()).branchOnRemainder(number, static_cast<int32_t>(test->divisor), comparedWith, targets);
    });
    return true;
}

Number OperatorEmitter::remainderByWholeDivisor(const ValueContext& value, Number dividend, Number divisor, Integer wholeDivisor)
{
    return IntegerArithmeticEmitter(generator()).remainderByWholeDivisor(value, dividend, divisor, wholeDivisor);
}

Number OperatorEmitter::arithmeticOnNumbers(ArithmeticOperator op, Number left, Number right, const ValueContext* value)
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
        return IntegerArithmeticEmitter(generator()).remainder(value, left, right);
    case ArithmeticOperator::Pow:
        return pow(left, right, value && value->block() && generator().usesLoopForms(value->block()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::pow(Number base, Number exponent, bool usesLoopForms)
{
    return PowerEmitter(generator()).power(base, exponent, usesLoopForms);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
