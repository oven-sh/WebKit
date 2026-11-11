/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCastEmitter.h"
#include "AOTOperationEmitter.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsPropertyKeys.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class ArithmeticOperator : uint8_t { Add, Sub, Mul, Div, Mod, Pow };
enum class BitwiseOperator : uint8_t { And, Or, Xor, LeftShift, RightShift, UnsignedRightShift };
enum class UnaryOperator : uint8_t { Negate, Increment, Decrement, BitNot };
enum class Relation : uint8_t { Less, LessOrEqual, Greater, GreaterOrEqual };

class OperatorEmitter final : public OperationEmitter {
public:
    explicit OperatorEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Number remainderByWholeDivisor(const ValueContext&, Number dividend, Number divisor, Integer wholeDivisor);
    void generateAdd(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Add)); }
    void generateSub(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Sub)); }
    void generateMul(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Mul)); }
    void generateDiv(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Div)); }
    void generateMod(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Mod)); }
    void generatePow(const ValueContext& value) { defineHeldToItsType(value, arithmeticResult(value, ArithmeticOperator::Pow)); }
    void generateBitAnd(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::And)); }
    void generateBitOr(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::Or)); }
    void generateBitXor(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::Xor)); }
    void generateLShift(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::LeftShift)); }
    void generateRShift(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::RightShift)); }
    void generateURShift(const ValueContext& value) { defineHeldToItsType(value, bitwiseResult(value, BitwiseOperator::UnsignedRightShift)); }
    void generateNegate(const ValueContext& value) { defineHeldToItsType(value, unaryResult(value, UnaryOperator::Negate)); }
    void generateIncrement(const ValueContext& value) { defineHeldToItsType(value, unaryResult(value, UnaryOperator::Increment)); }
    void generateDecrement(const ValueContext& value) { defineHeldToItsType(value, unaryResult(value, UnaryOperator::Decrement)); }
    void generateBitNot(const ValueContext& value) { defineHeldToItsType(value, unaryResult(value, UnaryOperator::BitNot)); }
    void generateLess(const ValueContext& value) { generateRelation(value, Relation::Less); }
    void generateLessEq(const ValueContext& value) { generateRelation(value, Relation::LessOrEqual); }
    void generateGreater(const ValueContext& value) { generateRelation(value, Relation::Greater); }
    void generateGreaterEq(const ValueContext& value) { generateRelation(value, Relation::GreaterOrEqual); }
    void generateEq(const ValueContext& value) { generateEquality(value, false); }
    void generateStrictEq(const ValueContext& value) { generateEquality(value, true); }
    void generateIntegerBound(const ValueContext&);
    void generateNumberOrNaN(const ValueContext&);
    void generateToNumeric(const ValueContext&);
    void generateToNumber(const ValueContext&);
    bool isCutToInt32Alone(ValueID number);
    void generateInstanceOf(const ValueContext&);
    bool leaveOperandToInstanceOf(const ValueContext& read, InstanceOfOperand, NumberedImmediate);
    void generateHasInPrototypeChain(const ValueContext&);
    void generateTryFindInPrototypeChain(const ValueContext&);
    void generateHasDefaultHasInstance(const ValueContext&);
    void generateToBoolean(const ValueContext&);
    void generateLogicalNot(const ValueContext&);
    void generateTypeOf(const ValueContext&);
    void generateToString(const ValueContext&);
    void generateStrcat(const ValueContext& value) { define(value, concatenationOfParts(value)); }
    void generateToPropertyKey(const ValueContext&);
    void generateToObject(const ValueContext&);
    void generateToThis(const ValueContext&);

    Number pow(Number, Number);
    Boxed boxedSum(ValueID sum);
    CellPointer toString(const OperationContext&, Operand, const Type&);
    Operand failWithZero(const ValueContext&);

private:
    Operand arithmeticResult(const ValueContext&, ArithmeticOperator);
    Operand bitwiseResult(const ValueContext&, BitwiseOperator);
    Operand unaryResult(const ValueContext&, UnaryOperator);
    void defineHeldToItsType(const ValueContext&, Operand);
    void generateRelation(const ValueContext&, Relation);
    void generateEquality(const ValueContext&, bool isStrict);
    template<typename Functor> Operand convertedToNumber(const ValueContext&, const Functor& convertValue);

    Number asNumericOperand(ValueID, const OperationContext* user = nullptr);
    Number numericOperandAsItComes(ValueID);
    Number asNumericOperandWithAnyNaN(ValueID);
    enum class PayloadOfNaN : bool { Goes, Stays };
    Number asNumber(Operand, const Type&, PayloadOfNaN = PayloadOfNaN::Goes);
    Number asNumberLikeOperand(ValueID);
    template<typename Functor> Operand withIntegerOperand(const ValueContext&, const Functor&);
    template<typename T, typename Functor> T passingOrConverted(Boxed, const TypeTestPlan&, Form formOfConversion, const Functor& convert);

    LValue callBinaryStub(const ValueContext&, BinaryOperator, Boxed left, Boxed right);
    bool standsInRareBlock(const ValueContext& value) const { return m_code.at(value.block()).frequency == BlockFrequency::Rare; }
    Operand arithmeticOnIntegers(const ValueContext&, ArithmeticOperator);
    Operand arithmeticOnCheckedIntegers(const ValueContext&, ArithmeticOperator);
    Number arithmeticOnNumbers(ArithmeticOperator, Number, Number, const ValueContext* = nullptr);
    Boxed sumOfBoxedNumbers(ArithmeticOperator, Operand left, const Type& leftType, Operand right, const Type& rightType, std::span<const CastEmitter::DeferredNumberCheck> = { });
    Number arithmeticOnNumberAndValue(const ValueContext&, ArithmeticOperator);
    Boxed arithmeticOnBoxedOperands(const ValueContext&, ArithmeticOperator, Boxed left, Boxed right);
    bool sumsInt32sInPlace(const ValueContext&, ArithmeticOperator);
    Boxed sumOfAnyValues(const ValueContext&, ArithmeticOperator, Boxed left, Boxed right);
    Operand addStrings(const ValueContext&);
    CellPointer concatenationOfParts(const ValueContext&);
    Operand unaryOnIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnCheckedIntegers(const ValueContext&, UnaryOperator);
    Operand unaryOnNumbers(UnaryOperator, ValueID operand);
    Word32 bitwiseOnWords(BitwiseOperator, Word32, Word32);

    template<typename T> Condition compare(std::optional<Relation>, T, T);
    void generateComparisonOfNumbers(const ValueContext&, std::optional<Relation>);
    Condition relationOnStringCells(const ValueContext&, Relation, CellPointer left, CellPointer right);
    Condition relationOnBigInts(const ValueContext&, Relation, CellPointer left, CellPointer right);
    Condition relationOnNullableStrings(const ValueContext&, Relation);
    enum class ValueInRelation : uint8_t { MayBeInt32, IsNoInt32 };
    Condition relationOnValues(const ValueContext&, Relation, ValueInRelation = ValueInRelation::MayBeInt32);
    bool generateRelationOfIntegerAndValue(const ValueContext&, Relation);
    bool generateRelationOfBoxedNumberAndValue(const ValueContext&, Relation);
    bool generateRelationOfDoubleAndValue(const ValueContext&, Relation);
    bool generateRelationOfValuesThatAreLikelyInt32s(const ValueContext&, Relation);

    struct OperandLeftToInstanceOf {
        ValueID read;
        InstanceOfOperand operand;
        NumberedImmediate number;
    };
    std::optional<OperandLeftToInstanceOf> m_operandLeftToInstanceOf;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
