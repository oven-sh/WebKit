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
#include "B3LoopUtilities.h"
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

static bool isSameValueInPreHeader(const LoopInvariance& invariance, Value* other, Value* value)
{
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
            Value* left = isSameValueInPreHeader(invariance, bound, value->child(0)) ? bound : value->child(0);
            Value* right = isSameValueInPreHeader(invariance, bound, value->child(1)) ? bound : value->child(1);
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

static void noteWhatBoundsTurns(const Procedure& proc, Value* limit, RangeCheckPlan& plan)
{
    if (plan.endsWithinData && !plan.lengthThatArrayMustHold)
        return;
    auto endsWithoutGuard = [&] {
        plan.endsWithinData = true;
        plan.lengthThatArrayMustHold = nullptr;
    };
    if (intervalFor(proc, limit, Signedness::Unsigned).max <= Limits::maxTurnsWithoutPoll)
        return endsWithoutGuard();
    while (limit->opcode() == ZExt32 || limit->opcode() == SExt32)
        limit = limit->child(0);
    auto* load = limit->as<MemoryValue>();
    if (!load || !load->isLoad())
        return;
    const AbstractHeapRepository& heaps = proc.heaps();
    if (heaps.holdsLengthOfData(load->range()))
        return endsWithoutGuard();
    if (!plan.endsWithinData && load->type() == Int32 && heaps.Butterfly_publicLength.hasRange() && heaps.rangeOf(heaps.Butterfly_publicLength) == load->range()) {
        plan.endsWithinData = true;
        plan.lengthThatArrayMustHold = load;
    }
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

namespace {

struct CounterEnd {
    Value* value { nullptr };
    int64_t addend { 0 };
};

} // anonymous namespace

static CounterEnd minCounter(const CountedLoop& loop)
{
    if (loop.increases())
        return { loop.start, 0 };
    return { loop.bound, loop.includesEqual() ? 0 : 1 };
}

static CounterEnd maxCounter(const CountedLoop& loop)
{
    if (!loop.increases())
        return { loop.start, 0 };
    return { loop.bound, loop.includesEqual() ? 0 : -1 };
}

static CheckedRange rangeOf(const Procedure& proc, const CountedLoop& loop, CounterEnd end)
{
    return checkedAdd(intervalFor(proc, end.value, loop.signedness()), checkedConstant(end.addend));
}

static bool planStridedGuards(const Procedure& proc, const CountedLoop& loop, StridedRangeCheck& check)
{
    CheckedRange scale = checkedRangeOf(proc, check.index.scale);
    CheckedRange offset = checkedRangeOf(proc, check.index.offset);
    if (!scale || (!scale->min && !scale->max))
        return false;
    check.isIncreasing = scale->max > 0;
    check.needsScaleGuard = check.isIncreasing ? scale->min <= 0 : !scale->max;
    if (check.isIncreasing)
        scale->min = std::max<Int128>(scale->min, 1);
    else
        scale->max = std::min<Int128>(scale->max, -1);

    auto indexAt = [&](CounterEnd end, int64_t constant) {
        return checkedAdd(checkedAdd(checkedMultiply(scale, rangeOf(proc, loop, end)), offset), checkedConstant(constant));
    };
    CheckedRange lowest = indexAt(check.isIncreasing ? minCounter(loop) : maxCounter(loop), check.minOffset);
    CheckedRange highest = indexAt(check.isIncreasing ? maxCounter(loop) : minCounter(loop), check.maxOffset);
    if (!lowest || !highest || lowest->max < 0)
        return false;
    check.needsLowerGuard = lowest->min < 0;
    int64_t maxInt32Limit = maxLimitForRangeChecks(Int32);
    check.needsLimitGuard = check.mustFitInt32 && highest->max > maxInt32Limit && intervalFor(proc, check.limit, Signedness::Unsigned).max > maxInt32Limit;
    return true;
}

static std::optional<AffineIndex> affineIndexInLoop(const RangeCheckInputs& inputs, Value* value)
{
    const Procedure& proc = inputs.proc;
    const CountedLoop& loop = inputs.loop;
    const LoopInvariance& invariance = inputs.invariance;
    CheckedRange lowestCounter = rangeOf(proc, loop, minCounter(loop));
    CheckedRange highestCounter = rangeOf(proc, loop, maxCounter(loop));
    if (!lowestCounter || !highestCounter || lowestCounter->min > highestCounter->max)
        return std::nullopt;
    return affineIndexOf({ proc, loop.inductionVariable, { lowestCounter->min, highestCounter->max }, [&](Value* value) -> Value* {
        if (!invariance.isInvariant(value))
            return nullptr;
        Value* canonical = invariance.canonicalValue(value);
        if (!invariance.isAvailableInPreHeader(canonical) || isStoredInLoop(*inputs.region.loop, canonical))
            return nullptr;
        return canonical;
    } }, value);
}

static void planStridedRangeCheck(const RangeCheckInputs& inputs, RangeCheckPlan& plan, Value* comparison, const RangeCheck& rangeCheck, Value* limit)
{
    const Procedure& proc = inputs.proc;
    const CountedLoop& loop = inputs.loop;
    const LoopInvariance& invariance = inputs.invariance;
    Value* index = rangeCheck.index->foldIdentity();
    bool isSignExtended = index->opcode() == SExt32;
    if (isSignExtended || index->opcode() == ZExt32)
        index = index->child(0);
    auto affineIndex = affineIndexInLoop(inputs, index);
    if (!affineIndex)
        return;
    int64_t offset = std::exchange(affineIndex->offset.constant, 0);

    size_t found = plan.stridedChecks.findIf([&](auto& check) {
        return isSameValueInPreHeader(invariance, check.limit, limit) && check.index.scale == affineIndex->scale && check.index.offset == affineIndex->offset;
    });
    StridedRangeCheck check;
    if (found != notFound)
        check = plan.stridedChecks[found];
    else {
        if (plan.bounds.size() + plan.stridedChecks.size() >= proc.limit(Limit::MaxRangeCheckBoundsPerLoop)) {
            proc.didReachLimit(Limit::MaxRangeCheckBoundsPerLoop, comparison->origin());
            return;
        }
        check = { WTF::move(*affineIndex), limit, offset, offset };
    }
    check.minOffset = std::min(check.minOffset, offset);
    check.maxOffset = std::max(check.maxOffset, offset);
    check.mustFitInt32 |= isSignExtended;
    if (!planStridedGuards(proc, loop, check))
        return;

    if (found != notFound)
        plan.stridedChecks[found] = WTF::move(check);
    else {
        plan.stridedChecks.append(WTF::move(check));
        noteWhatBoundsTurns(proc, limit, plan);
    }
    plan.decidedComparisons.add(comparison, rangeCheck.isTrueInBounds);
}

static bool isFoldedIntoAddress(const AffineIndex& index, unsigned shift, Type counterType)
{
    constexpr int64_t maxScale = 1 << 16;
    if (!index.scale.isConstant() || index.scale.constant < 0 || index.scale.constant > maxScale)
        return false;
    uint64_t bytesPerStep = static_cast<uint64_t>(index.scale.constant) << shift;
    if (!hasOneBitSet(bytesPerStep))
        return false;
    if (isX86())
        return bytesPerStep <= 8;
    return counterType == Int64 && index.offset.terms.isEmpty();
}

static bool isEmptyBehindBottomTest(const RangeCheckInputs& inputs)
{
    const CountedLoop& loop = inputs.loop;
    unsigned numberOfBlocks = 0;
    for (BasicBlock* block = loop.latch; block != loop.header; block = block->successorBlock(0)) {
        if (!inputs.region.contains(block) || block->last()->opcode() != Jump || ++numberOfBlocks > inputs.region.blocks.size())
            return false;
        for (Value* value : *block) {
            if (emitsInstructions(value))
                return false;
        }
    }
    return true;
}

static void planStrengthReduction(const RangeCheckInputs& inputs, RangeCheckPlan& plan)
{
    const CountedLoop& loop = inputs.loop;
    if (loop.isTopTested) {
        for (BasicBlock* predecessor : loop.header->predecessors()) {
            if (inputs.region.contains(predecessor) && predecessor->last()->opcode() != Jump)
                return;
        }
    } else {
        if (!isEmptyBehindBottomTest(inputs))
            return;
        plan.indexStepBlock = loop.test;
    }
    UncheckedKeyHashMap<Value*, unsigned> shifts;
    for (BasicBlock* block : inputs.region.blocks) {
        for (Value* value : *block) {
            if (value->opcode() == Shl && value->child(1)->hasInt32())
                shifts.add(value->child(0), 0).iterator->value = value->child(1)->asInt32() & 63;
        }
    }
    for (BasicBlock* block : inputs.region.blocks) {
        if (loop.isTopTested && inputs.blocksBeforeTopTest.contains(block))
            continue;
        for (Value* value : *block) {
            if (value->type() != Int64)
                continue;
            bool isExtension = value->opcode() == SExt32 || value->opcode() == ZExt32;
            if (!isExtension && value->opcode() != Add && value->opcode() != Sub && value->opcode() != Mul && value->opcode() != Shl)
                continue;
            auto affineIndex = affineIndexInLoop(inputs, isExtension ? value->child(0) : value);
            if (!affineIndex)
                continue;
            int64_t offset = std::exchange(affineIndex->offset.constant, 0);
            size_t found = plan.stridedChecks.findIf([&](auto& check) {
                return check.index.scale == affineIndex->scale && check.index.offset == affineIndex->offset && offset >= check.minOffset && offset <= check.maxOffset;
            });
            if (found == notFound || isFoldedIntoAddress(*affineIndex, shifts.get(value), loop.inductionVariable->type()))
                continue;
            StridedRangeCheck check = plan.stridedChecks[found];
            check.mustFitInt32 |= isExtension;
            check.isStrengthReduced = true;
            if (!planStridedGuards(inputs.proc, loop, check))
                continue;
            plan.stridedChecks[found] = WTF::move(check);
            plan.reducedIndices.add(value, ReducedIndex { static_cast<unsigned>(found), offset });
        }
    }
}

static Opcode safeBoundRelation(const CountedLoop& loop)
{
    if (loop.increases())
        return loop.isUnsigned() ? BelowEqual : LessEqual;
    return loop.isUnsigned() ? AboveEqual : GreaterEqual;
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
            if (!invariance.isInvariant(limit) || value == loop.test->last()->child(0))
                continue;
            if (isStoredInLoop(*inputs.region.loop, limit))
                continue;
            limit = invariance.canonicalValue(limit);
            if (!invariance.isAvailableInPreHeader(limit))
                continue;
            std::optional<int64_t> offset;
            if (rangeCheck->index->type() == loop.type())
                offset = offsetFromCounter(inputs, rangeCheck->index);
            if (!offset) {
                planStridedRangeCheck(inputs, plan, value, *rangeCheck, limit);
                continue;
            }
            if (maxStart + *offset < 0)
                continue;
            size_t found = plan.bounds.findIf([&](auto& bound) { return bound.limit == limit; });
            if (found == notFound) {
                if (plan.bounds.size() == proc.limit(Limit::MaxRangeCheckBoundsPerLoop)) {
                    proc.didReachLimit(Limit::MaxRangeCheckBoundsPerLoop, value->origin());
                    continue;
                }
                found = plan.bounds.size();
                plan.bounds.append(RangeCheckBound { limit, *offset, *offset });
                noteWhatBoundsTurns(proc, limit, plan);
            }
            plan.bounds[found].minOffset = std::min(plan.bounds[found].minOffset, *offset);
            plan.bounds[found].maxOffset = std::max(plan.bounds[found].maxOffset, *offset);
            plan.decidedComparisons.add(value, rangeCheck->isTrueInBounds);
        }
    }

    if (plan.stridedChecks.isEmpty())
        return;
    planStrengthReduction(inputs, plan);
    if (plan.clampsBound())
        return;
    Signedness signedness = loop.signedness();
    if (!loop.isTopTested && !plan.relationHoldsOnEntry)
        plan.entryGuards.append({ loop.relation, loop.start, signedness, loop.bound, signedness, 0 });
    if (!loop.cannotWrap())
        plan.entryGuards.append({ safeBoundRelation(loop), loop.bound, signedness, nullptr, Signedness::Signed, static_cast<int64_t>(loop.farthestSafeBound) });
}

RangeCheckPlan planRangeChecks(const RangeCheckInputs& inputs)
{
    RangeCheckPlan plan;
    decideComparisonsWithBound(inputs, plan);
    findRangeChecks(inputs, plan);
    const CountedLoop& loop = inputs.loop;
    if (inputs.canClampBound && loop.increases() && loop.cannotWrap() && (loop.isUnsigned() || intervalFor(inputs.proc, loop.start, Signedness::Signed).min >= 0)) {
        // What the fast version compares its counter with is what the bound was in front of it, whatever an unlikely path stores there later.
        Value* bound = inputs.invariance.canonicalValue(loop.bound);
        if (inputs.invariance.isAvailableInPreHeader(bound))
            noteWhatBoundsTurns(inputs.proc, bound, plan);
    }
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
    for (Value* limit : limits) {
        plan.entryGuards.insert(0, EntryGuard { BelowEqual, limit, Signedness::Unsigned, nullptr, Signedness::Signed, maxLimitForRangeChecks(counters->rising.type()) });
        noteWhatBoundsTurns(inputs.proc, limit, plan);
    }
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

Vector<StridedIndexStart, 2> appendStridedRangeCheckGuards(FastPreHeader& front, std::span<const StridedRangeCheck> checks, const CountedLoop& loop, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Procedure& proc = front.procedure();
    Origin origin = loop.test->last()->origin();
    auto constant = [&](int64_t value) {
        return front.appendIntConstant(origin, Int64, value);
    };
    auto append = [&](Opcode opcode, Value* left, Value* right) -> Value* {
        return front.last()->appendNew<Value>(proc, opcode, origin, left, right);
    };
    auto addConstant = [&](Value* value, int64_t addend) {
        return addend ? append(Add, value, constant(addend)) : value;
    };
    auto widen = [&](Value* value, Signedness signedness) -> Value* {
        value = valueInPreHeader(value);
        if (value->type() == Int64)
            return value;
        return front.last()->appendNew<Value>(proc, signedness == Signedness::Signed ? SExt32 : ZExt32, origin, value);
    };
    auto appendSum = [&](const InvariantSum& sum) -> Value* {
        Value* result = nullptr;
        for (const InvariantTerm& term : sum.terms) {
            Value* product = widen(term.first, Signedness::Signed);
            if (term.second)
                product = append(Mul, product, widen(term.second, Signedness::Signed));
            product = append(Mul, product, constant(term.coefficient));
            result = result ? append(Add, result, product) : product;
        }
        return result ? addConstant(result, sum.constant) : constant(sum.constant);
    };

    Vector<StridedIndexStart, 2> starts;
    for (const StridedRangeCheck& check : checks) {
        Value* scale = appendSum(check.index.scale);
        if (check.needsScaleGuard)
            front.appendGuard(origin, check.isIncreasing ? GreaterThan : LessThan, scale, constant(0));
        Value* offset = appendSum(check.index.offset);
        auto indexAt = [&](CounterEnd end, int64_t addend) {
            Value* counter = addConstant(widen(end.value, loop.signedness()), end.addend);
            return addConstant(append(Add, append(Mul, scale, counter), offset), addend);
        };
        if (check.needsLowerGuard)
            front.appendGuard(origin, GreaterEqual, indexAt(check.isIncreasing ? minCounter(loop) : maxCounter(loop), check.minOffset), constant(0));
        Value* limit = widen(check.limit, Signedness::Unsigned);
        if (check.needsLimitGuard)
            front.appendGuard(origin, BelowEqual, limit, constant(maxLimitForRangeChecks(Int32)));
        front.appendGuard(origin, LessThan, indexAt(check.isIncreasing ? maxCounter(loop) : minCounter(loop), check.maxOffset), limit);
        if (check.isStrengthReduced)
            starts.append({ indexAt({ loop.start, 0 }, 0), append(Mul, scale, constant(loop.step)) });
        else
            starts.append({ });
    }
    return starts;
}

void appendGuardThatArrayHoldsLength(FastPreHeader& front, Origin origin, MemoryValue* length, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Procedure& proc = front.procedure();
    const AbstractHeapRepository& heaps = proc.heaps();
    auto offset = static_cast<MemoryValue::OffsetType>(length->offset() + heaps.Butterfly_vectorLength.offset() - heaps.Butterfly_publicLength.offset());
    Value* room = front.last()->appendNew<MemoryValue>(proc, Load, Int32, origin, valueInPreHeader(length->lastChild()), offset, heaps.rangeOf(heaps.Butterfly_vectorLength));
    front.appendGuard(origin, BelowEqual, valueInPreHeader(length), room);
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
