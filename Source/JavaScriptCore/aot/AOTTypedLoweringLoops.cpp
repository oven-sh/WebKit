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

// Natural loops of the function's blocks. A back edge whose target does not dominate its source belongs to no loop here.
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
    // The header of an outer loop comes first, so that a block ends up with the innermost loop it is in.
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
    for (unsigned index = 0; index < m_loops.size(); ++index) {
        auto& loop = m_loops[index];
        for (size_t blockIndex : loop.blocks) {
            auto& terminator = m_function.blocks[blockIndex].terminator;
            if (terminator.kind == TerminatorKind::Invoke || terminator.kind == TerminatorKind::Throw)
                loop.hasHandlerInside |= loop.blocks.get(terminator.targets.last().block.index());
        }
    }
}

bool Lowering::isLoopInvariant(const Loop& loop, ValueID id) const
{
    BlockID definingBlock = m_definingBlock[id.index()];
    return definingBlock && !loop.blocks.get(definingBlock.index());
}

// As B3::hoistLoopInvariantValues() has it for a position-independent procedure: from the header to the block being appended to there is one way that is not rare.
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

// Whether the back end moves what is computed here from the operand alone in front of the innermost loop.
bool Lowering::willHoistComputationOn(ValueID operand) const
{
    unsigned index = m_innermostLoop[m_currentBlockID.index()];
    // A handler is another way into the loop for the back end, which then takes it for none.
    if (index == noLoop || m_loops[index].hasHandlerInside)
        return false;
    return isLoopInvariant(m_loops[index], operand) && isGuaranteedToExecute(m_loops[index]);
}

// For a kept integer i and any number: `i opcode number` holds exactly where `i opcode bound` does.
// i < d is i < ceil(d), i <= d is i <= floor(d), i > d is i > floor(d), i >= d is i >= ceil(d). Nothing holds of NaN. No kept integer reaches the least or the greatest int64.
Integer Lowering::integerBound(Opcode opcode, Number number)
{
    bool roundsUp = opcode == Opcode::Less || opcode == Opcode::GreaterEq;
    bool holdsOfLeastNumbers = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
    constexpr int64_t least = std::numeric_limits<int64_t>::min();
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    if (number.value()->hasDouble()) {
        double constant = number.value()->asDouble();
        if (std::isnan(constant))
            return integer(holdsOfLeastNumbers ? greatest : least);
        double rounded = roundsUp ? std::ceil(constant) : std::floor(constant);
        return integer(rounded >= 0x1p63 ? greatest : rounded < -0x1p63 ? least : static_cast<int64_t>(rounded));
    }
    // Below -2^63 truncation yields the least integer on every processor.
    Integer rounded = truncateToInteger(roundsUp ? ceil(number) : floor(number));
    Number beyond = this->number(0x1p63);
    if (holdsOfLeastNumbers)
        return select(lessThan(number, beyond), rounded, integer(greatest));
    return select(greaterThanOrEqual(number, beyond), integer(greatest), select(equal(number, number), rounded, integer(least)));
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

// ToInt32 of a sum or a difference of numbers of which one is kept as an integer. Where the other is an int32 too, -0 counting as 0, and a double stands for the result exactly, that is the low half of what integers come out with.
std::optional<Word32> Lowering::toInt32OfSum(ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::Add && value.opcode != Opcode::Sub)
        return std::nullopt;
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (!isNumericOperand(left) || !isNumericOperand(right) || (!left.isInteger() && !right.isInteger()))
        return std::nullopt;
    // Trying a double as an int32 costs what the sum of doubles and its conversion cost. It keeps the integer from going through doubles.
    if ((!left.isInteger() || !right.isInteger()) && (numberOfUsesOf(id) != 1 || m_function.at(left.isInteger() ? a : b).opcode == Opcode::Integer))
        return std::nullopt;

    bool isAdd = value.opcode == Opcode::Add;
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    // By operand: the integer that was taken in the place of the number.
    std::array<Integer, 2> keptBesideNumber;
    auto asInteger = [&](ValueID operand, const Type& type) {
        if (type.isInteger())
            return in<Integer>(operand);
        // What is kept beside the number is all of it. What stands for "not a number" there is, read as unsigned, above every int32.
        if (auto found = m_integersOfNumbers.find(operand.index()); found != m_integersOfNumbers.end() && found->value.least && *found->value.least >= 0 && get(operand).value == found->value.number) {
            Integer kept(found->value.integer);
            orElse(below(kept, integer(1ll << 31)), otherwise);
            keptBesideNumber[operand == b] = kept;
            return kept;
        }
        Number number = asNumericOperand(operand);
        Word32 low = low32(truncateToInteger(number));
        orElse(equal(toNumber(low), number), otherwise);
        return signExtended(low);
    };
    Integer x = asInteger(a, left);
    Integer y = asInteger(b, right);
    Range rangeOfLeft = left.isInteger() ? left.integers : Range::int32();
    Range rangeOfRight = right.isInteger() ? right.integers : Range::int32();
    Int128 least = isAdd ? static_cast<Int128>(rangeOfLeft.least) + rangeOfRight.least : static_cast<Int128>(rangeOfLeft.least) - rangeOfRight.most;
    Int128 most = isAdd ? static_cast<Int128>(rangeOfLeft.most) + rangeOfRight.most : static_cast<Int128>(rangeOfLeft.most) - rangeOfRight.least;
    // Kept integers are at most 2^62: a result that 64 bits have no room for wraps around to the least integer.
    Integer result = isAdd ? add(x, y) : sub(x, y);
    if (most > maxExactInteger)
        orElse(lessThanOrEqual(result, integer(maxExactInteger)), otherwise);
    if (least < -maxExactInteger || most >= static_cast<Int128>(1) << 63)
        orElse(greaterThanOrEqual(result, integer(-maxExactInteger)), otherwise);
    join.add(low32(result));

    m_out.appendTo(otherwise);
    // Made here, so that it is not made where it is not wanted.
    auto asNumber = [&](ValueID operand, Integer kept) {
        if (!kept)
            return asNumericOperand(operand);
        return select(equal(kept, integer(INT64_MIN)), number(PNaN), toNumber(kept));
    };
    Number p = asNumber(a, a == b ? Integer() : keptBesideNumber[0]);
    Number q = asNumber(b, keptBesideNumber[1]);
    join.add(toInt32(isAdd ? add(p, q) : sub(p, q)));
    return join.finish().as<Word32>();
}

// For an integer that exceeds the limit by what rounding takes back. In a loop a branch keeps the comparison off the loop-carried dependency chain.
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
