/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFolding.h"

#if ENABLE(AOT)

#include "AOTMachineTarget.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Machine {

static constexpr int bitFoldLeastCostOfLowWord = std::numeric_limits<int>::min() / 2;

static int32_t bitFoldWidth(Type type)
{
    ASSERT(type.isInt());
    return type == Int32 ? 32 : 64;
}

static uint64_t bitFoldPossibleBits(const Operation* value)
{
    if (isBoolean(value))
        return 1;
    IntegerRange range = rangeOf(value);
    if (range.min < 0)
        return std::numeric_limits<uint64_t>::max();
    if (!range.max)
        return 0;
    return std::numeric_limits<uint64_t>::max() >> std::countl_zero(static_cast<uint64_t>(range.max));
}

static bool bitFoldIsSignExtended(const Operation* value, unsigned fromBits)
{
    ASSERT(fromBits == 8 || fromBits == 16);
    Opcode opcode = value->opcode();
    if (opcode == SExt8 || opcode == Load8S)
        return true;
    if (opcode == SExt16 || opcode == Load16S)
        return fromBits == 16;
    int64_t limit = int64_t { 1 } << (fromBits - 1);
    return IntegerRange { -limit, limit - 1 }.contains(rangeOf(value));
}

static Operation* bitFoldMaskWithImmediate(Builder& builder, Type type, Operation* value, uint64_t mask)
{
    int64_t bits = type == Int32 ? static_cast<int32_t>(mask) : static_cast<int64_t>(mask);
    if (!(bitFoldPossibleBits(value) & ~static_cast<uint64_t>(bits)))
        return value;
    if (!isEncodableBitImmediate(widthOf(type), bits))
        return nullptr;
    return builder.binary(BitAnd, value, builder.constantOfType(type, bits));
}

Operation* foldMachineBitAndOfBoundedValue(Builder& builder, Type type, Operation* left, Operation* right)
{
    int64_t mask = right->asInt();
    Opcode leftOpcode = left->opcode();
    ASSERT(left != right && !left->hasInt() && mask && mask != -1 && hasFoldTrait(leftOpcode, FoldTrait::BoundsIntegerRange));
    if (leftOpcode == ZExt32 && (mask & bitFoldLowWord) == bitFoldLowWord)
        return left;

    uint64_t possibleBits = bitFoldPossibleBits(left);
    if (!(possibleBits & ~static_cast<uint64_t>(mask)))
        return left;
    if (!(possibleBits & static_cast<uint64_t>(mask)))
        return builder.constantOfType(type, 0);

    if (leftOpcode == BitAnd && left->child(1)->hasInt())
        return builder.binary(BitAnd, left->child(0), builder.constantOfType(type, left->child(1)->asInt() & mask));

    if ((leftOpcode == BitOr || leftOpcode == BitXor) && left->child(1)->hasInt() && !(left->child(1)->asInt() & mask))
        return builder.binary(BitAnd, left->child(0), right);

    if (leftOpcode == SExt32 && mask == bitFoldLowWord)
        return builder.unary(ZExt32, left->child(0));

    if (leftOpcode == SExt8 && !(mask & ~int64_t { 0xff }))
        return builder.binary(BitAnd, left->child(0), right);
    if (leftOpcode == SExt16 && !(mask & ~int64_t { 0xffff }))
        return builder.binary(BitAnd, left->child(0), right);

    return nullptr;
}

Operation* foldMachineBitOrOfBoundedValue(Builder& builder, Type type, Operation* left, Operation* right)
{
    int64_t bits = right->asInt();
    ASSERT(left != right && !left->hasInt() && bits && bits != -1 && hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange));
    if (!(bitFoldPossibleBits(left) & ~static_cast<uint64_t>(bits)))
        return right;

    if (left->opcode() == BitOr && left->child(1)->hasInt())
        return builder.binary(BitOr, left->child(0), builder.constantOfType(type, left->child(1)->asInt() | bits));

    return nullptr;
}

static int64_t bitFoldShiftConstant(Opcode opcode, Type type, int64_t value, int32_t amount)
{
    if (type == Int32) {
        if (opcode == Shl)
            return static_cast<int32_t>(static_cast<uint32_t>(value) << amount);
        if (opcode == ZShr)
            return static_cast<int32_t>(static_cast<uint32_t>(value) >> amount);
        return static_cast<int32_t>(value) >> amount;
    }
    if (opcode == Shl)
        return static_cast<int64_t>(static_cast<uint64_t>(value) << amount);
    if (opcode == ZShr)
        return static_cast<int64_t>(static_cast<uint64_t>(value) >> amount);
    return value >> amount;
}

static Operation* foldMachineShiftOfShift(Builder& builder, Opcode opcode, Type type, Operation* inner, int32_t amount)
{
    int32_t width = bitFoldWidth(type);
    int32_t innerAmount = inner->child(1)->asInt32() & (width - 1);
    Operation* value = inner->child(0);

    if (inner->opcode() == opcode) {
        if (innerAmount + amount < width)
            return builder.binary(opcode, value, builder.const32(innerAmount + amount));
        if (opcode == SShr)
            return builder.binary(SShr, value, builder.const32(width - 1));
        return builder.constantOfType(type, 0);
    }

    if (innerAmount != amount)
        return nullptr;

    uint64_t everyBit = std::numeric_limits<uint64_t>::max() >> (64 - width);
    if (opcode == Shl)
        return bitFoldMaskWithImmediate(builder, type, value, everyBit << amount);
    if (inner->opcode() != Shl)
        return nullptr;

    if (opcode == SShr) {
        if (type == Int32 && amount == 24)
            return builder.unary(SExt8, value);
        if (type == Int32 && amount == 16)
            return builder.unary(SExt16, value);
        if (type == Int64 && amount == 32)
            return builder.unary(SExt32, builder.unary(Trunc, value));
        return nullptr;
    }

    if (type == Int64 && amount == 32)
        return builder.unary(ZExt32, builder.unary(Trunc, value));
    return bitFoldMaskWithImmediate(builder, type, value, everyBit >> amount);
}

static Operation* foldMachineShift(Builder& builder, Opcode opcode, Type type, Operation* value, Operation* amountOperand)
{
    if (value->isInt(0) || (opcode == SShr && value->isInt(-1)))
        return value;

    int32_t amountMask = bitFoldWidth(type) - 1;
    if (!amountOperand->hasInt32()) {
        if (amountOperand->opcode() == BitAnd && amountOperand->child(1)->hasInt32() && (amountOperand->child(1)->asInt32() & amountMask) == amountMask)
            return builder.binary(opcode, value, amountOperand->child(0));
        return nullptr;
    }

    int32_t amount = amountOperand->asInt32() & amountMask;
    if (amount != amountOperand->asInt32())
        return builder.binary(opcode, value, builder.const32(amount));
    if (!amount)
        return value;
    if (value->hasInt())
        return builder.constantOfType(type, bitFoldShiftConstant(opcode, type, value->asInt(), amount));

    Opcode innerOpcode = value->opcode();
    if ((innerOpcode == Shl || innerOpcode == SShr || innerOpcode == ZShr) && value->child(1)->hasInt32())
        return foldMachineShiftOfShift(builder, opcode, type, value, amount);

    return nullptr;
}

static Operation* foldMachineBitCount(Builder& builder, Opcode opcode, Type type, Operation* operand)
{
    if (operand->hasInt32()) {
        uint32_t bits = static_cast<uint32_t>(operand->asInt32());
        return builder.constantOfType(type, opcode == Clz ? std::countl_zero(bits) : std::popcount(bits));
    }
    if (operand->hasInt64()) {
        uint64_t bits = static_cast<uint64_t>(operand->asInt64());
        return builder.constantOfType(type, opcode == Clz ? std::countl_zero(bits) : std::popcount(bits));
    }
    return nullptr;
}

Operation* foldMachineRotationOutOfShifts(Builder& builder, Type type, Operation* left, Operation* right)
{
    if (left->opcode() == ZShr)
        std::swap(left, right);
    if (left->opcode() != Shl || right->opcode() != ZShr || left->child(0) != right->child(0) || !left->child(1)->hasInt32() || !right->child(1)->hasInt32())
        return nullptr;
    int32_t width = bitFoldWidth(type);
    int32_t toLeft = left->child(1)->asInt32() & (width - 1);
    int32_t toRight = right->child(1)->asInt32() & (width - 1);
    if (!toLeft || toLeft + toRight != width)
        return nullptr;
    return builder.binary(RotR, left->child(0), builder.const32(toRight));
}

static Operation* foldMachineRotation(Builder& builder, Type type, Operation* value, Operation* amountOperand)
{
    RELEASE_ASSERT(amountOperand->hasInt32());
    int32_t amount = amountOperand->asInt32() & (bitFoldWidth(type) - 1);
    if (amount != amountOperand->asInt32())
        return builder.binary(RotR, value, builder.const32(amount));
    if (!amount)
        return value;
    if (value->hasInt32())
        return builder.const32(static_cast<int32_t>(std::rotr(static_cast<uint32_t>(value->asInt32()), amount)));
    if (value->hasInt64())
        return builder.const64(static_cast<int64_t>(std::rotr(static_cast<uint64_t>(value->asInt64()), amount)));
    return nullptr;
}

Operation* foldMachineShiftOrBitCount(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull)
{
    if (opcode == RotR)
        return foldMachineRotation(builder, type, left, rightOrNull);
    if (opcode == Shl || opcode == SShr || opcode == ZShr)
        return foldMachineShift(builder, opcode, type, left, rightOrNull);
    if (opcode == Clz || opcode == PopCount)
        return foldMachineBitCount(builder, opcode, type, left);
    RELEASE_ASSERT_NOT_REACHED();
}

static Operation* foldMachineNarrowSignExtension(Builder& builder, Opcode opcode, Operation* operand)
{
    unsigned fromBits = opcode == SExt8 ? 8 : 16;

    if (operand->hasInt32()) {
        int32_t value = operand->asInt32();
        return builder.const32(opcode == SExt8 ? static_cast<int8_t>(value) : static_cast<int16_t>(value));
    }

    if (bitFoldIsSignExtended(operand, fromBits))
        return operand;

    if (opcode == SExt8 && operand->opcode() == SExt16)
        return builder.unary(SExt8, operand->child(0));

    if (operand->opcode() == BitAnd && operand->child(1)->hasInt32()) {
        int32_t mask = operand->child(1)->asInt32();
        int32_t bitsRead = (1 << fromBits) - 1;
        int32_t signBit = 1 << (fromBits - 1);
        if ((mask & bitsRead) == bitsRead)
            return builder.unary(opcode, operand->child(0));
        if (!(mask & signBit))
            return builder.binary(BitAnd, operand->child(0), builder.const32(mask & bitsRead));
    }

    return nullptr;
}

static unsigned bitFoldNumberOfLowWordsRead(const Operation* wide)
{
    Opcode opcode = wide->opcode();
    if (opcode == Add || opcode == Sub || opcode == Mul || opcode == BitAnd || opcode == BitOr || opcode == BitXor)
        return 2;
    if (opcode == Neg)
        return 1;
    if (opcode == Shl && wide->child(1)->hasInt32() && !(wide->child(1)->asInt32() & ~31))
        return 1;
    return 0;
}

template<typename Answer, typename IsAskedAbout, typename AnswerFor>
static Answer bitFoldAnswerOperandsFirst(Operation* root, UncheckedKeyHashMap<Operation*, Answer>& answers, const IsAskedAbout& isAskedAbout, const AnswerFor& answerFor)
{
    Vector<Operation*, 16> pending;
    pending.append(root);
    for (;;) {
        Operation* wide = pending.last();
        if (answers.contains(wide)) {
            pending.removeLast();
            continue;
        }
        size_t sizeBefore = pending.size();
        for (unsigned index = bitFoldNumberOfLowWordsRead(wide); index--;) {
            Operation* operand = wide->child(index);
            if (isAskedAbout(operand) && !answers.contains(operand))
                pending.append(operand);
        }
        if (pending.size() != sizeBefore)
            continue;
        pending.removeLast();
        if (pending.isEmpty())
            return answerFor(wide);
        answers.add(wide, answerFor(wide));
    }
}

Operation* foldLowWord(Builder& builder, Operation* root)
{
    ASSERT(root->type() == Int64);
    auto readsLowWords = [](const Operation* wide) {
        return !!bitFoldNumberOfLowWordsRead(wide);
    };
    if (!readsLowWords(root))
        return nullptr;

    UncheckedKeyHashMap<Operation*, int> costs;
    auto costOf = [&](Operation* wide) {
        Opcode opcode = wide->opcode();
        if (opcode == SExt32 || opcode == ZExt32)
            return -1;
        if (wide->hasInt64())
            return 0;
        return readsLowWords(wide) ? costs.get(wide) : 1;
    };
    int costOfRoot = bitFoldAnswerOperandsFirst<int>(root, costs, readsLowWords, [&](Operation* wide) {
        int cost = 0;
        for (unsigned index = bitFoldNumberOfLowWordsRead(wide); index--;)
            cost += costOf(wide->child(index));
        return std::clamp(cost, bitFoldLeastCostOfLowWord, 1);
    });
    if (costOfRoot > 0)
        return nullptr;

    UncheckedKeyHashMap<Operation*, Operation*> lowWords;
    auto isMadeOfLowWords = [&](Operation* wide) {
        return readsLowWords(wide) && costs.get(wide) <= 0;
    };
    auto lowWordOf = [&](Operation* wide) {
        return isMadeOfLowWords(wide) ? lowWords.get(wide) : builder.unary(Trunc, wide);
    };
    return bitFoldAnswerOperandsFirst<Operation*>(root, lowWords, isMadeOfLowWords, [&](Operation* wide) {
        Opcode opcode = wide->opcode();
        Operation* left = lowWordOf(wide->child(0));
        if (opcode == Neg)
            return builder.unary(Neg, left);
        if (opcode == Shl)
            return builder.binary(Shl, left, wide->child(1));
        return builder.binary(opcode, left, lowWordOf(wide->child(1)));
    });
}

static Operation* foldMachineIntegerToDouble(Builder& builder, Operation* operand)
{
    if (operand->hasInt())
        return builder.constDouble(static_cast<double>(operand->asInt()));

    if (operand->opcode() == SExt32)
        return builder.unary(IToD, operand->child(0));
    if (operand->opcode() == ZExt32 && rangeOf(operand->child(0)).min >= 0)
        return builder.unary(IToD, operand->child(0));

    return nullptr;
}

static Operation* foldMachineBitwiseCast(Builder& builder, Type result, Operation* operand)
{
    if (operand->type() == result)
        return operand;
    if (operand->opcode() == BitwiseCast && operand->child(0)->type() == result)
        return operand->child(0);
    if (!operand->isConstant())
        return nullptr;

    switch (result.kind()) {
    case Int32:
        return operand->hasFloat() ? builder.const32(static_cast<int32_t>(operand->constantBits())) : nullptr;
    case Int64:
        return operand->hasDouble() ? builder.const64(operand->constantBits()) : nullptr;
    case Float:
        return operand->hasInt32() ? builder.constFloat(std::bit_cast<float>(operand->asInt32())) : nullptr;
    case Double:
        return operand->hasInt64() ? builder.constDouble(std::bit_cast<double>(operand->asInt64())) : nullptr;
    case Void:
    case Tuple:
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Operation* singleThatDoubleIs(Builder& builder, Operation* number)
{
    if (number->opcode() == FloatToDouble)
        return number->child(0);
    if (!number->hasDouble() || !(std::abs(number->asDouble()) <= std::numeric_limits<float>::max()))
        return nullptr;
    if (std::bit_cast<uint64_t>(static_cast<double>(static_cast<float>(number->asDouble()))) == std::bit_cast<uint64_t>(number->asDouble()))
        return builder.constFloat(static_cast<float>(number->asDouble()));
    return nullptr;
}

static Operation* foldMachineOperatorOnSinglesRoundedToSingle(Builder& builder, Operation* rounded)
{
    static_assert(std::numeric_limits<double>::digits >= 2 * std::numeric_limits<float>::digits + 2);
    switch (rounded->opcode()) {
    case Add:
    case Sub:
    case Mul:
    case Div: {
        if (rounded->child(0)->hasDouble() && rounded->child(1)->hasDouble())
            return nullptr;
        Operation* left = singleThatDoubleIs(builder, rounded->child(0));
        Operation* right = left ? singleThatDoubleIs(builder, rounded->child(1)) : nullptr;
        return right ? builder.binary(rounded->opcode(), left, right) : nullptr;
    }
    case Sqrt: {
        Operation* taken = rounded->child(0)->hasDouble() ? nullptr : singleThatDoubleIs(builder, rounded->child(0));
        return taken ? builder.unary(Sqrt, taken) : nullptr;
    }
    default:
        return nullptr;
    }
}

Operation* foldConversionOtherThanOfWords(Builder& builder, Opcode opcode, Type result, Operation* operand)
{
    if (opcode == SExt8 || opcode == SExt16)
        return foldMachineNarrowSignExtension(builder, opcode, operand);
    if (opcode == IToD)
        return foldMachineIntegerToDouble(builder, operand);
    if (opcode == BitwiseCast)
        return foldMachineBitwiseCast(builder, result, operand);

    if (opcode == FloatToDouble)
        return operand->hasFloat() ? builder.constDouble(static_cast<double>(operand->asFloat())) : nullptr;

    if (opcode == DoubleToFloat) {
        if (operand->hasDouble())
            return builder.constFloat(static_cast<float>(operand->asDouble()));
        if (operand->opcode() == FloatToDouble)
            return operand->child(0);
        return foldMachineOperatorOnSinglesRoundedToSingle(builder, operand);
    }

    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
