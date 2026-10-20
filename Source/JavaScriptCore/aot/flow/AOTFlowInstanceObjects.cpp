/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

Vector<FlowObjectID> createObjectsOfInstance(FlowSolver& solver, FlowInstanceID instance)
{
    const FlowInstance& record = solver.graph().at(instance);
    FunctionID function = record.function;
    uint32_t count = solver.objects().numberOfSitesOf(function);
    RELEASE_ASSERT(count == solver.graph().summaryOf(instance).sites.size());

    bool usesSharedObjects = solver.isPastWorkLimit(function)
        || (!record.isCallSiteInstance() && solver.objectState().functionsUsingSharedObjects.add(function.index()));

    Vector<FlowObjectID> objects;
    objects.reserveInitialCapacity(count);
    HashMap<AllocationClassID, FlowObjectID> copies;
    for (uint32_t index = 0; index < count; ++index) {
        AllocationSiteID site = solver.objects().siteOf(function, index);
        FlowObjectID shared = solver.objects().at(site).sharedObject;
        if (usesSharedObjects || shared == unknownFlowObject || solver.objects().at(shared).standsForLayout) {
            objects.append(shared);
            continue;
        }

        AllocationClassID allocationClass = solver.objects().at(shared).allocationClass;
        objects.append(copies.ensure(allocationClass, [&] {
            return copyObject(solver, shared, site, instance);
        }).iterator->value);
    }
    return objects;
}

FlowObjectID copyObject(FlowSolver& solver, FlowObjectID original, AllocationSiteID site, FlowInstanceID owner)
{
    FlowObject record = solver.objects().at(original);
    RELEASE_ASSERT(record.allocationClass && !record.standsForLayout);
    record.site = site;
    record.instance = owner;

    FlowObjectID copy = solver.appendObject(record);
    if (copy == unknownFlowObject)
        return original;

    solver.objects().mutableAt(record.allocationClass).copies.append(copy);
    return copy;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
