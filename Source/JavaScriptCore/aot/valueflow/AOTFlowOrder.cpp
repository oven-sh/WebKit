/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

FlowMemberHolder memberHolderKindOf(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
        return FlowMemberHolder::Always;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return FlowMemberHolder::WhenPrototype;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return FlowMemberHolder::No;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool followsMembersOf(const FlowOrderInputs& inputs, FlowObjectID holder)
{
    switch (memberHolderKindOf(inputs.kindOf(holder))) {
    case FlowMemberHolder::No:
        return false;
    case FlowMemberHolder::WhenPrototype:
        return inputs.isPrototype(holder);
    case FlowMemberHolder::Always:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ModuleID> indexRuntimeModules(const Program& program)
{
    Vector<ModuleID> index(program.builtins().moduleCount());
    for (uint32_t module = 0; module < program.modules.size(); ++module) {
        if (RuntimeModuleID runtimeModule = program.modules[module].runtimeModule)
            index[runtimeModule.index()] = ModuleID(module);
    }
    return index;
}

RuntimeModuleID findCompiledModuleWithObject(const Program& program, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (entry.holder || !isObjectKind(entry.kind) || entry.origin != BuiltinOrigin::Host)
        return { };
    for (const Module& module : program.modules) {
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        BuiltinID object = builtins.namespaceOf(module.runtimeModule);
        if (object && builtins.canonicalID(object) == builtin)
            return module.runtimeModule;
    }
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
