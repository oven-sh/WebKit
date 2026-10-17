/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

void Lowering::computeLoops(const Vector<BlockID>& order)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    m_innermostLoop.fill(noLoop, numberOfBlocks);
    m_definingBlock.fill(BlockID(), m_function.values.size());
    Vector<Vector<BlockID, 2>> predecessors(numberOfBlocks);
    for (BlockID block : order) {
        for (ValueID parameter : m_function.at(block).parameters)
            m_definingBlock[parameter.index()] = block;
        for (ValueID value : m_function.at(block).values)
            m_definingBlock[value.index()] = block;
        for (auto& target : successorsOf(m_function.at(block).terminator))
            predecessors[target.block.index()].append(block);
    }
    for (BlockID header : order) {
        Loop loop;
        loop.header = header;
        loop.blocks.ensureSize(numberOfBlocks);
        loop.blocks.set(header.index());
        bool hasBackEdge = false;
        Vector<BlockID, 16> worklist;
        for (BlockID predecessor : predecessors[header.index()]) {
            if (!dominates(header, predecessor))
                continue;
            hasBackEdge = true;
            if (!loop.blocks.set(predecessor.index()))
                worklist.append(predecessor);
        }
        if (!hasBackEdge)
            continue;
        while (!worklist.isEmpty()) {
            for (BlockID predecessor : predecessors[worklist.takeLast().index()]) {
                if (!loop.blocks.set(predecessor.index()))
                    worklist.append(predecessor);
            }
        }
        for (size_t block : loop.blocks)
            m_innermostLoop[block] = m_loops.size();
        m_loops.append(WTF::move(loop));
    }
}

bool Lowering::isLoopInvariant(const Loop& loop, ValueID id) const
{
    BlockID definingBlock = m_definingBlock[id.index()];
    return definingBlock && !loop.blocks.get(definingBlock.index());
}

bool Lowering::isGuaranteedToExecute(const Loop& loop) const
{
    static constexpr unsigned maxNumberOfBlocks = 32;
    LBasicBlock block = m_blocks[loop.header.index()];
    for (unsigned i = 0; i < maxNumberOfBlocks && block != m_out.m_block; ++i) {
        LBasicBlock next = nullptr;
        for (auto& successor : block->successors()) {
            if (successor.frequency() == FrequencyClass::Rare)
                continue;
            if (next)
                return false;
            next = successor.block();
        }
        if (!next)
            return false;
        block = next;
    }
    return block == m_out.m_block;
}

bool Lowering::willHoistComputationOn(ValueID operand) const
{
    unsigned index = m_innermostLoop[m_currentBlockID.index()];
    if (index == noLoop)
        return false;
    return isLoopInvariant(m_loops[index], operand) && isGuaranteedToExecute(m_loops[index]);
}

Integer Lowering::integerBound(Opcode opcode, Number number)
{
    bool roundsUp = opcode == Opcode::Less || opcode == Opcode::GreaterEq;
    bool holdsForMinimum = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    if (number.value()->hasDouble()) {
        double constant = number.value()->asDouble();
        if (std::isnan(constant))
            return integer(holdsForMinimum ? greatest : minimum);
        double rounded = roundsUp ? std::ceil(constant) : std::floor(constant);
        return integer(rounded >= 0x1p63 ? greatest : rounded < -0x1p63 ? minimum : static_cast<int64_t>(rounded));
    }
    Integer rounded = truncateToInteger(roundsUp ? ceil(number) : floor(number));
    Number beyond = this->number(0x1p63);
    if (holdsForMinimum)
        return select(lessThan(number, beyond), rounded, integer(greatest));
    return select(greaterThanOrEqual(number, beyond), integer(greatest), select(equal(number, number), rounded, integer(minimum)));
}

std::optional<Word32> Lowering::lowerRelationalWithIntegerBound(const Value& value)
{
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID numberOperand = value.child(integerIsFirst ? 1 : 0);
    auto& numberType = typeOf(numberOperand);
    if (!typeOf(integerOperand).isInteger() || !numberType.isNumber() || numberType.isInteger())
        return std::nullopt;
    if (m_function.at(numberOperand).opcode != Opcode::Double && !willHoistComputationOn(numberOperand))
        return std::nullopt;
    Opcode opcode = value.opcode;
    if (!integerIsFirst) {
        switch (opcode) {
        case Opcode::Less:
            opcode = Opcode::Greater;
            break;
        case Opcode::LessEq:
            opcode = Opcode::GreaterEq;
            break;
        case Opcode::Greater:
            opcode = Opcode::Less;
            break;
        default:
            opcode = Opcode::LessEq;
            break;
        }
    }
    Integer x = in<Integer>(integerOperand);
    Integer bound = integerBound(opcode, in<Number>(numberOperand));
    switch (opcode) {
    case Opcode::Less:
        return lessThan(x, bound);
    case Opcode::LessEq:
        return lessThanOrEqual(x, bound);
    case Opcode::Greater:
        return greaterThan(x, bound);
    default:
        return greaterThanOrEqual(x, bound);
    }
}

std::optional<Word32> Lowering::lowerRelationalWithLikelyInteger(const Value& value)
{
    if (m_likelyIntegerNumbers.isEmpty())
        return std::nullopt;
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID numberOperand = value.child(integerIsFirst ? 1 : 0);
    if (!typeOf(integerOperand).isInteger())
        return std::nullopt;
    auto found = m_likelyIntegerNumbers.find(numberOperand.index());
    if (found == m_likelyIntegerNumbers.end() || get(numberOperand).value != found->value.number)
        return std::nullopt;
    Integer kept(found->value.integer);
    bool savedIsNonNegative = found->value.minimum >= 0;
    bool neitherIsNegative = savedIsNonNegative && typeOf(integerOperand).integers.minimum >= 0;
    return either<Word32>(savedIsNonNegative ? greaterThanOrEqual(kept, integer(0)) : greaterThan(kept, integer(INT64_MIN)), [&] {
        Integer x = integerIsFirst ? in<Integer>(integerOperand) : kept;
        Integer y = integerIsFirst ? kept : in<Integer>(integerOperand);
        switch (value.opcode) {
        case Opcode::Less:
            return neitherIsNegative ? below(x, y) : lessThan(x, y);
        case Opcode::Greater:
            return neitherIsNegative ? below(y, x) : greaterThan(x, y);
        case Opcode::LessEq:
            return neitherIsNegative ? belowOrEqual(x, y) : lessThanOrEqual(x, y);
        default:
            return neitherIsNegative ? belowOrEqual(y, x) : greaterThanOrEqual(x, y);
        }
    }, [&] {
        Number x = asNumericOperand(value.child(0));
        Number y = asNumericOperand(value.child(1));
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    });
}

std::optional<Word32> Lowering::lowerRelationalWithOptionalInteger(const Value& value)
{
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID optionalOperand = value.child(integerIsFirst ? 1 : 0);
    auto& optionalType = typeOf(optionalOperand);
    if (!typeOf(integerOperand).isInteger() || !isNumericOperand(optionalType) || optionalType.primitives & Primitive::Null)
        return std::nullopt;
    Operand optional = get(optionalOperand);
    if (optional.rep != Rep::OptionalInt64)
        return std::nullopt;
    Integer kept = assumedInteger(optional.as<OptionalInteger>());
    bool neitherIsNegative = optionalType.integers.minimum >= 0 && typeOf(integerOperand).integers.minimum >= 0;
    return either<Word32>(lessThan(kept, integer(undefinedInteger)), [&] {
        Integer x = integerIsFirst ? in<Integer>(integerOperand) : kept;
        Integer y = integerIsFirst ? kept : in<Integer>(integerOperand);
        switch (value.opcode) {
        case Opcode::Less:
            return neitherIsNegative ? below(x, y) : lessThan(x, y);
        case Opcode::Greater:
            return neitherIsNegative ? below(y, x) : greaterThan(x, y);
        case Opcode::LessEq:
            return neitherIsNegative ? belowOrEqual(x, y) : lessThanOrEqual(x, y);
        default:
            return neitherIsNegative ? belowOrEqual(y, x) : greaterThanOrEqual(x, y);
        }
    }, [&] {
        return boolean(false);
    });
}

std::optional<Word32> Lowering::sumToInt32(ValueID id)
{
    auto& value = m_function.at(id);
    if ((value.opcode != Opcode::Add && value.opcode != Opcode::Sub) || !operandsAreAvailable(id))
        return std::nullopt;
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (!isNumericOperand(left) || !isNumericOperand(right) || (!left.isInteger() && !right.isInteger()))
        return std::nullopt;
    if ((!left.isInteger() || !right.isInteger()) && (useCountOf(id) != 1 || m_function.at(left.isInteger() ? a : b).opcode == Opcode::Integer))
        return std::nullopt;

    bool isAdd = value.opcode == Opcode::Add;
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    std::array<Integer, 2> savedWithNumber;
    auto asInteger = [&](ValueID operand, const Type& type) {
        if (type.isInteger())
            return in<Integer>(operand);
        if (auto found = m_numbersAsIntegers.find(operand.index()); found != m_numbersAsIntegers.end() && found->value.minimum && *found->value.minimum >= 0 && get(operand).value == found->value.number) {
            Integer kept(found->value.integer);
            orElse(below(kept, integer(1ll << 31)), otherwise);
            savedWithNumber[operand == b] = kept;
            return kept;
        }
        Number number = asNumericOperand(operand);
        Word32 low = low32(truncateToInteger(number));
        orElse(equal(toNumber(low), number), otherwise);
        return signExtended(low);
    };
    Integer x = asInteger(a, left);
    Integer y = asInteger(b, right);
    Range leftRange = left.isInteger() ? left.integers : Range::int32();
    Range rightRange = right.isInteger() ? right.integers : Range::int32();
    Int128 minimum = isAdd ? static_cast<Int128>(leftRange.minimum) + rightRange.minimum : static_cast<Int128>(leftRange.minimum) - rightRange.maximum;
    Int128 maximum = isAdd ? static_cast<Int128>(leftRange.maximum) + rightRange.maximum : static_cast<Int128>(leftRange.maximum) - rightRange.minimum;
    Integer result = isAdd ? add(x, y) : sub(x, y);
    if (maximum > maxExactInteger)
        orElse(lessThanOrEqual(result, integer(maxExactInteger)), otherwise);
    if (minimum < -maxExactInteger || maximum >= static_cast<Int128>(1) << 63)
        orElse(greaterThanOrEqual(result, integer(-maxExactInteger)), otherwise);
    join.add(low32(result));

    m_out.appendTo(otherwise);
    auto asNumber = [&](ValueID operand, Integer kept) {
        if (!kept)
            return asNumericOperand(operand);
        return select(equal(kept, integer(INT64_MIN)), number(PNaN), toNumber(kept));
    };
    Number p = asNumber(a, a == b ? Integer() : savedWithNumber[0]);
    Number q = asNumber(b, savedWithNumber[1]);
    join.add(toInt32(isAdd ? add(p, q) : sub(p, q)));
    return join.finish().as<Word32>();
}

Integer Lowering::clampToMaximum(Integer value, int64_t limit)
{
    Word32 isWithinLimit = lessThanOrEqual(value, integer(limit));
    if (!m_isInLoop)
        return select(isWithinLimit, value, integer(limit));
    return either<Integer>(isWithinLimit, [&] {
        return value;
    }, [&] {
        return integer(limit);
    });
}

Integer Lowering::clampToMinimum(Integer value, int64_t limit)
{
    Word32 isWithinLimit = greaterThanOrEqual(value, integer(limit));
    if (!m_isInLoop)
        return select(isWithinLimit, value, integer(limit));
    return either<Integer>(isWithinLimit, [&] {
        return value;
    }, [&] {
        return integer(limit);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
