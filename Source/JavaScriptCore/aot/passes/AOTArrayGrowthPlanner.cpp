/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTResultRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ReservedArray {
    BlockID loopHeader;
    ValueID array;
};

} // anonymous namespace

static bool appendsOneElement(const Program& program, const Value& append)
{
    if (append.opcode == Opcode::CallBuiltin)
        return builtinCallOf(program, append).arguments.size() == 1;
    return append.opcode == Opcode::AppendElement;
}

// The capacity test makes the append's load of the indexing mode control dependent, and B3 hoists such a load out of the loop only from a pointer that code generation publishes as dereferenceable.
bool ArrayCapacityPlanner::hasUnconditionalCellType(ValueID array) const
{
    const Value& origin = m_function.at(m_analyses.sameObjectThroughCasts(array));
    return origin.opcode == Opcode::Parameter || resultRuleOf(origin.opcode) == ResultRule::Allocation;
}

// A capacity above the limit is found when the literal runs, which then allocates as if nothing were planned.
bool ArrayCapacityPlanner::isAllocatedWithPlannedCapacity(ValueID array) const
{
    const Value& origin = m_function.at(m_analyses.sameObjectThroughCasts(array));
    if (origin.opcode != Opcode::NewArray || !origin.capacityOperand())
        return false;
    const Type& capacity = m_context.program.typeOf(m_function, origin.capacityOperand());
    return capacity.isInteger() && capacity.integers.maximum <= static_cast<int64_t>(m_context.limit(LimitID::MaxPlannedArrayCapacity));
}

TripCount ArrayCapacityPlanner::remainingIterations(const NaturalLoop& loop, ValueID array, ProgramPoint point)
{
    auto [counter, test] = findUnitStepCounter(loop);
    if (!counter)
        return { };

    auto termAtPoint = [&](ValueID id) -> CapacityTerm {
        CapacityTerm term = termOf({ array, point, &loop }, id);
        if (term.value && !m_analyses.dominates(term.value, point))
            return { };
        return term;
    };
    CapacityTerm current = termAtPoint(counter->parameter);
    CapacityTerm bound = termAtPoint(test->bound);
    int64_t adjustment = test->relation == Opcode::LessEq || test->relation == Opcode::GreaterEq;
    if (counter->step.constant > 0)
        return { bound, current, adjustment };
    return { current, bound, adjustment };
}

std::optional<GrowthPlan> ArrayCapacityPlanner::planGrowth(const NaturalLoop& loop, ProgramPoint point)
{
    const Program& program = m_context.program;
    ValueID id = m_function.at(point.block).values[point.index];
    const Value& append = m_function.at(id);
    if (appendKindOf(append) != AppendKind::Elements || !appendsOneElement(program, append))
        return std::nullopt;
    if (m_analyses.blockFrequencies().classOf(point.block) == BlockFrequency::Rare)
        return std::nullopt;

    ValueID array = appendedArrayOf(append);
    if (!program.typeOf(m_function, array).isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program))
        return std::nullopt;
    if (!isInvariantIn(m_analyses, loop, array) || !hasUnconditionalCellType(array))
        return std::nullopt;
    if (isAllocatedWithPlannedCapacity(array) || !runsOncePerIteration(loop, point.block))
        return std::nullopt;

    TripCount count = remainingIterations(loop, array, point);
    if (count.added.kind == CapacityTermKind::Unknown || count.subtracted.kind == CapacityTermKind::Unknown)
        return std::nullopt;
    return GrowthPlan { id, array, loop.header(), count, 1 };
}

TripCount ArrayCapacityPlanner::finishedIterations(const NaturalLoop& loop, ValueID collection, ProgramPoint point)
{
    auto [counter, test] = findUnitStepCounter(loop);
    if (!counter)
        return { };

    auto termAtPoint = [&](ValueID id) -> CapacityTerm {
        CapacityTerm term = termOf({ collection, point, &loop }, id);
        if (term.value && !m_analyses.dominates(term.value, point))
            return { };
        return term;
    };
    CapacityTerm current = termAtPoint(counter->parameter);
    CapacityTerm start = termAtPoint(counter->start);
    if (counter->step.constant > 0)
        return { current, start, 0 };
    return { start, current, 0 };
}

// The one block from which the loop is entered, where that block leads nowhere else.
BlockID ArrayCapacityPlanner::findPreheader(const NaturalLoop& loop)
{
    BlockID preheader;
    for (Edge edge : m_analyses.cfg().incomingEdges(loop.header())) {
        if (m_analyses.naturalLoops().belongsTo(edge.from, loop))
            continue;
        if (preheader)
            return { };
        preheader = edge.from;
    }
    if (!preheader || m_function.at(preheader).terminator.kind != TerminatorKind::Jump)
        return { };
    return preheader;
}

// The table of a Map or a Set grows by half, and each step leaves the table before it behind. In a counted loop a table that is full grows once, to what the loop is still to add.
// An iteration may skip the addition: the run time goes by the share of them that have added an entry.
std::optional<GrowthPlan> ArrayCapacityPlanner::planCollectionGrowth(const NaturalLoop& loop, ProgramPoint point)
{
    const Program& program = m_context.program;
    ValueID id = m_function.at(point.block).values[point.index];
    const Value& addition = m_function.at(id);
    if (addition.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    std::optional<InlineBuiltin> implementation = inlineImplementationOf(program, addition);
    if (implementation != InlineBuiltin::MapSet && implementation != InlineBuiltin::SetAdd)
        return std::nullopt;
    if (m_analyses.blockFrequencies().classOf(point.block) == BlockFrequency::Rare)
        return std::nullopt;

    ValueID collection = addition.receiverOperand();
    BlockID preheader = findPreheader(loop);
    if (!preheader || !isInvariantIn(m_analyses, loop, collection))
        return std::nullopt;
    // A value that is invariant may still be computed in the loop.
    if (!m_analyses.dominates(collection, ProgramPoint { preheader, static_cast<uint32_t>(m_function.at(preheader).values.size()) }))
        return std::nullopt;
    if (!makeSizeRead(m_context, collection, implementation == InlineBuiltin::MapSet ? Builtins::MapPrototypeSize : Builtins::SetPrototypeSize))
        return std::nullopt;

    auto isKnown = [](const TripCount& count) {
        auto isWhole = [](const CapacityTerm& term) {
            return term.kind == CapacityTermKind::Constant || term.kind == CapacityTermKind::Integer;
        };
        return isWhole(count.added) && isWhole(count.subtracted);
    };
    TripCount remaining = remainingIterations(loop, collection, point);
    TripCount finished = finishedIterations(loop, collection, point);
    if (!isKnown(remaining) || !isKnown(finished))
        return std::nullopt;
    return GrowthPlan { id, collection, loop.header(), remaining, 1, finished, preheader };
}

Vector<GrowthPlan, 4> ArrayCapacityPlanner::collectGrowthPlans()
{
    return collectGrowthPlans(Opcode::ReserveArrayCapacity, &ArrayCapacityPlanner::planGrowth);
}

Vector<GrowthPlan, 4> ArrayCapacityPlanner::collectCollectionGrowthPlans()
{
    auto keyOf = [&](const GrowthPlan& plan) {
        return m_analyses.identityOf(builtinCallOf(m_context.program, m_function.at(plan.append)).arguments[0]);
    };
    auto addsTo = [&](const GrowthPlan& plan, const GrowthPlan& other) {
        return plan.loopHeader == other.loopHeader && m_analyses.identityOf(plan.array) == m_analyses.identityOf(other.array);
    };
    // The second addition of a key adds nothing.
    Vector<GrowthPlan, 4> plans;
    for (GrowthPlan& plan : collectGrowthPlans(Opcode::ReserveCollectionCapacity, &ArrayCapacityPlanner::planCollectionGrowth)) {
        bool repeatsKey = plans.containsIf([&](const GrowthPlan& other) {
            return addsTo(plan, other) && keyOf(plan) == keyOf(other);
        });
        if (!repeatsKey)
            plans.append(WTF::move(plan));
    }
    for (GrowthPlan& plan : plans) {
        plan.elementsPerIteration = std::ranges::count_if(plans, [&](const GrowthPlan& other) {
            return addsTo(plan, other);
        });
    }
    return plans;
}

Vector<GrowthPlan, 4> ArrayCapacityPlanner::collectGrowthPlans(Opcode reservation, GrowthPlanner planAt)
{
    Vector<GrowthPlan, 4> plans;
    if (!m_analyses.cfg().hasCycle())
        return plans;

    const NaturalLoops& loops = m_analyses.naturalLoops();
    Vector<ReservedArray, 4> reservedArrays;
    if (!loops.numLoops())
        return plans;

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const NaturalLoop* loop = loops.innerMostLoopOf(block);
        if (!loop)
            continue;
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = m_function.at(values[index]);
            if (value.opcode == reservation)
                reservedArrays.append({ loop->header(), m_analyses.identityOf(value.operand(0)) });
            else if (std::optional<GrowthPlan> plan = (this->*planAt)(*loop, { block, index }))
                plans.append(WTF::move(*plan));
        }
    }

    auto appendsTo = [&](const GrowthPlan& plan, BlockID loopHeader, ValueID array) {
        return plan.loopHeader == loopHeader && m_analyses.identityOf(plan.array) == array;
    };
    plans.removeAllMatching([&](const GrowthPlan& plan) {
        return reservedArrays.containsIf([&](const ReservedArray& reserved) {
            return appendsTo(plan, reserved.loopHeader, reserved.array);
        });
    });
    for (GrowthPlan& plan : plans) {
        plan.elementsPerIteration = std::ranges::count_if(plans, [&](const GrowthPlan& other) {
            return appendsTo(other, plan.loopHeader, m_analyses.identityOf(plan.array));
        });
    }
    return plans;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
