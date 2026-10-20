/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsArithmetic.h"

namespace JSC { namespace AOT { namespace Typed {

static BinaryOperator binaryOperatorFor(Relation relation)
{
    switch (relation) {
    case Relation::Less:
        return BinaryOperator::Less;
    case Relation::LessOrEqual:
        return BinaryOperator::LessEq;
    case Relation::Greater:
        return BinaryOperator::Greater;
    case Relation::GreaterOrEqual:
        return BinaryOperator::GreaterEq;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static Condition withIntegerOperands(Operand left, Operand right, const Functor& functor)
{
    switch (left.representation) {
    case Representation::Int32:
        return functor(left.as<Word32>(), right.as<Word32>());
    case Representation::Int64:
        return functor(left.as<Integer>(), right.as<Integer>());
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
    return { };
}

// An element access tests Below(index, length), so B3 merges its test with a loop condition of this form.
static Condition compareNonNegative(Output& out, Relation relation, LValue left, LValue right)
{
    switch (relation) {
    case Relation::Less:
        return Condition(out.below(left, right));
    case Relation::LessOrEqual:
        return Condition(out.belowOrEqual(left, right));
    case Relation::Greater:
        return Condition(out.below(right, left));
    case Relation::GreaterOrEqual:
        return Condition(out.belowOrEqual(right, left));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void OperatorEmitter::generateLess(const ValueContext& value)
{
    generateRelation(value, Relation::Less);
}

void OperatorEmitter::generateLessEq(const ValueContext& value)
{
    generateRelation(value, Relation::LessOrEqual);
}

void OperatorEmitter::generateGreater(const ValueContext& value)
{
    generateRelation(value, Relation::Greater);
}

void OperatorEmitter::generateGreaterEq(const ValueContext& value)
{
    generateRelation(value, Relation::GreaterOrEqual);
}

void OperatorEmitter::generateRelation(const ValueContext& value, Relation relation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::Generic:
    case OperatorForm::NumberAndValue:
        result = relationOnValues(value, relation);
        break;
    case OperatorForm::Integers: {
        bool areBothNonNegative = typeOf(left).integers.minimum >= 0 && typeOf(right).integers.minimum >= 0;
        result = withIntegerOperands(get(left), get(right), [&](auto x, auto y) {
            return areBothNonNegative ? compareNonNegative(m_out, relation, x.value(), y.value()) : compare(relation, x, y);
        });
        break;
    }
    case OperatorForm::Numbers: {
        Number x = asNumericOperand(left);
        Number y = asNumericOperand(right);
        result = compare(relation, x, y);
        break;
    }
    case OperatorForm::BigInts:
        result = callOperation<Condition>(value, runtimeOperationOf(relation), { boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)) });
        break;
    case OperatorForm::Strings:
        result = relationOnStrings(value, relation);
        break;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
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
    if (!result) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        result = boolean(false);
    }
    defineTest(value, result);
}

#define AOT_DEFINE_COMPARE(OperandType) \
    Condition OperatorEmitter::compare(Relation relation, OperandType left, OperandType right) \
    { \
        switch (relation) { \
        case Relation::Less: \
            return lessThan(left, right); \
        case Relation::LessOrEqual: \
            return lessThanOrEqual(left, right); \
        case Relation::Greater: \
            return greaterThan(left, right); \
        case Relation::GreaterOrEqual: \
            return greaterThanOrEqual(left, right); \
        } \
        RELEASE_ASSERT_NOT_REACHED(); \
    }
AOT_DEFINE_COMPARE(Number)
AOT_DEFINE_COMPARE(Integer)
AOT_DEFINE_COMPARE(Word32)
#undef AOT_DEFINE_COMPARE

Condition OperatorEmitter::relationOnStrings(const ValueContext& value, Relation relation)
{
    Word32 order = emitter<StringEmitter>().order(value, in<CellPointer>(value->leftOperand()), in<CellPointer>(value->rightOperand()));
    return compare(relation, order, word32(0));
}

Condition OperatorEmitter::relationOnValues(const ValueContext& value, Relation relation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    if (value->form() == OperatorForm::Generic) {
        BinaryOperator binaryOperator = binaryOperatorFor(relation);
        bool mayBothBeInt32 = typeOf(left).primitives & Primitive::Integer && typeOf(right).primitives & Primitive::Integer;
        bool isAfterInt32Check = !mayBothBeInt32 && hasFormAfterInt32Check(binaryOperator);
        return Condition(m_out.castToInt32(callStub(value, binaryOperation(binaryOperator, isAfterInt32Check), { in<Boxed>(left).value(), in<Boxed>(right).value() }).first));
    }

    bool numberIsOnRight = representationOf(left) != Representation::Double;
    ValueID otherID = numberIsOnRight ? left : right;
    Number numberOperand = in<Number>(numberIsOnRight ? right : left);
    Boxed otherOperand = in<Boxed>(otherID);
    auto compareByOperation = [&](const CallOptions& options) {
        Boxed boxedNumber = boxDouble(emitter<ConversionEmitter>().purified(numberOperand));
        Boxed x = numberIsOnRight ? otherOperand : boxedNumber;
        Boxed y = numberIsOnRight ? boxedNumber : otherOperand;
        return callOperation<Condition>(value, runtimeOperationOf(relation), { x, y }, options);
    };
    if (!(typeOf(otherID).primitives & Primitive::Number))
        return compareByOperation(CallOptions { });
    return either<Condition>(isNumber(otherOperand), [&] {
        Number otherNumber = numberToDouble(otherOperand);
        return numberIsOnRight ? compare(relation, otherNumber, numberOperand) : compare(relation, numberOperand, otherNumber);
    }, [&] {
        return compareByOperation(CallOptions { .isOnRarePath = true });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
