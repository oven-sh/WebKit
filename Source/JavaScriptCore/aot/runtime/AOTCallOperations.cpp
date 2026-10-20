/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTTypeErrorMessages.h"
#include "DeferGCInlines.h"
#include "ErrorConstructor.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "Interpreter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "VMInlines.h"
#include <wtf/StackPointer.h>

namespace JSC { namespace AOT {

void throwNotAFunctionError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callee)
{
    String description = errorDescriptionForValue(globalObject, callee);
    RETURN_IF_EXCEPTION(scope, void());
    throwTypeError(globalObject, scope, makeString(description, " is not a function"_s));
}

void throwNotAFunctionError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callee, const Identifier& name)
{
    throwTypeError(globalObject, scope, makeString(name.string(), " is not a function, it is "_s, describe(callee)));
}

static bool fitsOnStack(VM& vm, uint64_t length)
{
    return length <= maxArguments && vm.ensureJSStackCapacityFor(static_cast<Register*>(currentStackPointer()) - length);
}

static bool appendArrayLike(JSGlobalObject* globalObject, VM& vm, MarkedArgumentBuffer& arguments, JSValue list)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (list.isUndefinedOrNull())
        return true;
    if (!list.isObject()) {
        throwTypeError(globalObject, scope, "second argument to Function.prototype.apply must be an Array-like object"_s);
        return false;
    }
    JSObject* object = asObject(list);
    uint64_t length;
    if (auto* array = dynamicDowncast<JSArray>(object)) [[likely]]
        length = array->length();
    else {
        length = toLength(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!fitsOnStack(vm, length)) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        arguments.append(object->getIndex(globalObject, index));
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

static JSValue constructValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue newTarget, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto constructData = JSC::getConstructData(callee);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, makeString(describe(callee), " is not a constructor"_s));
        return { };
    }
    RELEASE_AND_RETURN(scope, construct(globalObject, callee, constructData, arguments, newTarget));
}

static JSValue constructParent(JSGlobalObject* globalObject, JSValue parent, JSValue newTarget, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto constructData = JSC::getConstructData(parent);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
        return { };
    }
    if (auto* errorConstructor = dynamicDowncast<ErrorConstructor>(parent); errorConstructor && errorConstructor->realm() == globalObject && newTarget.isObject() && newTarget != parent) {
        Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, asObject(newTarget), errorConstructor);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, ErrorInstance::create(globalObject, structure, arguments.at(0), arguments.at(1), nullptr, TypeNothing, ErrorType::Error, true, asObject(newTarget)));
    }
    RELEASE_AND_RETURN(scope, construct(globalObject, parent, constructData, arguments, newTarget));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue newTarget, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConstruct);
    MarkedArgumentBuffer arguments;
    fill(arguments, count, argumentValues);
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(constructValue(globalObject, vm, JSValue::decode(callee), JSValue::decode(newTarget), arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTApply, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTApply);
    JSValue function = JSValue::decode(callee);
    auto callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None) {
        throwNotAFunctionError(globalObject, scope, function);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, JSValue::decode(list));
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, callData, JSValue::decode(thisValue), arguments)));
}

// Under validation, for the calls of native functions that would go through the engine's own trampoline: the interpreter compares the call with the entry of the built-in.
JSC_DEFINE_JIT_OPERATION(operationAOTCallNativeFunction, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCallNativeFunction);
    MarkedArgumentBuffer arguments;
    fill(arguments, count, argumentValues);
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSValue function = JSValue::decode(callee);
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, JSC::getCallData(function), JSValue::decode(thisValue), arguments)));
}

// Function.prototype.apply up to the call: the callee is tested before the list is read.
JSC_DEFINE_JIT_OPERATION(operationAOTNewArgumentList, JSCell*, (Instance* instance, EncodedJSValue callee, EncodedJSValue arrayLike))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArgumentList);
    JSValue function = JSValue::decode(callee);
    if (!function.isCallable()) {
        throwNotAFunctionError(globalObject, scope, function);
        OPERATION_RETURN(scope, nullptr);
    }
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, JSValue::decode(arrayLike));
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), arguments));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConstructWithArray, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue newTarget, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConstructWithArray);
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, JSValue::decode(list));
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(constructValue(globalObject, vm, JSValue::decode(callee), JSValue::decode(newTarget), arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue parent, EncodedJSValue newTarget, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperConstruct);
    MarkedArgumentBuffer arguments;
    fill(arguments, count, argumentValues);
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(constructParent(globalObject, JSValue::decode(parent), JSValue::decode(newTarget), arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperConstructList, EncodedJSValue, (Instance* instance, EncodedJSValue parent, EncodedJSValue newTarget, JSCell* list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperConstructList);
    JSArray* array = cellArgument<JSArray>(list, Entry::operationAOTSuperConstructList);
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, array);
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(constructParent(globalObject, JSValue::decode(parent), JSValue::decode(newTarget), arguments)));
}

static JSValue calleeOfDecodedReference(Instance* instance, EncodedJSValue callee)
{
    JSValue value = valueOfDecodedReference(instance, static_cast<uint64_t>(callee));
    return value ? value : jsUndefined();
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowUnlessCallable, void, (Instance* instance, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowUnlessCallable);
    DeferGCForAWhile deferGC(vm);
    JSValue value = calleeOfDecodedReference(instance, callee);
    uint32_t nameOfMethod = std::exchange(instance->runtime.exceptions.nameOfUncallableMethod, 0);
    if (value.isCallable())
        OPERATION_RETURN(scope);
    if (nameOfMethod)
        throwNotAFunctionError(globalObject, scope, value, identifierAt(instance, nameOfMethod - 1));
    else
        throwNotAFunctionError(globalObject, scope, value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowUnlessCallableNamed, void, (Instance* instance, EncodedJSValue callee, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowUnlessCallableNamed);
    DeferGCForAWhile deferGC(vm);
    JSValue value = calleeOfDecodedReference(instance, callee);
    if (!value.isCallable())
        throwNotAFunctionError(globalObject, scope, value, identifierAt(instance, name));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
