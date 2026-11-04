/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the scan asks about promises that no operation of the program fills: the promise of a known import(), and the promise that the ramp of an async function makes and settles.
// Each answer goes by the records of the program alone, so it is the same whoever asks and whenever.

#include "config.h"
#include "AOTLinkCoroutines.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

LayoutID layoutOfNamespaceThatImportGives(const Program& program, const BodyValue& value)
{
    if (value.opcode() != Opcode::ImportModule)
        return { };
    auto operation = value.as<Opcode::ImportModule>();
    // The attributes say as what the file is loaded, and nobody has read them: formsInImage().
    if (!operation.module() || operation.optionsOperand())
        return { };
    // The loader answers with what a copy from disk has made, or with what the factory of a mock has.
    if (program.at(operation.module()).flags.containsAny({ ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked }))
        return { };
    // As BuilderOfProgramFacts::mayExport() has it. `export * from "./legacy.cjs"`: which names the namespace object has is known when that file has run, and `then` may be one.
    const Module& module = program.at(operation.module());
    if (module.exportsVariable || std::ranges::any_of(module.requests, &ModuleRequest::isDynamicStarExport))
        return { };
    LayoutID layout = program.layoutOfWholeNamespace(operation.module());
    if (!layout)
        return { };
    // `export function then(resolve) { .. }`: the promise is resolved through that function, with whatever it hands over. Every other namespace object lacks the name, and nothing stands above it.
    for (BuiltinName name : program.builtins().keysReadBy(Protocol::Thenable)) {
        if (program.at(layout).findField(program.keyOf(name)))
            return { };
    }
    return layout;
}

Effects effectsThatNoOperationOfCoroutineShows(const Function& function)
{
    Effects effects = Effects::none();
    if (!function.has(FunctionFlags::IsAsync) || function.has(FunctionFlags::IsGenerator))
        return effects;
    effects.merge(declaredBoundOf(Opcode::NewPromise));
    effects.merge(declaredBoundOf(Opcode::ResolvePromise));
    effects.merge(declaredBoundOf(Opcode::RejectPromise));
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
