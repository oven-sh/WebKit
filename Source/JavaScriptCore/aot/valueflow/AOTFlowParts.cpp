/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowCoroutines.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPrototypes.h"
#include "AOTFunctionKinds.h"

namespace JSC { namespace AOT { namespace Typed {

BuiltinSlotID indexedElementSlotOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    BuiltinSlotID slot = builtinClass ? builtins.findSlot(builtinClass, ContentsSlot::Element) : BuiltinSlotID();
    if (!slot)
        return { };
    AbstractHeap heap = builtins.at(slot).heap;
    if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
        return slot;
    return { };
}

BuiltinID prototypeOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    BuiltinClassID builtinClass = isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
    if (function.has(FunctionFlags::IsGenerator))
        builtinClass = isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return BuiltinTable::singleton().at(builtinClass).prototype;
}

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

std::optional<CoroutineKind> coroutineKindOf(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    if (isAsync && isGenerator)
        return CoroutineKind::AsyncGenerator;
    if (isAsync)
        return CoroutineKind::AsyncFunction;
    if (isGenerator)
        return CoroutineKind::Generator;
    return std::nullopt;
}

BuiltinClassID resultClassOf(CoroutineKind kind)
{
    switch (kind) {
    case CoroutineKind::Generator:
        return BuiltinClasses::Generator;
    case CoroutineKind::AsyncFunction:
        return BuiltinClasses::Promise;
    case CoroutineKind::AsyncGenerator:
        return BuiltinClasses::AsyncGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
