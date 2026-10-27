/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopRangeChecks.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3LoopInvariance.h"
#include "B3LoopRegions.h"
#include "B3MemoryValue.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <algorithm>

namespace JSC { namespace B3 {

std::optional<RangeCheck> rangeCheckFor(Value* comparison)
{
    switch (comparison->opcode()) {
    case Below:
        return RangeCheck { comparison->child(0), comparison->child(1), true };
    case AboveEqual:
        return RangeCheck { comparison->child(0), comparison->child(1), false };
    case Above:
        return RangeCheck { comparison->child(1), comparison->child(0), true };
    case BelowEqual:
        return RangeCheck { comparison->child(1), comparison->child(0), false };
    default:
        return std::nullopt;
    }
}

static bool isBound(const LoopInvariance& invariance, Value* bound, Value* value)
{
    Value* other = bound;
    while (true) {
        value = invariance.canonicalValue(value);
        other = invariance.canonicalValue(other);
        if (value == other)
            return true;
        if (value->opcode() != other->opcode() || value->type() != other->type())
            return false;
        if (value->opcode() != ZExt32 && value->opcode() != SExt32) {
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad() || load->offset() != other->as<MemoryValue>()->offset())
                return false;
            if (!invariance.plan().isHoisted(value) || !invariance.plan().isHoisted(other))
                return false;
        }
        value = value->lastChild();
        other = other->lastChild();
    }
}

static void decideComparisonsWithBound(const RangeCheckInputs& inputs, RangeCheckPlan& plan)
{
    const CountedLoop& loop = inputs.loop;
    const LoopInvariance& invariance = inputs.invariance;
    Value* bound = loop.bound;
    for (BasicBlock* block : inputs.region.blocks) {
        if (loop.isTopTested && inputs.blocksBeforeTopTest.contains(block))
            continue;
        for (Value* value : *block) {
            if (value->numChildren() != 2)
                continue;
            Value* left = isBound(invariance, bound, value->child(0)) ? bound : value->child(0);
            Value* right = isBound(invariance, bound, value->child(1)) ? bound : value->child(1);
            TriState outcome = evaluateComparisonWithBound(loop, value->opcode(), left, right);
            if (outcome != TriState::Indeterminate)
                plan.decidedComparisons.add(value, outcome == TriState::True);
        }
    }

    Value* boundOnEntry = loop.bound;
    if (Value* repeated = invariance.plan().repeatedValues.get(loop.bound))
        boundOnEntry = repeated;
    plan.relationHoldsOnEntry = loop.relationHoldsOnEntry || relationHoldsOnEntry(inputs.proc, loop, boundOnEntry);
    if (!loop.isTopTested && !plan.relationHoldsOnEntry)
        plan.decidedComparisons.clear();
}

static bool lagsOneIteration(const RangeCheckInputs& inputs, Value* phi)
{
    const CountedLoop& loop = inputs.loop;
    if (phi->opcode() != Phi || phi->owner != loop.header || phi == loop.inductionVariable || phi->type() != loop.type())
        return false;
    auto upsilons = inputs.phiChildren[phi];
    if (!upsilons.size())
        return false;
    auto isStartMinusStep = [&](Value* value) {
        Value* start = loop.start;
        if (start->hasInt())
            return value->hasInt() && Int128 { value->asInt() } == Int128 { start->asInt() } - loop.step;
        return start->opcode() == Add && start->child(1)->hasInt() && start->child(1)->asInt() == loop.step && inputs.invariance.isSameValue(start->child(0), value);
    };
    for (UpsilonValue* upsilon : upsilons) {
        if (upsilon->owner == loop.preHeader ? !isStartMinusStep(upsilon->child(0)) : upsilon->child(0) != loop.inductionVariable)
            return false;
    }
    return true;
}

static bool isStoredInLoop(const NaturalLoop& loop, Value* limit)
{
    while (limit->opcode() == ZExt32 || limit->opcode() == SExt32)
        limit = limit->child(0);
    auto* load = limit->as<MemoryValue>();
    if (!load || !load->isLoad())
        return false;
    for (unsigned i = loop.size(); i--;) {
        for (Value* value : *loop.at(i)) {
            auto* store = value->as<MemoryValue>();
            if (store && store->isStore() && store->range().overlaps(load->range()))
                return true;
        }
    }
    return false;
}

static std::optional<int64_t> offsetFromCounter(const RangeCheckInputs& inputs, Value* index)
{
    const CountedLoop& loop = inputs.loop;
    int64_t offset = -loop.step;
    if (!lagsOneIteration(inputs, index)) {
        auto form = affineFormOf(inputs.proc, index, loop.inductionVariable, Signedness::Signed);
        if (!form)
            return std::nullopt;
        IntegerInterval full = fullInterval(loop.type(), Signedness::Signed);
        IntegerInterval counterAtRemovedCheck {
            std::max<Int128>(full.min, -Int128 { form->offset }),
            std::min<Int128>(full.max, Int128 { maxLimitForRangeChecks(loop.type()) } - 1 - form->offset)
        };
        if (!form->exactBaseRange.contains(counterAtRemovedCheck))
            return std::nullopt;
        offset = form->offset;
    }
    if (offset < -Limits::maxRangeCheckOffset || offset > Limits::maxRangeCheckOffset)
        return std::nullopt;
    return offset;
}

static void findRangeChecks(const RangeCheckInputs& inputs, RangeCheckPlan& plan)
{
    const Procedure& proc = inputs.proc;
    const CountedLoop& loop = inputs.loop;
    const LoopInvariance& invariance = inputs.invariance;
    if (!inputs.canClampBound)
        return;
    Int128 maxStart = intervalFor(proc, loop.start, Signedness::Signed).max;
    for (BasicBlock* block : inputs.region.blocks) {
        if (loop.isTopTested && inputs.blocksBeforeTopTest.contains(block))
            continue;
        for (Value* value : *block) {
            auto rangeCheck = rangeCheckFor(value);
            if (!rangeCheck || plan.decidedComparisons.contains(value) || invariance.plan().isHoisted(value))
                continue;
            Value* limit = rangeCheck->limit;
            if (rangeCheck->index->type() != loop.type() || !invariance.isInvariant(limit) || value == loop.test->last()->child(0))
                continue;
            auto offset = offsetFromCounter(inputs, rangeCheck->index);
            if (!offset)
                continue;
            if (maxStart + *offset < 0)
                continue;
            if (isStoredInLoop(*inputs.region.loop, limit))
                continue;
            limit = invariance.canonicalValue(limit);
            if (!invariance.isAvailableInPreHeader(limit))
                continue;
            size_t found = plan.bounds.findIf([&](auto& bound) { return bound.limit == limit; });
            if (found == notFound) {
                if (plan.bounds.size() == proc.limit(Limit::MaxRangeCheckBoundsPerLoop)) {
                    proc.didReachLimit(Limit::MaxRangeCheckBoundsPerLoop, value->origin());
                    continue;
                }
                found = plan.bounds.size();
                plan.bounds.append(RangeCheckBound { limit, *offset, *offset });
            }
            plan.bounds[found].minOffset = std::min(plan.bounds[found].minOffset, *offset);
            plan.bounds[found].maxOffset = std::max(plan.bounds[found].maxOffset, *offset);
            plan.decidedComparisons.add(value, rangeCheck->isTrueInBounds);
        }
    }
}

RangeCheckPlan planRangeChecks(const RangeCheckInputs& inputs)
{
    RangeCheckPlan plan;
    decideComparisonsWithBound(inputs, plan);
    findRangeChecks(inputs, plan);
    return plan;
}

namespace {

struct MeetingCounters {
    CountedLoop rising;
    CountedLoop falling;
    bool isStrict { false };
};

} // anonymous namespace

static std::optional<CountedLoop> findSteppingCounter(const MeetingCountersInputs& inputs, Value* phi)
{
    const NaturalLoop& loop = *inputs.region.loop;
    if (phi->opcode() != Phi || phi->owner != inputs.region.header || !phi->type().isInt())
        return std::nullopt;
    CountedLoop counter;
    counter.preHeader = inputs.region.preHeader;
    counter.header = inputs.region.header;
    counter.inductionVariable = phi;
    for (UpsilonValue* upsilon : inputs.phiChildren[phi]) {
        Value* input = upsilon->child(0)->foldIdentity();
        if (upsilon->owner == counter.preHeader) {
            if (counter.start)
                return std::nullopt;
            counter.start = input;
            continue;
        }
        if (!inputs.loops.belongsTo(upsilon->owner, loop))
            return std::nullopt;
        auto form = affineFormOf(inputs.proc, input, phi, Signedness::Signed);
        if (!form || !form->offset || (counter.step && counter.step != form->offset))
            return std::nullopt;
        IntegerInterval known = intervalFor(inputs.proc, phi, Signedness::Signed);
        if (!form->exactBaseRange.contains(known))
            return std::nullopt;
        counter.step = form->offset;
        if (inputs.region.contains(upsilon->owner)) {
            if (counter.next && counter.next != input)
                return std::nullopt;
            counter.next = input;
        }
    }
    if (!counter.start || !counter.next || counter.step > Limits::maxCountedLoopStep || counter.step < -Limits::maxCountedLoopStep)
        return std::nullopt;
    if (!inputs.invariance.isAvailableInPreHeader(counter.start))
        return std::nullopt;
    return counter;
}

static std::optional<MeetingCounters> findMeetingCounters(const MeetingCountersInputs& inputs)
{
    const NaturalLoop& loop = *inputs.region.loop;
    Dominators& dominators = inputs.proc.dominators();
    for (BasicBlock* block : inputs.region.blocks) {
        if (block->last()->opcode() != Branch)
            continue;
        bool firstStays = inputs.loops.belongsTo(block->successorBlock(0), loop);
        if (firstStays == inputs.loops.belongsTo(block->successorBlock(1), loop))
            continue;
        DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || isUnsignedRelation(comparison->opcode()))
            continue;
        Opcode relation = firstStays == condition.isNonZero ? comparison->opcode() : *inverted;
        Value* left = comparison->child(0)->foldIdentity();
        Value* right = comparison->child(1)->foldIdentity();
        if (!isLessRelation(relation)) {
            std::swap(left, right);
            relation = flippedRelation(relation);
        }

        auto counterSteppedTo = [&](Value* next) -> std::optional<CountedLoop> {
            for (Value* phi : *inputs.region.header) {
                if (phi->opcode() != Phi || phi->type() != next->type())
                    continue;
                std::optional<CountedLoop> counter = findSteppingCounter(inputs, phi);
                if (counter && counter->next == next)
                    return counter;
            }
            return std::nullopt;
        };
        std::optional<CountedLoop> rising = counterSteppedTo(left);
        std::optional<CountedLoop> falling = rising ? counterSteppedTo(right) : std::nullopt;
        if (!falling || rising->step < 0 || falling->step > 0)
            continue;
        bool runsEveryTurn = true;
        for (BasicBlock* predecessor : inputs.region.header->predecessors())
            runsEveryTurn &= !inputs.loops.belongsTo(predecessor, loop) || dominators.dominates(block, predecessor);
        if (!runsEveryTurn)
            continue;
        rising->bound = falling->inductionVariable;
        falling->bound = rising->inductionVariable;
        rising->test = falling->test = block;
        return MeetingCounters { *rising, *falling, relation == LessThan };
    }
    return std::nullopt;
}

RangeCheckPlan planRangeChecksOfMeetingCounters(const MeetingCountersInputs& inputs)
{
    RangeCheckPlan plan;
    std::optional<MeetingCounters> counters = findMeetingCounters(inputs);
    if (!counters)
        return plan;
    const LoopInvariance& invariance = inputs.invariance;
    Value* low = counters->rising.start;
    Value* high = counters->falling.start;
    IndexSet<BasicBlock*> noBlocks;
    Vector<Value*, 4> limits;
    for (BasicBlock* block : inputs.region.blocks) {
        for (Value* value : *block) {
            auto rangeCheck = rangeCheckFor(value);
            if (!rangeCheck || invariance.plan().isHoisted(value) || rangeCheck->index->type() != counters->rising.type())
                continue;
            Value* limit = rangeCheck->limit;
            if (!invariance.isInvariant(limit) || isStoredInLoop(*inputs.region.loop, limit))
                continue;
            limit = invariance.canonicalValue(limit);
            if (!invariance.isAvailableInPreHeader(limit))
                continue;
            auto offsetFrom = [&](const CountedLoop& counter) {
                return offsetFromCounter({ inputs.proc, inputs.phiChildren, inputs.region, invariance, counter, noBlocks, true }, rangeCheck->index);
            };
            int64_t strict = counters->isStrict;
            if (auto offset = offsetFrom(counters->rising)) {
                plan.entryGuards.append({ GreaterEqual, low, Signedness::Signed, nullptr, Signedness::Signed, -*offset });
                plan.entryGuards.append({ LessEqual, high, Signedness::Signed, limit, Signedness::Unsigned, -*offset - 1 + strict });
            } else if (auto offset = offsetFrom(counters->falling)) {
                plan.entryGuards.append({ GreaterEqual, low, Signedness::Signed, nullptr, Signedness::Signed, -*offset - strict });
                plan.entryGuards.append({ LessEqual, high, Signedness::Signed, limit, Signedness::Unsigned, -*offset - 1 });
            } else
                continue;
            if (!limits.contains(limit))
                limits.append(limit);
            plan.decidedComparisons.add(value, rangeCheck->isTrueInBounds);
        }
    }
    if (plan.entryGuards.isEmpty())
        return plan;
    for (Value* limit : limits)
        plan.entryGuards.insert(0, EntryGuard { BelowEqual, limit, Signedness::Unsigned, nullptr, Signedness::Signed, maxLimitForRangeChecks(counters->rising.type()) });
    plan.entryGuards.append({ counters->isStrict ? LessThan : LessEqual, low, Signedness::Signed, high, Signedness::Signed, 0 });
    return plan;
}

void appendEntryGuards(FastPreHeader& front, Origin origin, std::span<const EntryGuard> guards, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Procedure& proc = front.procedure();
    auto widen = [&](Value* value, Signedness signedness) -> Value* {
        value = valueInPreHeader(value);
        if (value->type() == Int64)
            return value;
        return front.last()->appendNew<Value>(proc, signedness == Signedness::Signed ? SExt32 : ZExt32, origin, value);
    };
    for (const EntryGuard& guard : guards) {
        Value* left = widen(guard.left, guard.leftSignedness);
        Value* right = front.appendIntConstant(origin, Int64, guard.addend);
        if (guard.right)
            right = front.last()->appendNew<Value>(proc, Add, origin, widen(guard.right, guard.rightSignedness), right);
        front.appendGuard(origin, guard.relation, left, right);
    }
}

static Opcode safeBoundRelation(const CountedLoop& loop)
{
    if (loop.increases())
        return loop.isUnsigned() ? BelowEqual : LessEqual;
    return loop.isUnsigned() ? AboveEqual : GreaterEqual;
}

static Value* appendClampedBound(FastPreHeader& front, const RangeCheckPlan& plan, const CountedLoop& loop, Value* bound, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Procedure& proc = front.procedure();
    Origin origin = loop.test->last()->origin();
    int64_t maxLimit = maxLimitForRangeChecks(loop.type());
    auto constant = [&](int64_t value) {
        return front.appendIntConstant(origin, Int64, value);
    };
    auto widen = [&](Value* value, Signedness signedness) -> Value* {
        if (value->type() == Int64)
            return value;
        return front.last()->appendNew<Value>(proc, signedness == Signedness::Signed ? SExt32 : ZExt32, origin, value);
    };

    Value* clamped = widen(bound, loop.signedness());
    if (loop.isUnsigned())
        front.appendGuard(origin, BelowEqual, clamped, constant(maxLimit));
    Value* startInPreHeader = valueInPreHeader(loop.start);
    Value* start = widen(startInPreHeader, Signedness::Signed);
    for (auto& rangeCheckBound : plan.bounds) {
        Value* limit = widen(valueInPreHeader(rangeCheckBound.limit), Signedness::Unsigned);
        if (!loop.increases()) {
            front.appendGuard(origin, BelowEqual, limit, constant(maxLimit));
            front.appendGuard(origin, LessThan, start, front.last()->appendNew<Value>(proc, Sub, origin, limit, constant(rangeCheckBound.maxOffset)));
            Value* first = constant(-rangeCheckBound.minOffset - !loop.includesEqual());
            clamped = front.last()->appendNew<Value>(proc, Select, origin, front.last()->appendNew<Value>(proc, GreaterThan, origin, first, clamped), first, clamped);
            continue;
        }
        front.appendGuard(origin, GreaterEqual, start, constant(-rangeCheckBound.minOffset));
        front.appendGuard(origin, BelowEqual, limit, constant(maxLimit));
        Value* last = front.last()->appendNew<Value>(proc, Sub, origin, limit, constant(rangeCheckBound.maxOffset + loop.includesEqual()));
        front.appendGuard(origin, GreaterEqual, last, constant(0));
        clamped = front.last()->appendNew<Value>(proc, Select, origin, front.last()->appendNew<Value>(proc, LessThan, origin, last, clamped), last, clamped);
    }
    if (!loop.isTopTested)
        front.appendGuard(origin, loop.relation, widen(startInPreHeader, loop.signedness()), clamped);
    if (loop.type() == Int64)
        return clamped;
    return front.last()->appendNew<Value>(proc, Trunc, origin, clamped);
}

FastLoopBounds appendEntryGuards(FastPreHeader& front, const RangeCheckPlan& plan, const CountedLoop& loop, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Origin origin = loop.test->last()->origin();
    Value* bound = valueInPreHeader(loop.bound);
    Value* clamped = plan.clampsBound() ? appendClampedBound(front, plan, loop, bound, valueInPreHeader) : bound;
    if (!loop.cannotWrap())
        front.appendGuard(origin, safeBoundRelation(loop), clamped, front.appendIntConstant(origin, loop.type(), static_cast<int64_t>(loop.farthestSafeBound)));
    return { bound, plan.clampsBound() ? clamped : nullptr };
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
