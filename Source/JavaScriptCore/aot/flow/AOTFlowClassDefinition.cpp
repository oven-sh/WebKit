/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowPropertyOperations.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowNodeID connectClassParent(FlowSolver& solver, FlowStatementSite site, FlowNodeID heritage, const Witness& witness)
{
    if (!heritage)
        return { };

    FlowNodeID classParent = solver.temporaryFor(objectTemporaryKey(site, FlowObjectTemporaryRole::ParentOfClassObject));
    solver.connect(heritage, classParent, TypeTable::object, witness);
    if (solver.valueOf(heritage).primitives & Primitive::Null) {
        BuiltinID functionPrototype = solver.builtins().at(BuiltinClasses::Function).prototype;
        solver.joinObject(classParent, ensureBuiltinObject(solver, functionPrototype), witness);
    }
    solver.waitOn(heritage, site);
    return classParent;
}

void runClassDefinition(FlowSolver& solver, FlowStatementSite site, const FlowClassDefinition& definition)
{
    Witness witness = solver.witnessAt(site, definition.origin);
    FlowNodeID heritage = solver.node(site, definition.heritage);
    FlowNodeID prototypeParent = solver.node(site, definition.prototypeParent);

    solver.connect(heritage, solver.graph().heritage(definition.layout), { }, witness);
    FlowNodeID classParent = connectClassParent(solver, site, heritage, witness);

    FlowObjectSet constructors = solver.takeNewObjects(site, 0, solver.node(site, definition.constructor));
    constructors.forEach([&](FlowObjectID classObject) {
        if (solver.objects().kindOf(classObject) != FlowObjectKind::Closure)
            return;

        FlowObjectID prototypeObject = ensurePrototypeObject(solver, classObject);
        if (!prototypeObject || prototypeObject == unknownFlowObject)
            return;

        if (classParent)
            connectCreationPrototype(solver, classObject, classParent);
        if (prototypeParent)
            connectCreationPrototype(solver, prototypeObject, prototypeParent);
        solver.records().addClassEvaluation(FlowClassEvaluation { definition.layout, site.instance, definition.origin, classObject, prototypeObject, definition.isInCycle });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
