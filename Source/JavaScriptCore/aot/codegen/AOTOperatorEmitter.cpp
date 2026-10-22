/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTTypeTable.h"
#include "AOTTypeTestPlan.h"
#include "PureNaN.h"

namespace JSC { namespace AOT { namespace Typed {

static BinaryOperator binaryOperatorFor(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return BinaryOperator::Add;
    case ArithmeticOperator::Sub:
        return BinaryOperator::Sub;
    case ArithmeticOperator::Mul:
        return BinaryOperator::Mul;
    case ArithmeticOperator::Div:
        return BinaryOperator::Div;
    case ArithmeticOperator::Mod:
        return BinaryOperator::Mod;
    case ArithmeticOperator::Pow:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnaryOperationForm unaryOperationFormFor(UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return UnaryOperationForm::Negate;
    case UnaryOperator::Increment:
        return UnaryOperationForm::Increment;
    case UnaryOperator::Decrement:
        return UnaryOperationForm::Decrement;
    case UnaryOperator::BitNot:
        return UnaryOperationForm::BitNot;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<RuntimeOperation> runtimeOperationOnNumberAndValue(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return RuntimeOperation::AddNumberAndValueAsNumber;
    case ArithmeticOperator::Sub:
        return RuntimeOperation::SubNumberAndValueAsNumber;
    case ArithmeticOperator::Mul:
        return RuntimeOperation::MulNumberAndValueAsNumber;
    case ArithmeticOperator::Div:
        return RuntimeOperation::DivNumberAndValueAsNumber;
    case ArithmeticOperator::Mod:
    case ArithmeticOperator::Pow:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OperatorEmitter::OperatorEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

#define AOT_DEFINE_ARITHMETIC_CASE(name) \
    void OperatorEmitter::generate##name(const ValueContext& value) \
    { \
        generateArithmetic(value, ArithmeticOperator::name); \
    }
AOT_DEFINE_ARITHMETIC_CASE(Add)
AOT_DEFINE_ARITHMETIC_CASE(Sub)
AOT_DEFINE_ARITHMETIC_CASE(Mul)
AOT_DEFINE_ARITHMETIC_CASE(Div)
AOT_DEFINE_ARITHMETIC_CASE(Mod)
AOT_DEFINE_ARITHMETIC_CASE(Pow)
#undef AOT_DEFINE_ARITHMETIC_CASE

#define AOT_DEFINE_UNARY_CASE(name) \
    void OperatorEmitter::generate##name(const ValueContext& value) \
    { \
        generateUnary(value, UnaryOperator::name); \
    }
AOT_DEFINE_UNARY_CASE(Negate)
AOT_DEFINE_UNARY_CASE(Increment)
AOT_DEFINE_UNARY_CASE(Decrement)
AOT_DEFINE_UNARY_CASE(BitNot)
#undef AOT_DEFINE_UNARY_CASE

void OperatorEmitter::failOnUnsupportedForm(const ValueContext& value)
{
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
}

void OperatorEmitter::generateArithmetic(const ValueContext& value, ArithmeticOperator op)
{
    switch (value->form()) {
    case OperatorForm::Generic:
        define(value, arithmeticOnValues(value, op));
        return;
    case OperatorForm::Integers:
        define(value, arithmeticOnIntegers(value, op));
        return;
    case OperatorForm::CheckedIntegers:
        define(value, arithmeticOnCheckedIntegers(value, op));
        return;
    case OperatorForm::Numbers: {
        if (value.representation() == Representation::JSValue) {
            define(value, sumOfBoxedNumbers(op, get(value->leftOperand()), typeOf(value->leftOperand()), get(value->rightOperand()), typeOf(value->rightOperand())));
            return;
        }
        Number left = op == ArithmeticOperator::Add ? asTermOfSum(value->leftOperand(), value->rightOperand()) : asNumericOperand(value->leftOperand());
        Number right = op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub ? asTermOfSum(value->rightOperand(), value->leftOperand()) : asNumericOperand(value->rightOperand());
        define(value, arithmeticOnNumbers(op, left, right));
        return;
    }
    case OperatorForm::NumberAndValue:
        if (std::optional<RuntimeOperation> operation = runtimeOperationOnNumberAndValue(op)) {
            define(value, arithmeticOnNumberAndValue(value, op, *operation));
            return;
        }
        break;
    case OperatorForm::BigInts:
        define(value, arithmeticOnBigInts(value, op));
        return;
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
        if (op != ArithmeticOperator::Add)
            break;
        define(value, addStrings(value));
        return;
    case OperatorForm::Unselected:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    failOnUnsupportedForm(value);
}

Boxed OperatorEmitter::arithmeticOnBoxedOperands(const ValueContext& value, ArithmeticOperator op, Boxed left, Boxed right)
{
    if (op == ArithmeticOperator::Pow)
        return callOperation<Boxed>(value, runtimeOperationOf(op), { left, right });
    BinaryOperator binaryOperator = binaryOperatorFor(op);
    bool mayBothBeInt32 = typeOf(value->leftOperand()).primitives & Primitive::Integer && typeOf(value->rightOperand()).primitives & Primitive::Integer;
    bool isAfterInt32Check = !mayBothBeInt32 && hasFormAfterInt32Check(binaryOperator);
    return Boxed(callStub(value, binaryOperation(binaryOperator, isAfterInt32Check), { left.value(), right.value() }).first);
}

Boxed OperatorEmitter::arithmeticOnValues(const ValueContext& value, ArithmeticOperator op)
{
    Boxed left = in<Boxed>(value->leftOperand());
    Boxed right = in<Boxed>(value->rightOperand());
    return arithmeticOnBoxedOperands(value, op, left, right);
}

CellPointer OperatorEmitter::arithmeticOnBigInts(const ValueContext& value, ArithmeticOperator op)
{
    Boxed left = boxCell(in<CellPointer>(value->leftOperand()));
    Boxed right = boxCell(in<CellPointer>(value->rightOperand()));
    return unboxCell(arithmeticOnBoxedOperands(value, op, left, right));
}

Number OperatorEmitter::arithmeticOnNumberAndValue(const ValueContext& value, ArithmeticOperator op, RuntimeOperation operation)
{
    bool numberIsOnRight = representationOf(value->leftOperand()) != Representation::Double;
    ValueID numberID = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    Number numberOperand = in<Number>(numberID);
    Boxed otherOperand = in<Boxed>(otherID);
    Word32 typeAndNumberSide = word32(static_cast<int32_t>(NumberAndValueBits::encode(TypeTable::number.index(), numberIsOnRight)));
    if (typeOf(otherID).primitives & Primitive::Number)
        return Number(callStub(value, binaryWithDouble(binaryOperatorFor(op), numberIsOnRight), { numberOperand.value(), otherOperand.value(), typeAndNumberSide.value() }).first);
    return callOperation<Number>(value, operation, { otherOperand, typeAndNumberSide, numberOperand });
}

void OperatorEmitter::generateUnary(const ValueContext& value, UnaryOperator op)
{
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        define(value, Boxed(callStub(value, unaryOperation(unaryOperationFormFor(op)), { in<Boxed>(operand).value() }).first));
        return;
    case OperatorForm::Integers:
        define(value, unaryOnIntegers(value, op));
        return;
    case OperatorForm::CheckedIntegers:
        define(value, unaryOnCheckedIntegers(value, op));
        return;
    case OperatorForm::Numbers:
        if (value.representation() == Representation::JSValue) {
            RELEASE_ASSERT(op == UnaryOperator::Increment || op == UnaryOperator::Decrement);
            ArithmeticOperator sum = op == UnaryOperator::Increment ? ArithmeticOperator::Add : ArithmeticOperator::Sub;
            define(value, sumOfBoxedNumbers(sum, get(operand), typeOf(operand), word32(1), typeOf(operand)));
            return;
        }
        define(value, unaryOnNumbers(op, asNumericOperand(operand)));
        return;
    case OperatorForm::BigInts:
        define(value, unboxCell(callOperation<Boxed>(value, runtimeOperationOf(op), { boxCell(in<CellPointer>(operand)) })));
        return;
    case OperatorForm::Unselected:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    failOnUnsupportedForm(value);
}

template<typename Functor>
Operand OperatorEmitter::withIntegerOperand(const ValueContext& value, const Functor& functor)
{
    ValueID operand = value->valueOperand();
    switch (representationOf(operand)) {
    case Representation::Int32:
        return functor(in<Word32>(operand));
    case Representation::Int64:
        return functor(in<Integer>(operand));
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

Operand OperatorEmitter::unaryOnIntegers(const ValueContext& value, UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return withIntegerOperand(value, [&](auto operand) -> Operand {
            return neg(operand);
        });
    case UnaryOperator::Increment:
        return arithmeticOnIntegers(value, ArithmeticOperator::Add);
    case UnaryOperator::Decrement:
        return arithmeticOnIntegers(value, ArithmeticOperator::Sub);
    case UnaryOperator::BitNot:
        return bitXor(in<Word32>(value->valueOperand()), word32(-1));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::unaryOnCheckedIntegers(const ValueContext& value, UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return withIntegerOperand(value, [&](auto operand) -> Operand {
            return neg(toNumber(operand));
        });
    case UnaryOperator::Increment:
        return arithmeticOnCheckedIntegers(value, ArithmeticOperator::Add);
    case UnaryOperator::Decrement:
        return arithmeticOnCheckedIntegers(value, ArithmeticOperator::Sub);
    case UnaryOperator::BitNot:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Operand OperatorEmitter::unaryOnNumbers(UnaryOperator op, Number operand)
{
    switch (op) {
    case UnaryOperator::Negate:
        return neg(operand);
    case UnaryOperator::Increment:
        return add(operand, number(1));
    case UnaryOperator::Decrement:
        return sub(operand, number(1));
    case UnaryOperator::BitNot:
        return bitXor(emitter<ConversionEmitter>().toInt32(operand), word32(-1));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::asNumberLikeOperand(ValueID id)
{
    if (representationOf(id) == Representation::Boolean)
        return toNumber(asWord32(in<Condition>(id)));
    Boxed value = in<Boxed>(id);
    Condition isTrue = equal(value, Boxed(m_out.constInt64(JSValue::ValueTrue)));
    Number convertedPrimitive = select(equal(value, undefined()), number(PNaN), toNumber(asWord32(isTrue)));
    return select(isNumber(value), numberToDouble(value), convertedPrimitive);
}

template<typename Functor>
void OperatorEmitter::generateConversionToNumber(const ValueContext& value, const Functor& convertValue)
{
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        define(value, convertValue(in<Boxed>(operand)));
        return;
    case OperatorForm::Integers:
        define(value, get(operand));
        return;
    case OperatorForm::Numbers:
        if (representationOf(operand) == Representation::Double)
            define(value, get(operand));
        else
            define(value, emitter<ConversionEmitter>().purified(asNumericOperand(operand)));
        return;
    case OperatorForm::NullableNumber:
        define(value, asNumberLikeOperand(operand));
        return;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    failOnUnsupportedForm(value);
}

void OperatorEmitter::generateToNumber(const ValueContext& value)
{
    generateConversionToNumber(value, [&](Boxed operand) -> Operand {
        return Number(callStub(value, toNumberAsDouble(), { operand.value() }).first);
    });
}

void OperatorEmitter::generateToNumeric(const ValueContext& value)
{
    generateConversionToNumber(value, [&](Boxed operand) -> Operand {
        return Boxed(callStub(value, unaryOperation(UnaryOperationForm::ToNumeric), { operand.value() }).first);
    });
}

void OperatorEmitter::generateLogicalNot(const ValueContext& value)
{
    define(value, logicalNot(in<Condition>(value->valueOperand())));
}

void OperatorEmitter::generateTypeOf(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::TypeOf, { in<Boxed>(value->valueOperand()) }));
}

template<typename Functor>
CellPointer OperatorEmitter::objectOrConverted(ValueID operand, const Functor& convertPrimitive)
{
    Boxed value = in<Boxed>(operand);
    const TypeTestPlan& plan = typeTestPlanFor(m_function.at(operand).type, Representation::JSValue, TypeTable::object);
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return unboxCell(value);
    case TypeTestVerdict::NeverPasses:
        return convertPrimitive(value, CallOptions { });
    case TypeTestVerdict::NeedsTest:
        break;
    }

    LBasicBlock isObject = newBlock();
    LBasicBlock isPrimitive = newBlock();
    Join<CellPointer> join(m_out);
    branchOnType(value, plan, { normalEdge(isObject), rareEdge(isPrimitive) });

    m_out.appendTo(isObject);
    join.add(unboxCell(value));

    m_out.appendTo(isPrimitive);
    join.add(convertPrimitive(value, CallOptions { .isOnRarePath = true }));
    return join.finish();
}

void OperatorEmitter::generateToObject(const ValueContext& value)
{
    StringID message = value->message() ? value->message() : m_program.strings.emptyString();
    CellPointer object = objectOrConverted(value->valueOperand(), [&](Boxed primitive, const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::ToObject, { primitive, word32(message.index()) }, options);
    });
    // The conversion of undefined or null throws, and a value of type never has no representation of an object.
    if (!typeOf(value.id()).isNever())
        define(value, object);
}

void OperatorEmitter::generateToThis(const ValueContext& value)
{
    define(value, objectOrConverted(value->valueOperand(), [&](Boxed primitive, const CallOptions& options) {
        return unboxCell(callOperation<Boxed>(value, RuntimeOperation::ToThis, { primitive }, options));
    }));
}

RuntimeOperation OperatorEmitter::runtimeOperationOf(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return RuntimeOperation::ValueAdd;
    case ArithmeticOperator::Sub:
        return RuntimeOperation::ValueSub;
    case ArithmeticOperator::Mul:
        return RuntimeOperation::ValueMul;
    case ArithmeticOperator::Div:
        return RuntimeOperation::ValueDiv;
    case ArithmeticOperator::Mod:
        return RuntimeOperation::ValueMod;
    case ArithmeticOperator::Pow:
        return RuntimeOperation::ValuePow;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RuntimeOperation OperatorEmitter::runtimeOperationOf(BitwiseOperator op)
{
    switch (op) {
    case BitwiseOperator::And:
        return RuntimeOperation::ValueBitAnd;
    case BitwiseOperator::Or:
        return RuntimeOperation::ValueBitOr;
    case BitwiseOperator::Xor:
        return RuntimeOperation::ValueBitXor;
    case BitwiseOperator::LeftShift:
        return RuntimeOperation::ValueLShift;
    case BitwiseOperator::RightShift:
        return RuntimeOperation::ValueRShift;
    case BitwiseOperator::UnsignedRightShift:
        return RuntimeOperation::ValueURShift;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RuntimeOperation OperatorEmitter::runtimeOperationOf(UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return RuntimeOperation::ValueNegate;
    case UnaryOperator::Increment:
        return RuntimeOperation::Increment;
    case UnaryOperator::Decrement:
        return RuntimeOperation::Decrement;
    case UnaryOperator::BitNot:
        return RuntimeOperation::ValueBitNot;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RuntimeOperation OperatorEmitter::runtimeOperationOf(Relation relation)
{
    switch (relation) {
    case Relation::Less:
        return RuntimeOperation::CompareLess;
    case Relation::LessOrEqual:
        return RuntimeOperation::CompareLessEq;
    case Relation::Greater:
        return RuntimeOperation::CompareGreater;
    case Relation::GreaterOrEqual:
        return RuntimeOperation::CompareGreaterEq;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
