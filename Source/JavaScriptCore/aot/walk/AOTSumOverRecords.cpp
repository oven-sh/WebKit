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

static std::optional<std::pair<ValueID, int64_t>> findConstantFactor(const Code& code, const TypeArena& types, const Operation& record)
{
    if (record.opcode != Opcode::Mul || !types.at(record.type).isInteger())
        return std::nullopt;
    for (uint32_t side : { 0u, 1u }) {
        const Operation& factor = code.at(record.operand(side));
        ValueID other = record.operand(1 - side);
        if (!code.isConstant(record.operand(side)) || factor.opcode != Opcode::Integer || !isIntegerInSumOverRecords(code, types, other))
            continue;
        Range range = types.at(code.at(other).type).integers;
        CheckedInt64 first = CheckedInt64(range.minimum) * factor.integer();
        CheckedInt64 last = CheckedInt64(range.maximum) * factor.integer();
        if (first.hasOverflowed() || last.hasOverflowed() || !Range { std::min(first.value(), last.value()), std::max(first.value(), last.value()) }.isSubsetOf(Range::exact()))
            return std::nullopt;
        return std::pair { other, factor.integer() };
    }
    return std::nullopt;
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
        bool isExact = steps < mostSteps && isExactInSumOverRecords(m_code, m_types, record);
        if (record.opcode == Opcode::Integer)
            m_number += CheckedInt64(record.integer()) * timesOfRoot;
        else if (isExact && (record.opcode == Opcode::Add || record.opcode == Opcode::Sub) && takesInteger(0) && takesInteger(1)) {
            waiting.append({ record.operand(0), timesOfRoot, Range::full() });
            waiting.append({ record.operand(1), record.opcode == Opcode::Add ? timesOfRoot : -timesOfRoot, Range::full() });
        } else if (isExact && (record.opcode == Opcode::Increment || record.opcode == Opcode::Decrement) && takesInteger(0)) {
            m_number += record.opcode == Opcode::Increment ? timesOfRoot : -timesOfRoot;
            waiting.append({ record.operand(0), timesOfRoot, movedInSumOverRecords(range, record.opcode == Opcode::Increment ? -1 : 1) });
        } else if (auto product = findConstantFactor(m_code, m_types, record)) {
            CheckedInt64 timesOfFactor = CheckedInt64(timesOfRoot) * product->second;
            if (timesOfFactor.hasOverflowed())
                return false;
            waiting.append({ product->first, timesOfFactor.value(), Range::full() });
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

bool SumOverRecords::subtractPosition(ValueID boundsTest, bool testResult)
{
    return testResult && add(m_code.at(boundsTest).positionOperand(), -1);
}

bool SumOverRecords::takeAwayWhatIsFromZeroUp(ValueID each, bool testResult)
{
    const Operation& record = m_code.at(each);
    if (record.opcode == Opcode::IsBelowLength) {
        m_number += 1;
        return testResult && add(record.lengthOperand(), -1) && add(record.positionOperand(), 1);
    }
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

std::optional<Range> findStepsFrom(const Code& code, const TypeArena& types, ValueID value, ValueID from, StepsAreExact exactness)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    static constexpr size_t mostNames = 64;
    static constexpr int64_t mostWritten = 1 << 20;
    static constexpr int64_t mostAdded = int64_t { 1 } << 32;
    Range found = Range::empty();
    Vector<std::pair<ValueID, Range>, 8> reached;
    bool isBounded = true;
    auto follow = [&](ValueID next, Range added) {
        isBounded &= reached.size() < mostNames && added.isSubsetOf(Range { -mostAdded, mostAdded });
        if (isBounded && !reached.contains(std::pair(next, added)))
            reached.append({ next, added });
    };
    follow(value, Range::one(0));
    for (size_t next = 0; next < reached.size() && isBounded; ++next) {
        auto [each, added] = reached[next];
        if (each == from) {
            found = found.unionWith(added);
            continue;
        }
        const Operation& record = code.at(each);
        if (code.isConstant(each))
            return std::nullopt;
        if (cellPreservingOpcodes.contains(record.opcode)) {
            follow(record.operand(0), added);
            continue;
        }
        if (exactness == StepsAreExact::AsAssumed ? steps.contains(record.opcode) : isExactInSumOverRecords(code, types, record)) {
            bool isSum = record.opcode == Opcode::Add || record.opcode == Opcode::Sub;
            bool firstIsAddend = record.opcode == Opcode::Add && (code.at(record.operand(0)).opcode == Opcode::Integer || (rootInSumOverRecords(code, record.operand(1)) == from && rootInSumOverRecords(code, record.operand(0)) != from));
            const Operation* written = isSum ? &code.at(record.operand(firstIsAddend ? 0 : 1)) : nullptr;
            bool isWritten = !written || written->opcode == Opcode::Integer;
            if (written && isWritten && std::abs(written->integer()) > mostWritten)
                return std::nullopt;
            if (!isWritten && (exactness == StepsAreExact::AsAssumed || !types.at(written->type).isInteger()))
                return std::nullopt;
            Range step = !written ? Range::one(1) : isWritten ? Range::one(written->integer()) : types.at(written->type).integers;
            if (!step.isSubsetOf(Range { -mostAdded, mostAdded }))
                return std::nullopt;
            bool isSubtracted = record.opcode == Opcode::Decrement || record.opcode == Opcode::Sub;
            follow(record.operand(firstIsAddend ? 1 : 0), isSubtracted ? Range { added.minimum - step.maximum, added.maximum - step.minimum } : Range { added.minimum + step.minimum, added.maximum + step.maximum });
            continue;
        }
        const Block* block = record.opcode == Opcode::Parameter ? &code.at(record.block()) : nullptr;
        if (!block || block->isEntry || block->isLoopHead || !block->firstEdgeIn())
            return std::nullopt;
        size_t position = block->placeOfParameter(each);
        for (EdgeID edge = block->firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
            std::span<const ValueID> arguments = code.at(edge).arguments();
            if (position >= arguments.size() || !arguments[position])
                return std::nullopt;
            follow(arguments[position], added);
        }
    }
    return isBounded && !found.isEmpty() ? std::optional { found } : std::nullopt;
}

bool stepsSecondCounter(const Code& code, const TypeArena& types, const SecondCounter& second, ValueID stepped, ValueID counter)
{
    std::optional<Range> steps = stepped && counter ? findStepsFrom(code, types, stepped, second.stepped) : std::nullopt;
    std::optional<Range> counted = steps ? findStepsFrom(code, types, counter, second.counter) : std::nullopt;
    return counted && steps->isSubsetOf(second.steps) && (second.counterRises ? counted->minimum > 0 : counted->maximum < 0);
}

SecondCounter secondCounterOf(const SteppedWithCounter& beside)
{
    return SecondCounter { beside.counter, beside.stepped, Range { std::numeric_limits<int32_t>::min(), 1 }, beside.counterRises };
}

bool isSteppedWithCounter(const Code& code, const TypeArena& types, const SteppedWithCounter& beside)
{
    BlockID header = code.blockOf(beside.stepped);
    if (code.blockOf(beside.counter) != header || beside.stepped == beside.counter || code.at(header).isEntry || code.at(header).isHandlerBlock() || !code.at(header).firstEdgeIn())
        return false;
    auto isSame = [&](ValueID one, ValueID other) {
        one = rootInSumOverRecords(code, one);
        other = rootInSumOverRecords(code, other);
        return one == other || (code.at(one).opcode == Opcode::Integer && code.at(other).opcode == Opcode::Integer && code.at(one).integer() == code.at(other).integer());
    };
    size_t placeOfStepped = code.at(header).placeOfParameter(beside.stepped);
    size_t placeOfCounter = code.at(header).placeOfParameter(beside.counter);
    for (EdgeID edge = code.at(header).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
        std::span<const ValueID> arguments = code.at(edge).arguments();
        if (placeOfStepped >= arguments.size() || placeOfCounter >= arguments.size())
            return false;
        ValueID stepped = arguments[placeOfStepped];
        ValueID counter = arguments[placeOfCounter];
        if (!stepped || !counter)
            return false;
        bool enters = isSame(stepped, beside.steppedStart) && isSame(counter, beside.counterStart);
        if (!enters && !stepsSecondCounter(code, types, secondCounterOf(beside), stepped, counter))
            return false;
    }
    return true;
}

bool secondCounterStaysWithin(const Code& code, const TypeArena& types, const GroundsOfPosition& grounds)
{
    ValueID stepped = rootInSumOverRecords(code, grounds.counter);
    if (!grounds.start || !grounds.loweredFrom || code.isConstant(stepped) || code.at(stepped).opcode != Opcode::Parameter || rootInSumOverRecords(code, grounds.loweredCounter) != stepped)
        return false;
    const Operation& test = code.at(grounds.counterTest);
    BlockID header = code.blockOf(stepped);
    if (!relationsInSumOverRecords.contains(test.opcode) || test.form() != OperatorForm::Integers || !code.at(header).isLoopHead || code.at(header).isEntry || code.at(header).isHandlerBlock())
        return false;
    bool admitsEqual = (test.opcode == Opcode::LessEq || test.opcode == Opcode::GreaterEq) == grounds.counterTestResult;
    bool leftIsLesser = (test.opcode == Opcode::Less || test.opcode == Opcode::LessEq) == grounds.counterTestResult;
    for (uint32_t side : { 0u, 1u }) {
        ValueID counter = rootInSumOverRecords(code, test.operand(side));
        ValueID bound = test.operand(1 - side);
        if (code.isConstant(counter) || code.at(counter).opcode != Opcode::Parameter || code.blockOf(counter) != header || counter == stepped || !isIntegerInSumOverRecords(code, types, bound))
            continue;
        bool counterRises = leftIsLesser == !side;
        size_t placeOfStepped = code.at(header).placeOfParameter(stepped);
        size_t placeOfCounter = code.at(header).placeOfParameter(counter);
        Range steps = Range::one(0);
        ValueID enteringStepped;
        ValueID enteringCounter;
        ValueID addedInIteration;
        bool isSubtracted = false;
        bool addsOneNumber = true;
        auto isSame = [&](ValueID one, ValueID other) {
            one = rootInSumOverRecords(code, one);
            other = rootInSumOverRecords(code, other);
            return one == other || (code.at(one).opcode == Opcode::Integer && code.at(other).opcode == Opcode::Integer && code.at(one).integer() == code.at(other).integer());
        };
        bool isShown = true;
        for (EdgeID edge = code.at(header).firstEdgeIn(); edge && isShown; edge = code.at(edge).nextInto()) {
            std::span<const ValueID> arguments = code.at(edge).arguments();
            if (placeOfStepped >= arguments.size() || placeOfCounter >= arguments.size() || !arguments[placeOfStepped] || !arguments[placeOfCounter])
                return false;
            ValueID nextStepped = arguments[placeOfStepped];
            ValueID nextCounter = arguments[placeOfCounter];
            std::optional<Range> added = findStepsFrom(code, types, nextStepped, stepped);
            std::optional<Range> counted = added ? findStepsFrom(code, types, nextCounter, counter) : std::nullopt;
            if (counted) {
                steps = steps.unionWith(*added);
                isShown = counterRises ? counted->minimum > 0 : counted->maximum < 0;
                const Operation& sum = code.at(rootInSumOverRecords(code, nextStepped));
                addsOneNumber &= (sum.opcode == Opcode::Add || sum.opcode == Opcode::Sub) && rootInSumOverRecords(code, sum.operand(0)) == stepped && (!addedInIteration || (isSame(addedInIteration, sum.operand(1)) && isSubtracted == (sum.opcode == Opcode::Sub)));
                if (addsOneNumber) {
                    addedInIteration = sum.operand(1);
                    isSubtracted = sum.opcode == Opcode::Sub;
                }
                continue;
            }
            isShown = (!enteringStepped || isSame(enteringStepped, nextStepped)) && (!enteringCounter || isSame(enteringCounter, nextCounter));
            enteringStepped = nextStepped;
            enteringCounter = nextCounter;
        }
        if (!isShown || !enteringStepped || !enteringCounter)
            continue;
        auto isBeyond = [&](ValueID limit, int64_t step, int64_t sign) {
            int64_t timesOfBound = counterRises ? step : -step;
            SumOverRecords distance(code, types, admitsEqual ? 0 : sign * step);
            return distance.add(limit, sign) && distance.add(enteringStepped, -sign) && distance.add(bound, -sign * timesOfBound) && distance.add(enteringCounter, sign * timesOfBound) && distance.isFromZeroUp();
        };
        auto isAfterAllSteps = [&](ValueID limit, bool limitIsLess) {
            const Operation& sum = code.at(rootInSumOverRecords(code, limit));
            if (!addsOneNumber || !addedInIteration || isSubtracted != limitIsLess || sum.opcode != (limitIsLess ? Opcode::Sub : Opcode::Add) || !isExactInSumOverRecords(code, types, sum) || !isSame(sum.operand(0), enteringStepped))
                return false;
            const Operation& product = code.at(rootInSumOverRecords(code, sum.operand(1)));
            if (product.opcode != Opcode::Mul || !types.at(product.type).isInteger() || !types.at(product.type).integers.isSubsetOf(Range::exact()) || !isIntegerInSumOverRecords(code, types, addedInIteration) || types.at(code.at(addedInIteration).type).integers.minimum < 0)
                return false;
            for (uint32_t factor : { 0u, 1u }) {
                if (!isSame(product.operand(factor), addedInIteration))
                    continue;
                int64_t timesOfBound = counterRises ? 1 : -1;
                SumOverRecords iterations(code, types, admitsEqual ? 0 : 1);
                if (iterations.add(product.operand(1 - factor), 1) && iterations.add(bound, -timesOfBound) && iterations.add(enteringCounter, timesOfBound) && iterations.isFromZeroUp())
                    return true;
            }
            return false;
        };
        if ((isBeyond(grounds.loweredFrom, steps.maximum, 1) || isAfterAllSteps(grounds.loweredFrom, false)) && (isBeyond(grounds.start, steps.minimum, -1) || isAfterAllSteps(grounds.start, true)))
            return true;
    }
    return false;
}

std::optional<EnteringCounters> findEnteringCounters(const Code& code, const TypeArena& types, const SecondCounter& second)
{
    BlockID header = code.blockOf(second.stepped);
    if (code.blockOf(second.counter) != header || second.counter == second.stepped || code.at(header).isEntry || code.at(header).isHandlerBlock())
        return std::nullopt;

    struct Arrival {
        ValueID stepped;
        ValueID counter;
        EnteringCounters bounds;
        bool operator==(const Arrival&) const = default;
    };
    static constexpr size_t mostArrivals = 64;
    Vector<Arrival, 8> arrivals;
    bool isBounded = true;
    auto followEdgesInto = [&](BlockID block, ValueID stepped, ValueID counter, EnteringCounters bounds) {
        size_t placeOfStepped = code.at(block).placeOfParameter(stepped);
        size_t placeOfCounter = code.at(block).placeOfParameter(counter);
        for (EdgeID edge = code.at(block).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
            std::span<const ValueID> arguments = code.at(edge).arguments();
            Arrival arrival { { }, { }, bounds };
            if (placeOfStepped < arguments.size() && placeOfCounter < arguments.size()) {
                arrival.stepped = arguments[placeOfStepped];
                arrival.counter = arguments[placeOfCounter];
            }
            isBounded &= arrival.stepped && arrival.counter && arrivals.size() < mostArrivals;
            if (isBounded && !arrivals.contains(arrival))
                arrivals.append(arrival);
        }
    };
    followEdgesInto(header, second.stepped, second.counter, EnteringCounters { Range::full(), Range::full() });
    EnteringCounters entering;
    for (size_t next = 0; next < arrivals.size() && isBounded; ++next) {
        auto [stepped, counter, bounds] = arrivals[next];
        if (stepsSecondCounter(code, types, second, stepped, counter))
            continue;
        if (isIntegerInSumOverRecords(code, types, stepped))
            bounds.stepped = bounds.stepped.intersectWith(types.at(code.at(stepped).type).integers);
        if (isIntegerInSumOverRecords(code, types, counter))
            bounds.counter = bounds.counter.intersectWith(types.at(code.at(counter).type).integers);
        ValueID rootOfStepped = rootInSumOverRecords(code, stepped);
        ValueID rootOfCounter = rootInSumOverRecords(code, counter);
        bool areParameters = !code.isConstant(rootOfStepped) && !code.isConstant(rootOfCounter) && code.at(rootOfStepped).opcode == Opcode::Parameter && code.at(rootOfCounter).opcode == Opcode::Parameter;
        BlockID join = areParameters && code.blockOf(rootOfStepped) == code.blockOf(rootOfCounter) ? code.blockOf(rootOfStepped) : BlockID();
        if (join && join != header && !code.at(join).isEntry && !code.at(join).isHandlerBlock() && code.at(join).firstEdgeIn()) {
            followEdgesInto(join, rootOfStepped, rootOfCounter, bounds);
            continue;
        }
        entering.stepped = entering.stepped.unionWith(bounds.stepped);
        entering.counter = entering.counter.unionWith(bounds.counter);
    }
    return isBounded ? std::optional { entering } : std::nullopt;
}

std::optional<Range> rangeOfSecondCounter(const Code& code, const TypeArena& types, const SecondCounter& second, const EnteringCounters& entering, const Operation& test, uint32_t operandIndex, bool testIsTrue)
{
    if (!relationsInSumOverRecords.contains(test.opcode) || test.form() != OperatorForm::Integers)
        return std::nullopt;
    if (code.blockOf(second.counter) != code.blockOf(second.stepped) || second.counter == second.stepped || !isIntegerInSumOverRecords(code, types, test.operand(1 - operandIndex)))
        return std::nullopt;
    bool isLess = test.opcode == Opcode::Less || test.opcode == Opcode::LessEq;
    bool admitsEqual = (test.opcode == Opcode::LessEq || test.opcode == Opcode::GreaterEq) == testIsTrue;
    bool liesBelow = (isLess == testIsTrue) == !operandIndex;
    if (liesBelow != second.counterRises)
        return std::nullopt;

    Range bound = types.at(code.at(test.operand(1 - operandIndex)).type).integers;
    if (entering.stepped.isEmpty() || entering.counter.isEmpty() || bound.isEmpty() || !entering.stepped.isSubsetOf(Range::exact()) || !entering.counter.isSubsetOf(Range::exact()) || !bound.isSubsetOf(Range::exact()))
        return std::nullopt;
    CheckedInt64 turns = second.counterRises ? bound.maximum : entering.counter.maximum;
    turns -= second.counterRises ? entering.counter.minimum : bound.minimum;
    turns -= admitsEqual ? 0 : 1;
    if (turns.hasOverflowed())
        return std::nullopt;
    CheckedInt64 mostTurns = std::max<int64_t>(turns.value(), 0);
    CheckedInt64 least = mostTurns * std::min<int64_t>(second.steps.minimum, 0);
    CheckedInt64 greatest = mostTurns * std::max<int64_t>(second.steps.maximum, 0);
    least += entering.stepped.minimum;
    greatest += entering.stepped.maximum;
    if (least.hasOverflowed() || greatest.hasOverflowed())
        return std::nullopt;
    Range found { least.value(), greatest.value() };
    return found.isSubsetOf(Range::exact()) ? std::optional { found } : std::nullopt;
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

bool everyWayStepsDownFrom(const Code& code, const TypeArena& types, ValueID value, ValueID from, NOESCAPE const ScopedLambda<bool(ValueID)>& mayGoThrough, NOESCAPE const ScopedLambda<bool(BlockID)>& isReached)
{
    from = rootInSumOverRecords(code, from);
    Vector<ValueID, 8> waiting;
    Vector<ValueID, 8> seen;
    waiting.append(value);
    while (!waiting.isEmpty()) {
        ValueID each = rootInSumOverRecords(code, waiting.takeLast());
        const Operation& record = code.at(each);
        if (each == from || seen.contains(each) || (record.opcode == Opcode::Integer && code.at(from).opcode == Opcode::Integer && record.integer() <= code.at(from).integer()))
            continue;
        seen.append(each);
        if (code.isConstant(each) || !mayGoThrough(each))
            return false;
        if (isExactInSumOverRecords(code, types, record) && (record.opcode == Opcode::Decrement || record.opcode == Opcode::Sub)) {
            if (record.opcode == Opcode::Sub && types.at(code.at(record.operand(1)).type).integers.minimum < 0)
                return false;
            waiting.append(record.operand(0));
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
        bool isTest = each && (relationsInSumOverRecords.contains(code.at(each).opcode) || code.at(each).opcode == Opcode::IsBelowLength);
        return each && (!isTest || holds(each, testResult));
    };
    SumOverRecords fromZero(code, types);
    SumOverRecords belowLength(code, types, -1);
    if (!grounds.length || !fromZero.add(position, 1) || !belowLength.add(grounds.length, 1) || !belowLength.add(position, -1))
        return false;
    if (grounds.counterTest && !holds(grounds.counterTest, grounds.counterTestResult))
        return false;
    bool isFromZeroUp = (isIntegerInSumOverRecords(code, types, position) && types.at(code.at(position).type).integers.minimum >= 0) || fromZero.isFromZeroUp();
    if (!isFromZeroUp && grounds.counter && !(fromZero.add(grounds.counter, -1) && fromZero.add(grounds.start, 1)))
        return false;
    auto subtractNonNegativeTest = [&] {
        if (code.at(grounds.isFromZeroUp).opcode == Opcode::IsBelowLength)
            return fromZero.subtractPosition(grounds.isFromZeroUp, grounds.isNonNegativeResult);
        return fromZero.takeAwayWhatIsFromZeroUp(grounds.isFromZeroUp, grounds.isNonNegativeResult);
    };
    auto isFromZeroUpAfterLoop = [&] {
        ValueID counter = grounds.loweringLoopIsClosed && grounds.loweredCounter ? rootInSumOverRecords(code, grounds.loweredCounter) : ValueID();
        if (!counter || code.isConstant(counter) || code.at(counter).opcode != Opcode::Parameter || !code.at(code.at(counter).block()).isLoopHead)
            return false;
        std::span<const ValueID> parameters = code.at(code.at(counter).block()).parameters();
        size_t place = std::ranges::find(parameters, counter) - parameters.begin();
        std::optional<int64_t> least;
        for (EdgeID edge = code.at(code.at(counter).block()).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
            std::span<const ValueID> arguments = code.at(edge).arguments();
            if (place >= arguments.size())
                return false;
            if (rootInSumOverRecords(code, arguments[place]) == counter)
                continue;
            if (!isIntegerInSumOverRecords(code, types, arguments[place]))
                return false;
            int64_t brought = types.at(code.at(arguments[place]).type).integers.minimum;
            least = least ? std::min(*least, brought) : brought;
        }
        SumOverRecords above(code, types, least.value_or(0));
        return least && above.add(position, 1) && above.add(counter, -1) && above.isFromZeroUp();
    };
    isFromZeroUp = isFromZeroUp || isFromZeroUpAfterLoop();
    isFromZeroUp = isFromZeroUp || fromZero.isFromZeroUp() || (mayBeTaken(grounds.isFromZeroUp, grounds.isNonNegativeResult) && subtractNonNegativeTest() && fromZero.isFromZeroUp());
    if (grounds.loweredCounter && !(belowLength.add(grounds.loweredCounter, 1) && belowLength.add(grounds.loweredFrom, -1)))
        return false;
    if (const SteppedWithCounter& beside = grounds.steppedBeside; beside.stepped) {
        int64_t direction = beside.counterRises ? -1 : 1;
        if (!(belowLength.add(beside.stepped, 1) && belowLength.add(beside.steppedStart, -1) && belowLength.add(beside.counter, direction) && belowLength.add(beside.counterStart, -direction)))
            return false;
    }
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

static ValueID recordThroughNamesForSumOverRecords(const Code& code, ValueID record)
{
    while (cellPreservingOpcodes.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

static bool isSameForSumOverRecords(const Code& code, ValueID one, ValueID other)
{
    one = recordThroughNamesForSumOverRecords(code, one);
    other = recordThroughNamesForSumOverRecords(code, other);
    return one == other || (code.at(one).opcode == Opcode::Integer && code.at(other).opcode == Opcode::Integer && code.at(one).integer() == code.at(other).integer());
}

bool testsShowElementPresent(const Code& code, ValueID list, ValueID position, std::span<const PassedTest::Side> sides)
{
    return std::ranges::all_of(sides, [&](const PassedTest::Side& side) {
        const Operation& test = code.at(side.test);
        return side.testResult && test.opcode == Opcode::IsElementPresent && isSameForSumOverRecords(code, test.arrayOperand(), list) && isSameForSumOverRecords(code, test.positionOperand(), position);
    });
}

bool testsProveElementAtOrAfter(const Code& code, const TypeArena& types, ValueID list, ValueID position, std::span<const PassedTest::Side> sides)
{
    if (!isIntegerInSumOverRecords(code, types, position) || types.at(code.at(position).type).integers.minimum < 0)
        return false;
    return std::ranges::any_of(sides, [&](const PassedTest::Side& side) {
        const Operation& test = code.at(side.test);
        if (!side.testResult || test.opcode != Opcode::IsElementPresent || !isSameForSumOverRecords(code, test.arrayOperand(), list))
            return false;
        SumOverRecords distance(code, types);
        return distance.add(test.positionOperand(), 1) && distance.add(position, -1) && distance.isFromZeroUp();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
