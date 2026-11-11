/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTRepresentationRules.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BitwiseOperatorTraits {
    BinaryOperator stub;
    RuntimeOperation onBigInts;
};

} // anonymous namespace

static BitwiseOperatorTraits traitsOfBitwiseOperator(BitwiseOperator op)
{
    switch (op) {
    case BitwiseOperator::And:
        return { BinaryOperator::BitAnd, RuntimeOperation::ValueBitAnd };
    case BitwiseOperator::Or:
        return { BinaryOperator::BitOr, RuntimeOperation::ValueBitOr };
    case BitwiseOperator::Xor:
        return { BinaryOperator::BitXor, RuntimeOperation::ValueBitXor };
    case BitwiseOperator::LeftShift:
        return { BinaryOperator::LShift, RuntimeOperation::ValueLShift };
    case BitwiseOperator::RightShift:
        return { BinaryOperator::RShift, RuntimeOperation::ValueRShift };
    case BitwiseOperator::UnsignedRightShift:
        return { BinaryOperator::URShift, RuntimeOperation::ValueURShift };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::bitwiseResult(const ValueContext& value, BitwiseOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    bool isUnsigned = op == BitwiseOperator::UnsignedRightShift;
    auto onWords = [&](Word32 x, Word32 y) -> Operand {
        Word32 word = bitwiseOnWords(op, x, y);
        return isUnsigned ? Operand(zeroExtended(word)) : Operand(word);
    };
    auto isMaskOfLowestBits = [&](ValueID id) {
        return op == BitwiseOperator::And && isMaskOfLowestBitsOfInt32(typeOf(id));
    };
    auto formOfNumber = [&](ValueID id) {
        if (!constantsInFormOfUses.contains(opcodeOf(id)))
            return representationOf(id);
        const Type& type = typeOf(id);
        if (type.isInteger())
            return type.isInt32() ? Representation::Int32 : Representation::Int64;
        return type.isNumber() ? Representation::Double : Representation::OptionalDouble;
    };
    auto numberToInt32 = [&](ValueID id, ValueID other) {
        ConversionEmitter::BitsOfInt32 bits = isMaskOfLowestBits(other) ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All;
        switch (formOfNumber(id)) {
        case Representation::Int32:
            return in<Word32>(id);
        case Representation::Int64:
            if (opcodeOf(id) == Opcode::MultiplyAddIntegers)
                return low32(in<Integer>(id));
            return low32InWords(in<Integer>(id));
        case Representation::OptionalInt64:
            static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));
            return low32(Integer(in<OptionalInteger>(id).value()));
        case Representation::Double:
        case Representation::OptionalDouble:
            if (representationOf(id) == Representation::OptionalDouble && !generator().holdsAnyNaN(id))
                return emitter<ConversionEmitter>().toInt32(asNumber(in<OptionalNumber>(id), typeOf(id), PayloadOfNaN::Stays), bits);
            return emitter<ConversionEmitter>().toInt32(asNumericOperandWithAnyNaN(id), bits);
        case Representation::JSValue:
            if (typeOf(id).isNumber())
                return emitter<ConversionEmitter>().toInt32OfBoxedNumber(in<Boxed>(id));
            if (!typeOf(id).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
                break;
            return emitter<ConversionEmitter>().truncateNullableNumber(in<Boxed>(id));
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::RawWord:
            break;
        }
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return word32(0);
    };

    switch (value->form()) {
    case OperatorForm::Integers: {
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        if (std::optional<Integer> extended = signExtendedBitwiseResult(value, op)) {
            Word32 word = low32(*extended);
            emitter<ConversionEmitter>().noteSignExtended(word, *extended);
            return word;
        }
        Word32 x = in<Word32>(left);
        Word32 y = in<Word32>(right);
        return onWords(x, y);
    }
    case OperatorForm::Numbers: {
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        Word32 x = numberToInt32(left, right);
        Word32 y = numberToInt32(right, left);
        return onWords(x, y);
    }
    case OperatorForm::BigInts:
        if (isUnsigned)
            break;
        m_out.noteForm(Form::Runtime, StateOfKnowledge::BigInts);
        return unboxCell(callOperation<Boxed>(value, traitsOfBitwiseOperator(op).onBigInts, { boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)) }));
    case OperatorForm::Generic: {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        bool mayBothBeInt32 = typeOf(left).primitives & Primitive::Integer && typeOf(right).primitives & Primitive::Integer;
        if (!isUnsigned && mayBothBeInt32 && !standsInRareBlock(value)) {
            auto isWrittenInt32 = [&](ValueID id) {
                return operationAt(definitionOf(id)).opcode == Opcode::Integer && typeOf(id).isInt32();
            };
            Condition areInt32 = isWrittenInt32(left) ? isInt32(y) : isWrittenInt32(right) ? isInt32(x) : logicalAnd(isInt32(x), isInt32(y));
            bool givesWord = value.representation() == Representation::Int32;
            LBasicBlock worksOnWords = newBlock();
            LBasicBlock callsStub = newBlock();
            Join<Boxed> boxedJoin(m_out);
            Join<Word32> wordJoin(m_out);
            ScopeFork fork = m_out.forkScope();
            m_out.branch(areInt32.value(), normalEdge(worksOnWords), rareEdge(callsStub));
            m_out.appendToArm(worksOnWords, fork);
            Word32 word = bitwiseOnWords(op, int32InBox(x), int32InBox(y));
            if (givesWord)
                wordJoin.add(word);
            else
                boxedJoin.add(boxInt32(word));
            m_out.appendToArm(callsStub, fork);
            Boxed answer(callStub(value, binaryOperation(traitsOfBitwiseOperator(op).stub, true), { x.value(), y.value() }).first);
            if (givesWord) {
                wordJoin.add(unboxInt32(answer));
                return wordJoin.finishBehind(fork);
            }
            boxedJoin.add(answer);
            return boxedJoin.finishBehind(fork);
        }
        Boxed result(callBinaryStub(value, traitsOfBitwiseOperator(op).stub, x, y));
        if (isUnsigned)
            return emitter<ConversionEmitter>().unbox(result, typeOf(value.id()), value.representation());
        return value.representation() == Representation::Int32 ? Operand(unboxInt32(result)) : Operand(result);
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
    return failWithZero(value);
}

std::optional<Integer> OperatorEmitter::signExtendedOperand(ValueID id)
{
    const Operation& operation = operationAt(definitionOf(id));
    if (operation.opcode == Opcode::Integer)
        return integer(operation.integer());
    if (!emittersCheckProofs() && operation.opcode == Opcode::Unbox && representationOf(operation.operand(0)) == Representation::OptionalInt64 && typeOf(operation.operand(0)).isInt32())
        return Integer(in<OptionalInteger>(operation.operand(0)).value());
    return emitter<ConversionEmitter>().signExtendedIntegerOf(in<Word32>(id));
}

std::optional<Integer> OperatorEmitter::signExtendedBitwiseResult(const ValueContext& value, BitwiseOperator op)
{
    bool isBitByBit = op == BitwiseOperator::And || op == BitwiseOperator::Or || op == BitwiseOperator::Xor;
    if (!isBitByBit || value.operationsBehind().empty() || typeOf(value.id()).integers.minimum >= 0)
        return std::nullopt;
    const Operation& next = operationAt(value.operationsBehind().front());
    bool isWidened = next.opcode == Opcode::WidenInteger || (next.opcode == Opcode::Box && next.representation == Representation::OptionalInt64);
    if (!isWidened || next.operand(0) != value.id())
        return std::nullopt;
    std::optional<Integer> left = signExtendedOperand(value->leftOperand());
    std::optional<Integer> right = left ? signExtendedOperand(value->rightOperand()) : std::nullopt;
    if (!right)
        return std::nullopt;
    switch (op) {
    case BitwiseOperator::And:
        return bitAnd(*left, *right);
    case BitwiseOperator::Or:
        return bitOr(*left, *right);
    default:
        return bitXor(*left, *right);
    }
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
