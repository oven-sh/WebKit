/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CountedLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3DominatingConditions.h"
#include "B3LoopUtilities.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

Type CountedLoop::type() const
{
    return bound->type();
}

namespace {

struct AffineNode {
    Value* operand { nullptr };
    Int128 addend { 0 };
    IntegerInterval exactResults;
};

} // anonymous namespace

static std::optional<AffineNode> affineNodeFor(Value* value, Signedness signedness)
{
    if (!value->type().isInt())
        return std::nullopt;
    IntegerInterval allResults = fullInterval(value->type(), signedness);
    IntegerInterval nonNegativeInt32 { 0, std::numeric_limits<int32_t>::max() };
    switch (value->opcode()) {
    case Identity:
    case Trunc:
        return AffineNode { value->child(0), 0, allResults };
    case SExt32:
        return AffineNode { value->child(0), 0, signedness == Signedness::Signed ? allResults : nonNegativeInt32 };
    case ZExt32:
        return AffineNode { value->child(0), 0, signedness == Signedness::Unsigned ? allResults : nonNegativeInt32 };
    case Add: {
        unsigned constantIndex = value->child(1)->hasInt() ? 1 : 0;
        Value* constant = value->child(constantIndex);
        Value* operand = value->child(1 - constantIndex);
        if (!constant->hasInt() || operand->hasInt())
            return std::nullopt;
        return AffineNode { operand, constant->asInt(), allResults };
    }
    case Sub:
        if (!value->child(1)->hasInt())
            return std::nullopt;
        return AffineNode { value->child(0), -Int128 { value->child(1)->asInt() }, allResults };
    default:
        return std::nullopt;
    }
}

std::expected<AffineForm, GaveUpReason> affineFormOf(const Procedure& proc, Value* value, Value* base, Signedness signedness)
{
    if (!base->type().isInt())
        return makeUnexpected(GaveUpReason::NotCounted);

    Vector<AffineNode, 8> chain;
    while (value != base) {
        std::optional<AffineNode> node = affineNodeFor(value, signedness);
        if (!node)
            return makeUnexpected(GaveUpReason::NotCounted);
        if (chain.size() == proc.limit(Limit::MaxRangeAnalysisDepth)) {
            proc.didReachLimit(Limit::MaxRangeAnalysisDepth, value->origin());
            return makeUnexpected(GaveUpReason::NotCounted);
        }
        chain.append(*node);
        value = node->operand;
    }

    Int128 offset = 0;
    IntegerInterval exactBaseRange = fullInterval(base->type(), signedness);
    for (unsigned index = chain.size(); index--;) {
        const AffineNode& node = chain[index];
        offset += node.addend;
        exactBaseRange.min = std::max<Int128>(exactBaseRange.min, node.exactResults.min - offset);
        exactBaseRange.max = std::min<Int128>(exactBaseRange.max, node.exactResults.max - offset);
    }
    if (exactBaseRange.min > exactBaseRange.max || !fullInterval(Int64, Signedness::Signed).contains({ offset, offset }))
        return makeUnexpected(GaveUpReason::NotCounted);
    return AffineForm { static_cast<int64_t>(offset), exactBaseRange };
}

static bool advancesByStep(const Procedure& proc, Value* value, const InductionRecord& record)
{
    auto form = affineFormOf(proc, value, record.phi, Signedness::Signed);
    return form && form->offset == record.step;
}

static std::expected<void, GaveUpReason> findTestBlock(const CountedLoopInputs& inputs, const InductionRecord& record, CountedLoop& result, bool& isNonZero)
{
    isNonZero = true;
    if (inputs.test)
        result.test = inputs.test->block;
    else {
        for (unsigned i = inputs.loop.size(); i--;) {
            BasicBlock* block = inputs.loop.at(i);
            if (block->last()->opcode() != Branch)
                continue;
            DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
            if (condition.value->foldIdentity() != record.test)
                continue;
            if (result.test)
                return makeUnexpected(GaveUpReason::NotCounted);
            result.test = block;
            isNonZero = condition.isNonZero;
        }
    }
    if (!result.test || result.test->last()->opcode() != Branch)
        return makeUnexpected(GaveUpReason::NotCounted);

    bool firstStays = inputs.loops.belongsTo(result.test->successorBlock(0), inputs.loop);
    if (firstStays == inputs.loops.belongsTo(result.test->successorBlock(1), inputs.loop))
        return makeUnexpected(GaveUpReason::NotCounted);
    result.latchSuccessorIndex = firstStays ? 0 : 1;
    result.latch = result.test->successorBlock(result.latchSuccessorIndex);
    return { };
}

static std::expected<void, GaveUpReason> readTest(const CountedLoopInputs& inputs, const InductionRecord& record, CountedLoop& result)
{
    bool isNonZero = true;
    if (auto found = findTestBlock(inputs, record, result, isNonZero); !found)
        return found;

    Value* test = record.test;
    if (!test->child(0)->type().isInt())
        return makeUnexpected(GaveUpReason::NotCounted);
    std::optional<Opcode> inverted = invertedRelation(test->opcode());
    if (!inverted)
        return makeUnexpected(GaveUpReason::NotCounted);
    bool continuesWhenTrue = !result.latchSuccessorIndex == isNonZero;
    Opcode relation = continuesWhenTrue ? test->opcode() : *inverted;

    auto isTested = [&](Value* operand) {
        return operand == record.phi || operand == record.next;
    };
    Value* left = test->child(0)->foldIdentity();
    Value* right = test->child(1)->foldIdentity();
    if (isTested(left) == isTested(right))
        return makeUnexpected(GaveUpReason::NotCounted);
    Value* tested = isTested(left) ? left : right;
    result.bound = isTested(left) ? right : left;
    result.relation = isTested(left) ? relation : flippedRelation(relation);
    result.isTopTested = tested == record.phi;
    if (result.isTopTested && result.latch == result.header)
        return makeUnexpected(GaveUpReason::NotCounted);
    if (!inputs.isInvariant(result.bound))
        return makeUnexpected(GaveUpReason::BoundNotInvariant);
    return { };
}

static std::expected<void, GaveUpReason> readCounter(const CountedLoopInputs& inputs, const InductionRecord& record, CountedLoop& result)
{
    for (UpsilonValue* upsilon : inputs.phiChildren[record.phi]) {
        Value* input = upsilon->child(0)->foldIdentity();
        if (upsilon->owner == result.preHeader) {
            if (result.start)
                return makeUnexpected(GaveUpReason::NotCounted);
            result.start = input;
            continue;
        }
        if (!inputs.loops.belongsTo(upsilon->owner, inputs.loop))
            return makeUnexpected(GaveUpReason::NotCounted);
        if (input != record.next && !advancesByStep(inputs.proc, input, record))
            return makeUnexpected(GaveUpReason::NotCounted);
    }
    if (!result.start || !advancesByStep(inputs.proc, record.next, record))
        return makeUnexpected(GaveUpReason::NotCounted);

    result.inductionVariable = record.phi;
    result.next = record.next;
    result.step = record.step;
    if (!result.step || result.step > Limits::maxCountedLoopStep || result.step < -Limits::maxCountedLoopStep || (result.step > 0) != result.increases())
        return makeUnexpected(GaveUpReason::NotCounted);
    return { };
}

static std::expected<void, GaveUpReason> computeSafeBound(const CountedLoopInputs& inputs, const InductionRecord& record, CountedLoop& loop)
{
    const Procedure& proc = inputs.proc;
    Signedness signedness = loop.signedness();
    bool increases = loop.increases();
    bool recordProvesNoWrap = record.doesNotWrap && signedness == Signedness::Signed;
    auto form = affineFormOf(proc, loop.next, loop.inductionVariable, signedness);
    if (!form)
        return makeUnexpected(form.error());
    IntegerInterval exactBaseRange = form->exactBaseRange;
    IntegerInterval full = fullInterval(loop.type(), signedness);
    Int128 strict = loop.includesEqual() ? 0 : 1;
    loop.farthestSafeBound = increases ? std::min<Int128>(exactBaseRange.max + strict, full.max) : std::max<Int128>(exactBaseRange.min - strict, full.min);

    IntegerInterval counter = intervalFor(proc, loop.start, signedness);
    for (UpsilonValue* upsilon : inputs.phiChildren[loop.inductionVariable]) {
        if (upsilon->owner != loop.preHeader)
            counter = counter.join(intervalFor(proc, upsilon->child(0), signedness));
    }
    IntegerInterval known = intervalFor(proc, loop.inductionVariable, signedness);
    counter.min = std::max(counter.min, known.min);
    counter.max = std::min(counter.max, known.max);
    if (!recordProvesNoWrap && (increases ? counter.min < exactBaseRange.min : counter.max > exactBaseRange.max))
        return makeUnexpected(GaveUpReason::CounterMayWrap);
    loop.boundIsSafe = loop.isSafeBound(intervalFor(proc, loop.bound, signedness));
    loop.counterRangeIsSafe = recordProvesNoWrap || (increases ? counter.max <= exactBaseRange.max : counter.min >= exactBaseRange.min);
    return { };
}

static std::expected<CountedLoop, GaveUpReason> analyzeWithRecord(const CountedLoopInputs& inputs, BasicBlock* preHeader, const InductionRecord& record)
{
    CountedLoop result;
    result.preHeader = preHeader;
    result.header = inputs.loop.header();
    if (auto outcome = readTest(inputs, record, result); !outcome)
        return makeUnexpected(outcome.error());
    if (auto outcome = readCounter(inputs, record, result); !outcome)
        return makeUnexpected(outcome.error());
    if (auto outcome = computeSafeBound(inputs, record, result); !outcome)
        return makeUnexpected(outcome.error());
    // The record speaks of the numbers with their signs. `while (++i < n)` from -1 on is tested as Below(i + 1, n), and Below(-1, n) does not hold.
    auto isNeverNegative = [&](Value* value) {
        return intervalFor(inputs.proc, value, Signedness::Signed).min >= 0;
    };
    bool recordSpeaksOfRelation = result.signedness() == Signedness::Signed || (isNeverNegative(result.start) && isNeverNegative(result.bound));
    result.relationHoldsOnEntry = (record.relationHoldsOnEntry && recordSpeaksOfRelation) || relationHoldsOnEntry(inputs.proc, result, result.bound);
    return result;
}

std::expected<CountedLoop, GaveUpReason> analyzeCountedLoop(const CountedLoopInputs& inputs)
{
    BasicBlock* preHeader = findPreHeader(inputs.loops, inputs.loop);
    if (!preHeader)
        return makeUnexpected(GaveUpReason::SeveralEntries);

    GaveUpReason reason = GaveUpReason::NotCounted;
    for (Value* phi : *inputs.loop.header()) {
        if (phi->opcode() != Phi)
            continue;
        std::optional<InductionRecord> record = inputs.proc.knownInduction(phi);
        if (!record || !record->test)
            continue;
        if (inputs.test && inputs.test->condition->foldIdentity() != record->test)
            continue;
        auto result = analyzeWithRecord(inputs, preHeader, *record);
        if (result) {
            inputs.proc.count(Counter::KnownInductionsUsed);
            return result;
        }
        reason = result.error();
    }
    return makeUnexpected(reason);
}

bool relationHoldsOnEntry(Procedure& proc, const CountedLoop& loop, Value* boundOnEntry)
{
    Signedness signedness = loop.signedness();
    if (evaluateComparison(loop.relation, intervalFor(proc, loop.start, signedness), intervalFor(proc, boundOnEntry, signedness)) == TriState::True)
        return true;
    return isRelationImpliedAt(proc, loop.preHeader, loop.relation, loop.start, boundOnEntry);
}

TriState evaluateComparisonWithBound(const CountedLoop& loop, Opcode opcode, Value* left, Value* right)
{
    if (!invertedRelation(opcode))
        return TriState::Indeterminate;
    if (left == loop.bound && right == loop.inductionVariable)
        opcode = flippedRelation(opcode);
    else if (left != loop.inductionVariable || right != loop.bound)
        return TriState::Indeterminate;
    if (opcode == loop.relation)
        return TriState::True;
    if (*invertedRelation(opcode) == loop.relation)
        return TriState::False;
    return TriState::Indeterminate;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
