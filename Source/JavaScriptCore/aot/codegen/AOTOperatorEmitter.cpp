/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTStringEmitter.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeTable.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include "PureNaN.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ArithmeticOperatorTraits {
    BinaryOperator stub;
    bool hasStubWithDouble;
    RuntimeOperation onBigInts;
};

struct UnaryOperatorTraits {
    UnaryOperationForm stub;
    RuntimeOperation onBigInt;
};

} // anonymous namespace

static ArithmeticOperatorTraits traitsOfArithmeticOperator(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return { BinaryOperator::Add, true, RuntimeOperation::ValueAdd };
    case ArithmeticOperator::Sub:
        return { BinaryOperator::Sub, true, RuntimeOperation::ValueSub };
    case ArithmeticOperator::Mul:
        return { BinaryOperator::Mul, true, RuntimeOperation::ValueMul };
    case ArithmeticOperator::Div:
        return { BinaryOperator::Div, true, RuntimeOperation::ValueDiv };
    case ArithmeticOperator::Mod:
        return { BinaryOperator::Mod, false, RuntimeOperation::ValueMod };
    case ArithmeticOperator::Pow:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnaryOperatorTraits traitsOfUnaryOperator(UnaryOperator op)
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

void OperatorEmitter::defineHeldToItsType(const ValueContext& value, Operand result)
{
    bool walkHasAsked = (m_code.role() == CodeRole::Body || m_code.role() == CodeRole::CoroutineBody) && AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime);
    if (!walkHasAsked && emittersCheckProofs() && numberRepresentations.contains(result.representation)) {
        const TypeTestPlan& plan = typeTestPlanFor(widestTypeOf(result.representation), result.representation, typeIDOf(value.id()));
        if (plan.verdict != TypeTestVerdict::AlwaysPasses)
            emitter<ThrowEmitter>().requireOrCrash(value, hasType(result, plan), std::nullopt, ProofSource::OtherOperation);
    }
    define(value, result);
}

static bool markersMayMeetInArithmetic(ArithmeticOperator op, const Type& left, const Type& right)
{
    constexpr uint16_t numberOrUndefined = Primitive::Number | Primitive::Undefined;
    bool isOfMachine = op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub || op == ArithmeticOperator::Mul || op == ArithmeticOperator::Div;
    return isOfMachine && left.hasOnlyPrimitives(numberOrUndefined) && right.hasOnlyPrimitives(numberOrUndefined);
}

Operand OperatorEmitter::arithmeticResult(const ValueContext& value, ArithmeticOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    switch (value->form()) {
    case OperatorForm::Generic: {
        m_out.noteForm(op == ArithmeticOperator::Pow ? Form::Runtime : Form::Closed, StateOfKnowledge::Generic);
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        if (sumsInt32sInPlace(value, op))
            return sumOfAnyValues(value, op, x, y);
        return arithmeticOnBoxedOperands(value, op, x, y);
    }
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return arithmeticOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        m_out.noteForm(Form::Open, StateOfKnowledge::CheckedIntegers);
        return arithmeticOnCheckedIntegers(value, op);
    case OperatorForm::Numbers: {
        if (op != ArithmeticOperator::Mod && op != ArithmeticOperator::Pow)
            m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (value.representation() == Representation::JSValue) {
            auto termOfBoxedSum = [&](ValueID term) -> Operand {
                return representationOf(term) == Representation::Int32 ? get(term) : Operand(in<Boxed>(term));
            };
            Operand x = termOfBoxedSum(left);
            Operand y = termOfBoxedSum(right);
            return sumOfBoxedNumbers(op, x, typeOf(left), y, typeOf(right), emitter<CastEmitter>().takeNumberChecksOf(value.id()).span());
        }
        if (handsOnBitsOfNaN(value.value()) && (generator().holdsAnyNaN(left) || generator().holdsAnyNaN(right))) {
            Number x = asNumericOperandWithAnyNaN(left);
            Number y = asNumericOperandWithAnyNaN(right);
            return generator().settleNaNOf(value, arithmeticOnNumbers(op, x, y, &value));
        }
        if (representationOf(left) == Representation::OptionalDouble && representationOf(right) == Representation::OptionalDouble && markersMayMeetInArithmetic(op, typeOf(left), typeOf(right))) {
            build().statistics().count(StatisticID::ArithmeticOnMarkers);
            return emitter<ConversionEmitter>().withoutPayload(arithmeticOnNumbers(op, Number(get(left).value), Number(get(right).value)));
        }
        Number x = asNumericOperand(left, &value);
        Number y = asNumericOperand(right, &value);
        return arithmeticOnNumbers(op, x, y, &value);
    }
    case OperatorForm::NumberAndValue:
        if (op != ArithmeticOperator::Pow && traitsOfArithmeticOperator(op).hasStubWithDouble)
            return arithmeticOnNumberAndValue(value, op);
        break;
    case OperatorForm::BigInts: {
        m_out.noteForm(op == ArithmeticOperator::Pow ? Form::Runtime : Form::Closed, StateOfKnowledge::BigInts);
        Boxed x = boxCell(in<CellPointer>(left));
        Boxed y = boxCell(in<CellPointer>(right));
        if (op != ArithmeticOperator::Pow && generator().isInLoop(value.block()))
            return unboxCell(callOperation<Boxed>(value, traitsOfArithmeticOperator(op).onBigInts, { x, y }));
        return unboxCell(arithmeticOnBoxedOperands(value, op, x, y));
    }
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::MayBeString:
        if (op == ArithmeticOperator::Add) {
            m_out.noteForm(Form::Closed, stateOf(value->form()));
            return addStrings(value);
        }
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
    return Boxed(callBinaryStub(value, traitsOfArithmeticOperator(op).stub, left, right));
}

bool OperatorEmitter::sumsInt32sInPlace(const ValueContext& value, ArithmeticOperator op)
{
    if ((op != ArithmeticOperator::Add && op != ArithmeticOperator::Sub) || !generator().isInLoop(value.block()) || standsInRareBlock(value))
        return false;
    return typeOf(value->leftOperand()).primitives & Primitive::Integer && typeOf(value->rightOperand()).primitives & Primitive::Integer;
}

Boxed OperatorEmitter::sumOfAnyValues(const ValueContext& value, ArithmeticOperator op, Boxed left, Boxed right)
{
    bool isLeftInt32 = typeOf(value->leftOperand()).isInt32();
    bool isRightInt32 = typeOf(value->rightOperand()).isInt32();
    Boxed tested = isLeftInt32 ? right : isRightInt32 ? left : Boxed(m_out.bitAnd(left.value(), right.value()));
    LBasicBlock int32Case = newBlock();
    LBasicBlock otherCase = newBlock();
    Join<Boxed> join(m_out);
    Condition areInt32 = isInt32(tested);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(areInt32.value(), normalEdge(int32Case), unlikelyEdge(otherCase));

    m_out.appendToArm(int32Case, fork);
    Integer x = signExtended(low32(left));
    Integer y = signExtended(low32(right));
    Integer exact = op == ArithmeticOperator::Add ? add(x, y) : sub(x, y);
    join.add(emitter<ConversionEmitter>().boxInteger(exact, { 2 * static_cast<int64_t>(INT32_MIN), 2 * static_cast<int64_t>(INT32_MAX) + 1 }));

    m_out.appendToArm(otherCase, fork);
    join.add(arithmeticOnBoxedOperands(value, op, left, right));
    return join.finishBehind(fork);
}

Number OperatorEmitter::arithmeticOnNumberAndValue(const ValueContext& value, ArithmeticOperator op)
{
    bool numberIsOnRight = !typeOf(value->leftOperand()).isNumber();
    ValueID numberID = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    Number numberOperand = in<Number>(numberID);
    Boxed otherOperand = in<Boxed>(otherID);
    ArithmeticOperatorTraits traits = traitsOfArithmeticOperator(op);
    Word32 typeAndNumberSide = word32(static_cast<int32_t>(NumberAndValueBits::encode(0, numberIsOnRight)));
    auto byStub = [&] {
        return Number(callStub(value, binaryWithDouble(traits.stub, numberIsOnRight), { numberOperand.value(), otherOperand.value(), typeAndNumberSide.value() }).first);
    };
    if (typeOf(otherID).primitives & Primitive::Number) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::NumberAndValue);
        if (standsInRareBlock(value))
            return byStub();
        auto withNumber = [&](Number otherNumber) {
            return numberIsOnRight ? arithmeticOnNumbers(op, otherNumber, numberOperand, &value) : arithmeticOnNumbers(op, numberOperand, otherNumber, &value);
        };
        LBasicBlock int32Case = newBlock();
        LBasicBlock noInt32Case = newBlock();
        LBasicBlock doubleCase = newBlock();
        LBasicBlock otherCase = newBlock();
        Join<Number> join(m_out);
        Condition holdsInt32 = isInt32(otherOperand);
        ScopeFork fork = m_out.forkScope();
        m_out.branch(holdsInt32.value(), normalEdge(int32Case), unlikelyEdge(noInt32Case));
        m_out.appendToArm(int32Case, fork);
        join.add(withNumber(toNumber(low32(otherOperand))));
        m_out.appendToArm(noInt32Case, fork);
        Condition holdsDouble = isNumber(otherOperand);
        ScopeFork forkOfNoInt32 = m_out.forkScope();
        m_out.branch(holdsDouble.value(), normalEdge(doubleCase), rareEdge(otherCase));
        m_out.appendToArm(doubleCase, forkOfNoInt32);
        join.add(withNumber(unboxDouble(otherOperand)));
        m_out.appendToArm(otherCase, forkOfNoInt32);
        join.add(byStub());
        return join.finishBehind(fork);
    }
    m_out.noteForm(Form::Closed, StateOfKnowledge::NumberAndOtherValue);
    return byStub();
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
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return Boxed(callStub(value, unaryOperation(traitsOfUnaryOperator(op).stub), { in<Boxed>(operand).value() }).first);
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return unaryOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        m_out.noteForm(Form::Open, StateOfKnowledge::CheckedIntegers);
        return unaryOnCheckedIntegers(value, op);
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (value.representation() == Representation::JSValue) {
            RELEASE_ASSERT(op == UnaryOperator::Increment || op == UnaryOperator::Decrement);
            ArithmeticOperator sum = op == UnaryOperator::Increment ? ArithmeticOperator::Add : ArithmeticOperator::Sub;
            Boxed stepped = in<Boxed>(operand);
            return sumOfBoxedNumbers(sum, stepped, typeOf(operand), word32(1), typeOf(operand));
        }
        return unaryOnNumbers(op, operand);
    case OperatorForm::BigInts:
        m_out.noteForm(Form::Runtime, StateOfKnowledge::BigInts);
        return unboxCell(callOperation<Boxed>(value, traitsOfUnaryOperator(op).onBigInt, { boxCell(in<CellPointer>(operand)) }));
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

Operand OperatorEmitter::unaryOnNumbers(UnaryOperator op, ValueID id)
{
    Number operand = asNumericOperand(id);
    switch (op) {
    case UnaryOperator::Negate:
        return neg(operand);
    case UnaryOperator::Increment:
        return add(operand, number(1));
    case UnaryOperator::Decrement:
        return sub(operand, number(1));
    case UnaryOperator::BitNot:
        return bitXor(emitter<ConversionEmitter>().toInt32(operand, ConversionEmitter::BitsOfInt32::All), -1);
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
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return get(operand);
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        return asNumericOperand(operand, &value);
    case OperatorForm::NullableNumber:
        m_out.noteForm(Form::Open, StateOfKnowledge::NullableNumber);
        return asNumberLikeOperand(operand);
    case OperatorForm::Booleans:
        m_out.noteForm(Form::Open, StateOfKnowledge::Booleans);
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

bool OperatorEmitter::isCutToInt32Alone(ValueID number)
{
    ValueID user = generator().code().useCountOf(number) == 1 ? generator().code().onlyUserOf(number) : ValueID();
    return user && (opcodeOf(user) == Opcode::ToInt32 || opcodeOf(user) == Opcode::ToUint32);
}

void OperatorEmitter::generateToNumber(const ValueContext& value)
{
    if (ValueID operand = value->valueOperand(); handsOnBitsOfNaN(value.value()) && generator().holdsAnyNaN(operand)) {
        define(value, generator().settleNaNOf(value, generator().getWithAnyNaN(operand).as<Number>()));
        return;
    }
    if (ValueID operand = value->valueOperand(); representationOf(operand) == Representation::OptionalDouble && value->form() == OperatorForm::Numbers && isCutToInt32Alone(value.id())) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        define(value, asNumber(get(operand), typeOf(operand), PayloadOfNaN::Stays));
        return;
    }
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return Number(callStub(value, toNumberAsDouble(), { operand.value() }).first);
    }));
}

void OperatorEmitter::generateToNumeric(const ValueContext& value)
{
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
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

template<typename T, typename Functor>
T OperatorEmitter::passingOrConverted(Boxed value, const TypeTestPlan& plan, Form formOfConversion, const Functor& convert)
{
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        return T(value.value());
    case TypeTestVerdict::NeverPasses:
        m_out.noteForm(formOfConversion, StateOfKnowledge::FailsTest);
        return convert(CallOptions { });
    case TypeTestVerdict::NeedsTest:
        break;
    }

    m_out.noteForm(Form::Open, StateOfKnowledge::NeedsTest);
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
    bool hasResult = !isNeverType(value->type);
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, TypeTable::object);
    if (hasResult && plan.verdict == TypeTestVerdict::AlwaysPasses) {
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        defineNarrowed(value, operand);
        return;
    }

    StringID message = value->message() ? value->message() : m_program.strings.emptyString();
    CellPointer object = passingOrConverted<CellPointer>(operand, plan, Form::Runtime, [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::ToObject, { operand, numberOf(m_mentions.add(NumberSpace::ShownString, message)) }, options);
    });
    if (hasResult)
        define(value, object);
}

void OperatorEmitter::generateToThis(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, TypeTable::object);
    if (plan.verdict == TypeTestVerdict::AlwaysPasses) {
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        defineNarrowed(value, operand);
        return;
    }
    define(value, passingOrConverted<CellPointer>(operand, plan, Form::Runtime, [&](const CallOptions& options) {
        return unboxCell(callOperation<Boxed>(value, RuntimeOperation::ToThis, { operand }, options));
    }));
}

void OperatorEmitter::generateToPropertyKey(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    constexpr TypeID keyType = typeOfPrimitives(Primitive::String | Primitive::Symbol);
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, keyType);
    define(value, unboxCell(passingOrConverted<Boxed>(operand, plan, Form::Closed, [&](const CallOptions&) {
        return Boxed(callStub(value, AOT::toPropertyKey(), { operand.value() }).first);
    })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
