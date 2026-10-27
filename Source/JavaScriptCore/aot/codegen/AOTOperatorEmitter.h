/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubsArithmetic.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

enum class ArithmeticOperator : uint8_t { Add, Sub, Mul, Div, Mod, Pow };
enum class BitwiseOperator : uint8_t { And, Or, Xor, LeftShift, RightShift, UnsignedRightShift };
enum class UnaryOperator : uint8_t { Negate, Increment, Decrement, BitNot };
enum class Relation : uint8_t { Less, LessOrEqual, Greater, GreaterOrEqual };

struct WholeRemainder {
    Number number;
    Operand whole;
    Operand zeroWhereWholeIsZero;
    Condition isNumber;
    Operand dividend;
    uint64_t powerOfTwoDivisor { 0 };
};

class OperatorEmitter final : public OperationEmitter {
public:
    explicit OperatorEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateAdd(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Add)); }
    void generateSub(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Sub)); }
    void generateMul(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Mul)); }
    void generateDiv(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Div)); }
    void generateMod(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Mod)); }
    void generatePow(const ValueContext& value) { define(value, arithmeticResult(value, ArithmeticOperator::Pow)); }
    void generateBitAnd(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::And)); }
    void generateBitOr(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::Or)); }
    void generateBitXor(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::Xor)); }
    void generateLShift(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::LeftShift)); }
    void generateRShift(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::RightShift)); }
    void generateURShift(const ValueContext& value) { define(value, bitwiseResult(value, BitwiseOperator::UnsignedRightShift)); }
    void generateNegate(const ValueContext& value) { define(value, unaryResult(value, UnaryOperator::Negate)); }
    void generateIncrement(const ValueContext& value) { define(value, unaryResult(value, UnaryOperator::Increment)); }
    void generateDecrement(const ValueContext& value) { define(value, unaryResult(value, UnaryOperator::Decrement)); }
    void generateBitNot(const ValueContext& value) { define(value, unaryResult(value, UnaryOperator::BitNot)); }
    void generateLess(const ValueContext& value) { generateRelation(value, Relation::Less); }
    void generateLessEq(const ValueContext& value) { generateRelation(value, Relation::LessOrEqual); }
    void generateGreater(const ValueContext& value) { generateRelation(value, Relation::Greater); }
    void generateGreaterEq(const ValueContext& value) { generateRelation(value, Relation::GreaterOrEqual); }
    void generateEq(const ValueContext& value) { generateEquality(value, false); }
    void generateStrictEq(const ValueContext& value) { generateEquality(value, true); }
    void generateToNumeric(const ValueContext&);
    void generateToNumber(const ValueContext&);
    void generateInstanceOf(const ValueContext&);
    void generateHasInPrototypeChain(const ValueContext&);
    bool fetchesPrototypeItself(ValueID builtin);
    void generateToBoolean(const ValueContext&);
    void generateLogicalNot(const ValueContext&);
    void generateTypeOf(const ValueContext&);
    void generateToString(const ValueContext&);
    void generateStrcat(const ValueContext& value) { define(value, concatenationOfParts(value)); }
    void generateToPropertyKey(const ValueContext&);
    void generateToObject(const ValueContext&);
    void generateToThis(const ValueContext&);

    Number pow(Number, Number);
    CellPointer toString(const OperationContext&, Operand, const Type&);
    std::optional<Integer> wholeRemainderAsIndex(ValueID position, LBasicBlock isNotAnIndex);
    bool leavesRemainderToTest(ValueID remainder) { return !!findRemainderLeftToTest(remainder); }
    Operand failWithZero(const ValueContext&);

private:
    Operand arithmeticResult(const ValueContext&, ArithmeticOperator);
    Operand bitwiseResult(const ValueContext&, BitwiseOperator);
    Operand unaryResult(const ValueContext&, UnaryOperator);
    void generateRelation(const ValueContext&, Relation);
    void generateEquality(const ValueContext&, bool isStrict);
    template<typename Functor> Operand convertedToNumber(const ValueContext&, const Functor& convertValue);

    Number asNumericOperand(ValueID);
    Number asNumber(Operand, const Type&);
    Number asNumberLikeOperand(ValueID);
    template<typename Functor> Operand withIntegerOperand(const ValueContext&, const Functor&);
    template<typename T, typename Functor> T passingOrConverted(Boxed, const TypeTestPlan&, const Functor& convert);

    LValue callBinaryStub(const ValueContext&, BinaryOperator, Boxed left, Boxed right);
    Operand arithmeticOnIntegers(const ValueContext&, ArithmeticOperator);
    Operand arithmeticOnCheckedIntegers(const ValueContext&, ArithmeticOperator);
    Number arithmeticOnNumbers(ArithmeticOperator, Number, Number);
    Boxed sumOfBoxedNumbers(ArithmeticOperator, Operand left, const Type& leftType, Operand right, const Type& rightType);
    Number arithmeticOnNumberAndValue(const ValueContext&, ArithmeticOperator);
    Boxed arithmeticOnBoxedOperands(const ValueContext&, ArithmeticOperator, Boxed left, Boxed right);
    Operand addStrings(const ValueContext&);
    CellPointer concatenationOfParts(const ValueContext&);
    Operand unaryOnIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnCheckedIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnNumbers(UnaryOperator, Number);
    Word32 bitwiseOnWords(BitwiseOperator, Word32, Word32);
    template<typename Functor> Word32 int32ConvertedOnce(const ValueContext& user, ValueID number, bool needsLowestBitsAlone, const Functor& convert);

    // Without a relation the operands are compared for equality.
    template<typename T> Condition compare(std::optional<Relation>, T, T);
    void generateComparisonOfNumbers(const ValueContext&, std::optional<Relation>);
    bool tryGenerateComparisonOfWholeRemainder(const ValueContext&, std::optional<Relation>);
    Condition relationOnStringCells(const ValueContext&, Relation, CellPointer left, CellPointer right);
    Condition relationOnNullableStrings(const ValueContext&, Relation);
    Condition relationOnValues(const ValueContext&, Relation);

    struct Int32OfNumber {
        BlockID block;
        Word32 word;
        bool hasLowestBitsAlone;
    };
    UncheckedKeyHashMap<uint32_t, Vector<Int32OfNumber, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_int32OfNumbers;

    struct RemainderLeftToTest {
        ValueID dividend;
        int32_t divisor;
        std::optional<int64_t> comparedWith;
    };
    std::optional<RemainderLeftToTest> findRemainderLeftToTest(ValueID remainder);
    bool tryGenerateTestOfRemainder(const ValueContext& test, ValueID remainder);
    const WholeRemainder* findWholeRemainder(ValueID number);
    Number asTermOfSum(ValueID term, ValueID otherTerm);
    UncheckedKeyHashMap<uint32_t, WholeRemainder, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_wholeRemainders;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
