/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSumOverRecords.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet relationsInSumOverRecords { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static ValueID rootInSumOverRecords(const Code& code, ValueID value)
{
    while (cellPreservingOpcodes.contains(code.at(value).opcode))
        value = code.at(value).operand(0);
    return value;
}

static bool isIntegerInSumOverRecords(const Code& code, const TypeArena& types, ValueID value)
{
    return types.at(code.at(value).type).isInteger();
}

static Range movedInSumOverRecords(Range range, int64_t by)
{
    CheckedInt64 minimum = range.minimum;
    CheckedInt64 maximum = range.maximum;
    minimum += by;
    maximum += by;
    return minimum.hasOverflowed() || maximum.hasOverflowed() ? Range::full() : Range { minimum.value(), maximum.value() };
}

static bool isExactInSumOverRecords(const Code& code, const TypeArena& types, const Operation& record)
{
    bool hasTwo = record.opcode == Opcode::Add || record.opcode == Opcode::Sub;
    if ((!hasTwo && record.opcode != Opcode::Increment && record.opcode != Opcode::Decrement) || !types.at(record.type).isInteger())
        return false;
    if (!isIntegerInSumOverRecords(code, types, record.operand(0)) || (hasTwo && !isIntegerInSumOverRecords(code, types, record.operand(1))))
        return false;
    Range left = types.at(code.at(record.operand(0)).type).integers;
    Range by = hasTwo ? types.at(code.at(record.operand(1)).type).integers : Range::one(1);
    bool goesUp = record.opcode == Opcode::Add || record.opcode == Opcode::Increment;
    CheckedInt64 minimum = left.minimum;
    CheckedInt64 maximum = left.maximum;
    if (goesUp) {
        minimum += by.minimum;
        maximum += by.maximum;
    } else {
        minimum -= by.maximum;
        maximum -= by.minimum;
    }
    return !minimum.hasOverflowed() && !maximum.hasOverflowed() && Range { minimum.value(), maximum.value() }.isSubsetOf(Range::exact());
}

bool SumOverRecords::add(ValueID value, int64_t times)
{
    struct Waiting {
        ValueID value;
        int64_t times;
        Range heldTo;
    };
    Vector<Waiting, 8> waiting;
    waiting.append({ value, times, Range::full() });
    for (unsigned steps = 0; !waiting.isEmpty(); ++steps) {
        if (steps == mostSteps)
            return false;
        auto [root, timesOfRoot, range] = waiting.takeLast();
        bool someNameIsInteger = false;
        for (;; root = m_code.at(root).operand(0)) {
            if (Type type = m_types.at(m_code.at(root).type); type.isInteger()) {
                range = range.intersectWith(type.integers);
                someNameIsInteger = true;
            }
            if (!cellPreservingOpcodes.contains(m_code.at(root).opcode))
                break;
        }
        const Operation& record = m_code.at(root);
        auto takesInteger = [&](uint32_t operandIndex) {
            return isIntegerInSumOverRecords(m_code, m_types, record.operand(operandIndex));
        };
        bool isExact = isExactInSumOverRecords(m_code, m_types, record);
        if (record.opcode == Opcode::Integer)
            m_number += CheckedInt64(record.integer()) * timesOfRoot;
        else if (isExact && (record.opcode == Opcode::Add || record.opcode == Opcode::Sub) && takesInteger(0) && takesInteger(1)) {
            waiting.append({ record.operand(0), timesOfRoot, Range::full() });
            waiting.append({ record.operand(1), record.opcode == Opcode::Add ? timesOfRoot : -timesOfRoot, Range::full() });
        } else if (isExact && (record.opcode == Opcode::Increment || record.opcode == Opcode::Decrement) && takesInteger(0)) {
            m_number += record.opcode == Opcode::Increment ? timesOfRoot : -timesOfRoot;
            waiting.append({ record.operand(0), timesOfRoot, movedInSumOverRecords(range, record.opcode == Opcode::Increment ? -1 : 1) });
        } else if (size_t place = m_terms.findIf([&](const Term& term) { return term.root == root; }); place != notFound) {
            m_terms[place].times += timesOfRoot;
            m_terms[place].range = m_terms[place].range.intersectWith(range);
        } else if (someNameIsInteger)
            m_terms.append(Term { root, timesOfRoot, range });
        else
            return false;
    }
    return true;
}

bool SumOverRecords::takeAwayWhatIsFromZeroUp(ValueID each, bool testResult)
{
    const Operation& record = m_code.at(each);
    if (!relationsInSumOverRecords.contains(record.opcode))
        return testResult && isIntegerInSumOverRecords(m_code, m_types, each) && m_types.at(record.type).integers.minimum >= 0 && add(each, -1);
    bool leftIsGreater = (record.opcode == Opcode::Greater || record.opcode == Opcode::GreaterEq) == testResult;
    bool isStrict = (record.opcode == Opcode::Less || record.opcode == Opcode::Greater) == testResult;
    m_number += static_cast<int64_t>(isStrict);
    return add(record.operand(!leftIsGreater), -1) && add(record.operand(leftIsGreater), 1);
}

static std::optional<std::array<ValueID, 2>> valuesAtOrAboveLesserInSumOverRecords(const Code& code, const TypeArena& types, ValueID met)
{
    if (code.at(met).opcode != Opcode::Parameter || code.at(code.at(met).block()).isLoopHead || code.at(code.at(met).block()).isEntry)
        return std::nullopt;
    const Block& block = code.at(code.at(met).block());
    size_t position = block.placeOfParameter(met);
    std::array<ValueID, 2> brought;
    ValueID relation;
    unsigned ways = 0;
    for (EdgeID edge = block.firstEdgeIn(); edge; edge = code.at(edge).nextInto(), ++ways) {
        const Block& side = code.at(code.at(edge).from);
        EdgeID into = side.firstEdgeIn();
        if (ways == 2 || !into || side.isEntry || code.at(into).nextInto() || position >= code.at(edge).arguments().size())
            return std::nullopt;
        const Terminator& end = code.at(code.at(into).from).terminator;
        if (!end.isConditionalBranch() || (into != end.taken() && into != end.notTaken()) || (relation && relation != end.conditionOperand()))
            return std::nullopt;
        relation = end.conditionOperand();
        brought[into == end.notTaken()] = rootInSumOverRecords(code, code.at(edge).arguments()[position]);
    }
    if (ways != 2 || !brought[0] || !brought[1] || code.at(relation).opcode != Opcode::Less)
        return std::nullopt;
    for (uint32_t side : { 0u, 1u }) {
        ValueID operand = code.at(relation).operand(side);
        ValueID compared = rootInSumOverRecords(code, operand);
        bool isSameNumber = code.at(compared).opcode == Opcode::Integer && code.at(brought[side]).opcode == Opcode::Integer && code.at(compared).integer() == code.at(brought[side]).integer();
        if ((compared != brought[side] && !isSameNumber) || !isIntegerInSumOverRecords(code, types, operand))
            return std::nullopt;
    }
    return std::array { code.at(relation).operand(0), code.at(relation).operand(1) };
}

bool SumOverRecords::isFromZeroUp() const
{
    unsigned sumsLeft = mostSums;
    return isFromZeroUp(0, sumsLeft);
}

bool SumOverRecords::isFromZeroUp(size_t firstTerm, unsigned& sumsLeft) const
{
    CheckedInt64 least = m_number;
    for (const Term& term : m_terms)
        least += CheckedInt64(term.times > 0 ? term.range.minimum : term.range.maximum) * term.times;
    if (!least.hasOverflowed() && least.value() >= 0)
        return true;
    for (size_t place = firstTerm; place < m_terms.size(); ++place) {
        ValueID root = m_terms[place].root;
        auto above = m_terms[place].times == -1 ? valuesAtOrAboveLesserInSumOverRecords(m_code, m_types, root) : std::nullopt;
        for (uint32_t side = 0; above && side < 2; ++side) {
            if (!sumsLeft)
                return false;
            --sumsLeft;
            SumOverRecords sum(*this);
            if (sum.add(root, 1) && sum.add((*above)[side], -1) && sum.isFromZeroUp(place, sumsLeft))
                return true;
        }
    }
    return false;
}

bool everyWayStepsUpFrom(const Code& code, const TypeArena& types, ValueID value, ValueID from, ValueID stepNonNegativeCheck, NOESCAPE const ScopedLambda<bool(ValueID)>& mayGoThrough, NOESCAPE const ScopedLambda<bool(BlockID)>& isReached)
{
    auto isFromZeroUp = [&](ValueID by) {
        SumOverRecords sum(code, types);
        return sum.add(by, 1) && (sum.isFromZeroUp() || (stepNonNegativeCheck && sum.takeAwayWhatIsFromZeroUp(stepNonNegativeCheck) && sum.isFromZeroUp()));
    };
    auto age = [&](ValueID each) {
        ValueID root = rootInSumOverRecords(code, each);
        return code.isConstant(root) ? 0 : root.index() + 1;
    };
    from = rootInSumOverRecords(code, from);
    Vector<ValueID, 8> waiting;
    Vector<ValueID, 8> seen;
    waiting.append(value);
    while (!waiting.isEmpty()) {
        ValueID each = rootInSumOverRecords(code, waiting.takeLast());
        const Operation& record = code.at(each);
        if (each == from || seen.contains(each) || (record.opcode == Opcode::Integer && code.at(from).opcode == Opcode::Integer && record.integer() >= code.at(from).integer()))
            continue;
        seen.append(each);
        if (code.isConstant(each) || !mayGoThrough(each))
            return false;
        bool isInteger = isIntegerInSumOverRecords(code, types, each);
        if (isInteger && record.opcode == Opcode::Increment && isIntegerInSumOverRecords(code, types, record.operand(0))) {
            waiting.append(record.operand(0));
            continue;
        }
        if (isInteger && record.opcode == Opcode::Add) {
            bool mayBeFirst = isFromZeroUp(record.operand(1)) && isIntegerInSumOverRecords(code, types, record.operand(0));
            bool mayBeSecond = isFromZeroUp(record.operand(0)) && isIntegerInSumOverRecords(code, types, record.operand(1));
            if (!mayBeFirst && !mayBeSecond)
                return false;
            waiting.append(record.operand(!(mayBeFirst && (!mayBeSecond || age(record.operand(0)) > age(record.operand(1))))));
            continue;
        }
        if (record.opcode != Opcode::Parameter || code.at(record.block()).isEntry || !code.at(record.block()).firstEdgeIn())
            return false;
        std::span<const ValueID> parameters = code.at(record.block()).parameters();
        size_t position = std::ranges::find(parameters, each) - parameters.begin();
        for (EdgeID edge = code.at(record.block()).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
            if (!isReached(code.at(edge).from))
                continue;
            if (position >= code.at(edge).arguments().size())
                return false;
            waiting.append(code.at(edge).arguments()[position]);
        }
    }
    return true;
}

bool groundsShowPositionInside(const Code& code, const TypeArena& types, ValueID position, const GroundsOfPosition& grounds, NOESCAPE const ScopedLambda<bool(ValueID, bool testResult)>& holds)
{
    auto mayBeTaken = [&](ValueID each, bool testResult = true) {
        return each && (!relationsInSumOverRecords.contains(code.at(each).opcode) || holds(each, testResult));
    };
    SumOverRecords fromZero(code, types);
    SumOverRecords belowLength(code, types, -1);
    if (!grounds.length || !fromZero.add(position, 1) || !belowLength.add(grounds.length, 1) || !belowLength.add(position, -1))
        return false;
    bool isFromZeroUp = (isIntegerInSumOverRecords(code, types, position) && types.at(code.at(position).type).integers.minimum >= 0) || fromZero.isFromZeroUp();
    if (!isFromZeroUp && grounds.counter && !(fromZero.add(grounds.counter, -1) && fromZero.add(grounds.start, 1)))
        return false;
    isFromZeroUp = isFromZeroUp || fromZero.isFromZeroUp() || (mayBeTaken(grounds.isFromZeroUp) && fromZero.takeAwayWhatIsFromZeroUp(grounds.isFromZeroUp) && fromZero.isFromZeroUp());
    auto isShownBelow = [&](const SumOverRecords& sum) {
        SumOverRecords behindTest(sum);
        bool isBehindTest = mayBeTaken(grounds.isBelow, grounds.isBelowResult) && behindTest.takeAwayWhatIsFromZeroUp(grounds.isBelow, grounds.isBelowResult);
        if ((isBehindTest && behindTest.isFromZeroUp()) || sum.isFromZeroUp())
            return true;
        SumOverRecords withinLength(isBehindTest ? behindTest : sum);
        return mayBeTaken(grounds.limitWithinLengthCheck) && withinLength.takeAwayWhatIsFromZeroUp(grounds.limitWithinLengthCheck) && withinLength.isFromZeroUp();
    };
    int64_t most = isIntegerInSumOverRecords(code, types, position) ? types.at(code.at(position).type).integers.maximum : Range::full().maximum;
    SumOverRecords wholePosition(code, types, -1 - most);
    return isFromZeroUp && ((wholePosition.add(grounds.length, 1) && isShownBelow(wholePosition)) || isShownBelow(belowLength));
}

bool testsShowElementPresent(const Code& code, ValueID list, ValueID position, std::span<const PassedTest::Side> sides)
{
    auto behindNames = [&](ValueID record) {
        while (cellPreservingOpcodes.contains(code.at(record).opcode))
            record = code.at(record).operand(0);
        return record;
    };
    auto isSame = [&](ValueID one, ValueID other) {
        one = behindNames(one);
        other = behindNames(other);
        return one == other || (code.at(one).opcode == Opcode::Integer && code.at(other).opcode == Opcode::Integer && code.at(one).integer() == code.at(other).integer());
    };
    return std::ranges::all_of(sides, [&](const PassedTest::Side& side) {
        const Operation& test = code.at(side.test);
        return side.testResult && test.opcode == Opcode::IsElementPresent && isSame(test.arrayOperand(), list) && isSame(test.positionOperand(), position);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
