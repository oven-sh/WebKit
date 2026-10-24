/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPrototypes.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFunctionKinds.h"

namespace JSC { namespace AOT { namespace Typed {

BuiltinID prototypeOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    BuiltinClassID builtinClass = isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
    if (function.has(FunctionFlags::IsGenerator))
        builtinClass = isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return BuiltinTable::singleton().at(builtinClass).prototype;
}

// The run time answers the same question with FunctionTables::hasPrototypePropertyAtCreation.
bool hasPrototypeProperty(const Program& program, const Function& function)
{
    switch (function.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        break;
    }
    if (isInRuntimeModule(program, function))
        return false;
    if (function.has(FunctionFlags::IsGenerator))
        return true;
    return function.kind == FunctionKind::Plain && !function.has(FunctionFlags::IsAsync);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
