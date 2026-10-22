/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

enum class ArithmeticOperator : uint8_t { Add, Sub, Mul, Div, Mod, Pow };
enum class BitwiseOperator : uint8_t { And, Or, Xor, LeftShift, RightShift, UnsignedRightShift };
enum class UnaryOperator : uint8_t { Negate, Increment, Decrement, BitNot };
enum class Relation : uint8_t { Less, LessOrEqual, Greater, GreaterOrEqual };

class OperatorEmitter final : public OperationEmitter {
public:
    explicit OperatorEmitter(CodeGenerator&);

    void generateAdd(const ValueContext&);
    void generateSub(const ValueContext&);
    void generateMul(const ValueContext&);
    void generateDiv(const ValueContext&);
    void generateMod(const ValueContext&);
    void generatePow(const ValueContext&);
    void generateBitAnd(const ValueContext&);
    void generateBitOr(const ValueContext&);
    void generateBitXor(const ValueContext&);
    void generateLShift(const ValueContext&);
    void generateRShift(const ValueContext&);
    void generateURShift(const ValueContext&);
    void generateNegate(const ValueContext&);
    void generateToNumeric(const ValueContext&);
    void generateIncrement(const ValueContext&);
    void generateDecrement(const ValueContext&);
    void generateBitNot(const ValueContext&);
    void generateToNumber(const ValueContext&);
    void generateLess(const ValueContext&);
    void generateLessEq(const ValueContext&);
    void generateGreater(const ValueContext&);
    void generateGreaterEq(const ValueContext&);
    void generateEq(const ValueContext&);
    void generateStrictEq(const ValueContext&);
    void generateInstanceOf(const ValueContext&);
    void generateHasInPrototypeChain(const ValueContext&);
    bool fetchesPrototypeItself(ValueID builtin);
    void generateToBoolean(const ValueContext&);
    void generateLogicalNot(const ValueContext&);
    void generateTypeOf(const ValueContext&);
    void generateToString(const ValueContext&);
    void generateStrcat(const ValueContext&);
    void generateToPropertyKey(const ValueContext&);
    void generateToObject(const ValueContext&);
    void generateToThis(const ValueContext&);

    Number fmod(Number, Number);
    Number pow(Number, Number);
    Condition compare(Relation, Number, Number);
    Condition compare(Relation, Integer, Integer);
    Condition compare(Relation, Word32, Word32);
    Condition isStrictlyEqual(const OperationContext&, Boxed, const Type&, Boxed, const Type&);
    CellPointer toString(const OperationContext&, Operand, const Type&);
    Boxed toPropertyKey(const OperationContext&, Boxed, const Type&);
    std::optional<Integer> wholeRemainderAsIndex(ValueID position, LBasicBlock isNotAnIndex);
    bool leavesRemainderToTest(ValueID remainder) { return !!findRemainderLeftToTest(remainder); }

private:
    bool testsForCellBeforeWalk(const Value& walk);
    void generateArithmetic(const ValueContext&, ArithmeticOperator);
    void generateBitwise(const ValueContext&, BitwiseOperator);
    void generateUnary(const ValueContext&, UnaryOperator);
    void generateRelation(const ValueContext&, Relation);
    void generateEquality(const ValueContext&, bool isStrict);
    template<typename Functor> void generateConversionToNumber(const ValueContext&, const Functor& convertValue);
    void failOnUnsupportedForm(const ValueContext&);

    Number asNumericOperand(ValueID);
    Number asNumber(Operand, const Type&);
    Number asNumberLikeOperand(ValueID);
    template<typename Functor> Operand withIntegerOperand(const ValueContext&, const Functor&);
    template<typename Functor> CellPointer objectOrConverted(ValueID, const Functor& convertPrimitive);

    Operand arithmeticOnIntegers(const ValueContext&, ArithmeticOperator);
    Operand arithmeticOnCheckedIntegers(const ValueContext&, ArithmeticOperator);
    Number arithmeticOnNumbers(ArithmeticOperator, Number, Number);
    Boxed sumOfBoxedNumbers(ArithmeticOperator, Operand left, const Type& leftType, Operand right, const Type& rightType);
    Number arithmeticOnNumberAndValue(const ValueContext&, ArithmeticOperator, RuntimeOperation);
    CellPointer arithmeticOnBigInts(const ValueContext&, ArithmeticOperator);
    Boxed arithmeticOnValues(const ValueContext&, ArithmeticOperator);
    Boxed arithmeticOnBoxedOperands(const ValueContext&, ArithmeticOperator, Boxed left, Boxed right);
    Operand addStrings(const ValueContext&);
    Operand unaryOnIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnCheckedIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnNumbers(UnaryOperator, Number);
    Word32 bitwiseOnWords(BitwiseOperator, Word32, Word32);
    template<typename Functor> Word32 int32ConvertedOnce(const ValueContext& user, ValueID number, const Functor& convert);
    Boxed bitwiseOnValues(const ValueContext&, BitwiseOperator);
    void generateRelationOnNumbers(const ValueContext&, Relation);
    bool tryGenerateComparisonOfWholeRemainder(const ValueContext&, std::optional<Relation>);
    void generateEqualityOfNumbers(const ValueContext&);
    Condition relationOnStrings(const ValueContext&, Relation);
    Condition relationOnStringCells(const ValueContext&, Relation, CellPointer left, CellPointer right);
    Condition relationOnNullableStrings(const ValueContext&, Relation);
    Condition relationOnValues(const ValueContext&, Relation);
    Condition equalityOnNumberAndValue(const ValueContext&, bool isStrict);
    Condition equalityOnStrings(const ValueContext&);
    Condition equalityOnStringAndValue(const ValueContext&, bool isStrict);
    Condition equalityOnValues(const ValueContext&, bool isStrict);
    static RuntimeOperation runtimeOperationOf(ArithmeticOperator);
    static RuntimeOperation runtimeOperationOf(BitwiseOperator);
    static RuntimeOperation runtimeOperationOf(UnaryOperator);
    static RuntimeOperation runtimeOperationOf(Relation);

    struct Int32OfNumber {
        BlockID block;
        Word32 word;
    };
    UncheckedKeyHashMap<uint32_t, Vector<Int32OfNumber, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_int32OfNumbers;

    struct WholeRemainder {
        Operand whole;
        Operand zeroWhereWholeIsZero;
        Condition isNumber;
        Operand dividend;
        uint64_t powerOfTwoDivisor { 0 };
    };
    template<typename T> Number remainderOfCheckedIntegers(const ValueContext&);
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
