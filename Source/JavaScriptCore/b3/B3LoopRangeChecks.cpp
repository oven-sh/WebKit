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
#include "B3VariableValue.h"
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

static CounterEnd counterEnd(const CountedLoop& loop, bool isMax)
{
    if (isMax != loop.increases())
        return { loop.start, 0 };
    if (loop.includesEqual())
        return { loop.bound, 0 };
    return { loop.bound, isMax ? -1 : 1 };
}

static CheckedRange rangeOf(const Procedure& proc, const CountedLoop& loop, CounterEnd end)
{
    return checkedAdd(checkedRangeOf(proc, end.value, loop.signedness()), IntRange(end.addend, end.addend));
}

static bool planStridedGuards(const Procedure& proc, const CountedLoop& loop, const AffineIndex& index, Value* limit, StridedGuards& guards)
{
    CheckedRange scale = checkedRangeOf(proc, index.scale);
    if (!scale || (!scale->min() && !scale->max()))
        return false;
    guards.isIncreasing = scale->max() > 0;
    guards.needsScaleGuard = guards.isIncreasing ? scale->min() <= 0 : !scale->max();
    scale = guards.isIncreasing ? IntRange(std::max<int64_t>(scale->min(), 1), scale->max()) : IntRange(scale->min(), std::min<int64_t>(scale->max(), -1));

    CheckedRange offset = checkedRangeOf(proc, index.offset);
    CheckedRange ends[2];
#pragma clang loop unroll(disable)
    for (unsigned isHighest = 0; isHighest < 2; ++isHighest) {
        int64_t constant = isHighest ? guards.maxOffset : guards.minOffset;
        CheckedRange counter = rangeOf(proc, loop, counterEnd(loop, guards.isIncreasing == !!isHighest));
        ends[isHighest] = checkedAdd(checkedAdd(checkedMultiply(scale, counter), offset), IntRange(constant, constant));
        if (!ends[isHighest])
            return false;
    }
    if (ends[0]->max() < 0)
        return false;
    guards.needsLowerGuard = ends[0]->min() < 0;
    int64_t maxInt32Limit = maxLimitForRangeChecks(Int32);
    guards.needsLimitGuard = guards.mustFitInt32 && ends[1]->max() > maxInt32Limit && intervalFor(proc, limit, Signedness::Unsigned).max > maxInt32Limit;
    return true;
}

static bool findAffineIndexInLoop(const RangeCheckInputs& inputs, Value* value, AffineIndex& result)
{
    const Procedure& proc = inputs.proc;
    const CountedLoop& loop = inputs.loop;
    const LoopInvariance& invariance = inputs.invariance;
    CheckedRange lowestCounter = rangeOf(proc, loop, counterEnd(loop, false));
    CheckedRange highestCounter = rangeOf(proc, loop, counterEnd(loop, true));
    if (!lowestCounter || !highestCounter || lowestCounter->min() > highestCounter->max())
        return false;
    if (value->opcode() == SExt32 || value->opcode() == ZExt32)
        value = value->child(0);
    return findAffineIndex({ proc, loop.inductionVariable, IntRange(lowestCounter->min(), highestCounter->max()), [&](Value* value) -> Value* {
        if (!invariance.isInvariant(value))
            return nullptr;
        Value* canonical = invariance.canonicalValue(value);
        if (!invariance.isAvailableInPreHeader(canonical) || isStoredInLoop(*inputs.region.loop, canonical))
            return nullptr;
        return canonical;
    } }, value, result);
}

static void planStridedRangeCheck(const RangeCheckInputs& inputs, RangeCheckPlan& plan, Value* comparison, const RangeCheck& rangeCheck, Value* limit)
{
    const Procedure& proc = inputs.proc;
    Value* value = rangeCheck.index->foldIdentity();
    AffineIndex index;
    if (!findAffineIndexInLoop(inputs, value, index))
        return;
    int64_t offset = std::exchange(index.offset.constant, 0);

    if (!plan.strided)
        plan.strided = makeUnique<StridedRangeChecks>();
    StridedRangeChecks& strided = *plan.strided;
    unsigned found = 0;
#pragma clang loop unroll(disable)
    for (; found < strided.numberOfChecks; ++found) {
        const StridedRangeCheck& other = strided.checks[found];
        if (isSameValueInPreHeader(inputs.invariance, other.limit, limit) && isSameExceptConstantOffset(other.index, index))
            break;
    }
    StridedRangeCheck& check = strided.checks[found < strided.checks.size() ? found : 0];
    bool isNew = found == strided.numberOfChecks;
    StridedGuards guards = check;
    if (isNew) {
        if (plan.bounds.size() + found >= std::min<uint64_t>(proc.limit(Limit::MaxRangeCheckBoundsPerLoop), strided.checks.size())) {
            proc.didReachLimit(Limit::MaxRangeCheckBoundsPerLoop, comparison->origin());
            return;
        }
        guards = { offset, offset };
    }
    guards.minOffset = std::min(guards.minOffset, offset);
    guards.maxOffset = std::max(guards.maxOffset, offset);
    guards.mustFitInt32 |= value->opcode() == SExt32;
    if (!planStridedGuards(proc, inputs.loop, index, limit, guards))
        return;

    static_cast<StridedGuards&>(check) = guards;
    if (isNew) {
        check.index = index;
        check.limit = limit;
        ++strided.numberOfChecks;
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
    return counterType == Int64 && index.offset.isConstant();
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

static unsigned shiftOfUserInRegion(const HotRegion& region, Value* index)
{
    unsigned shift = 0;
    for (BasicBlock* block : region.blocks) {
        for (Value* value : *block) {
            if (value->opcode() == Shl && value->child(0) == index && value->child(1)->hasInt32())
                shift = value->child(1)->asInt32() & 63;
        }
    }
    return shift;
}

static void planStrengthReduction(const RangeCheckInputs& inputs, StridedRangeChecks& strided)
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
        strided.indexStepBlock = loop.test;
    }
    for (BasicBlock* block : inputs.region.blocks) {
        if (loop.isTopTested && inputs.blocksBeforeTopTest.contains(block))
            continue;
        for (Value* value : *block) {
            if (value->type() != Int64 || strided.numberOfReducedIndices == strided.reducedIndices.size())
                continue;
            bool isExtension = value->opcode() == SExt32 || value->opcode() == ZExt32;
            if (!isExtension && value->opcode() != Add && value->opcode() != Sub && value->opcode() != Mul && value->opcode() != Shl)
                continue;
            AffineIndex index;
            if (!findAffineIndexInLoop(inputs, value, index))
                continue;
            int64_t offset = index.offset.constant;
            unsigned found = 0;
#pragma clang loop unroll(disable)
            for (; found < strided.numberOfChecks; ++found) {
                const StridedRangeCheck& check = strided.checks[found];
                if (isSameExceptConstantOffset(check.index, index) && offset >= check.minOffset && offset <= check.maxOffset)
                    break;
            }
            if (found == strided.numberOfChecks || isFoldedIntoAddress(index, shiftOfUserInRegion(inputs.region, value), loop.inductionVariable->type()))
                continue;
            StridedRangeCheck& check = strided.checks[found];
            StridedGuards guards = check;
            guards.mustFitInt32 |= isExtension;
            guards.isStrengthReduced = true;
            if (!planStridedGuards(inputs.proc, loop, check.index, check.limit, guards))
                continue;
            static_cast<StridedGuards&>(check) = guards;
            strided.reducedIndices[strided.numberOfReducedIndices++] = { value, found, offset };
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
            }
            plan.bounds[found].minOffset = std::min(plan.bounds[found].minOffset, *offset);
            plan.bounds[found].maxOffset = std::max(plan.bounds[found].maxOffset, *offset);
            plan.decidedComparisons.add(value, rangeCheck->isTrueInBounds);
        }
    }

    if (!plan.strided)
        return;
    if (!plan.strided->numberOfChecks) {
        plan.strided = nullptr;
        return;
    }
    planStrengthReduction(inputs, *plan.strided);
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
    for (const EntryGuard& guard : guards) {
        Value* left = front.appendWidened(origin, valueInPreHeader(guard.left), guard.leftSignedness);
        Value* right = front.appendIntConstant(origin, Int64, guard.addend);
        if (guard.right)
            right = front.append(origin, Add, front.appendWidened(origin, valueInPreHeader(guard.right), guard.rightSignedness), right);
        front.appendGuard(origin, guard.relation, left, right);
    }
}

static Value* appendSum(FastPreHeader& front, Origin origin, const InvariantSum& sum, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Value* result = nullptr;
#pragma clang loop unroll(disable)
    for (const InvariantTerm& term : sum.usedTerms()) {
        Value* product = front.appendWidened(origin, valueInPreHeader(term.first), Signedness::Signed);
        if (term.second)
            product = front.append(origin, Mul, product, front.appendWidened(origin, valueInPreHeader(term.second), Signedness::Signed));
        product = front.append(origin, Mul, product, front.appendIntConstant(origin, Int64, term.coefficient));
        result = result ? front.append(origin, Add, result, product) : product;
    }
    return result ? front.appendAddConstant(origin, result, sum.constant) : front.appendIntConstant(origin, Int64, sum.constant);
}

namespace {

struct StridedIndexAppender {
    NEVER_INLINE Value* indexAt(CounterEnd end, int64_t addend) const
    {
        Value* counter = front.appendAddConstant(origin, front.appendWidened(origin, valueInPreHeader(end.value), signedness), end.addend);
        return front.appendAddConstant(origin, front.append(origin, Add, front.append(origin, Mul, scale, counter), offset), addend);
    }

    FastPreHeader& front;
    Origin origin;
    Signedness signedness;
    const ScopedLambda<Value*(Value*)>& valueInPreHeader;
    Value* scale;
    Value* offset;
};

} // anonymous namespace

void appendStridedRangeCheckGuards(FastPreHeader& front, const StridedRangeChecks& strided, const CountedLoop& loop, const ScopedLambda<Value*(Value*)>& valueInPreHeader, StridedIndexVariables& variables)
{
    Procedure& proc = front.procedure();
    Origin origin = loop.test->last()->origin();
    std::array<Value*, Limits::maxStridedRangeChecksPerLoop> firstIndices { };
    for (unsigned i = 0; i < strided.numberOfChecks; ++i) {
        const StridedRangeCheck& check = strided.checks[i];
        Value* scale = appendSum(front, origin, check.index.scale, valueInPreHeader);
        if (check.needsScaleGuard)
            front.appendGuard(origin, check.isIncreasing ? GreaterThan : LessThan, scale, front.appendIntConstant(origin, Int64, 0));
        StridedIndexAppender appender { front, origin, loop.signedness(), valueInPreHeader, scale, appendSum(front, origin, check.index.offset, valueInPreHeader) };
        if (check.needsLowerGuard)
            front.appendGuard(origin, GreaterEqual, appender.indexAt(counterEnd(loop, !check.isIncreasing), check.minOffset), front.appendIntConstant(origin, Int64, 0));
        Value* limit = front.appendWidened(origin, valueInPreHeader(check.limit), Signedness::Unsigned);
        if (check.needsLimitGuard)
            front.appendGuard(origin, BelowEqual, limit, front.appendIntConstant(origin, Int64, maxLimitForRangeChecks(Int32)));
        if (!proc.policy().validation.omitsStridedLimitGuard)
            front.appendGuard(origin, LessThan, appender.indexAt(counterEnd(loop, check.isIncreasing), check.maxOffset), limit);
        if (!check.isStrengthReduced)
            continue;
        firstIndices[i] = appender.indexAt({ loop.start, 0 }, 0);
        variables[i].stride = front.append(origin, Mul, scale, front.appendIntConstant(origin, Int64, loop.step));
    }
#pragma clang loop unroll(disable)
    for (unsigned i = 0; i < strided.numberOfChecks; ++i) {
        if (!firstIndices[i])
            continue;
        variables[i].variable = proc.addVariable(Int64);
        front.last()->appendNew<VariableValue>(proc, Set, origin, variables[i].variable, firstIndices[i]);
    }
}

static Value* appendClampedBound(FastPreHeader& front, const RangeCheckPlan& plan, const CountedLoop& loop, Value* bound, const ScopedLambda<Value*(Value*)>& valueInPreHeader)
{
    Procedure& proc = front.procedure();
    Origin origin = loop.test->last()->origin();
    int64_t maxLimit = maxLimitForRangeChecks(loop.type());
    auto constant = [&](int64_t value) {
        return front.appendIntConstant(origin, Int64, value);
    };
    auto widen = [&](Value* value, Signedness signedness) {
        return front.appendWidened(origin, value, signedness);
    };
    auto select = [&](Opcode relation, Value* candidate, Value* other) -> Value* {
        return front.last()->appendNew<Value>(proc, Select, origin, front.append(origin, relation, candidate, other), candidate, other);
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
            front.appendGuard(origin, LessThan, start, front.append(origin, Sub, limit, constant(rangeCheckBound.maxOffset)));
            clamped = select(GreaterThan, constant(-rangeCheckBound.minOffset - !loop.includesEqual()), clamped);
            continue;
        }
        front.appendGuard(origin, GreaterEqual, start, constant(-rangeCheckBound.minOffset));
        front.appendGuard(origin, BelowEqual, limit, constant(maxLimit));
        Value* last = front.append(origin, Sub, limit, constant(rangeCheckBound.maxOffset + loop.includesEqual()));
        front.appendGuard(origin, GreaterEqual, last, constant(0));
        clamped = select(LessThan, last, clamped);
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
