/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledCallee.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

std::optional<FunctionID> functionOf(const JSCell* cell)
{
    switch (cell->type()) {
    case SlimClosureType:
        return uncheckedDowncast<SlimClosure>(cell)->function();
    case SlimClosureWithPropertiesType:
        return uncheckedDowncast<ReifiedSlimClosure>(cell)->function();
    case CompiledClosureType:
        return uncheckedDowncast<Closure>(cell)->function();
    default:
        return std::nullopt;
    }
}

static FunctionID checkedFunctionOf(const JSCell* callee)
{
    std::optional<FunctionID> function = functionOf(callee);
    RELEASE_ASSERT(function);
    return *function;
}

static ClosureTable& closureTableOf(JSObject* callee)
{
    return State::of(callee->structure()->realm()).objects->closures;
}

namespace CompiledCallee {

bool isCompiledCallee(const JSCell* cell)
{
    return isCompiledCalleeType(cell->type());
}

String name(JSObject* callee)
{
    return initialNameProperty(checkedFunctionOf(callee));
}

unsigned length(JSObject* callee)
{
    return functionLengthOf(checkedFunctionOf(callee));
}

JSValue sourceText(JSGlobalObject* globalObject, JSObject* callee)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "function "_s, writtenFunctionName(function), "() { [native code] }"_s));
    ASCIILiteral async = kindData.isAsync ? "async "_s : ""_s;
    bool isProgramFunction = !kindData.isInRuntimeModule;
    JSString* text = isProgramFunction ? functionSourceText(globalObject, function) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    if (isProgramFunction && callableKind == CallableKind::Arrow)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s));
    bool isWrittenWithoutName = callableKind == CallableKind::Arrow || (!isProgramFunction && kindData.hasInferredName);
    String name = isWrittenWithoutName ? emptyString() : initialNameProperty(function);
    if (kindData.isConstructor)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s));
    if (isProgramFunction && callableKind == CallableKind::Method)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "*"_s : ""_s, name, "() { [native code] }"_s));
    RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "function* "_s : "function "_s, name, "() { [native code] }"_s));
}

bool isBoundFunction(JSObject* callee)
{
    return functionKindData(checkedFunctionOf(callee)).callable() == CallableKind::Bound;
}

FunctionKind kind(JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    if (kindData.isInRuntimeModule)
        return FunctionKind::Builtin;
    if (kindData.isConstructor)
        return FunctionKind::Class;
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        return FunctionKind::Strict;
    if (kindData.isGenerator)
        return kindData.isAsync ? FunctionKind::AsyncGenerator : FunctionKind::Generator;
    if (kindData.isAsync)
        return FunctionKind::Async;
    switch (callableKind) {
    case CallableKind::Arrow:
        return FunctionKind::Arrow;
    case CallableKind::Method:
        return FunctionKind::Method;
    case CallableKind::Plain:
        return FunctionDescription(function).isStrict() ? FunctionKind::Strict : FunctionKind::Sloppy;
    case CallableKind::Bound:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String displayName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    if (FunctionDescription(*function).isNamedBySymbol())
        return emptyString();
    return writtenFunctionName(*function);
}

bool reifyProperties(VM& vm, JSObject* object)
{
    if (object->type() != SlimClosureType)
        return false;
    uncheckedDowncast<SlimClosure>(object)->reify(vm);
    return true;
}

void setPrototypeProperty(VM& vm, JSObject* callee, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(isCompiledCallee(callee));
    if (callee->type() == CompiledClosureType) {
        uncheckedDowncast<Closure>(callee)->setPrototypeProperty(vm, prototype, attributes);
        return;
    }
    reifyProperties(vm, callee);
    callee->putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void setNameProperty(JSGlobalObject* globalObject, JSObject* callee, JSString* name, unsigned attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(isCompiledCallee(callee));
    String text = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (callee->type() == CompiledClosureType)
        uncheckedDowncast<Closure>(callee)->setNameProperty(vm, name, attributes);
    else {
        reifyProperties(vm, callee);
        callee->putDirect(vm, vm.propertyNames->name, name, attributes);
    }
    didDefineNameProperty(vm, callee, text);
}

Structure* cachedBoundFunctionStructure(JSObject* callee)
{
    if (!isSlimClosureType(callee->type()))
        return nullptr;
    return closureTableOf(callee).asyncBoundFunctionStructure();
}

void setCachedBoundFunctionStructure(VM&, JSObject* callee, Structure* structure)
{
    if (isSlimClosureType(callee->type()))
        closureTableOf(callee).setAsyncBoundFunctionStructure(structure);
}

} // namespace CompiledCallee

DeclaredAs declaredAs(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    if (!function)
        return DeclaredAs::NotCompiled;
    FunctionKindData kindData = functionKindData(*function);
    if (kindData.isConstructor)
        return DeclaredAs::Class;
    if (kindData.callable() == CallableKind::Bound)
        return DeclaredAs::Function;
    if (kindData.isGenerator)
        return kindData.isAsync ? DeclaredAs::AsyncGeneratorFunction : DeclaredAs::GeneratorFunction;
    return kindData.isAsync ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
}

String closureDeclaredName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (takesNameFromOwner && kindData.callable() != CallableKind::Bound)
        return emptyString();
    return writtenFunctionName(*function);
}

bool belongsToBuiltinModule(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    return function && functionKindData(*function).isInRuntimeModule;
}

bool isCompiledClassObject(JSFunction* candidate)
{
    std::optional<FunctionID> function = functionOf(candidate);
    return function && functionKindData(*function).isConstructor;
}

bool isClosureOf(JSCell* cell, FunctionID function)
{
    if (std::optional<FunctionID> closureFunction = functionOf(cell))
        return sourceFunctionOf(*closureFunction) == sourceFunctionOf(function);
    std::optional<uint32_t> implemented = programTables().builtinImplementedBy(function.index());
    JSObject* object = cell->getObject();
    if (!implemented || !object)
        return false;
    JSGlobalObject* realm = object->realmMayBeNull();
    if (!realm)
        return false;
    auto found = findInRealm(realm, BuiltinCellKey::object(BuiltinID(*implemented)));
    return found && *found == cell;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
