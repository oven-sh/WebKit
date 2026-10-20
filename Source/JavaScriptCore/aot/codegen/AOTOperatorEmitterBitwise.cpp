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

namespace JSC { namespace AOT { namespace Typed {

static BinaryOperator binaryOperatorFor(BitwiseOperator op)
{
    switch (op) {
    case BitwiseOperator::And:
        return BinaryOperator::BitAnd;
    case BitwiseOperator::Or:
        return BinaryOperator::BitOr;
    case BitwiseOperator::Xor:
        return BinaryOperator::BitXor;
    case BitwiseOperator::LeftShift:
        return BinaryOperator::LShift;
    case BitwiseOperator::RightShift:
        return BinaryOperator::RShift;
    case BitwiseOperator::UnsignedRightShift:
        return BinaryOperator::URShift;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void OperatorEmitter::generateBitAnd(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::And);
}

void OperatorEmitter::generateBitOr(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::Or);
}

void OperatorEmitter::generateBitXor(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::Xor);
}

void OperatorEmitter::generateLShift(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::LeftShift);
}

void OperatorEmitter::generateRShift(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::RightShift);
}

void OperatorEmitter::generateURShift(const ValueContext& value)
{
    generateBitwise(value, BitwiseOperator::UnsignedRightShift);
}

void OperatorEmitter::generateBitwise(const ValueContext& value, BitwiseOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    bool isUnsigned = op == BitwiseOperator::UnsignedRightShift;
    auto defineWord = [&](Word32 word) {
        if (isUnsigned)
            define(value, zeroExtended(word));
        else
            define(value, word);
    };
    auto numberToInt32 = [&](ValueID id) {
        switch (representationOf(id)) {
        case Representation::Int32:
            return in<Word32>(id);
        case Representation::Int64:
            return low32(in<Integer>(id));
        case Representation::OptionalInt64:
            static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));
            return low32(Integer(in<OptionalInteger>(id).value()));
        case Representation::Double:
        case Representation::OptionalDouble:
            return emitter<ConversionEmitter>().toInt32(asNumericOperand(id));
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::LiteralString:
        case Representation::RawWord:
            break;
        }
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return word32(0);
    };

    switch (value->form()) {
    case OperatorForm::Integers:
        defineWord(bitwiseOnWords(op, in<Word32>(left), in<Word32>(right)));
        return;
    case OperatorForm::Numbers: {
        Word32 leftWord = numberToInt32(left);
        Word32 rightWord = numberToInt32(right);
        defineWord(bitwiseOnWords(op, leftWord, rightWord));
        return;
    }
    case OperatorForm::BigInts:
        if (isUnsigned)
            break;
        define(value, unboxCell(callOperation<Boxed>(value, runtimeOperationOf(op), { boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)) })));
        return;
    case OperatorForm::Generic: {
        Boxed result = bitwiseOnValues(value, op);
        // A result from 2^31 on comes back as a double.
        if (isUnsigned)
            define(value, emitter<ConversionEmitter>().unbox(result, typeOf(value.id()), value.representation()));
        else
            define(value, result);
        return;
    }
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
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
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
}

Word32 OperatorEmitter::bitwiseOnWords(BitwiseOperator op, Word32 left, Word32 right)
{
    switch (op) {
    case BitwiseOperator::And:
        return bitAnd(left, right);
    case BitwiseOperator::Or:
        return bitOr(left, right);
    case BitwiseOperator::Xor:
        return bitXor(left, right);
    case BitwiseOperator::LeftShift:
        return shiftLeft(left, bitAnd(right, word32(31)));
    case BitwiseOperator::RightShift:
        return shiftRight(left, bitAnd(right, word32(31)));
    case BitwiseOperator::UnsignedRightShift:
        return shiftRightUnsigned(left, bitAnd(right, word32(31)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed OperatorEmitter::bitwiseOnValues(const ValueContext& value, BitwiseOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    BinaryOperator binaryOperator = binaryOperatorFor(op);
    bool mayBothBeInt32 = typeOf(left).primitives & Primitive::Integer && typeOf(right).primitives & Primitive::Integer;
    bool isAfterInt32Check = !mayBothBeInt32 && hasFormAfterInt32Check(binaryOperator);
    return Boxed(callStub(value, binaryOperation(binaryOperator, isAfterInt32Check), { in<Boxed>(left).value(), in<Boxed>(right).value() }).first);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
