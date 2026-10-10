/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CountedLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
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

UpsilonsByPhi collectUpsilonsByPhi(Procedure& proc)
{
    UpsilonsByPhi result;
    for (Value* value : proc.values()) {
        if (auto* upsilon = value->as<UpsilonValue>())
            result.add(upsilon->phi(), Vector<UpsilonValue*, 2>()).iterator->value.append(upsilon);
    }
    return result;
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
    if (!result.preHeader || result.preHeader->numSuccessors() != 1 || !result.latch || result.latch->numSuccessors() != 1 || result.latch->numPredecessors() != 1)
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
    if (!result.inductionVariable)
        return std::nullopt;
    for (UpsilonValue* upsilon : upsilonsByPhi.get(result.inductionVariable)) {
        if (upsilon->owner == result.preHeader)
            result.start = upsilon->child(0);
        else if (upsilon->owner != result.latch)
            return std::nullopt;
    }
    if (!result.start)
        return std::nullopt;

    Value* increment = result.next;
    if (increment->opcode() == Phi) {
        auto upsilons = upsilonsByPhi.get(increment);
        if (upsilons.size() != 2)
            return std::nullopt;
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
    int64_t noStrip = loop.isUnsigned() ? (isIncreasing ? 0 : -1) : isIncreasing ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    return append(proc.add<Value>(Select, origin, append(proc.add<Value>(loop.relation, origin, from, bound)), end, constant(noStrip)));
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
