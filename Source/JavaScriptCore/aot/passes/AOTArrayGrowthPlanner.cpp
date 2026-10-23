/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTResultRules.h"
#include "AOTVector.h"
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

Vector<GrowthPlan, 4> ArrayCapacityPlanner::collectGrowthPlans()
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
            if (value.opcode == Opcode::ReserveArrayCapacity)
                reservedArrays.append({ loop->header(), m_analyses.identityOf(value.operand(0)) });
            else if (std::optional<GrowthPlan> plan = planGrowth(*loop, { block, index }))
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
