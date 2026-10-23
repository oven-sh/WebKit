/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTReturnAddressWalk.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "ClonedArguments.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "ErrorConstructor.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "Interpreter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "ProxyObject.h"
#include "VMInlines.h"
#include <wtf/StackPointer.h>

namespace JSC { namespace AOT {

void throwNotAFunctionError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callee)
{
    throwException(globalObject, scope, createNotAFunctionError(globalObject, callee));
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
        throwException(globalObject, scope, createInvalidFunctionApplyParameterError(globalObject, list));
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

static StringView nameOfCalleeAtCallSite(VM&);

static JSValue constructValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue newTarget, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto constructData = JSC::getConstructData(callee);
    if (constructData.type == CallData::Type::None) {
        throwException(globalObject, scope, createNotAConstructorError(globalObject, callee, nameOfCalleeAtCallSite(vm)));
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
        throwException(globalObject, scope, createNotAConstructorError(globalObject, parent));
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareProxyCall, UGPRPair, (Instance* instance, CallFrame* hostCallFrame, uint32_t makesList))
{
    VM& vm = *instance->vm;
    // As for a native function.
    vm.topCallFrame = hostCallFrame;
    DeferTerminationForAWhile deferTermination(vm);
    const UGPRPair isEngines = makeUGPRPair(0, 0);
    // The target may be a proxy again, and no function is entered that would test the stack. performProxyCall() throws the error.
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return isEngines;
    ProxyObject* proxy = uncheckedDowncast<ProxyObject>(hostCallFrame->jsCallee());
    JSValue handlerValue = proxy->handler();
    if (!handlerValue.isObject())
        return isEngines;
    // ProxyObject::createStructure().
    if (!(proxy->inlineTypeFlags() & ImplementsDefaultHasInstance))
        return isEngines;
    JSObject* target = proxy->target();
    JSObject* handler = asObject(handlerValue);
    JSGlobalObject* globalObject = instance->globalObject;
    const Identifier& name = vm.aotState().runtimeTable->nameOfApplyTrap(vm);
    JSValue trap = Object::tryGetOwnDataProperty(vm, handler, name);
    if (!trap) {
        PropertySlot slot(handler, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isPresent = handler->getPropertySlot(globalObject, name, slot);
        if (vm.exceptionForInspection() || slot.isTaintedByOpaqueObject()) [[unlikely]]
            return isEngines;
        if (!isPresent)
            return makeUGPRPair(std::bit_cast<UCPURegister>(target), 0);
        if (!slot.isValue())
            return isEngines;
        trap = slot.getValue(globalObject, name);
    }
    if (trap.isUndefinedOrNull())
        return makeUGPRPair(std::bit_cast<UCPURegister>(target), 0);
    if (!trap.isCell())
        return isEngines;
    JSType typeOfTrap = trap.asCell()->type();
    if (typeOfTrap != JSFunctionType && !isSlimClosureType(typeOfTrap) && !trap.asCell()->isCallable())
        return isEngines;
    JSValue thisValue = hostCallFrame->thisValue();
    if (thisValue.isCell())
        hostCallFrame->setThisValue(thisValue.toThis(globalObject, ECMAMode::strict()));
    if (!makesList)
        return makeUGPRPair(std::bit_cast<UCPURegister>(trap.asCell()), 1);
    if (globalObject->isHavingABadTime())
        return isEngines;
    unsigned count = hostCallFrame->argumentCount();
    JSArray* list;
    {
        ObjectInitializationScope initializationScope(vm);
        list = JSArray::tryCreateUninitializedRestricted(initializationScope, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
        if (!list)
            return isEngines;
        for (unsigned i = 0; i < count; ++i)
            list->initializeIndexWithoutBarrier(initializationScope, i, hostCallFrame->uncheckedArgument(i), ArrayWithContiguous);
    }
    return makeUGPRPair(std::bit_cast<UCPURegister>(trap.asCell()), std::bit_cast<UCPURegister>(list));
}

// An unmapped arguments object with the Structure it was made with has its length and its elements as plain data: they are read without a question to anybody.
static JSArray* tryCopyUnmappedArguments(Instance* instance, JSValue arrayLike)
{
    constexpr uint32_t maxLengthCopied = 64;
    VM& vm = *instance->vm;
    if (!arrayLike.isCell() || arrayLike.asCell()->structure() != instance->state->builtinFamilies->unmappedArguments.structure)
        return nullptr;
    JSObject* arguments = asObject(arrayLike);
    if (!hasContiguous(arguments->indexingType()))
        return nullptr;
    JSValue length = arguments->getDirect(clonedArgumentsLengthPropertyOffset);
    if (!length.isUInt32() || length.asUInt32() > maxLengthCopied || length.asUInt32() > arguments->butterfly()->publicLength())
        return nullptr;
    uint32_t count = length.asUInt32();
    const WriteBarrier<Unknown>* elements = arguments->butterfly()->contiguous().data();
    if (containsHole(elements, count))
        return nullptr;
    ObjectInitializationScope initializationScope(vm);
    JSArray* list = JSArray::tryCreateUninitializedRestricted(initializationScope, instance->globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
    if (!list)
        return nullptr;
    for (uint32_t i = 0; i < count; ++i)
        list->initializeIndexWithoutBarrier(initializationScope, i, elements[i].get(), ArrayWithContiguous);
    ensureStillAliveHere(arguments);
    return list;
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
    if (JSArray* list = tryCopyUnmappedArguments(instance, JSValue::decode(arrayLike)))
        OPERATION_RETURN(scope, list);
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, JSValue::decode(arrayLike));
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), arguments));
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

static StringView nameOfCalleeAtCallSite(VM& vm)
{
    std::optional<FaultSite> site;
    forEachReturnAddressIntoCompiledFunctions(vm, [&](void*& returnAddress, const CallSiteCache::Entry&) {
        site = Image::current()->faultSiteAt(static_cast<const uint8_t*>(removeCodePtrTag(returnAddress)) - sizeOfNearCall);
        return IterationStatus::Done;
    });
    if (!site || site->kind != FaultSiteKind::NullCheck)
        return { };
    ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(site->throwSite[0]);
    if (static_cast<ThrowKind>(word.kind) != ThrowKind::NotAFunction)
        return { };
    return Image::current()->text().view(StringID(word.hasWideA ? site->throwSite[1] : word.a));
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
    uint32_t namePlusOne = std::exchange(instance->runtime.exceptions.nameOfUncallableMethod, 0);
    if (!value.isCallable()) {
        StringView nameAtCallSite = nameOfCalleeAtCallSite(vm);
        throwException(globalObject, scope, namePlusOne && nameAtCallSite.isNull() ? createNotAFunctionError(globalObject, value, identifierAt(instance, namePlusOne - 1)) : createNotAFunctionError(globalObject, value, nameAtCallSite));
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowUnlessCallableNamed, void, (Instance* instance, EncodedJSValue callee, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowUnlessCallableNamed);
    DeferGCForAWhile deferGC(vm);
    JSValue value = calleeOfDecodedReference(instance, callee);
    if (!value.isCallable()) {
        StringView nameAtCallSite = nameOfCalleeAtCallSite(vm);
        throwException(globalObject, scope, nameAtCallSite.isNull() ? createNotAFunctionError(globalObject, value, identifierAt(instance, name)) : createNotAFunctionError(globalObject, value, nameAtCallSite));
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
