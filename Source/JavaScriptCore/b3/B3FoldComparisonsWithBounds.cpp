/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3FoldComparisonsWithBounds.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <expected>

namespace JSC { namespace B3 {

namespace {

class FoldComparisonsWithBounds {
public:
    explicit FoldComparisonsWithBounds(Procedure& proc)
        : m_proc(proc)
        , m_loops(proc.naturalLoops())
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        if (!m_loops.numLoops())
            return false;
        m_proc.resetValueOwners();
        m_phiChildren.emplace(m_proc);
        bool changed = false;
        for (unsigned loopIndex = m_loops.numLoops(); loopIndex--;) {
            const NaturalLoop& loop = m_loops.loop(loopIndex);
            if (auto bounded = analyze(loop))
                changed |= foldComparisonsInLoop(loop, *bounded);
        }
        return changed;
    }

private:
    struct BoundedLoop {
        CountedLoop loop;
        IntegerInterval signedBound;
    };

    struct CarriedValue {
        Value* phi;
        UpsilonValue* upsilonInLoop;
        UpsilonValue* upsilonOutsideLoop;
    };

    struct CounterRange {
        Int128 min;
        int64_t offsetFromLimit;
    };

    bool isOutsideLoop(const Value* value, const NaturalLoop& loop) const
    {
        return !m_loops.belongsTo(value->owner, loop);
    }

    std::expected<BoundedLoop, GaveUpReason> analyze(const NaturalLoop& loop)
    {
        auto counted = analyzeCountedLoop({ m_proc, m_loops, loop, *m_phiChildren, [](Value*) { return true; }, std::nullopt });
        if (!counted)
            return std::unexpected(counted.error());
        if (!counted->isTopTested && !isOutsideLoop(counted->bound, loop))
            return adoptCarriedBound(loop, *counted);
        if (counted->isTopTested ? counted->latch->numPredecessors() != 1 : !counted->relationHoldsOnEntry)
            return std::unexpected(GaveUpReason::NotCounted);
        return BoundedLoop { *counted, intervalFor(m_proc, counted->bound, Signedness::Signed) };
    }

    Vector<CarriedValue, 8> collectCarriedValues(const NaturalLoop& loop)
    {
        Vector<CarriedValue, 8> result;
        for (Value* phi : *loop.header()) {
            if (phi->opcode() != Phi)
                continue;
            auto upsilons = (*m_phiChildren)[phi];
            if (upsilons.size() != 2)
                continue;
            bool isFirstOutside = isOutsideLoop(upsilons[0], loop);
            if (isFirstOutside == isOutsideLoop(upsilons[1], loop))
                continue;
            result.append(CarriedValue { phi, upsilons[isFirstOutside ? 1 : 0]->as<UpsilonValue>(), upsilons[isFirstOutside ? 0 : 1]->as<UpsilonValue>() });
        }
        return result;
    }

    std::expected<BoundedLoop, GaveUpReason> adoptCarriedBound(const NaturalLoop& loop, CountedLoop counted)
    {
        Vector<CarriedValue, 8> carriedValues = collectCarriedValues(loop);
        auto findCarriedValue = [&](Value* value) -> const CarriedValue* {
            for (const CarriedValue& carried : carriedValues) {
                if (carried.upsilonInLoop->child(0) == value)
                    return &carried;
            }
            return nullptr;
        };
        Value* boundInLoop = counted.bound;
        const CarriedValue* carried = findCarriedValue(boundInLoop);
        if (!counted.increases() || !carried || carried->phi == counted.inductionVariable)
            return std::unexpected(GaveUpReason::BoundNotInvariant);
        if (carried->upsilonOutsideLoop->owner != counted.preHeader)
            return std::unexpected(GaveUpReason::BoundNotInvariant);
        if (!m_proc.dominators().dominates(carried->upsilonInLoop->owner, counted.latch))
            return std::unexpected(GaveUpReason::BoundNotInvariant);

        Value* boundOnEntry = carried->upsilonOutsideLoop->child(0);
        if (!relationHoldsOnEntry(m_proc, counted, boundOnEntry))
            return std::unexpected(GaveUpReason::NotCounted);

        auto carriedBoundInterval = [&](Signedness signedness) {
            return intervalFor(m_proc, boundOnEntry, signedness).join(intervalFor(m_proc, boundInLoop, signedness));
        };
        counted.boundIsSafe = counted.isSafeBound(carriedBoundInterval(counted.signedness()));
        counted.bound = carried->phi;
        return BoundedLoop { counted, carriedBoundInterval(Signedness::Signed) };
    }

    std::expected<CounterRange, GaveUpReason> computeCounterRange(const BoundedLoop& bounded, Value* limit, const IntegerInterval& limitInterval) const
    {
        const CountedLoop& loop = bounded.loop;
        if (loop.increases()) {
            if (limit != loop.bound)
                return std::unexpected(GaveUpReason::NotCounted);
            Int128 min = intervalFor(m_proc, loop.start, Signedness::Signed).min;
            if (loop.isUnsigned())
                min = std::max<Int128>(min, 0);
            return CounterRange { min, loop.includesEqual() ? 0 : -1 };
        }

        if (loop.isUnsigned())
            return std::unexpected(GaveUpReason::NotCounted);
        auto startForm = affineFormOf(m_proc, loop.start, limit, Signedness::Signed);
        if (!startForm)
            return std::unexpected(startForm.error());
        if (!startForm->exactBaseRange.contains(limitInterval))
            return std::unexpected(GaveUpReason::NotCounted);
        return CounterRange { bounded.signedBound.min + (loop.includesEqual() ? 0 : 1), startForm->offset };
    }

    TriState evaluateRangeCheck(const BoundedLoop& bounded, Opcode opcode, Value* left, Value* right)
    {
        const CountedLoop& loop = bounded.loop;
        if (!invertedRelation(opcode) || !loop.cannotWrap())
            return TriState::Indeterminate;

        Value* limit = right;
        auto form = affineFormOf(m_proc, left, loop.inductionVariable, Signedness::Signed);
        if (!form) {
            form = affineFormOf(m_proc, right, loop.inductionVariable, Signedness::Signed);
            opcode = flippedRelation(opcode);
            limit = left;
        }
        if (!form)
            return TriState::Indeterminate;

        IntegerInterval limitInterval = limit == loop.bound ? bounded.signedBound : intervalFor(m_proc, limit, Signedness::Signed);
        if (limitInterval.min < 0)
            return TriState::Indeterminate;
        auto range = computeCounterRange(bounded, limit, limitInterval);
        if (!range)
            return TriState::Indeterminate;

        Int128 offset = form->offset;
        Int128 offsetFromLimit = range->offsetFromLimit;
        if (range->min + offset < 0 || offsetFromLimit + offset >= 0)
            return TriState::Indeterminate;
        if (range->min < form->exactBaseRange.min || limitInterval.max + offsetFromLimit > form->exactBaseRange.max)
            return TriState::Indeterminate;

        return triState(isLessRelation(opcode));
    }

    bool foldComparisonsInLoop(const NaturalLoop& loop, const BoundedLoop& bounded)
    {
        bool changed = false;
        for (unsigned i = loop.size(); i--;) {
            BasicBlock* block = loop.at(i);
            if (bounded.loop.isTopTested && !m_proc.dominators().dominates(bounded.loop.latch, block))
                continue;
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                if (value->numChildren() != 2)
                    continue;
                TriState outcome = evaluateComparisonWithBound(bounded.loop, value->opcode(), value->child(0), value->child(1));
                if (outcome == TriState::Indeterminate) {
                    outcome = evaluateRangeCheck(bounded, value->opcode(), value->child(0), value->child(1));
                    if (outcome == TriState::Indeterminate)
                        continue;
                    if (isUnsignedRelation(value->opcode()))
                        m_proc.count(Counter::RangeChecksRemoved);
                }
                m_proc.movedLoadLog().addDecidedComparison(m_proc, value, block, index, value->child(0), value->child(1), outcome == TriState::True);
                value->replaceWithIdentity(m_insertionSet.insertIntConstant(index, value, outcome == TriState::True));
                changed = true;
            }
            m_insertionSet.execute(block);
        }
        return changed;
    }

    Procedure& m_proc;
    NaturalLoops& m_loops;
    std::optional<PhiChildren> m_phiChildren;
    InsertionSet m_insertionSet;
};

} // anonymous namespace

bool foldComparisonsWithBounds(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldComparisonsWithBounds"_s);
    FoldComparisonsWithBounds foldComparisonsWithBounds(proc);
    return foldComparisonsWithBounds.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
