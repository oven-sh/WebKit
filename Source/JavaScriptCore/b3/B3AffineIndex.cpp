/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3AffineIndex.h"

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <algorithm>

namespace JSC { namespace B3 {

static CheckedRange checkedRange(Int128 min, Int128 max)
{
    IntegerInterval range { min, max };
    if (!fullInterval(Int64, Signedness::Signed).contains(range))
        return std::nullopt;
    return range;
}

CheckedRange checkedAdd(const CheckedRange& left, const CheckedRange& right)
{
    if (!left || !right)
        return std::nullopt;
    return checkedRange(left->min + right->min, left->max + right->max);
}

CheckedRange checkedMultiply(const CheckedRange& left, const CheckedRange& right)
{
    if (!left || !right)
        return std::nullopt;
    auto [min, max] = std::minmax({ left->min * right->min, left->min * right->max, left->max * right->min, left->max * right->max });
    return checkedRange(min, max);
}

CheckedRange checkedRangeOf(const Procedure& proc, const InvariantSum& sum)
{
    CheckedRange result = checkedConstant(0);
    for (const InvariantTerm& term : sum.terms) {
        CheckedRange product = intervalFor(proc, term.first, Signedness::Signed);
        if (term.second)
            product = checkedMultiply(product, intervalFor(proc, term.second, Signedness::Signed));
        result = checkedAdd(result, checkedMultiply(product, checkedConstant(term.coefficient)));
    }
    return checkedAdd(result, checkedConstant(sum.constant));
}

namespace {

std::optional<int64_t> toInt64(Int128 value)
{
    if (value < std::numeric_limits<int64_t>::min() || value > std::numeric_limits<int64_t>::max())
        return std::nullopt;
    return static_cast<int64_t>(value);
}

bool hasSameFactors(const InvariantTerm& left, const InvariantTerm& right)
{
    return left.first == right.first && left.second == right.second;
}

bool isOrderedBefore(const InvariantTerm& left, const InvariantTerm& right)
{
    if (left.first != right.first)
        return left.first->index() < right.first->index();
    if (!left.second || !right.second)
        return !left.second && right.second;
    return left.second->index() < right.second->index();
}

bool addTerm(InvariantSum& sum, InvariantTerm term)
{
    if (!term.coefficient)
        return true;
    if (term.second && term.second->index() < term.first->index())
        std::swap(term.first, term.second);
    size_t index = 0;
    while (index < sum.terms.size() && isOrderedBefore(sum.terms[index], term))
        ++index;
    if (index == sum.terms.size() || !hasSameFactors(sum.terms[index], term)) {
        if (sum.terms.size() == Limits::maxTermsPerInvariantSum)
            return false;
        sum.terms.insert(index, term);
        return true;
    }
    auto coefficient = toInt64(Int128 { sum.terms[index].coefficient } + term.coefficient);
    if (!coefficient)
        return false;
    if (*coefficient)
        sum.terms[index].coefficient = *coefficient;
    else
        sum.terms.removeAt(index);
    return true;
}

bool isZero(const InvariantSum& sum)
{
    return sum.isConstant() && !sum.constant;
}

InvariantSum constantSum(int64_t constant)
{
    InvariantSum sum;
    sum.constant = constant;
    return sum;
}

std::optional<InvariantSum> add(const InvariantSum& left, const InvariantSum& right)
{
    InvariantSum result = left;
    auto constant = toInt64(Int128 { left.constant } + right.constant);
    if (!constant)
        return std::nullopt;
    result.constant = *constant;
    for (const InvariantTerm& term : right.terms) {
        if (!addTerm(result, term))
            return std::nullopt;
    }
    return result;
}

std::optional<InvariantSum> multiply(const InvariantSum& left, const InvariantSum& right)
{
    InvariantSum result;
    auto constant = toInt64(Int128 { left.constant } * right.constant);
    if (!constant)
        return std::nullopt;
    result.constant = *constant;
    auto addScaled = [&](const InvariantSum& sum, int64_t factor) {
        for (const InvariantTerm& term : sum.terms) {
            auto coefficient = toInt64(Int128 { term.coefficient } * factor);
            if (!coefficient || !addTerm(result, { *coefficient, term.first, term.second }))
                return false;
        }
        return true;
    };
    if (!addScaled(left, right.constant) || !addScaled(right, left.constant))
        return std::nullopt;
    for (const InvariantTerm& leftTerm : left.terms) {
        for (const InvariantTerm& rightTerm : right.terms) {
            if (leftTerm.second || rightTerm.second)
                return std::nullopt;
            auto coefficient = toInt64(Int128 { leftTerm.coefficient } * rightTerm.coefficient);
            if (!coefficient || !addTerm(result, { *coefficient, leftTerm.first, rightTerm.first }))
                return std::nullopt;
        }
    }
    return result;
}

std::optional<AffineIndex> makeIndex(std::optional<InvariantSum>&& scale, std::optional<InvariantSum>&& offset)
{
    if (!scale || !offset)
        return std::nullopt;
    return AffineIndex { WTF::move(*scale), WTF::move(*offset) };
}

std::optional<AffineIndex> add(const AffineIndex& left, const AffineIndex& right)
{
    return makeIndex(add(left.scale, right.scale), add(left.offset, right.offset));
}

std::optional<AffineIndex> multiply(const AffineIndex& left, const AffineIndex& right)
{
    if (!isZero(left.scale) && !isZero(right.scale))
        return std::nullopt;
    const AffineIndex& variant = isZero(left.scale) ? right : left;
    const AffineIndex& invariant = isZero(left.scale) ? left : right;
    return makeIndex(multiply(variant.scale, invariant.offset), multiply(variant.offset, invariant.offset));
}

std::optional<AffineIndex> multiply(const AffineIndex& index, int64_t factor)
{
    return multiply(index, AffineIndex { { }, constantSum(factor) });
}

class AffineIndexFinder {
public:
    explicit AffineIndexFinder(const AffineIndexInputs& inputs)
        : m_inputs(inputs)
        , m_counterRange(checkedRange(inputs.counterRange.min, inputs.counterRange.max))
    {
    }

    std::optional<AffineIndex> find(Value* value, uint64_t depth)
    {
        value = value->foldIdentity();
        if (!value->type().isInt())
            return std::nullopt;
        if (value == m_inputs.counter)
            return AffineIndex { constantSum(1), { } };
        if (value->hasInt())
            return AffineIndex { { }, constantSum(value->asInt()) };
        if (value->opcode() == SExt32 || value->opcode() == ZExt32) {
            if (auto result = findInOperands(value, depth))
                return result;
        }
        if (Value* invariant = m_inputs.canonicalInvariant(value)) {
            AffineIndex result;
            result.offset.terms.append(InvariantTerm { 1, invariant, nullptr });
            return result;
        }
        return findInOperands(value, depth);
    }

private:
    std::optional<AffineIndex> findInOperands(Value* value, uint64_t depth)
    {
        if (depth == m_inputs.proc.limit(Limit::MaxRangeAnalysisDepth)) {
            m_inputs.proc.didReachLimit(Limit::MaxRangeAnalysisDepth, value->origin());
            return std::nullopt;
        }
        switch (value->opcode()) {
        case Add:
        case Sub:
        case Mul: {
            auto left = find(value->child(0), depth + 1);
            auto right = left ? find(value->child(1), depth + 1) : std::nullopt;
            if (!right)
                return std::nullopt;
            if (value->opcode() == Mul)
                return multiply(*left, *right);
            if (value->opcode() == Sub)
                right = multiply(*right, -1);
            return right ? add(*left, *right) : std::nullopt;
        }
        case Neg: {
            auto operand = find(value->child(0), depth + 1);
            return operand ? multiply(*operand, -1) : std::nullopt;
        }
        case Shl: {
            if (!value->child(1)->hasInt32())
                return std::nullopt;
            unsigned shift = value->child(1)->asInt32() & (value->type() == Int32 ? 31 : 63);
            auto operand = shift < 63 ? find(value->child(0), depth + 1) : std::nullopt;
            return operand ? multiply(*operand, int64_t { 1 } << shift) : std::nullopt;
        }
        case Trunc:
            return find(value->child(0), depth + 1);
        case SExt32:
        case ZExt32: {
            auto operand = find(value->child(0), depth + 1);
            if (!operand)
                return std::nullopt;
            CheckedRange range = checkedAdd(checkedMultiply(checkedRangeOf(m_inputs.proc, operand->scale), m_counterRange), checkedRangeOf(m_inputs.proc, operand->offset));
            if (!range || !fullInterval(Int32, value->opcode() == SExt32 ? Signedness::Signed : Signedness::Unsigned).contains(*range))
                return std::nullopt;
            return operand;
        }
        default:
            return std::nullopt;
        }
    }

    const AffineIndexInputs& m_inputs;
    CheckedRange m_counterRange;
};

} // anonymous namespace

std::optional<AffineIndex> affineIndexOf(const AffineIndexInputs& inputs, Value* value)
{
    return AffineIndexFinder(inputs).find(value, 0);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
