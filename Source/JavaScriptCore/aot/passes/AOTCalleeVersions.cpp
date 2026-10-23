/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCalleeVersions.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTDerivedFunctions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mayBeNullishBit = 1u << 31;
static constexpr uint32_t isForAnyFunctionBit = 1u << 30;
static constexpr uint32_t isForBuiltinBit = 1u << 29;
static constexpr uint32_t entryParameterMask = isForBuiltinBit - 1;
static constexpr OptionSet<FunctionCapabilities> inheritedCapabilities { FunctionCapabilities::IsHiddenInStackTraces };

static uint32_t entryParameterIndex(uint32_t keyParameter)
{
    return keyParameter & entryParameterMask;
}

static bool isForAnyFunction(uint32_t keyParameter)
{
    return keyParameter & isForAnyFunctionBit;
}

static bool isForBuiltin(uint32_t keyParameter)
{
    return keyParameter & isForBuiltinBit;
}

static bool argumentMayBeNullish(const DerivedFunctionKey& key)
{
    return key.parameter & mayBeNullishBit;
}

DerivedFunctionKey calleeVersionKeyForAnyFunction(FunctionID function, uint32_t entryParameter, bool mayBeNullish)
{
    RELEASE_ASSERT(entryParameter <= entryParameterMask);
    return {
        .role = FunctionRole::CalleeVersion,
        .target = function,
        .parameter = entryParameter | isForAnyFunctionBit | (mayBeNullish ? mayBeNullishBit : 0),
        .node = 0,
    };
}

DerivedFunctionKey calleeVersionKey(FunctionID function, uint32_t entryParameter, FunctionID callee, bool mayBeNullish)
{
    RELEASE_ASSERT(callee && !ProvisionalFunctions::contains(callee) && entryParameter <= entryParameterMask);
    return {
        .role = FunctionRole::CalleeVersion,
        .target = function,
        .parameter = entryParameter | (mayBeNullish ? mayBeNullishBit : 0),
        .node = callee.index(),
    };
}

DerivedFunctionKey calleeVersionKeyForBuiltin(FunctionID function, uint32_t entryParameter, BuiltinID callee, bool mayBeNullish)
{
    RELEASE_ASSERT(callee && entryParameter <= entryParameterMask);
    return {
        .role = FunctionRole::CalleeVersion,
        .target = function,
        .parameter = entryParameter | isForBuiltinBit | (mayBeNullish ? mayBeNullishBit : 0),
        .node = callee.index(),
    };
}

BuiltinID singleBuiltinFunction(const Program& program, const Type& type)
{
    BuiltinID builtin = type.singleBuiltin();
    if (!builtin)
        return { };
    switch (program.builtins().at(builtin).kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return builtin;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canHaveCalleeVersion(const Program& program, FunctionID id, uint32_t entryParameter)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || !function.mayBeCloned() || function.isUnsplitCoroutine() || function.isModuleBody())
        return false;
    if (entryParameter >= function.numberOfEntryParameters() || function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    if (function.role == FunctionRole::CalleeVersion && isForAnyFunction(function.derivation.parameter) && entryParameterIndex(function.derivation.parameter) == entryParameter)
        return false;
    const Type& parameter = program.typeOf(function, function.at(entryBlock).parameters[entryParameter]);
    return !parameter.singleFunction() && !parameter.singleBuiltin();
}

Function buildCalleeVersion(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& original = program.at(key.target);
    uint32_t entryParameter = entryParameterIndex(key.parameter);
    RELEASE_ASSERT(canHaveCalleeVersion(program, key.target, entryParameter));

    Function version = original;
    version.role = FunctionRole::CalleeVersion;
    version.creator = CreatorRecord::Version { .provenVersion = key.target };
    version.capabilities = initialCapabilitiesOf(version) | (original.capabilities & inheritedCapabilities);
    version.derivation = Derivation {
        .source = key.target,
        .parameter = key.parameter,
        .node = key.node,
        .parameterMap = Vector<ParameterOrigin>(original.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };

    Value& parameter = version.edit(version.at(entryBlock).parameters[entryParameter]);
    Type type = isForAnyFunction(key.parameter) ? Type::function() : isForBuiltin(key.parameter) ? Type::forBuiltin(BuiltinID(key.node)) : Type::forFunction(FunctionID(key.node));
    if (argumentMayBeNullish(key))
        type = type.withPrimitives(program.at(parameter.type).primitives & Primitive::UndefinedOrNull);
    if (isForAnyFunction(key.parameter))
        type = program.at(parameter.type).intersectWith(type, inputs.types);
    parameter.type = inputs.types.intern(type);
    parameter.setExpected({ });
    *version.declaredEntryParameter(entryParameter) = DeclaredType { .type = parameter.type };
    return version;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
