/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2025-2026 the V8 project authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3IntRange.h"

#if ENABLE(B3_JIT)

#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <wtf/DataLog.h>

namespace JSC { namespace B3 {

namespace B3IntRangeInternal {
static constexpr bool verbose = false;
}

#define DUMP_INT_RANGE_AND_RETURN(range)                           \
    do {                                                           \
        if (B3IntRangeInternal::verbose) [[unlikely]]              \
            dataLogLn("    IntRange for ", *value, " is ", range); \
        return range;                                              \
    } while (false);

static int32_t shiftAmountOf(Value* shift)
{
    return shift->child(1)->asInt32() & (shift->type() == Int32 ? 31 : 63);
}

static bool mayHaveOwnRange(Value* value)
{
    switch (value->opcode()) {
    case BitAnd:
    case SShr:
    case ZShr:
    case Shl:
    case Add:
    case Sub:
    case Mul:
    case SExt8:
    case SExt8To64:
    case SExt16:
    case SExt16To64:
    case SExt32:
    case ZExt32:
    case Trunc:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
        return true;
    default:
        return false;
    }
}

static bool mayHaveRange(const Procedure& proc, Value* value)
{
    UNUSED_PARAM(proc);
    return mayHaveOwnRange(value);
}

static IntRange rangeFor(const Procedure&, Value*, uint64_t timeToLive);

static IntRange computedRangeFor(const Procedure& proc, Value* value, uint64_t timeToLive)
{
    if (!timeToLive) {
        if (mayHaveOwnRange(value))
            proc.didReachLimit(Limit::MaxRangeAnalysisDepth, value->origin());
        DUMP_INT_RANGE_AND_RETURN(IntRange::top(value->type()));
    }

    switch (value->opcode()) {
    case Const32:
    case Const64: {
        int64_t intValue = value->asInt();
        DUMP_INT_RANGE_AND_RETURN(IntRange(intValue, intValue));
    }

    case BitAnd:
        if (value->child(1)->hasInt())
            DUMP_INT_RANGE_AND_RETURN(IntRange::rangeForMask(value->child(1)->asInt(), value->type()));
        break;

    case SShr:
        if (value->child(1)->hasInt32()) {
            DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).sShr(
                shiftAmountOf(value), value->type()));
        }
        break;

    case ZShr:
        if (value->child(1)->hasInt32()) {
            DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).zShr(
                shiftAmountOf(value), value->type()));
        }
        break;

    case Shl:
        if (value->child(1)->hasInt32()) {
            DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).shl(
                shiftAmountOf(value), value->type()));
        }
        break;

    case Add:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).add(
            rangeFor(proc, value->child(1), timeToLive - 1), value->type()));

    case Sub:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).sub(
            rangeFor(proc, value->child(1), timeToLive - 1), value->type()));

    case Mul:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).mul(
            rangeFor(proc, value->child(1), timeToLive - 1), value->type()));

    case SExt8:
    case SExt8To64:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).sExt<int8_t>());
    case SExt16:
    case SExt16To64:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).sExt<int16_t>());
    case SExt32:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).sExt<int32_t>());

    case ZExt32:
        DUMP_INT_RANGE_AND_RETURN(rangeFor(proc, value->child(0), timeToLive - 1).zExt32());

    case Trunc:
        if (value->child(0)->type() == Int64) {
            IntRange range = rangeFor(proc, value->child(0), timeToLive - 1);
            if (range.isSubsetOf(IntRange::top<int32_t>()))
                DUMP_INT_RANGE_AND_RETURN(range);
        }
        break;

    case Load8Z:
        DUMP_INT_RANGE_AND_RETURN(IntRange(0, std::numeric_limits<uint8_t>::max()));
    case Load8S:
        DUMP_INT_RANGE_AND_RETURN(IntRange(std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()));
    case Load16Z:
        DUMP_INT_RANGE_AND_RETURN(IntRange(0, std::numeric_limits<uint16_t>::max()));
    case Load16S:
        DUMP_INT_RANGE_AND_RETURN(IntRange(std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));

    default:
        break;
    }

    DUMP_INT_RANGE_AND_RETURN(IntRange::top(value->type()));
}

#undef DUMP_INT_RANGE_AND_RETURN

static IntRange rangeFor(const Procedure& proc, Value* value, uint64_t timeToLive)
{
    return computedRangeFor(proc, value, timeToLive);
}

IntRange rangeFor(const Procedure& proc, Value* value)
{
    return rangeFor(proc, value, proc.limit(Limit::MaxRangeAnalysisDepth));
}

template<typename T>
static IntegerInterval fullIntervalOf()
{
    return { std::numeric_limits<T>::min(), std::numeric_limits<T>::max() };
}

IntegerInterval fullInterval(Type type, Signedness signedness)
{
    bool isSigned = signedness == Signedness::Signed;
    switch (type.kind()) {
    case Int32:
        return isSigned ? fullIntervalOf<int32_t>() : fullIntervalOf<uint32_t>();
    case Int64:
        return isSigned ? fullIntervalOf<int64_t>() : fullIntervalOf<uint64_t>();
    case Void:
    case Float:
    case Double:
    case V128:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static IntegerInterval unsignedIntervalFor(const Procedure& proc, Value* value)
{
    IntegerInterval top = fullInterval(value->type(), Signedness::Unsigned);
    uint64_t mask = static_cast<uint64_t>(top.max);
    if (value->hasInt()) {
        uint64_t bits = static_cast<uint64_t>(value->asInt()) & mask;
        return { bits, bits };
    }

    uint64_t displacement = 0;
    if (value->opcode() == Add && value->child(1)->hasInt()) {
        displacement = static_cast<uint64_t>(value->child(1)->asInt());
        value = value->child(0);
    }
    if (!mayHaveRange(proc, value))
        return top;

    IntRange range = rangeFor(proc, value);
    uint64_t min = (static_cast<uint64_t>(range.min()) + displacement) & mask;
    uint64_t max = (static_cast<uint64_t>(range.max()) + displacement) & mask;
    if (min > max)
        return top;
    return { min, max };
}

IntegerInterval intervalFor(const Procedure& proc, Value* value, Signedness signedness)
{
    ASSERT(value->type().isInt());
    if (signedness == Signedness::Unsigned)
        return unsignedIntervalFor(proc, value);
    IntRange range = rangeFor(proc, value);
    return { range.min(), range.max() };
}

template<typename T>
static TriState evaluateLessThan(T leftMin, T leftMax, T rightMin, T rightMax)
{
    if (leftMax < rightMin)
        return TriState::True;
    if (leftMin >= rightMax)
        return TriState::False;
    return TriState::Indeterminate;
}

static TriState evaluateLessThan(const IntRange& left, const IntRange& right)
{
    return evaluateLessThan(left.min(), left.max(), right.min(), right.max());
}

static TriState evaluateLessThan(const IntegerInterval& left, const IntegerInterval& right)
{
    return evaluateLessThan(left.min, left.max, right.min, right.max);
}

TriState evaluateComparison(Opcode opcode, const IntRange& left, const IntRange& right)
{
    switch (opcode) {
    case Equal:
        if (!left.overlaps(right))
            return TriState::False;
        if (left.min() == left.max() && left == right)
            return TriState::True;
        return TriState::Indeterminate;
    case NotEqual:
        return invert(evaluateComparison(Equal, left, right));
    case LessThan:
        return evaluateLessThan(left, right);
    case GreaterThan:
        return evaluateLessThan(right, left);
    case LessEqual:
        return invert(evaluateLessThan(right, left));
    case GreaterEqual:
        return invert(evaluateLessThan(left, right));
    default:
        return TriState::Indeterminate;
    }
}

TriState evaluateComparison(Opcode opcode, const IntegerInterval& left, const IntegerInterval& right)
{
    switch (opcode) {
    case Equal:
        if (!left.overlaps(right))
            return TriState::False;
        if (left.min == left.max && left == right)
            return TriState::True;
        return TriState::Indeterminate;
    case NotEqual:
        return invert(evaluateComparison(Equal, left, right));
    case LessThan:
    case Below:
        return evaluateLessThan(left, right);
    case GreaterThan:
    case Above:
        return evaluateLessThan(right, left);
    case LessEqual:
    case BelowEqual:
        return invert(evaluateLessThan(right, left));
    case GreaterEqual:
    case AboveEqual:
        return invert(evaluateLessThan(left, right));
    default:
        return TriState::Indeterminate;
    }
}

TriState evaluateComparison(const Procedure& proc, Value* comparison)
{
    if (!isIntegerComparison(comparison->opcode()))
        return TriState::Indeterminate;

    Value* left = comparison->child(0);
    Value* right = comparison->child(1);
    if (!left->type().isInt())
        return TriState::Indeterminate;
    if (!mayHaveRange(proc, left) && !mayHaveRange(proc, right))
        return TriState::Indeterminate;

    Signedness signedness = isUnsignedRelation(comparison->opcode()) ? Signedness::Unsigned : Signedness::Signed;
    return evaluateComparison(comparison->opcode(), intervalFor(proc, left, signedness), intervalFor(proc, right, signedness));
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
