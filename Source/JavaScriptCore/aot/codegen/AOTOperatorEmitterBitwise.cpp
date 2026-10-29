/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTThreshold.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BitwiseTraits {
    BinaryOperator stub;
    RuntimeOperation onBigInts;
};

} // anonymous namespace

static BitwiseTraits traitsOfOperator(BitwiseOperator op)
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

// The first conversion that serves the block. `(a | 0) && (a | 0) && ..` converts one number in thousands of blocks, none of which dominates another.
auto OperatorEmitter::findInt32ConvertedBefore(Int32sOfNumber& known, BlockID block, bool needsLowestBitsAlone) -> const Int32OfNumber*
{
    AOT_THRESHOLD(size_t, mostSearchedInOrder, 8);
    constexpr uint32_t none = UINT32_MAX;
    const Dominators& dominators = generator().analyses().dominators();
    Vector<Int32OfNumber, 1>& conversions = known.conversions;
    auto serves = [&](const Int32OfNumber& earlier) {
        if (earlier.hasLowestBitsAlone && !needsLowestBitsAlone)
            return false;
        return earlier.block == block || dominators.terminatorDominates(earlier.block, block);
    };
    if (conversions.size() <= mostSearchedInOrder) {
        auto found = std::ranges::find_if(conversions, serves);
        return found == conversions.end() ? nullptr : &*found;
    }

    auto endOfBlocksUpTo = [&](BlockID last) {
        return std::ranges::upper_bound(known.byBlock, dominators.numberOf(last), { }, [&](uint32_t position) {
            return dominators.numberOf(conversions[position].block);
        });
    };
    // What dominates a block has a lower number, and dominates every block with a number between the two.
    auto nearestConversionAbove = [&](BlockID below) {
        auto end = endOfBlocksUpTo(below);
        uint32_t position = end == known.byBlock.begin() ? none : end[-1];
        while (position != none && !dominators.dominates(conversions[position].block, below))
            position = conversions[position].conversionAbove;
        return position;
    };
    while (known.byBlock.size() < conversions.size()) {
        uint32_t position = known.byBlock.size();
        conversions[position].conversionAbove = nearestConversionAbove(conversions[position].block);
        known.byBlock.insert(endOfBlocksUpTo(conversions[position].block) - known.byBlock.begin(), position);
    }
    const Int32OfNumber* first = nullptr;
    for (uint32_t position = nearestConversionAbove(block); position != none; position = conversions[position].conversionAbove) {
        if (serves(conversions[position]))
            first = &conversions[position];
    }
    return first;
}

// The conversion of a number has a branch to a call for what lies beyond int64, and B3 shares the arithmetic of two conversions but not their branches.
template<typename Functor>
Word32 OperatorEmitter::int32ConvertedOnce(const ValueContext& user, ValueID number, bool needsLowestBitsAlone, const Functor& convert)
{
    Int32sOfNumber& known = m_int32OfNumbers.add(number.index(), Int32sOfNumber()).iterator->value;
    if (const Int32OfNumber* earlier = findInt32ConvertedBefore(known, user.block(), needsLowestBitsAlone))
        return earlier->word;
    Word32 word = convert();
    known.conversions.append({ user.block(), word, needsLowestBitsAlone });
    return word;
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
    // What is combined by `&` with an integer that has its lowest bits alone gives no other bit of its own to the result.
    auto hasLowestBitsAlone = [&](ValueID id) {
        const Type& type = typeOf(id);
        return op == BitwiseOperator::And && type.isInteger() && type.integers.minimum >= 0 && type.integers.maximum < (int64_t { 1 } << ConversionEmitter::numberOfLowestBits);
    };
    auto numberToInt32 = [&](ValueID id, ValueID other) {
        bool needsLowestBitsAlone = hasLowestBitsAlone(other);
        ConversionEmitter::BitsOfInt32 bits = needsLowestBitsAlone ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All;
        switch (representationOf(id)) {
        case Representation::Int32:
            return in<Word32>(id);
        case Representation::Int64:
            return low32(in<Integer>(id));
        case Representation::OptionalInt64:
            static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));
            return low32(Integer(in<OptionalInteger>(id).value()));
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
        case Representation::OptionalDouble:
            return int32ConvertedOnce(value, id, needsLowestBitsAlone, [&] {
                return emitter<ConversionEmitter>().toInt32(asNumericOperand(id), bits);
            });
        case Representation::JSValue: {
            if (!typeOf(id).isNumber())
                break;
            Boxed boxed = in<Boxed>(id);
            // The way of an int32 is the likely one, and it is shortest where the other way holds a call.
            return int32ConvertedOnce(value, id, false, [&] {
                return either<Word32>(isInt32(boxed), [&] {
                    return unboxInt32(boxed);
                }, [&] {
                    return emitter<ConversionEmitter>().toInt32(unboxDouble(boxed));
                });
            });
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::LiteralString:
        case Representation::RawWord:
            break;
        }
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return word32(0);
    };

    switch (value->form()) {
    case OperatorForm::Integers: {
        Word32 x = in<Word32>(left);
        Word32 y = in<Word32>(right);
        return onWords(x, y);
    }
    case OperatorForm::Numbers: {
        Word32 x = numberToInt32(left, right);
        Word32 y = numberToInt32(right, left);
        return onWords(x, y);
    }
    case OperatorForm::BigInts:
        if (isUnsigned)
            break;
        return unboxCell(callOperation<Boxed>(value, traitsOfOperator(op).onBigInts, { boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)) }));
    case OperatorForm::Generic: {
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        Boxed result(callBinaryStub(value, traitsOfOperator(op).stub, x, y));
        // A result from 2^31 on comes back as a double.
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
