/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyInternals.h"
#include "PyModule.h"
#include "PyNative.h"

namespace JSC {
namespace Python {

// The module _types: names for the built-in types that have none in builtins. (Lib/types.py can find most of them by itself.)

// There is no C API yet, so nothing makes one.
static constexpr TypeDef capsuleTypeDef { "PyCapsule"_s, { TypeFlag::DisallowInstantiation }, { }, { }, { } };

void initializeTypesModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "AsyncGeneratorType"_s, rt.asyncGeneratorType());
    module->add(globalObject, "BuiltinFunctionType"_s, rt.nativeFunctionType());
    module->add(globalObject, "BuiltinMethodType"_s, rt.nativeFunctionType());
    module->add(globalObject, "CapsuleType"_s, rt.typeFor(capsuleTypeDef));
    module->add(globalObject, "CellType"_s, rt.cellType());
    module->add(globalObject, "ClassMethodDescriptorType"_s, rt.classMethodDescriptorType());
    module->add(globalObject, "CodeType"_s, rt.codeType());
    module->add(globalObject, "CoroutineType"_s, rt.coroutineType());
    module->add(globalObject, "EllipsisType"_s, rt.ellipsisType());
    module->add(globalObject, "FrameType"_s, rt.frameType());
    module->add(globalObject, "FrameLocalsProxyType"_s, rt.frameLocalsProxyType());
    module->add(globalObject, "FunctionType"_s, rt.functionType());
    module->add(globalObject, "GeneratorType"_s, rt.generatorType());
    module->add(globalObject, "GenericAlias"_s, rt.genericAliasType());
    module->add(globalObject, "GetSetDescriptorType"_s, rt.getSetDescriptorType());
    module->add(globalObject, "LambdaType"_s, rt.functionType());
    module->add(globalObject, "LazyImportType"_s, typeOfLazyImports(globalObject));
    module->add(globalObject, "MappingProxyType"_s, rt.mappingProxyType());
    module->add(globalObject, "MemberDescriptorType"_s, rt.memberDescriptorType());
    module->add(globalObject, "MethodDescriptorType"_s, rt.methodDescriptorType());
    module->add(globalObject, "MethodType"_s, rt.methodType());
    module->add(globalObject, "MethodWrapperType"_s, rt.methodWrapperType());
    module->add(globalObject, "ModuleType"_s, rt.moduleType());
    module->add(globalObject, "NoneType"_s, rt.noneType());
    module->add(globalObject, "NotImplementedType"_s, rt.notImplementedType());
    module->add(globalObject, "SimpleNamespace"_s, rt.simpleNamespaceType());
    module->add(globalObject, "TracebackType"_s, rt.tracebackType());
    module->add(globalObject, "UnionType"_s, rt.unionType());
    module->add(globalObject, "WrapperDescriptorType"_s, rt.wrapperDescriptorType());
}

} // namespace Python
} // namespace JSC
