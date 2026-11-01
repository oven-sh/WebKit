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
#include "AOTTypeRelations.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mayBeNullishBit = 1u << 31;
static constexpr uint32_t isForAnyFunctionBit = 1u << 30;
static constexpr uint32_t isForBuiltinBit = 1u << 29;
static constexpr uint32_t entryParameterMask = isForBuiltinBit - 1;
static constexpr OptionSet<FunctionCapabilities> capabilitiesVersionsInherit { FunctionCapabilities::IsHiddenInStackTraces };

static DerivedFunctionKey makeCalleeVersionKey(FunctionID function, uint32_t entryParameter, uint32_t kindBit, bool mayBeNullish, uint32_t callee)
{
    RELEASE_ASSERT(entryParameter <= entryParameterMask);
    return { .role = FunctionRole::CalleeVersion, .target = function, .parameter = entryParameter | kindBit | (mayBeNullish ? mayBeNullishBit : 0), .node = callee };
}

DerivedFunctionKey calleeVersionKeyForAnyFunction(FunctionID function, uint32_t entryParameter, bool mayBeNullish)
{
    return makeCalleeVersionKey(function, entryParameter, isForAnyFunctionBit, mayBeNullish, 0);
}

DerivedFunctionKey calleeVersionKey(FunctionID function, uint32_t entryParameter, FunctionID callee, bool mayBeNullish)
{
    RELEASE_ASSERT(callee && !ProvisionalFunctions::contains(callee));
    return makeCalleeVersionKey(function, entryParameter, 0, mayBeNullish, callee.index());
}

DerivedFunctionKey calleeVersionKeyForBuiltin(FunctionID function, uint32_t entryParameter, BuiltinID callee, bool mayBeNullish)
{
    RELEASE_ASSERT(callee);
    return makeCalleeVersionKey(function, entryParameter, isForBuiltinBit, mayBeNullish, callee.index());
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

bool calleeVersionIsFor(const Function& version, FunctionID callee)
{
    return version.role == FunctionRole::CalleeVersion && !(version.derivation.parameter & (isForAnyFunctionBit | isForBuiltinBit)) && version.derivation.node == callee.index();
}

bool canHaveCalleeVersion(const Program& program, FunctionID id, uint32_t entryParameter)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || !function.mayBeCloned() || function.isUnsplitCoroutine() || function.isModuleBody())
        return false;
    if (entryParameter >= function.numberOfEntryParameters() || function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    for (const Function* version = &function; version->role == FunctionRole::CalleeVersion; version = &program.at(version->derivation.source)) {
        if ((version->derivation.parameter & isForAnyFunctionBit) && (version->derivation.parameter & entryParameterMask) == entryParameter)
            return false;
    }
    const Type& parameter = program.typeOf(function, function.at(entryBlock).parameters[entryParameter]);
    return !parameter.singleFunction() && !parameter.singleBuiltin();
}

Function buildCalleeVersion(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& original = program.at(key.target);
    uint32_t entryParameter = key.parameter & entryParameterMask;
    bool isForAnyFunction = key.parameter & isForAnyFunctionBit;
    RELEASE_ASSERT(canHaveCalleeVersion(program, key.target, entryParameter));

    Function version = original;
    version.role = FunctionRole::CalleeVersion;
    version.creator = CreatorRecord::Version { .provenVersion = key.target };
    version.capabilities = initialCapabilitiesOf(version) | (original.capabilities & capabilitiesVersionsInherit);
    version.derivation = Derivation {
        .source = key.target,
        .parameter = key.parameter,
        .node = key.node,
        .parameterMap = Vector<ParameterOrigin>(original.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };

    Value& parameter = version.edit(version.at(entryBlock).parameters[entryParameter]);
    Type type = isForAnyFunction ? Type::function() : key.parameter & isForBuiltinBit ? Type::forBuiltin(BuiltinID(key.node)) : Type::forFunction(FunctionID(key.node));
    if (key.parameter & mayBeNullishBit)
        type = type.withPrimitives(program.at(parameter.type).primitives & Primitive::UndefinedOrNull);
    if (isForAnyFunction) {
        // Met with `function`, a built-in object that is no function is named `function`, which is outside the type the body was typed with. No caller passes one.
        Type received = program.at(parameter.type);
        removeAllMatching(received.builtins, [&](BuiltinID builtin) {
            return !isCallableBuiltin(program.builtins(), builtin);
        });
        type = received.intersectWith(type, inputs.types);
        // So is an object of a class whose objects may be functions, a Proxy. It keeps the name that it has in the type of the body.
        if (type.anyFunction && !type.isSubsetOf(received, program)) {
            type.anyFunction = false;
            for (const ClassPart& part : received.classes) {
                if (!isFunctionClass(program.builtins(), part.builtinClass))
                    continue;
                Type named;
                named.classes.append(part);
                type = type.unionWith(named, inputs.types);
            }
        }
    }
    parameter.type = inputs.types.intern(type);
    parameter.setExpected({ });
    *version.declaredEntryParameter(entryParameter) = DeclaredType(Proven::byPosition(parameter.type));
    return version;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
