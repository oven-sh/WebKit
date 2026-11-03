/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyInternals.h"
#include "PyModule.h"
#include "PyNative.h"
#include "PyStr.h"
#include <wtf/text/MakeString.h>

namespace JSC {
namespace Python {

// The module _types: names for the built-in types that have none in builtins. (Lib/types.py can find most of them by itself.)

// ---- PyCapsule ---- (field 0: the name, a str; number: the pointer)
//
// There is no C API yet. They are there where programs can see that CPython has one.

static JSValue capsuleRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(globalObject, makeString("<capsule object \""_s, stringOf(asString(PyRecord::from(self)->get(0))), "\" at "_s, addressForRepr(globalObject, self), '>'));
}

static constexpr NativeDef capsuleMethods[] = {
    PY_SLOT("__repr__", capsuleRepr, "($self, /)"),
};

static constexpr TypeDef capsuleTypeDef { "PyCapsule"_s, { TypeFlag::DisallowInstantiation }, capsuleMethods, { }, { } };

JSValue makeCapsule(JSGlobalObject* globalObject, ASCIILiteral name, const void* pointer)
{
    VM& vm = getVM(globalObject);
    PyRecord* capsule = PyRecord::create(globalObject, runtime(globalObject).typeFor(capsuleTypeDef), 1);
    capsule->set(vm, 0, makeStr(vm, name));
    capsule->number = static_cast<int64_t>(std::bit_cast<intptr_t>(pointer));
    return capsule;
}

// One that owns what it points to. (Field 1: the cell whose memory that is.)
JSValue makeCapsuleWithMemory(JSGlobalObject* globalObject, ASCIILiteral name, JSCell* memory, const void* pointer)
{
    VM& vm = getVM(globalObject);
    PyRecord* capsule = PyRecord::create(globalObject, runtime(globalObject).typeFor(capsuleTypeDef), 2);
    capsule->set(vm, 0, makeStr(vm, name));
    capsule->set(vm, 1, memory);
    capsule->number = static_cast<int64_t>(std::bit_cast<intptr_t>(pointer));
    return capsule;
}

const void* pointerOfCapsule(JSGlobalObject* globalObject, JSValue value, ASCIILiteral name)
{
    PyRecord* capsule = PyRecord::tryFrom(value, runtime(globalObject).typeFor(capsuleTypeDef));
    if (!capsule || stringOf(asString(capsule->get(0))) != name)
        return nullptr;
    return std::bit_cast<const void*>(static_cast<intptr_t>(capsule->number));
}

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
