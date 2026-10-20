/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyOperations.h"

#if ENABLE(AOT)

#include "AOTFlowKeys.h"
#include "AOTFlowSolver.h"

namespace JSC { namespace AOT { namespace Typed {

// Both zeroes are equal to each other.
static Type valuesEqualTo(const Type& tested)
{
    Type result = tested;
    if (tested.primitives & Primitive::WholeNumber) {
        result.primitives |= Primitive::Integer;
        result.integers = tested.integers.unionWith(Range::one(0));
    }
    if (tested.integers.contains(0))
        result.primitives |= Primitive::WholeNumber;
    return result;
}

static Type testedTypeOf(const FlowSolver& solver, FlowStatementSite site, const FlowFilter& filter)
{
    Type tested = filter.range ? Type::integer(solver.summaryOf(site).ranges[*filter.range]) : solver.program().at(filter.type);
    return filter.kind == FlowFilterKind::Inside ? valuesEqualTo(tested) : tested;
}

static bool mayPass(FlowSolver& solver, FlowFilterKind kind, const FlowValue& read, const Type& tested)
{
    switch (kind) {
    case FlowFilterKind::Inside:
        return !solver.values().filter(read, tested).isEmpty();
    case FlowFilterKind::Outside:
        return !solver.values().filterOut(read, tested).isEmpty();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The branch was decided by what a read of the name gave at that time, and a location holds whatever is ever stored there. An object whose read can never have decided
// the branch this way is not the object that was read from, however long ago the read was.
void runFieldFilter(FlowSolver& solver, FlowStatementSite site, const FlowFilter& filter)
{
    FlowNodeID from = solver.node(site, filter.from);
    FlowNodeID to = solver.node(site, filter.to);
    FlowNewObjects objects = solver.takeNewObjects(site, 0, from);
    if (solver.takeNewPrimitives(site, 0, from))
        solver.join(to, solver.valueOf(from).withoutObjects());

    Type tested = testedTypeOf(solver, site, filter);
    bool absentPasses = !!(tested.primitives & Primitive::Undefined) == (filter.kind == FlowFilterKind::Inside);
    FlowKeySet keys = keySetOf(solver.program(), filter.field);
    FlowPropertyWalk walk { .waiter = site };
    solver.forEachMember(objects, [&](FlowObjectID object) {
        if (solver.isAboutToBeAbandoned(site))
            return;
        bool passes = object == unknownFlowObject;
        if (!passes) {
            forEachPropertySource(solver, object, keys, walk, [&](const FlowPropertySource& source) {
                switch (source.kind) {
                case FlowPropertySourceKind::Value:
                    if (mayPass(solver, filter.kind, solver.valueOf(source.node), tested))
                        passes = true;
                    else
                        solver.waitOn(source.node, site, FlowWakeUp::Revisit);
                    return;
                case FlowPropertySourceKind::Absent:
                case FlowPropertySourceKind::Setter:
                case FlowPropertySourceKind::BuiltinSetter:
                    passes |= absentPasses;
                    return;
                case FlowPropertySourceKind::Unknown:
                case FlowPropertySourceKind::Getter:
                case FlowPropertySourceKind::BuiltinMember:
                case FlowPropertySourceKind::BuiltinGetter:
                    passes = true;
                    return;
                }
                RELEASE_ASSERT_NOT_REACHED();
            });
        }
        if (passes)
            solver.joinObject(to, object);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
