/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStringRangeReadEmitter.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeTable.h"
#include "AOTTypeTestPlan.h"
#include "PureNaN.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ArithmeticTraits {
    BinaryOperator stub;
    std::optional<RuntimeOperation> onNumberAndValue;
};

struct UnaryTraits {
    UnaryOperationForm stub;
    RuntimeOperation onBigInt;
};

} // anonymous namespace

static ArithmeticTraits traitsOfOperator(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return { BinaryOperator::Add, RuntimeOperation::AddNumberAndValueAsNumber };
    case ArithmeticOperator::Sub:
        return { BinaryOperator::Sub, RuntimeOperation::SubNumberAndValueAsNumber };
    case ArithmeticOperator::Mul:
        return { BinaryOperator::Mul, RuntimeOperation::MulNumberAndValueAsNumber };
    case ArithmeticOperator::Div:
        return { BinaryOperator::Div, RuntimeOperation::DivNumberAndValueAsNumber };
    case ArithmeticOperator::Mod:
        return { BinaryOperator::Mod, std::nullopt };
    case ArithmeticOperator::Pow:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnaryTraits traitsOfOperator(UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return { UnaryOperationForm::Negate, RuntimeOperation::ValueNegate };
    case UnaryOperator::Increment:
        return { UnaryOperationForm::Increment, RuntimeOperation::Increment };
    case UnaryOperator::Decrement:
        return { UnaryOperationForm::Decrement, RuntimeOperation::Decrement };
    case UnaryOperator::BitNot:
        return { UnaryOperationForm::BitNot, RuntimeOperation::ValueBitNot };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::failWithZero(const ValueContext& value)
{
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Operand OperatorEmitter::arithmeticResult(const ValueContext& value, ArithmeticOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    switch (value->form()) {
    case OperatorForm::Generic: {
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        return arithmeticOnBoxedOperands(value, op, x, y);
    }
    case OperatorForm::Integers:
        return arithmeticOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        return arithmeticOnCheckedIntegers(value, op);
    case OperatorForm::Numbers: {
        if (value.representation() == Representation::JSValue)
            return sumOfBoxedNumbers(op, get(left), typeOf(left), get(right), typeOf(right));
        Number x = op == ArithmeticOperator::Add ? asTermOfSum(left, right) : asNumericOperand(left);
        Number y = op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub ? asTermOfSum(right, left) : asNumericOperand(right);
        return arithmeticOnNumbers(op, x, y);
    }
    case OperatorForm::NumberAndValue:
        if (op != ArithmeticOperator::Pow && traitsOfOperator(op).onNumberAndValue)
            return arithmeticOnNumberAndValue(value, op);
        break;
    case OperatorForm::BigInts: {
        Boxed x = boxCell(in<CellPointer>(left));
        Boxed y = boxCell(in<CellPointer>(right));
        return unboxCell(arithmeticOnBoxedOperands(value, op, x, y));
    }
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::MayBeString:
        if (op == ArithmeticOperator::Add)
            return addStrings(value);
        break;
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
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    return failWithZero(value);
}

LValue OperatorEmitter::callBinaryStub(const ValueContext& value, BinaryOperator op, Boxed left, Boxed right)
{
    bool mayBothBeInt32 = typeOf(value->leftOperand()).primitives & Primitive::Integer && typeOf(value->rightOperand()).primitives & Primitive::Integer;
    bool isAfterInt32Check = !mayBothBeInt32 && hasFormAfterInt32Check(op);
    return callStub(value, binaryOperation(op, isAfterInt32Check), { left.value(), right.value() }).first;
}

Boxed OperatorEmitter::arithmeticOnBoxedOperands(const ValueContext& value, ArithmeticOperator op, Boxed left, Boxed right)
{
    if (op == ArithmeticOperator::Pow)
        return callOperation<Boxed>(value, RuntimeOperation::ValuePow, { left, right });
    return Boxed(callBinaryStub(value, traitsOfOperator(op).stub, left, right));
}

Number OperatorEmitter::arithmeticOnNumberAndValue(const ValueContext& value, ArithmeticOperator op)
{
    bool numberIsOnRight = representationOf(value->leftOperand()) != Representation::Double;
    ValueID numberID = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    Number numberOperand = in<Number>(numberID);
    Boxed otherOperand = in<Boxed>(otherID);
    Word32 typeAndNumberSide = word32(static_cast<int32_t>(NumberAndValueBits::encode(TypeTable::number.index(), numberIsOnRight)));
    if (typeOf(otherID).primitives & Primitive::Number)
        return Number(callStub(value, binaryWithDouble(traitsOfOperator(op).stub, numberIsOnRight), { numberOperand.value(), otherOperand.value(), typeAndNumberSide.value() }).first);
    return callOperation<Number>(value, *traitsOfOperator(op).onNumberAndValue, { otherOperand, typeAndNumberSide, numberOperand });
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

Operand OperatorEmitter::unaryResult(const ValueContext& value, UnaryOperator op)
{
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        return Boxed(callStub(value, unaryOperation(traitsOfOperator(op).stub), { in<Boxed>(operand).value() }).first);
    case OperatorForm::Integers:
        return unaryOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        return unaryOnCheckedIntegers(value, op);
    case OperatorForm::Numbers:
        if (value.representation() == Representation::JSValue) {
            RELEASE_ASSERT(op == UnaryOperator::Increment || op == UnaryOperator::Decrement);
            ArithmeticOperator sum = op == UnaryOperator::Increment ? ArithmeticOperator::Add : ArithmeticOperator::Sub;
            return sumOfBoxedNumbers(sum, get(operand), typeOf(operand), word32(1), typeOf(operand));
        }
        return unaryOnNumbers(op, asNumericOperand(operand));
    case OperatorForm::BigInts:
        return unboxCell(callOperation<Boxed>(value, traitsOfOperator(op).onBigInt, { boxCell(in<CellPointer>(operand)) }));
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
    return failWithZero(value);
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
        return bitXor(in<Word32>(value->valueOperand()), -1);
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
    return failWithZero(value);
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
        return bitXor(emitter<ConversionEmitter>().toInt32(operand), -1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::asNumberLikeOperand(ValueID id)
{
    if (representationOf(id) == Representation::Boolean)
        return toNumber(asWord32(in<Condition>(id)));
    Boxed value = in<Boxed>(id);
    Condition isTrue = equal(value, Boxed(m_out.constInt64(JSValue::ValueTrue)));
    Condition isUndefined = equal(value, undefined());
    Number notANumber = number(PNaN);
    Number convertedPrimitive = select(isUndefined, notANumber, toNumber(asWord32(isTrue)));
    Condition valueIsNumber = isNumber(value);
    return select(valueIsNumber, numberToDouble(value), convertedPrimitive);
}

template<typename Functor>
Operand OperatorEmitter::convertedToNumber(const ValueContext& value, const Functor& convertValue)
{
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        return convertValue(in<Boxed>(operand));
    case OperatorForm::Integers:
        return get(operand);
    case OperatorForm::Numbers:
        if (representationOf(operand) == Representation::Double)
            return get(operand);
        return emitter<ConversionEmitter>().withoutPayload(asNumericOperand(operand));
    case OperatorForm::NullableNumber:
        return asNumberLikeOperand(operand);
    case OperatorForm::Booleans:
        return asWord32(in<Condition>(operand));
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
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
    return failWithZero(value);
}

void OperatorEmitter::generateToNumber(const ValueContext& value)
{
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        const Type& type = typeOf(value->valueOperand());
        if (type.primitives & Primitive::AnyString && type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull)) {
            return either<Number>(isCell(operand), [&] {
                return emitter<StringRangeReadEmitter>().toNumber(emitter<StringEmitter>().viewOfString(value, unboxCell(operand)));
            }, [&] {
                return asNumberLikeOperand(value->valueOperand());
            });
        }
        return Number(callStub(value, toNumberAsDouble(), { operand.value() }).first);
    }));
}

void OperatorEmitter::generateToNumeric(const ValueContext& value)
{
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        return Boxed(callStub(value, unaryOperation(UnaryOperationForm::ToNumeric), { operand.value() }).first);
    }));
}

void OperatorEmitter::generateLogicalNot(const ValueContext& value)
{
    define(value, logicalNot(in<Condition>(value->valueOperand())));
}

void OperatorEmitter::generateTypeOf(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::TypeOf, { in<Boxed>(value->valueOperand()) }));
}

// What passes the test is the result as it stands.
template<typename T, typename Functor>
T OperatorEmitter::passingOrConverted(Boxed value, const TypeTestPlan& plan, const Functor& convert)
{
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return T(value.value());
    case TypeTestVerdict::NeverPasses:
        return convert(CallOptions { });
    case TypeTestVerdict::NeedsTest:
        break;
    }

    LBasicBlock passes = newBlock();
    LBasicBlock fails = newBlock();
    Join<T> join(m_out);
    branchOnType(value, plan, { normalEdge(passes), rareEdge(fails) });

    m_out.appendTo(passes);
    join.add(T(value.value()));

    m_out.appendTo(fails);
    join.add(convert(CallOptions { .isOnRarePath = true }));
    return join.finish();
}

void OperatorEmitter::generateToObject(const ValueContext& value)
{
    StringID message = value->message() ? value->message() : m_program.strings.emptyString();
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(m_function.at(value->valueOperand()).type, Representation::JSValue, TypeTable::object);
    CellPointer object = passingOrConverted<CellPointer>(operand, plan, [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::ToObject, { operand, word32(message.index()) }, options);
    });
    // The conversion of undefined or null throws, and a value of type never has no representation of an object.
    if (!typeOf(value.id()).isNever())
        define(value, object);
}

void OperatorEmitter::generateToThis(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(m_function.at(value->valueOperand()).type, Representation::JSValue, TypeTable::object);
    define(value, passingOrConverted<CellPointer>(operand, plan, [&](const CallOptions& options) {
        return unboxCell(callOperation<Boxed>(value, RuntimeOperation::ToThis, { operand }, options));
    }));
}

void OperatorEmitter::generateToPropertyKey(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    TypeID keyType = types().intern(Type::fromPrimitives(Primitive::String | Primitive::Symbol));
    const TypeTestPlan& plan = typeTestPlanFor(types().intern(typeOf(value->valueOperand())), Representation::JSValue, keyType);
    define(value, unboxCell(passingOrConverted<Boxed>(operand, plan, [&](const CallOptions&) {
        return Boxed(callStub(value, AOT::toPropertyKey(), { operand.value() }).first);
    })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
