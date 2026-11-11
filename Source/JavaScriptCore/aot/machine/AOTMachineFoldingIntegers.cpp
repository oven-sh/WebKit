/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFolding.h"

#if ENABLE(AOT)

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned levelsOfChildrenReadForIntegerRange = 3;

static IntegerRange integerRangeKeptWithin(const IntegerRange& bounds, const IntegerRange& range)
{
    return bounds.contains(range) ? range : bounds;
}

static IntegerRange integerRangeOrWidest(Type type, CheckedInt64 min, CheckedInt64 max)
{
    IntegerRange widest = widestIntegerRangeOf(type);
    if (min.hasOverflowed() || max.hasOverflowed())
        return widest;
    return integerRangeKeptWithin(widest, { min.value(), max.value() });
}

static IntegerRange integerRangeOfProduct(Type type, const IntegerRange& left, const IntegerRange& right)
{
    std::array<CheckedInt64, 4> corners {
        CheckedInt64(left.min) * CheckedInt64(right.min),
        CheckedInt64(left.min) * CheckedInt64(right.max),
        CheckedInt64(left.max) * CheckedInt64(right.min),
        CheckedInt64(left.max) * CheckedInt64(right.max),
    };
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    for (const CheckedInt64& corner : corners) {
        if (corner.hasOverflowed())
            return widestIntegerRangeOf(type);
        min = std::min(min, corner.value());
        max = std::max(max, corner.value());
    }
    return integerRangeKeptWithin(widestIntegerRangeOf(type), { min, max });
}

static IntegerRange computeIntegerRange(const Operation*, unsigned levelsOfChildren);

static ALWAYS_INLINE IntegerRange integerRangeOf(const Operation* operation, unsigned levelsOfChildren)
{
    if (!hasFoldTrait(operation->opcode(), FoldTrait::BoundsIntegerRange))
        return widestIntegerRangeOf(operation->type());
    if (operation->hasInt())
        return { operation->asInt(), operation->asInt() };
    return computeIntegerRange(operation, levelsOfChildren);
}

static ALWAYS_INLINE IntegerRange computeIntegerRange(const Operation* operation, unsigned levelsOfChildren)
{
    Type type = operation->type();
    IntegerRange widest = widestIntegerRangeOf(type);
    unsigned widthInBits = 8 * sizeOf(type);

    auto rangeOfChild = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
        const Operation* child = operation->child(index);
        if (!levelsOfChildren && !child->hasInt())
            return widestIntegerRangeOf(child->type());
        return integerRangeOf(child, levelsOfChildren ? levelsOfChildren - 1 : 0);
    };

    switch (operation->opcode()) {
    case Const32:
    case Const64:
        return { operation->asInt(), operation->asInt() };

    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
        return { 0, 1 };

    case Load8Z:
        return { 0, std::numeric_limits<uint8_t>::max() };
    case Load8S:
        return { std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max() };
    case Load16Z:
        return { 0, std::numeric_limits<uint16_t>::max() };
    case Load16S:
        return { std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max() };

    case Clz:
    case PopCount:
        return { 0, 8 * sizeOf(operation->child(0)->type()) };

    case SExt8:
        return integerRangeKeptWithin({ std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max() }, rangeOfChild(0));
    case SExt16:
        return integerRangeKeptWithin({ std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max() }, rangeOfChild(0));
    case SExt32:
        return rangeOfChild(0);
    case Trunc:
        return integerRangeKeptWithin(widest, rangeOfChild(0));

    case ZExt32: {
        constexpr int64_t twoToThe32 = int64_t { 1 } << 32;
        IntegerRange value = rangeOfChild(0);
        if (value.min >= 0)
            return value;
        if (value.max < 0)
            return { value.min + twoToThe32, value.max + twoToThe32 };
        if (operation->has(OperationFlag::ExtendsWordThatIsNotNegative))
            return { 0, value.max };
        return { 0, std::numeric_limits<uint32_t>::max() };
    }

    case Add: {
        if (isDecodedReference(operation))
            return { int64_t { 1 } << 32, static_cast<int64_t>(bitsOfDecodedReference) };
        IntegerRange left = rangeOfChild(0);
        IntegerRange right = rangeOfChild(1);
        return integerRangeOrWidest(type, CheckedInt64(left.min) + CheckedInt64(right.min), CheckedInt64(left.max) + CheckedInt64(right.max));
    }

    case Sub: {
        IntegerRange left = rangeOfChild(0);
        IntegerRange right = rangeOfChild(1);
        return integerRangeOrWidest(type, CheckedInt64(left.min) - CheckedInt64(right.max), CheckedInt64(left.max) - CheckedInt64(right.min));
    }

    case Neg: {
        IntegerRange value = rangeOfChild(0);
        return integerRangeOrWidest(type, CheckedInt64(0) - CheckedInt64(value.max), CheckedInt64(0) - CheckedInt64(value.min));
    }

    case Mul:
        return integerRangeOfProduct(type, rangeOfChild(0), rangeOfChild(1));

    case BitAnd: {
        IntegerRange left = rangeOfChild(0);
        IntegerRange right = rangeOfChild(1);
        if (left.min >= 0 && right.min >= 0)
            return { 0, std::min(left.max, right.max) };
        if (left.min >= 0)
            return { 0, left.max };
        if (right.min >= 0)
            return { 0, right.max };
        break;
    }

    case BitOr:
    case BitXor: {
        IntegerRange left = rangeOfChild(0);
        IntegerRange right = rangeOfChild(1);
        if (left.min < 0 || right.min < 0)
            break;
        uint64_t highestBits = static_cast<uint64_t>(left.max | right.max);
        if (!highestBits)
            return { 0, 0 };
        return { 0, static_cast<int64_t>(std::numeric_limits<uint64_t>::max() >> std::countl_zero(highestBits)) };
    }

    case Shl:
    case SShr:
    case ZShr: {
        const Operation* amountOperand = operation->child(1);
        if (!amountOperand->hasInt32())
            break;
        unsigned amount = static_cast<uint32_t>(amountOperand->asInt32()) & (widthInBits - 1);
        IntegerRange value = rangeOfChild(0);
        if (!amount)
            return value;
        if (operation->opcode() == Shl) {
            int64_t min = static_cast<int64_t>(static_cast<uint64_t>(value.min) << amount);
            int64_t max = static_cast<int64_t>(static_cast<uint64_t>(value.max) << amount);
            if ((min >> amount) != value.min || (max >> amount) != value.max)
                break;
            return integerRangeKeptWithin(widest, { min, max });
        }
        if (operation->opcode() == ZShr && value.min < 0)
            return { 0, static_cast<int64_t>(JSC::mask(widthOf(type)) >> amount) };
        return { value.min >> amount, value.max >> amount };
    }

    case Select: {
        IntegerRange whenNotZero = rangeOfChild(1);
        IntegerRange whenZero = rangeOfChild(2);
        return { std::min(whenNotZero.min, whenZero.min), std::max(whenNotZero.max, whenZero.max) };
    }

    default:
        break;
    }
    return widest;
}

IntegerRange rangeBoundedByOpcode(const Operation* operation)
{
    IntegerRange range = integerRangeOf(operation, levelsOfChildrenReadForIntegerRange);
    ASSERT(range.min <= range.max);
    ASSERT(widestIntegerRangeOf(operation->type()).contains(range));
    return range;
}

namespace {

class IntegerArithmeticFolder {
public:
    IntegerArithmeticFolder(Builder& builder, Type type)
        : m_builder(builder)
        , m_type(type)
    {
    }

    Operation* foldAdd(Operation* left, Operation* right)
    {
        auto addsConstant = [](const Operation* sum) {
            return sum->opcode() == Add && sum->child(1)->hasInt() && !sum->isNarrowReference();
        };

        if (right->hasInt()) {
            if (left->hasInt())
                return constant(bitsOf(left) + bitsOf(right));
            if (right->isInt(0))
                return left;
            if (addsConstant(left))
                return m_builder.binary(Add, left->child(0), constant(bitsOf(left->child(1)) + bitsOf(right)));
            if (left->opcode() == Neg && right->isInt(-1))
                return m_builder.binary(BitXor, left->child(0), right);
            return nullptr;
        }

        if (left == right)
            return m_builder.binary(Shl, left, m_builder.const32(1));
        if (right->opcode() == Neg)
            return m_builder.binary(Sub, left, right->child(0));
        if (left->opcode() == Neg)
            return m_builder.binary(Sub, right, left->child(0));
        if (left->opcode() == Sub && left->child(1) == right)
            return left->child(0);
        if (right->opcode() == Sub && right->child(1) == left)
            return right->child(0);

        if (m_type != Int32)
            return nullptr;
        if (addsConstant(left) && addsConstant(right))
            return m_builder.binary(Add, m_builder.binary(Add, left->child(0), right->child(0)), constant(bitsOf(left->child(1)) + bitsOf(right->child(1))));
        if (addsConstant(left))
            return m_builder.binary(Add, m_builder.binary(Add, left->child(0), right), left->child(1));
        if (addsConstant(right))
            return m_builder.binary(Add, m_builder.binary(Add, left, right->child(0)), right->child(1));
        return nullptr;
    }

    Operation* foldSub(Operation* left, Operation* right)
    {
        if (right->hasInt()) {
            if (left->hasInt())
                return constant(bitsOf(left) - bitsOf(right));
            if (right->isInt(0))
                return left;
            return m_builder.binary(Add, left, constant(0 - bitsOf(right)));
        }

        if (left == right)
            return constant(0);
        if (left->isInt(0))
            return m_builder.unary(Neg, right);
        if (right->opcode() == Neg)
            return m_builder.binary(Add, left, right->child(0));
        if (left->opcode() == Add) {
            if (left->child(1) == right)
                return left->child(0);
            if (left->child(0) == right)
                return left->child(1);
        }
        if (right->opcode() == Sub && right->child(0) == left)
            return right->child(1);
        return nullptr;
    }

    Operation* foldNeg(Operation* value)
    {
        if (value->hasInt())
            return constant(0 - bitsOf(value));
        if (value->opcode() == Neg)
            return value->child(0);
        if (value->opcode() == Sub)
            return m_builder.binary(Sub, value->child(1), value->child(0));
        return nullptr;
    }

    Operation* foldMul(Operation* left, Operation* right)
    {
        if (!right->hasInt())
            return nullptr;
        if (left->hasInt())
            return constant(bitsOf(left) * bitsOf(right));
        if (right->isInt(0))
            return right;
        if (right->isInt(1))
            return left;
        if (right->isInt(-1))
            return m_builder.unary(Neg, left);
        uint64_t factor = unsignedValueOf(right);
        if (std::has_single_bit(factor))
            return m_builder.binary(Shl, left, shiftAmountFor(factor));
        return nullptr;
    }

    Operation* foldMulHigh(Opcode opcode, Operation* left, Operation* right)
    {
        if (!left->hasInt() || !right->hasInt())
            return nullptr;
        int widthInBits = static_cast<int>(8 * sizeOf(m_type));
        if (opcode == MulHigh) {
            Int128 product = static_cast<Int128>(left->asInt()) * static_cast<Int128>(right->asInt());
            return constant(static_cast<uint64_t>(static_cast<int64_t>(product >> widthInBits)));
        }
        UInt128 product = static_cast<UInt128>(unsignedValueOf(left)) * static_cast<UInt128>(unsignedValueOf(right));
        return constant(static_cast<uint64_t>(product >> widthInBits));
    }

    Operation* foldSignedDivision(Opcode opcode, Operation* dividend, Operation* divisor, bool isChill)
    {
        bool isQuotient = opcode == Div;
        if (divisor->hasInt()) {
            int64_t by = divisor->asInt();
            if (!by)
                return isChill ? divisor : nullptr;
            if (by == 1)
                return isQuotient ? dividend : constant(0);
            if (by == -1) {
                if (!isChill && dividend->isInt(widestIntegerRangeOf(m_type).min))
                    return nullptr;
                return isQuotient ? m_builder.unary(Neg, dividend) : constant(0);
            }
            if (dividend->hasInt())
                return constant(static_cast<uint64_t>(isQuotient ? dividend->asInt() / by : dividend->asInt() % by));
            if (by > 0 && std::has_single_bit(static_cast<uint64_t>(by)) && rangeOf(dividend).min >= 0)
                return m_builder.binary(isQuotient ? UDiv : UMod, dividend, divisor);
        }

        if (isChill && divisionNeverTraps(dividend, divisor))
            return m_builder.binary(opcode, dividend, divisor);
        return nullptr;
    }

    Operation* foldUnsignedDivision(Opcode opcode, Operation* dividend, Operation* divisor)
    {
        bool isQuotient = opcode == UDiv;
        if (!divisor->hasInt())
            return nullptr;
        uint64_t by = unsignedValueOf(divisor);
        if (!by)
            return nullptr;
        if (by == 1)
            return isQuotient ? dividend : constant(0);
        if (dividend->hasInt())
            return constant(isQuotient ? unsignedValueOf(dividend) / by : unsignedValueOf(dividend) % by);
        if (!std::has_single_bit(by))
            return nullptr;
        if (isQuotient)
            return m_builder.binary(ZShr, dividend, shiftAmountFor(by));
        return m_builder.binary(BitAnd, dividend, constant(by - 1));
    }

private:
    Operation* constant(uint64_t bits) { return m_builder.constantOfType(m_type, static_cast<int64_t>(bits)); }
    static uint64_t bitsOf(const Operation* operation) { return static_cast<uint64_t>(operation->asInt()); }
    uint64_t unsignedValueOf(const Operation* operation) const { return bitsOf(operation) & JSC::mask(widthOf(m_type)); }
    Operation* shiftAmountFor(uint64_t powerOfTwo) { return m_builder.const32(std::countr_zero(powerOfTwo)); }

    bool divisionNeverTraps(const Operation* dividend, const Operation* divisor) const
    {
        IntegerRange divisors = rangeOf(divisor);
        if (divisors.min <= 0 && divisors.max >= 0)
            return false;
        if (divisors.min > -1 || divisors.max < -1)
            return true;
        return rangeOf(dividend).min > widestIntegerRangeOf(m_type).min;
    }

    Builder& m_builder;
    Type m_type;
};

} // anonymous namespace

Operation* foldIntegerArithmetic(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull, bool isChill)
{
    ASSERT(type.isInt());
    ASSERT(left->type() == type);
    ASSERT(!rightOrNull == (opcode == Neg));
    ASSERT(!rightOrNull || rightOrNull->type() == type);
    ASSERT(!isChill || opcode == Div || opcode == Mod);
    ASSERT(!isCommutative(opcode) || !left->hasInt() || rightOrNull->hasInt());

    IntegerArithmeticFolder folder(builder, type);
    switch (opcode) {
    case Add:
        return folder.foldAdd(left, rightOrNull);
    case Sub:
        return folder.foldSub(left, rightOrNull);
    case Neg:
        return folder.foldNeg(left);
    case Mul:
        return folder.foldMul(left, rightOrNull);
    case MulHigh:
    case UMulHigh:
        return folder.foldMulHigh(opcode, left, rightOrNull);
    case Div:
    case Mod:
        return folder.foldSignedDivision(opcode, left, rightOrNull, isChill);
    case UDiv:
    case UMod:
        return folder.foldUnsignedDivision(opcode, left, rightOrNull);

    default:
        break;
    }
    ASSERT_NOT_REACHED();
    return nullptr;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
