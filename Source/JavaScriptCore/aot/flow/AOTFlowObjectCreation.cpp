/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowCallOperations.h"

namespace JSC { namespace AOT { namespace Typed {

FlowObjectID ensureBuiltinObject(FlowSolver& solver, BuiltinID builtin)
{
    RELEASE_ASSERT(builtin);
    BuiltinID canonical = solver.builtins().canonicalID(builtin);
    if (FlowObjectID existing = solver.objects().findBuiltin(canonical))
        return existing;

    FlowObject record;
    record.kind = FlowObjectKind::Builtin;
    record.builtin = canonical;
    FlowObjectID object = solver.appendObject(record);
    if (standsForWhatItHolds(solver.builtins(), canonical))
        solver.noteObjectLettingInUnseenCode(object);
    return object;
}

FlowObjectID ensureModuleBuiltinObject(FlowSolver& solver, BuiltinID builtin, ModuleID module)
{
    RELEASE_ASSERT(builtin && module);
    BuiltinID canonical = solver.builtins().canonicalID(builtin);
    if (FlowObjectID existing = solver.objects().findModuleBuiltin(canonical, module))
        return existing;

    FlowObject record;
    record.kind = FlowObjectKind::ModuleBuiltin;
    record.builtin = canonical;
    record.module = module;
    FlowObjectID object = solver.appendObject(record);
    if (standsForWhatItHolds(solver.builtins(), canonical))
        solver.noteObjectLettingInUnseenCode(object);
    return object;
}

FlowObjectID ensureNamespaceObject(FlowSolver& solver, ModuleID module)
{
    RELEASE_ASSERT(module);
    solver.records().addCreatedNamespace(module);
    if (FlowObjectID existing = solver.objects().findNamespace(module))
        return existing;

    FlowObject record;
    record.kind = FlowObjectKind::ModuleNamespace;
    record.module = module;
    record.layout = solver.program().at(module).namespaceLayout;
    return solver.appendObject(record);
}

FlowObjectID ensurePrototypeObject(FlowSolver& solver, FlowObjectID closure)
{
    FlowObject owner = solver.objects().at(closure);
    if (owner.kind != FlowObjectKind::Closure)
        return { };
    if (FlowObjectID existing = solver.objects().findPrototype(closure))
        return existing;

    const Function& function = solver.program().at(owner.function);
    if (!hasPrototypeProperty(solver.program(), function))
        return { };

    FlowObject record;
    record.kind = FlowObjectKind::Prototype;
    record.owner = closure;
    record.function = owner.function;
    record.layout = function.constructedClass;
    record.instance = owner.instance;
    return solver.appendObject(record);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
