/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CountedLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

namespace {

std::optional<Opcode> invertedRelation(Opcode relation)
{
    switch (relation) {
    case LessThan:
        return GreaterEqual;
    case LessEqual:
        return GreaterThan;
    case GreaterThan:
        return LessEqual;
    case GreaterEqual:
        return LessThan;
    case Below:
        return AboveEqual;
    case BelowEqual:
        return Above;
    case Above:
        return BelowEqual;
    case AboveEqual:
        return Below;
    default:
        return std::nullopt;
    }
}

Opcode flippedRelation(Opcode relation)
{
    switch (relation) {
    case LessThan:
        return GreaterThan;
    case LessEqual:
        return GreaterEqual;
    case GreaterThan:
        return LessThan;
    case GreaterEqual:
        return LessEqual;
    case Below:
        return Above;
    case BelowEqual:
        return AboveEqual;
    case Above:
        return Below;
    default:
        return BelowEqual;
    }
}

} // anonymous namespace

const Vector<UpsilonValue*, 2>* UpsilonsByPhi::find(Value* phi) const
{
    if (!m_map.size()) {
        m_map.resize(m_proc.values().size());
        for (Value* value : m_proc.values()) {
            if (auto* upsilon = value->as<UpsilonValue>())
                m_map[upsilon->phi()].append(upsilon);
        }
    }
    auto& upsilons = m_map[phi];
    return upsilons.isEmpty() ? nullptr : &upsilons;
}

static Value* lastValueHandedTo(BasicBlock* block, Value* phi)
{
    for (unsigned index = block->size(); index--;) {
        auto* upsilon = block->at(index)->as<UpsilonValue>();
        if (upsilon && upsilon->phi() == phi)
            return upsilon->child(0);
    }
    return nullptr;
}

std::optional<CountedLoop> analyzeCountedLoop(NaturalLoops& loops, const NaturalLoop& loop, const UpsilonsByPhi& upsilonsByPhi, const ScopedLambda<bool(Value*)>& isInvariant)
{
    for (unsigned i = loop.size(); i--;) {
        if (loops.innerMostLoopOf(loop.at(i)) != &loop)
            return std::nullopt;
    }
    CountedLoop result;
    result.header = loop.header();
    for (BasicBlock* predecessor : loop.header()->predecessors()) {
        BasicBlock*& slot = loops.belongsTo(predecessor, loop) ? result.latch : result.preHeader;
        if (slot)
            return std::nullopt;
        slot = predecessor;
    }
    if (!result.preHeader || result.preHeader->last()->opcode() != Jump || !result.latch || result.latch->last()->opcode() != Jump || result.latch->numPredecessors() != 1)
        return std::nullopt;
    result.test = result.latch->predecessor(0);
    if (result.test->last()->opcode() != Branch)
        return std::nullopt;
    result.indexOfLatchInSuccessors = result.test->successorBlock(0) == result.latch ? 0 : 1;
    if (loops.belongsTo(result.test->successorBlock(1 - result.indexOfLatchInSuccessors), loop))
        return std::nullopt;

    Value* condition = result.test->last()->child(0);
    if (condition->numChildren() != 2 || condition->child(0)->type() != Int64)
        return std::nullopt;
    auto inverted = invertedRelation(condition->opcode());
    if (!inverted)
        return std::nullopt;
    result.relation = result.indexOfLatchInSuccessors ? *inverted : condition->opcode();
    bool boundIsFirst = isInvariant(condition->child(0));
    result.bound = condition->child(boundIsFirst ? 0 : 1);
    result.next = condition->child(boundIsFirst ? 1 : 0);
    if (boundIsFirst)
        result.relation = flippedRelation(result.relation);
    if (!isInvariant(result.bound) || isInvariant(result.next))
        return std::nullopt;

    for (Value* value : *result.latch) {
        auto* upsilon = value->as<UpsilonValue>();
        if (upsilon && upsilon->child(0) == result.next && upsilon->phi()->owner == loop.header())
            result.inductionVariable = upsilon->phi();
    }
    if (!result.inductionVariable || lastValueHandedTo(result.latch, result.inductionVariable) != result.next)
        return std::nullopt;
    result.start = lastValueHandedTo(result.preHeader, result.inductionVariable);
    if (!result.start || lastValueHandedTo(loop.header(), result.inductionVariable))
        return std::nullopt;

    Value* increment = result.next;
    if (increment->opcode() == Phi) {
        auto* found = upsilonsByPhi.find(increment);
        if (!found || found->size() != 2)
            return std::nullopt;
        auto& upsilons = *found;
        unsigned indexOfConstant = upsilons[0]->child(0)->hasInt64() ? 0 : 1;
        if (!upsilons[indexOfConstant]->child(0)->hasInt64())
            return std::nullopt;
        result.saturation = upsilons[indexOfConstant]->child(0)->asInt64();
        increment = upsilons[1 - indexOfConstant]->child(0);
    }
    if (increment->opcode() == SExt32 && increment->child(0)->opcode() == Trunc) {
        increment = increment->child(0)->child(0);
        result.wrapsAroundInt32 = true;
    }
    if (increment->opcode() != Add || increment->child(0) != result.inductionVariable || !increment->child(1)->hasInt64())
        return std::nullopt;
    result.step = increment->child(1)->asInt64();
    if (!result.step || result.step > maxIterationsPerStrip || result.step < -maxIterationsPerStrip || (result.step > 0) != result.increases())
        return std::nullopt;
    if (result.saturation && (result.isUnsigned() || *result.saturation == std::numeric_limits<int64_t>::min() || *result.saturation == std::numeric_limits<int64_t>::max()))
        return std::nullopt;
    return result;
}

std::optional<CountedLoop> analyzeCountedLoopTestedAtTheTop(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* preHeader, BasicBlock* test, Value* condition, const UpsilonsByPhi& upsilonsByPhi, const ScopedLambda<bool(Value*)>& isInvariant, const ScopedLambda<bool(BasicBlock*)>& isLeftOut)
{
    CountedLoop result;
    result.header = loop.header();
    result.preHeader = preHeader;
    result.test = test;
    bool firstStays = loops.belongsTo(test->successorBlock(0), loop);
    if (test->last()->opcode() != Branch || firstStays == loops.belongsTo(test->successorBlock(1), loop))
        return std::nullopt;
    result.indexOfLatchInSuccessors = firstStays ? 0 : 1;
    result.latch = test->successorBlock(result.indexOfLatchInSuccessors);
    if (result.latch == loop.header() || condition->numChildren() != 2 || condition->child(0)->type() != Int64)
        return std::nullopt;
    auto inverted = invertedRelation(condition->opcode());
    if (!inverted)
        return std::nullopt;
    result.relation = result.indexOfLatchInSuccessors ? *inverted : condition->opcode();
    bool boundIsFirst = isInvariant(condition->child(0));
    result.bound = condition->child(boundIsFirst ? 0 : 1);
    result.inductionVariable = condition->child(boundIsFirst ? 1 : 0);
    if (boundIsFirst)
        result.relation = flippedRelation(result.relation);
    if (!isInvariant(result.bound) || result.inductionVariable->opcode() != Phi || result.inductionVariable->owner != loop.header())
        return std::nullopt;
    auto* upsilons = upsilonsByPhi.find(result.inductionVariable);
    if (!upsilons)
        return std::nullopt;
    for (UpsilonValue* upsilon : *upsilons) {
        if (upsilon->owner == preHeader) {
            result.start = upsilon->child(0);
            continue;
        }
        if (!loops.belongsTo(upsilon->owner, loop))
            return std::nullopt;
        if (isLeftOut(upsilon->owner))
            continue;
        if (result.next && result.next != upsilon->child(0))
            return std::nullopt;
        result.next = upsilon->child(0);
    }
    if (!result.start || !result.next)
        return std::nullopt;
    if (result.next->opcode() != Add || result.next->child(0) != result.inductionVariable || !result.next->child(1)->hasInt64())
        return std::nullopt;
    result.step = result.next->child(1)->asInt64();
    if (!result.step || result.step > maxIterationsPerStrip || result.step < -maxIterationsPerStrip || (result.step > 0) != result.increases())
        return std::nullopt;
    return result;
}

bool relationHoldsOnEntry(const CountedLoop& loop)
{
    static constexpr unsigned maxNumberOfBlocks = 4;
    auto isSame = [](Value* a, Value* b) {
        return a == b || (a->hasInt64() && b->hasInt64() && a->asInt64() == b->asInt64());
    };
    BasicBlock* block = loop.preHeader;
    for (unsigned i = 0; i < maxNumberOfBlocks && block->numPredecessors() == 1; ++i) {
        BasicBlock* predecessor = block->predecessor(0);
        Value* condition = predecessor->last()->opcode() == Branch ? predecessor->last()->child(0) : nullptr;
        if (condition && condition->numChildren() == 2 && predecessor->successorBlock(0) != predecessor->successorBlock(1)) {
            if (auto inverted = invertedRelation(condition->opcode())) {
                Opcode relation = predecessor->successorBlock(0) == block ? condition->opcode() : *inverted;
                if (relation == loop.relation && isSame(condition->child(0), loop.start) && isSame(condition->child(1), loop.bound))
                    return true;
                if (flippedRelation(relation) == loop.relation && isSame(condition->child(1), loop.start) && isSame(condition->child(0), loop.bound))
                    return true;
            }
        }
        block = predecessor;
    }
    return false;
}

std::optional<bool> outcomeOfComparisonWithBound(const CountedLoop& loop, Opcode opcode, Value* left, Value* right, Value* bound)
{
    if (!invertedRelation(opcode))
        return std::nullopt;
    if (left == bound && right == loop.inductionVariable)
        opcode = flippedRelation(opcode);
    else if (left != loop.inductionVariable || right != bound)
        return std::nullopt;
    if (opcode == loop.relation)
        return true;
    if (*invertedRelation(opcode) == loop.relation)
        return false;
    return std::nullopt;
}

static std::optional<bool> outcomeOfComparisonWithLimit(const CountedLoop& loop, Opcode opcode, Value* left, Value* right, bool boundIsZeroExtendedWord = false)
{
    static constexpr int64_t maxConstant = 1ll << 31;
    if (!invertedRelation(opcode))
        return std::nullopt;
    auto isSmall = [](Value* value) {
        return value->hasInt64() && value->asInt64() >= -maxConstant && value->asInt64() <= maxConstant;
    };
    auto addedToVariable = [&](Value* value) -> std::optional<int64_t> {
        if (value == loop.inductionVariable)
            return 0;
        if (value->opcode() == Add && value->child(0) == loop.inductionVariable && isSmall(value->child(1)))
            return value->child(1)->asInt64();
        return std::nullopt;
    };
    Value* limit = right;
    std::optional<int64_t> added = addedToVariable(left);
    if (!added) {
        added = addedToVariable(right);
        opcode = flippedRelation(opcode);
        limit = left;
    }
    if (!added || (limit->opcode() != ZExt32 && !(boundIsZeroExtendedWord && limit == loop.bound)) || loop.saturation)
        return std::nullopt;
    int64_t least;
    int64_t mostLessLimit;
    if (loop.increases()) {
        if (loop.bound != limit || loop.admitsEqual() || loop.wrapsAroundInt32 || !isSmall(loop.start))
            return std::nullopt;
        least = loop.start->asInt64();
        mostLessLimit = -1;
    } else {
        Value* start = loop.start;
        if (loop.isUnsigned() || !isSmall(loop.bound) || start->opcode() != Add || start->child(0) != limit || !isSmall(start->child(1)))
            return std::nullopt;
        least = loop.bound->asInt64() + (loop.admitsEqual() ? 0 : 1);
        if (loop.wrapsAroundInt32 && least + loop.step < std::numeric_limits<int32_t>::min())
            return std::nullopt;
        mostLessLimit = start->child(1)->asInt64();
    }
    if (least + *added < 0 || mostLessLimit + *added >= 0)
        return std::nullopt;
    return opcode == LessThan || opcode == LessEqual || opcode == Below || opcode == BelowEqual;
}

static std::optional<CountedLoop> analyzeLoopWithLimitHandedRound(NaturalLoops& loops, const NaturalLoop& loop, const UpsilonsByPhi& upsilonsByPhi)
{
    auto result = analyzeCountedLoop(loops, loop, upsilonsByPhi, [&](Value* value) {
        return !loops.belongsTo(value->owner, loop) || value->opcode() == ZExt32;
    });
    if (!result || !result->increases() || result->bound->opcode() != ZExt32 || !loops.belongsTo(result->bound->owner, loop))
        return std::nullopt;
    Value* limit = nullptr;
    for (Value* value : *result->latch) {
        auto* upsilon = value->as<UpsilonValue>();
        if (upsilon && upsilon->child(0) == result->bound && upsilon->phi()->owner == loop.header())
            limit = upsilon->phi();
    }
    if (!limit || lastValueHandedTo(result->latch, limit) != result->bound || lastValueHandedTo(loop.header(), limit))
        return std::nullopt;
    auto* upsilons = upsilonsByPhi.find(limit);
    if (!upsilons || upsilons->size() != 2)
        return std::nullopt;
    Value* limitOnEntry = lastValueHandedTo(result->preHeader, limit);
    if (!limitOnEntry || limitOnEntry->opcode() != ZExt32)
        return std::nullopt;
    result->bound = limitOnEntry;
    if (!relationHoldsOnEntry(*result))
        return std::nullopt;
    result->bound = limit;
    return result;
}

bool foldComparisonsWithBounds(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldComparisonsWithBounds"_s);
    NaturalLoops& loops = proc.naturalLoops();
    if (!loops.numLoops())
        return false;
    proc.resetValueOwners();
    UpsilonsByPhi upsilonsByPhi(proc);
    InsertionSet insertionSet(proc);
    bool changed = false;
    for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        auto countedLoop = analyzeCountedLoop(loops, loop, upsilonsByPhi, [&](Value* value) {
            return !loops.belongsTo(value->owner, loop);
        });
        if (!countedLoop) {
            auto loopWithLimit = analyzeLoopWithLimitHandedRound(loops, loop, upsilonsByPhi);
            if (!loopWithLimit)
                continue;
            for (unsigned i = loop.size(); i--;) {
                BasicBlock* block = loop.at(i);
                for (unsigned index = 0; index < block->size(); ++index) {
                    Value* value = block->at(index);
                    if (value->numChildren() != 2)
                        continue;
                    auto outcome = outcomeOfComparisonWithLimit(*loopWithLimit, value->opcode(), value->child(0), value->child(1), true);
                    if (!outcome)
                        continue;
                    value->replaceWithIdentity(insertionSet.insertIntConstant(index, value, *outcome));
                    changed = true;
                }
                insertionSet.execute(block);
            }
            continue;
        }
        if (!relationHoldsOnEntry(*countedLoop))
            continue;
        for (unsigned i = loop.size(); i--;) {
            BasicBlock* block = loop.at(i);
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                if (value->numChildren() != 2)
                    continue;
                auto outcome = outcomeOfComparisonWithBound(*countedLoop, value->opcode(), value->child(0), value->child(1), countedLoop->bound);
                if (!outcome)
                    outcome = outcomeOfComparisonWithLimit(*countedLoop, value->opcode(), value->child(0), value->child(1));
                if (!outcome)
                    continue;
                value->replaceWithIdentity(insertionSet.insertIntConstant(index, value, *outcome));
                changed = true;
            }
            insertionSet.execute(block);
        }
    }
    return changed;
}

Value* appendEndOfStrip(Procedure& proc, BasicBlock* block, const CountedLoop& loop, Value* from, Value* bound, bool relationMayNotHold)
{
    Origin origin = from->origin();
    bool hasTerminal = block->size() && block->last()->effects().terminal;
    auto append = [&](Value* value) {
        if (hasTerminal)
            block->appendNonTerminal(value);
        else
            block->append(value);
        return value;
    };
    auto constant = [&](int64_t value) {
        return append(proc.add<Const64Value>(origin, value));
    };
    bool isIncreasing = loop.increases();
    // Or reduceStrength() goes back to what `from` is the sum of, which then lives on beside it.
    if (!from->isConstant())
        from = append(proc.add<Value>(Opaque, origin, from));
    // The distance has room in 64 bits as an unsigned integer.
    Value* distance = append(isIncreasing ? proc.add<Value>(Sub, origin, bound, from) : proc.add<Value>(Sub, origin, from, bound));
    Value* most = constant(maxIterationsPerStrip);
    Value* length = append(proc.add<Value>(Select, origin, append(proc.add<Value>(Below, origin, distance, most)), distance, most));
    Value* end = append(proc.add<Value>(isIncreasing ? Add : Sub, origin, from, length));
    if (loop.saturation) {
        Value* limit = constant(*loop.saturation + (loop.admitsEqual() ? (isIncreasing ? -1 : 1) : 0));
        end = append(proc.add<Value>(Select, origin, append(proc.add<Value>(isIncreasing ? LessThan : GreaterThan, origin, end, limit)), end, limit));
    }
    if (!relationMayNotHold)
        return end;
    if (isIncreasing && !loop.saturation && bound->opcode() == ZExt32 && from->hasInt64() && (loop.isUnsigned() ? !from->asInt64() : from->asInt64() <= 0))
        return end;
    int64_t noStrip = loop.isUnsigned() ? (isIncreasing ? 0 : -1) : isIncreasing ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    return append(proc.add<Value>(Select, origin, append(proc.add<Value>(loop.relation, origin, from, bound)), end, constant(noStrip)));
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
