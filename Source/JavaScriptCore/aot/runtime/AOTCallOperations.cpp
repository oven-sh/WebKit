/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTCallSiteCache.h"
#include "AOTClosure.h"
#include "AOTCompiledCall.h"
#include "AOTCompiledCaller.h"
#include "AOTCompiledFrame.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionTables.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTStackTrace.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsCallAdapters.h"
#include "AOTThreshold.h"
#include "AOTUnwinding.h"
#include "CallFrameInlines.h"
#include "CellSize.h"
#include "ClonedArguments.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "DirectArguments.h"
#include "ErrorConstructor.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "InternalFunction.h"
#include "Interpreter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "LLIntThunks.h"
#include "ObjectPrototype.h"
#include "OwedOutOfMemoryError.h"
#include "ProtoCallFrameInlines.h"
#include "ProxyObject.h"
#include "VMEntryRecord.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include <algorithm>
#include <array>
#include <wtf/StackPointer.h>

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
#include <sanitizer/asan_interface.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct ArgumentsObjectSource {
    uint32_t count;
    JSArray* rest;
    const EncodedJSValue* parameters;
    uint32_t numberInFrame;
};

static ArgumentsObjectSource createArgumentSource(uint32_t count, JSCell* rest, const EncodedJSValue* parameters, Entry entry)
{
    JSArray* array = cellArgument<JSArray>(rest, entry);
    unsigned restLength = array->length();
    if (restLength > count) [[unlikely]]
        crashOnWrongOperand(entry);
    return { count, array, parameters, count - restLength };
}

static JSValue argumentAt(JSGlobalObject* globalObject, const ArgumentsObjectSource& source, uint32_t index)
{
    if (index < source.numberInFrame)
        return JSValue::decode(source.parameters[index]);
    return source.rest->getIndex(globalObject, index - source.numberInFrame);
}

static bool canReadRestQuickly(const ArgumentsObjectSource& source)
{
    unsigned restLength = source.count - source.numberInFrame;
    if (!restLength)
        return true;
    IndexingType type = source.rest->indexingType();
    return (hasContiguous(type) || hasInt32(type)) && !containsHole(source.rest->butterfly()->contiguous().data(), restLength);
}

// The object adds `callee` and Symbol.iterator to itself when they are first listed or changed. Until then it asks its callee whether the code is strict.
static JSFunction* calleeOfStrictCode(JSGlobalObject* globalObject)
{
    return globalObject->arrayProtoValuesFunction();
}

static ClonedArguments* tryCreateArgumentsFast(Instance* instance, const ArgumentsObjectSource& source)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    if (!canReadRestQuickly(source))
        return nullptr;
    Structure* structure = globalObject->clonedArgumentsStructure();
    if (!hasContiguous(structure->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->storedPrototypeObject()->needsSlowPutIndexing())
        return nullptr;
    ClonedArguments* result = ClonedArguments::createEmpty(vm, nullptr, structure, calleeOfStrictCode(globalObject), source.count, nullptr);
    if (!result)
        return nullptr;

    auto elements = result->butterfly()->contiguous();
    for (uint32_t i = 0; i < source.numberInFrame; ++i)
        elements.atUnsafe(i).setWithoutWriteBarrier(JSValue::decode(source.parameters[i]));
    if (uint32_t restLength = source.count - source.numberInFrame) {
        const WriteBarrier<Unknown>* rest = source.rest->butterfly()->contiguous().data();
        for (uint32_t i = 0; i < restLength; ++i)
            elements.atUnsafe(source.numberInFrame + i).setWithoutWriteBarrier(rest[i].get());
    }
    return result;
}

static ClonedArguments* createArguments(Instance* instance, const ArgumentsObjectSource& source)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (ClonedArguments* result = tryCreateArgumentsFast(instance, source))
        return result;
    ClonedArguments* result = ClonedArguments::createEmpty(vm, globalObject, globalObject->clonedArgumentsStructure(), calleeOfStrictCode(globalObject), source.count, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!result) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        JSValue value = argumentAt(globalObject, source, i);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result->putDirectIndex(globalObject, i, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateUnmappedArguments, JSCell*, (Instance* instance, uint32_t count, JSCell* rest, const EncodedJSValue* parameters))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateUnmappedArguments);
    OPERATION_RETURN(scope, createArguments(instance, createArgumentSource(count, rest, parameters, Entry::operationAOTCreateUnmappedArguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFromArguments, EncodedJSValue, (Instance* instance, JSCell* list, EncodedJSValue property))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetFromArguments);
    JSArray* array = cellArgument<JSArray>(list, Entry::operationAOTGetFromArguments);
    JSValue key = JSValue::decode(property);
    if (auto index = key.tryGetAsUint32Index())
        OPERATION_RETURN(scope, JSValue::encode(*index < array->length() ? array->getIndex(globalObject, *index) : jsUndefined()));
    ClonedArguments* arguments = createArguments(instance, ArgumentsObjectSource { array->length(), array, nullptr, 0 });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto propertyKey = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(arguments->get(globalObject, propertyKey)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateMappedArguments, JSCell*, (Instance* instance, JSCell* callee, uint32_t count, JSCell* rest, const EncodedJSValue* parameters))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateMappedArguments);
    Closure* closure = cellArgument<Closure>(callee, Entry::operationAOTCreateMappedArguments);
    ArgumentsObjectSource source = createArgumentSource(count, rest, parameters, Entry::operationAOTCreateMappedArguments);
    // A function with a mapped arguments object has a simple parameter list, so its length is its number of parameters.
    unsigned capacity = std::max<unsigned>(count, functionLengthOf(closure->function()));
    DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), count, capacity);
    result->setCallee(vm, closure);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = argumentAt(globalObject, source, i);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        result->argument(DirectArgumentsOffset(i)).set(vm, result, value);
    }
    // A parameter that was not passed lives in its slot too, and an object created late takes the value the parameter has by then.
    for (uint32_t i = count; i < capacity; ++i)
        result->argument(DirectArgumentsOffset(i)).set(vm, result, JSValue::decode(parameters[i]));
    OPERATION_RETURN(scope, result);
}

static DirectArguments* checkedMappedArguments(JSCell* arguments, uint32_t index, Entry entry)
{
    auto* object = cellArgument<DirectArguments>(arguments, entry);
    if (DirectArguments::offsetOfSlot(index) >= cellSize(object)) [[unlikely]]
        crashOnWrongOperand(entry);
    return object;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetMappedArgument, EncodedJSValue, (Instance*, JSCell* arguments, uint32_t index))
{
    DirectArguments* object = checkedMappedArguments(arguments, index, Entry::operationAOTGetMappedArgument);
    return JSValue::encode(object->argument(DirectArgumentsOffset(index)).get());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetMappedArgument, void, (Instance* instance, JSCell* arguments, uint32_t index, EncodedJSValue value))
{
    DirectArguments* object = checkedMappedArguments(arguments, index, Entry::operationAOTSetMappedArgument);
    object->argument(DirectArgumentsOffset(index)).set(*instance->vm, object, JSValue::decode(value));
}

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

template<bool isParent>
static JSValue constructValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue newTarget, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto constructData = JSC::getConstructData(callee);
    if (constructData.type == CallData::Type::None) {
        StringView name = isParent ? StringView() : nameOfCalleeAtCallSite(vm);
        if (auto* function = dynamicDowncast<JSFunction>(callee); function && function->isHostFunction()) {
            scope.release();
            throwNotAConstructorErrorInFrameOf(globalObject, function, name);
            return { };
        }
        throwException(globalObject, scope, isParent ? createNotAConstructorError(globalObject, callee) : createNotAConstructorError(globalObject, callee, name));
        return { };
    }
    if (auto* errorConstructor = isParent ? dynamicDowncast<ErrorConstructor>(callee) : nullptr; errorConstructor && errorConstructor->realm() == globalObject && newTarget.isObject() && newTarget != callee) {
        Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, asObject(newTarget), errorConstructor);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, ErrorInstance::create(globalObject, structure, arguments.at(0), arguments.at(1), nullptr, TypeNothing, ErrorType::Error, true, asObject(newTarget)));
    }
    RELEASE_AND_RETURN(scope, constructAllowingPrimitiveResult(globalObject, callee, constructData, arguments, newTarget));
}

// The argument values lie in the stack of the caller for as long as the call lasts.
JSC_DEFINE_JIT_OPERATION(operationAOTConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue newTarget, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConstruct);
    ArgList arguments(const_cast<EncodedJSValue*>(argumentValues), count);
    OPERATION_RETURN(scope, JSValue::encode(constructValue<false>(globalObject, vm, JSValue::decode(callee), JSValue::decode(newTarget), arguments)));
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

// For a long list, and under validation for the calls of native functions that would go through the engine's own trampoline: the interpreter compares the call with the entry of the built-in.
JSC_DEFINE_JIT_OPERATION(operationAOTCallEngineCallee, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCallEngineCallee);
    ArgList arguments(const_cast<EncodedJSValue*>(argumentValues), count);
    JSValue function = JSValue::decode(callee);
    auto callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None) {
        throwNotAFunctionError(globalObject, scope, function);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, callData, JSValue::decode(thisValue), arguments)));
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
    // As forwardProxyCallToTarget(): the target is called in place of the proxy, and no frame stands for the proxy.
    if (JSObject* forwardedTo = proxy->callTargetIfKnownToHaveNoApplyTrap()) {
        while (auto* next = dynamicDowncast<ProxyObject>(forwardedTo)) {
            JSObject* targetOfNext = next->callTargetIfKnownToHaveNoApplyTrap();
            if (!targetOfNext)
                break;
            forwardedTo = targetOfNext;
        }
        return makeUGPRPair(0, std::bit_cast<UCPURegister>(forwardedTo));
    }
    JSObject* target = proxy->target();
    JSObject* handler = asObject(handlerValue);
    JSGlobalObject* globalObject = instance->globalObject;
    const Identifier& name = vm.aotState().runtimeTable->nameOfApplyTrap(vm);
    // The lookup has run no code of the program, so this one runs none. The proxy remembers the answer, as in performProxyCall(), which it only can below Object.prototype.
    auto callTargetAndRemember = [&] {
        if (handler->getPrototypeDirect().inherits<ObjectPrototype>()) {
            // How many proxies of a chain remember after a call that ran out of stack depends on the frames between them: they are performProxyCall()'s.
            if (target->type() == ProxyObjectType)
                return isEngines;
            CallData callData;
            proxy->getHandlerTrap(globalObject, handler, callData, name, ProxyObject::HandlerTrap::Apply);
        }
        return makeUGPRPair(std::bit_cast<UCPURegister>(target), 0);
    };
    JSValue trap = Object::tryGetOwnDataProperty(vm, handler, name);
    if (!trap) {
        PropertySlot slot(handler, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isPresent = handler->getPropertySlot(globalObject, name, slot);
        if (vm.exceptionForInspection() || slot.isTaintedByOpaqueObject()) [[unlikely]]
            return isEngines;
        if (!isPresent)
            return callTargetAndRemember();
        if (!slot.isValue())
            return isEngines;
        trap = slot.getValue(globalObject, name);
    }
    if (trap.isUndefinedOrNull())
        return callTargetAndRemember();
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
    AOT_THRESHOLD(uint32_t, maxLengthCopied, 64);
    VM& vm = *instance->vm;
    if (!arrayLike.isCell() || arrayLike.asCell()->structure() != instance->globalObject->clonedArgumentsStructure())
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
    OPERATION_RETURN(scope, JSValue::encode(constructValue<false>(globalObject, vm, JSValue::decode(callee), JSValue::decode(newTarget), arguments)));
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
    OPERATION_RETURN(scope, JSValue::encode(constructValue<true>(globalObject, vm, JSValue::decode(parent), JSValue::decode(newTarget), arguments)));
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
    OPERATION_RETURN(scope, JSValue::encode(constructValue<true>(globalObject, vm, JSValue::decode(parent), JSValue::decode(newTarget), arguments)));
}

static StringView nameOfCalleeAt(const CallSiteCache::Entry& callSite)
{
    std::optional<FaultSite> site = Image::current()->faultSiteAt(static_cast<const uint8_t*>(callSite.address) - sizeOfNearCall);
    if (!site || site->kind != FaultSiteKind::NullCheck)
        return { };
    ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(site->throwSite[0]);
    if (static_cast<ThrowKind>(word.kind) != ThrowKind::NotAFunction)
        return { };
    return Image::current()->text().view(StringID(word.hasWideA ? site->throwSite[1] : word.narrowA()));
}

static StringView nameOfCalleeAtCallSite(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
    if (!frames || frames->isAtEnd())
        return { };
    return nameOfCalleeAt(frames->callSite());
}

StringView instanceofExpressionOnTop(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames || frames->isAtEnd())
        return { };
    return nameOfCalleeAt(frames->callSite());
}

bool readOnTopTestsObjectOfUpdate(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames || frames->isAtEnd())
        return false;
    const CallSiteCache::Entry& callSite = frames->callSite();
    std::optional<SourcePosition> position = Image::current()->positionOf(callSite.code.index(), callSite.site.callSite);
    return position && position->startIsForCallers;
}

StringView nameOfConstructorThatRunsOnTop(VM& vm)
{
    CallFrame* constructorFrame = vm.topCallFrame;
    EntryFrame* entryFrame = vm.topEntryFrame;
    if (!constructorFrame || !entryFrame || isTopFrameCompiled(vm))
        return { };
    CallFrame* calleeOfCompiledCode = constructorFrame;
    if (constructorFrame->callerFrameOrEntryFrame() == entryFrame) {
        VMEntryRecord* record = vmEntryRecord(entryFrame);
        calleeOfCompiledCode = record->prevTopCallFrame();
        entryFrame = record->prevTopEntryFrame();
        if (!calleeOfCompiledCode)
            return { };
    }
    auto frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, calleeOfCompiledCode, entryFrame);
    if (!frames || frames->isAtEnd() || !frames->callSite().site.isConstruct())
        return { };
    return nameOfCalleeAt(frames->callSite());
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
    if (!value.isCallable())
        throwException(globalObject, scope, createNotAFunctionError(globalObject, value, nameOfCalleeAtCallSite(vm)));
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

JSC_DEFINE_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowStackOverflowError);
    DeferGCForAWhile deferGC(vm);
    if (!throwOutOfMemoryErrorIfOwed(vm, globalObject, scope)) [[likely]]
        throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

// A caller that relies on the type has a landing for what is not of it. Any other caller takes whatever is returned, at its return address.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindLandingOfMisfit, void*, (Instance* instance, void* returnAddress, EncodedJSValue misfit))
{
    const VMState& state = instance->vm->aotState();
    const CallSiteCache::Entry* site = state.callSiteCache->find(returnAddress);
    if (!site)
        site = state.callSiteCache->resolve(*state.image, returnAddress);
    if (!site || site->isStubReturnSite() || !site->handler || site->handler->catchesExceptions())
        return returnAddress;
    instance->scratch.misfit = misfit;
    return const_cast<uint8_t*>(state.image->codeOf(site->code.index())) + site->handler->codeOffset;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM* vmPointer, Instance* instance))
{
    VM& vm = *vmPointer;
    CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD();
    CallFrame* callFrame = frameRecord->callerFrame;
    ExceptionWords& exceptions = instance->runtime.exceptions;
    Exception* exception = vm.exceptionForInspection();
    bool needsException = false;
    Exception* rethrownException = nullptr;
    if (exception)
        exceptions.thrownValue = 0;
    else {
        JSValue thrown = JSValue::decode(exceptions.thrownValue);
        needsException = needsExceptionForEveryThrow(*instance);
        if (thrown.isCell() && thrown.asCell()->type() == CellType)
            rethrownException = uncheckedDowncast<Exception>(thrown.asCell());
    }
    bool isTermination = exception && vm.isTerminationException(exception);
    CompiledUnwindResult found = isTermination ? CompiledUnwindResult { } : unwindRun(vm, { callFrame->rawReturnPC(), stackPointerOfCaller(callFrame), callFrame->callerFrame(), callFrame }, vm.topEntryFrame, *vmEntryRecord(vm.topEntryFrame), false);
    bool isHandled = found.status == CompiledUnwindStatus::FoundHandler;
    RELEASE_ASSERT(!isHandled || found.handler.code);
    bool takesException = found.handler.kind == CompiledHandlerKind::Finally;
    UGPRPair handlerCodeAndFrame = makeUGPRPair(std::bit_cast<UCPURegister>(found.handler.code), std::bit_cast<UCPURegister>(found.handler.framePointer));
    if (rethrownException && isHandled && !needsException) {
        if (!takesException)
            exceptions.thrownValue = JSValue::encode(rethrownException->value());
        return handlerCodeAndFrame;
    }
    bool clearsDeadStack = false;
    if (isHandled && vm.heap.cage().hasDeadStackToClear()) [[unlikely]] {
        clearsDeadStack = true;
        ExceptionWords& wordsOfHandler = found.instance->runtime.exceptions;
        uintptr_t bytes = std::bit_cast<uintptr_t>(wordsOfHandler.stackPointerOfHandler) - std::bit_cast<uintptr_t>(vm.heap.cage().takeLowestAddressOfDeadStack(vm));
        wordsOfHandler.bytesOfDeadStackBelowHandler = static_cast<uint32_t>(std::min<uintptr_t>(bytes, std::numeric_limits<uint32_t>::max()) & ~static_cast<uintptr_t>(sizeof(void*) - 1));
        // The landing pad finds the value without a call, which would leave its frame behind on the cleared stack.
        if (exception && !needsExceptionForEveryThrow(*instance) && (found.handler.kind == CompiledHandlerKind::Catch || takesException)) {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            wordsOfHandler.thrownValue = JSValue::encode(takesException ? JSValue(exception) : exception->value());
            topScope.clearException();
            return handlerCodeAndFrame;
        }
    }
    if (isHandled && (exception || !(takesException || needsException))) [[likely]]
        return handlerCodeAndFrame;
    bool stackIsUnobserved = false;
    if (!exception && !rethrownException) {
        if (removeCodePtrTag(frameRecord->callerFrame->rawReturnPC()) == instance->state->returnAddressOfThrowAfterCleanup) {
            if (uint64_t kept = instance->state->keptByCleanup)
                rethrownException = exceptionKeptByCleanup(kept, JSValue::decode(exceptions.thrownValue));
        }
        if (rethrownException)
            exceptions.thrownValue = JSValue::encode(rethrownException);
        if (isHandled && !needsException) {
            if (rethrownException)
                return handlerCodeAndFrame;
            stackIsUnobserved = isCaughtAboveCleanup(vm, found.siteOfHandler);
            // The handler takes the value, and makes an Exception of it if it wants one. That is a call.
            if (stackIsUnobserved && !clearsDeadStack)
                return handlerCodeAndFrame;
        }
    }
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecord);
    if (!exception) {
        JSValue thrown = JSValue::decode(std::exchange(exceptions.thrownValue, 0));
        RELEASE_ASSERT(thrown);
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto action = Exception::StackCaptureAction::DoNotCaptureStack;
        if (!stackIsUnobserved && !rethrownException)
            action = stackCaptureActionOfThrow(vm, *instance, thrown, frameRecord, found.hasLeftRun ? &found : nullptr);
        Exception* thrownException = throwException(instance->globalObject, scope, rethrownException ? thrown : JSValue(Exception::create(vm, thrown, action)));
        if (isHandled && takesException && !needsException) {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            exceptions.thrownValue = JSValue::encode(thrownException);
            topScope.clearException();
        }
    }
    if (isHandled)
        return handlerCodeAndFrame;
    unwindBeyondCompiledFrames(vm, std::bit_cast<CallFrame*>(frameRecord), found.status);
    return makeUGPRPair(0, 0);
}

// First what the function has allocated below its return address, zero if it has a frame record. Second what its entry has put above its return address.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, UGPRPair, (Instance*, const void* returnAddress))
{
    const Image& image = *Image::current();
    std::optional<PrologueCheck> check = image.prologueCheckAt(returnAddress);
    RELEASE_ASSERT(check);
    const ImageFrame& frame = image.frameOf(check->function);
    size_t bytesBelowReturnAddress = frame.hasFrameRecord() ? 0 : frame.frameSizeInBytes() + sizeof(void*);
    size_t bytesAboveReturnAddress = image.function(check->function).has(ImageFunctionFlag::SpillsArgumentRegisters) ? sizeOfSpilledArgumentRegisters : 0;
    return makeUGPRPair(bytesBelowReturnAddress, bytesAboveReturnAddress);
}

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE

// Behind the report of a check whose slow half lies at the end of the function stands a jump to the access.
static const void* addressOfAccess(const void* returnAddress)
{
    constexpr uint8_t breakpoint = 0xcc;
    constexpr uint8_t nearJump = 0xe9;
    auto* bytes = static_cast<const uint8_t*>(returnAddress);
    if (bytes[0] != breakpoint || bytes[1] != nearJump)
        return returnAddress;
    int32_t distance;
    memcpy(&distance, bytes + 2, sizeof(distance));
    return bytes + 2 + sizeof(distance) + distance;
}

static bool isBehindCall(const void* address)
{
    constexpr uint8_t nearCall = 0xe8;
    constexpr uint8_t group5 = 0xff;
    auto* bytes = static_cast<const uint8_t*>(address);
    if (bytes[-5] == nearCall)
        return true;
    for (int length : { 2, 3, 4, 6, 7 }) {
        if (bytes[-length] == group5 && (bytes[1 - length] & 0x38) == 0x10)
            return true;
    }
    return false;
}

static void printNameOfStubAt(const Image& image, uint32_t offsetInSharedCode)
{
    const StubAddressInImage* stub = nullptr;
    for (const StubAddressInImage& candidate : image.section<StubAddressInImage>(ImageSection::StubAddresses)) {
        if (candidate.offset <= offsetInSharedCode && (!stub || candidate.offset > stub->offset))
            stub = &candidate;
    }
    if (!stub)
        return;
    StubKey key = StubKey::fromBits(stub->key);
    const StubDescriptor& descriptor = descriptorOf(key.family());
    dataLog(" ", descriptor.name);
    if (key.form() < descriptor.formNames.size())
        dataLog(".", descriptor.formNames[key.form()]);
}

// A stub without a frame record, and a function without one, have their return address somewhere between the stack pointer and the frame record above.
SUPPRESS_ASAN static void collectCompiledFrames(const Image& image, bool hasFrameRecord, void* const* stackPointer, void* const* frameRecord, WTF::Vector<const void*, 64>& addresses)
{
    const StackBounds& stack = Thread::currentSingleton().stack();
    if (!stack.contains(const_cast<void**>(frameRecord)))
        return;
    if (!hasFrameRecord && stack.contains(const_cast<void**>(stackPointer))) {
        for (void* const* word = stackPointer; word < frameRecord; ++word) {
            if (image.locate(*word).kind == CodeKind::Function && isBehindCall(*word))
                addresses.append(*word);
        }
    }
    while (addresses.size() < addresses.capacity()) {
        addresses.append(frameRecord[1]);
        auto* next = static_cast<void* const*>(frameRecord[0]);
        if (next <= frameRecord || !stack.contains(const_cast<void**>(next)))
            break;
        frameRecord = next;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (Instance*, void* address, uint32_t sizeAndIsStore, void* const* stackPointer))
{
    void* returnAddress = __builtin_return_address(0);
    void* frame = __builtin_frame_address(1);
    const Image& image = *Image::current();
    CodeLocation location = image.locate(returnAddress);
    WTF::Vector<const void*, 64> addresses;
    bool hasFrameRecord = false;
    if (location.kind == CodeKind::Stub) {
        dataLog("AOT: compiled code touches poisoned memory: a stub");
        printNameOfStubAt(image, location.offset);
        dataLogLn(", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
    } else {
        dataLogLn("AOT: compiled code touches poisoned memory: function ", location.function, ", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
        const void* access = addressOfAccess(returnAddress);
        addresses.append(access);
        CodeLocation locationOfAccess = image.locate(access);
        hasFrameRecord = image.frameOf(location.function).hasFrameRecord() && image.hasFrameAt(locationOfAccess.function, locationOfAccess.offset);
    }
    collectCompiledFrames(image, hasFrameRecord, stackPointer, static_cast<void* const*>(frame), addresses);
    dataLog(describeCodeAddresses(addresses.span()));
    __asan_report_error(returnAddress, frame, frame, address, sizeAndIsStore >> 8, sizeAndIsStore & 0xff);
    CRASH();
}

#endif

static ALWAYS_INLINE JSValue enterWithArgumentList(VM& vm, const CallData& callData, JSObject* callee, JSValue thisSlot, JSCell* context, const ArgList& args)
{
    switch (args.size()) {
    case 0:
        return enterCompiledCode(vm, callData, callee, thisSlot, context);
    case 1:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0));
    case 2:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1));
    case 3:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2));
    case 4:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3));
    case 5:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3), args.at(4));
    case 6:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3), args.at(4), args.at(5));
    default:
        break;
    }
    ProtoCallFrame protoCallFrame;
    if (args.size() <= adapterArgumentCountWithOutOfLineArguments) {
        protoCallFrame.init(nullptr, callData.compiled.realm, callee, thisSlot, context, 1 + args.size(), args.data());
        return JSValue::decode(vmEntryToJavaScript(const_cast<void*>(callData.compiled.entrypoint), &vm, &protoCallFrame));
    }
    std::array<EncodedJSValue, adapterArgumentCountWithOutOfLineArguments> frameArguments;
    std::copy_n(args.data(), numberOfArgumentGPRs, frameArguments.begin());
    frameArguments[numberOfArgumentGPRs] = static_cast<EncodedJSValue>(static_cast<uint64_t>(outOfLineArgumentCountTag) << outOfLineArgumentCountTagShift | args.size());
    frameArguments[numberOfArgumentGPRs + 1] = std::bit_cast<EncodedJSValue>(args.data());
    protoCallFrame.init(nullptr, callData.compiled.realm, callee, thisSlot, context, 1 + frameArguments.size(), frameArguments.data());
    return JSValue::decode(vmEntryToJavaScript(const_cast<void*>(callData.compiled.entrypoint), &vm, &protoCallFrame));
}

// As Interpreter::executeCall and Interpreter::executeConstruct enter a function of the engine.
template<bool isConstruct>
static ALWAYS_INLINE JSValue enterFromEngine(VM& vm, JSObject* callee, const CallData& callData, JSValue thisSlot, JSCell* context, const ArgList& args)
{
    ASSERT(callData.type == CallData::Type::Compiled);

    auto clobberizeValidator = makeScopeExit([&] {
        vm.didEnterVM = true;
    });

    auto scope = DECLARE_THROW_SCOPE(vm);
    scope.assertNoException();
    ASSERT(!vm.isCollectorBusyOnCurrentThread());

    JSGlobalObject* realm = callData.compiled.realm;

    VMEntryScope entryScope(vm, realm);
    if (!vm.isSafeToRecurseSoft() || args.size() > maxArguments) [[unlikely]] {
        JSValue thrown = throwStackOverflowError(realm, scope);
        return isConstruct ? JSValue() : thrown;
    }

    if (vm.disallowVMEntryCount) [[unlikely]] {
        JSValue refused = VM::checkVMEntryPermission();
        return isConstruct ? JSValue(realm->globalThis()) : refused;
    }

    if constexpr (!isConstruct)
        RELEASE_AND_RETURN(scope, enterWithArgumentList(vm, callData, callee, thisSlot, context, args));
    JSValue result = enterWithArgumentList(vm, callData, callee, thisSlot, context, args);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue callCompiledCode(VM& vm, JSObject* callee, const CallData& callData, JSValue thisValue, JSCell* context, const ArgList& args)
{
    return enterFromEngine<false>(vm, callee, callData, thisValue, context, args);
}

JSValue callCompiledCode(JSGlobalObject* realm, JSCell* closure, JSValue thisValue, const ArgList& arguments)
{
    CallData callData = JSC::getCallData(closure);
    RELEASE_ASSERT(callData.type == CallData::Type::Compiled);
    return callCompiledCode(realm->vm(), asObject(closure), callData, thisValue, nullptr, arguments);
}

JSValue constructCompiledCode(VM& vm, JSObject* constructor, const CallData& constructData, const ArgList& args, JSValue newTarget)
{
    return enterFromEngine<true>(vm, constructor, constructData, newTarget, nullptr, args);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
