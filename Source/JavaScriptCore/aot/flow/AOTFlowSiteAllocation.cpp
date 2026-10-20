/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTLayout.h"
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isTypedResultOfLayout(const FlowSiteDescription& description)
{
    return description.kind == FlowSiteKind::Layout && description.origin == FlowSiteOrigin::TypedResult;
}

static bool isPastSiteLimit(const FlowObjectTable& objects, const FlowObjectState& state, LayoutID layout)
{
    return objects.layoutObject(layout) || objects.allocationClassesOf(layout).size() >= state.maxSitesPerLayout;
}

static FlowSolveAllocation allocateFirstObject(FlowSolver& solver, AllocationSiteID site, FlowInstanceID instance, const FlowSiteDescription& description)
{
    FlowObjectState& state = solver.objectState();
    FlowObjectTable& objects = solver.objects();
    auto appendToSolver = [&](const FlowObject& record) {
        return solver.appendObject(record);
    };
    FlowObjectAppender append = appendToSolver;

    if (description.kind == FlowSiteKind::Layout && isPastSiteLimit(objects, state, description.layout)) {
        FlowObjectID layoutObject = ensureLayoutObject(objects, solver.build(), description.layout, append);
        if (layoutObject == unknownFlowObject) {
            objects.mutableAt(site).sharedObject = unknownFlowObject;
            return { unknownFlowObject, false };
        }
        addSiteToLayoutObject(objects, site, layoutObject);
        return { layoutObject, true };
    }

    FlowObjectID object = appendAllocationClassFor(objects, singleElementSpan(site), append);
    if (object == unknownFlowObject)
        return { unknownFlowObject, false };

    state.solveObjects.add(std::pair { site, instance }, object);
    if (isTypedResultOfLayout(description))
        state.typedResultClasses.add(description.layout, objects.at(object).allocationClass);
    return { object, true };
}

FlowSolveAllocation ensureSolveAllocation(FlowSolver& solver, FlowInstanceID instance, const FlowSiteDescription& description)
{
    RELEASE_ASSERT(instance);
    FlowObjectState& state = solver.objectState();
    FlowObjectTable& objects = solver.objects();
    FlowSolveSiteKey key { solver.graph().at(instance).function, description };

    AllocationSiteID site = state.solveSites.get(key);
    if (!site) {
        site = objects.appendSite(FlowSite { key.function, description, { } });
        state.solveSites.add(key, site);

        AllocationClassID typedResultClass = isTypedResultOfLayout(description) ? state.typedResultClasses.get(description.layout) : AllocationClassID();
        if (!typedResultClass)
            return allocateFirstObject(solver, site, instance, description);

        FlowAllocationClassMembers& members = objects.mutableAt(typedResultClass);
        members.sites.append(site);
        objects.mutableAt(site).sharedObject = members.sharedObject;
    }

    FlowObjectID shared = objects.at(site).sharedObject;
    if (shared == unknownFlowObject || objects.at(shared).standsForLayout)
        return { shared, false };

    std::pair objectKey { site, instance };
    if (FlowObjectID existing = state.solveObjects.get(objectKey))
        return { existing, false };

    FlowObjectID copy = copyObject(solver, shared, site, instance);
    state.solveObjects.add(objectKey, copy);
    return { copy, copy != shared };
}

FlowObjectID allocateAtCallSite(FlowSolver& solver, const FlowCallSiteAllocation& allocation)
{
    const FlowSiteDescription& description = allocation.description;
    FlowSolveAllocation result = ensureSolveAllocation(solver, allocation.site.instance, description);
    if (result.object == unknownFlowObject)
        return result.object;

    if (!result.isNew) {
        solver.noteEvent(result.object, FlowObjectEvent::Allocated, solver.witnessAt(allocation.site, description.value));
        return result.object;
    }

    FlowAllocatedObject allocated { allocation.site, description.value, result.object, false, false };
    Vector<uint32_t, 8> initialFields;
    if (description.kind == FlowSiteKind::Layout) {
        const Layout& layout = solver.program().at(description.layout);
        bool isMadeForConstructor = description.origin == FlowSiteOrigin::Operation || description.origin == FlowSiteOrigin::BuiltinConstructor;
        allocated.isByConstructor = isMadeForConstructor && layout.constructor;
        if (!allocated.isByConstructor)
            initialFields = requiredFieldsOf(layout);
    }
    noteAllocation(solver, allocated, initialFields.span());

    if (FlowNodeID undeclaredPart = solver.graph().undeclaredPart(result.object))
        solver.join(undeclaredPart, solver.values().unknownValue().withoutObjects());
    return result.object;
}

FlowObjectID allocateCoroutineResult(FlowSolver& solver, FlowStatementSite caller, ValueID origin, FlowInstanceID calleeInstance)
{
    const FlowInstance& callee = solver.graph().at(calleeInstance);
    const FlowSummary& summary = solver.graph().summaryOf(calleeInstance);
    RELEASE_ASSERT(summary.coroutine.resultSite);
    uint32_t resultSite = *summary.coroutine.resultSite;
    FlowObjectID own = solver.graph().objectOf(calleeInstance, resultSite);

    if (!caller.instance) {
        solver.noteEvent(own, FlowObjectEvent::Allocated, solver.witnessAt(calleeInstance, ValueID()));
        return own;
    }

    Witness witness = solver.witnessAt(caller, origin);
    if (own == unknownFlowObject || callee.isCallSiteInstance()) {
        solver.noteEvent(own, FlowObjectEvent::Allocated, witness);
        return own;
    }

    FlowObjectState& state = solver.objectState();
    std::tuple key { caller.instance, origin, calleeInstance };
    if (FlowObjectID existing = state.coroutineResults.get(key))
        return existing;

    FlowObjectID copy = copyObject(solver, own, solver.objects().siteOf(callee.function, resultSite), caller.instance);
    state.coroutineResults.add(key, copy);
    solver.noteEvent(copy, FlowObjectEvent::Allocated, witness);
    return copy;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
