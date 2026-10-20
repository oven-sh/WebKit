/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowSiteDescription plainDataDescription(FlowSiteKind kind, BuiltinClassID builtinClass, ValueID origin)
{
    FlowSiteDescription description;
    description.kind = kind;
    description.origin = FlowSiteOrigin::BuiltinResult;
    description.value = origin;
    description.builtinClass = builtinClass;
    return description;
}

FlowPlainDataObjects allocatePlainData(FlowSolver& solver, FlowStatementSite site, ValueID origin)
{
    FlowSolveAllocation object = ensureSolveAllocation(solver, site.instance, plainDataDescription(FlowSiteKind::PlainObject, BuiltinClasses::Object, origin));
    FlowSolveAllocation array = ensureSolveAllocation(solver, site.instance, plainDataDescription(FlowSiteKind::PlainArray, BuiltinClasses::Array, origin));
    if (object.object == unknownFlowObject || array.object == unknownFlowObject)
        return { unknownFlowObject, unknownFlowObject };

    Witness witness = solver.witnessAt(site, origin);
    solver.noteEvent(object.object, FlowObjectEvent::Allocated, witness);
    solver.noteEvent(array.object, FlowObjectEvent::Allocated, witness);

    if (object.isNew || array.isNew) {
        FlowValue contents = solver.values().unknownValue().withoutObjects();
        contents.objects.add(object.object);
        contents.objects.add(array.object);
        solver.join(solver.ensureOpenPart(object.object, FlowOpenPart::StringProperties), contents);
        solver.join(solver.ensureOpenPart(object.object, FlowOpenPart::IndexedProperties), contents);
        solver.join(contentsNode(solver.graph(), array.object, ContentsSlot::Element), contents);
    }
    return { object.object, array.object };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
