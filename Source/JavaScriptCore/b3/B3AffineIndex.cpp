/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3AffineIndex.h"

#if ENABLE(B3_JIT)

#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace B3 {

CheckedRange checkedAdd(const CheckedRange& left, const CheckedRange& right)
{
    if (!left || !right)
        return std::nullopt;
    CheckedInt64 min = CheckedInt64(left->min()) + right->min();
    CheckedInt64 max = CheckedInt64(left->max()) + right->max();
    if (min.hasOverflowed() || max.hasOverflowed())
        return std::nullopt;
    return IntRange(min.value(), max.value());
}

CheckedRange checkedMultiply(const CheckedRange& left, const CheckedRange& right)
{
    if (!left || !right)
        return std::nullopt;
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    for (int64_t leftEnd : { left->min(), left->max() }) {
        for (int64_t rightEnd : { right->min(), right->max() }) {
            CheckedInt64 product = CheckedInt64(leftEnd) * rightEnd;
            if (product.hasOverflowed())
                return std::nullopt;
            min = std::min(min, product.value());
            max = std::max(max, product.value());
        }
    }
    return IntRange(min, max);
}

CheckedRange checkedRangeOf(const Procedure& proc, Value* value, Signedness signedness)
{
    if (signedness == Signedness::Signed)
        return rangeFor(proc, value);
    IntegerInterval interval = intervalFor(proc, value, signedness);
    if (interval.max > std::numeric_limits<int64_t>::max())
        return std::nullopt;
    return IntRange(static_cast<int64_t>(interval.min), static_cast<int64_t>(interval.max));
}

CheckedRange checkedRangeOf(const Procedure& proc, const InvariantSum& sum)
{
    CheckedRange result = IntRange(sum.constant, sum.constant);
#pragma clang loop unroll(disable)
    for (const InvariantTerm& term : sum.usedTerms()) {
        CheckedRange product = IntRange(term.coefficient, term.coefficient);
        product = checkedMultiply(product, rangeFor(proc, term.first));
        if (term.second)
            product = checkedMultiply(product, rangeFor(proc, term.second));
        result = checkedAdd(result, product);
    }
    return result;
}

static NEVER_INLINE bool hasSameTerms(const InvariantSum& left, const InvariantSum& right)
{
    return equalSpans(left.usedTerms(), right.usedTerms());
}

bool isSameExceptConstantOffset(const AffineIndex& left, const AffineIndex& right)
{
    return left.scale.constant == right.scale.constant && hasSameTerms(left.scale, right.scale) && hasSameTerms(left.offset, right.offset);
}

namespace {

bool multiplyBy(int64_t& value, int64_t factor)
{
    CheckedInt64 product = CheckedInt64(value) * factor;
    if (product.hasOverflowed())
        return false;
    value = product.value();
    return true;
}

bool increaseBy(int64_t& value, int64_t addend)
{
    CheckedInt64 sum = CheckedInt64(value) + addend;
    if (sum.hasOverflowed())
        return false;
    value = sum.value();
    return true;
}

bool addTerm(InvariantSum& sum, int64_t coefficient, Value* first, Value* second)
{
    if (!first)
        std::swap(first, second);
    if (!first)
        return increaseBy(sum.constant, coefficient);
    if (!coefficient)
        return true;
    if (second && second->index() < first->index())
        std::swap(first, second);
    auto rank = [](Value* first, Value* second) {
        return static_cast<uint64_t>(first->index()) << 32 | (second ? second->index() + 1 : 0);
    };
    unsigned index = 0;
#pragma clang loop unroll(disable)
    while (index < sum.numberOfTerms && rank(sum.terms[index].first, sum.terms[index].second) < rank(first, second))
        ++index;
    auto& terms = sum.terms;
    if (index < sum.numberOfTerms && terms[index].first == first && terms[index].second == second) {
        if (!increaseBy(terms[index].coefficient, coefficient))
            return false;
        if (terms[index].coefficient)
            return true;
        memmoveSpan(std::span { terms }.subspan(index), sum.usedTerms().subspan(index + 1));
        --sum.numberOfTerms;
        return true;
    }
    if (sum.numberOfTerms == terms.size())
        return false;
    memmoveSpan(std::span { terms }.subspan(index + 1), sum.usedTerms().subspan(index));
    terms[index] = { coefficient, first, second };
    ++sum.numberOfTerms;
    return true;
}

bool addScaled(InvariantSum& result, const InvariantSum& sum, int64_t coefficient, Value* factor)
{
    int64_t constant = sum.constant;
    if (!multiplyBy(constant, coefficient) || !addTerm(result, constant, factor, nullptr))
        return false;
#pragma clang loop unroll(disable)
    for (InvariantTerm term : sum.usedTerms()) {
        if ((term.second && factor) || !multiplyBy(term.coefficient, coefficient) || !addTerm(result, term.coefficient, term.first, term.second ? term.second : factor))
            return false;
    }
    return true;
}

class AffineIndexFinder {
public:
    explicit AffineIndexFinder(const AffineIndexInputs& inputs)
        : m_inputs(inputs)
    {
    }

    // Adds coefficient * factor * value to the result.
    bool add(AffineIndex& result, Value* value, int64_t coefficient, Value* factor, uint64_t depth)
    {
        value = value->foldIdentity();
        if (!value->type().isInt())
            return false;
        if (value == m_inputs.counter)
            return addTerm(result.scale, coefficient, factor, nullptr);
        if (value->hasInt())
            return multiplyBy(coefficient, value->asInt()) && addTerm(result.offset, coefficient, factor, nullptr);
        if (isExtension(value)) {
            AffineIndex saved = result;
            if (addOperands(result, value, coefficient, factor, depth))
                return true;
            result = saved;
        }
        if (Value* invariant = m_inputs.canonicalInvariant(value))
            return addTerm(result.offset, coefficient, invariant, factor);
        return addOperands(result, value, coefficient, factor, depth);
    }

private:
    static bool isExtension(Value* value) { return value->opcode() == SExt32 || value->opcode() == ZExt32; }

    NEVER_INLINE Value* invariantFactor(Value* value)
    {
        value = value->foldIdentity();
        if (isExtension(value)) {
            Value* operand = m_inputs.canonicalInvariant(value->child(0)->foldIdentity());
            if (operand && (value->opcode() == SExt32 || rangeFor(m_inputs.proc, operand).min() >= 0))
                return operand;
        }
        return m_inputs.canonicalInvariant(value);
    }

    bool addOperands(AffineIndex& result, Value* value, int64_t coefficient, Value* factor, uint64_t depth)
    {
        const Procedure& proc = m_inputs.proc;
        if (depth == proc.limit(Limit::MaxRangeAnalysisDepth)) {
            proc.didReachLimit(Limit::MaxRangeAnalysisDepth, value->origin());
            return false;
        }
        ++depth;
        if (!value->numChildren() || value->numChildren() > 2)
            return false;
        Value* operand = value->child(0);
        Value* right = value->numChildren() == 2 ? value->child(1) : nullptr;
        int64_t multiplier = 1;
        switch (value->opcode()) {
        case Add:
        case Sub:
            if (!add(result, operand, coefficient, factor, depth))
                return false;
            operand = right;
            if (value->opcode() == Sub)
                multiplier = -1;
            break;
        case Neg:
            multiplier = -1;
            break;
        case Trunc:
            break;
        case Shl: {
            if (!right->hasInt32())
                return false;
            unsigned shift = right->asInt32() & (value->type() == Int32 ? 31 : 63);
            if (shift == 63)
                return false;
            multiplier = int64_t { 1 } << shift;
            break;
        }
        case Mul: {
            if (operand->foldIdentity()->hasInt())
                std::swap(operand, right);
            if (Value* constant = right->foldIdentity(); constant->hasInt()) {
                multiplier = constant->asInt();
                break;
            }
            if (factor)
                return false;
            factor = invariantFactor(operand);
            if (factor)
                operand = right;
            else
                factor = invariantFactor(right);
            if (!factor)
                return false;
            break;
        }
        case SExt32:
        case ZExt32: {
            AffineIndex inner;
            if (!add(inner, operand, 1, nullptr, depth))
                return false;
            CheckedRange range = checkedAdd(checkedMultiply(checkedRangeOf(proc, inner.scale), m_inputs.counterRange), checkedRangeOf(proc, inner.offset));
            bool isSigned = value->opcode() == SExt32;
            if (!range || range->min() < (isSigned ? std::numeric_limits<int32_t>::min() : 0) || range->max() > (isSigned ? std::numeric_limits<int32_t>::max() : std::numeric_limits<uint32_t>::max()))
                return false;
            return addScaled(result.scale, inner.scale, coefficient, factor) && addScaled(result.offset, inner.offset, coefficient, factor);
        }
        default:
            return false;
        }
        return multiplyBy(coefficient, multiplier) && add(result, operand, coefficient, factor, depth);
    }

    const AffineIndexInputs& m_inputs;
};

} // anonymous namespace

bool findAffineIndex(const AffineIndexInputs& inputs, Value* value, AffineIndex& result)
{
    result = { };
    return AffineIndexFinder(inputs).add(result, value, 1, nullptr, 0);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
