/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledCall.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTEffectsNet.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionTables.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTStubsCallAdapters.h"
#include "AOTUnwinding.h"
#include "AOTVMState.h"
#include "CallFrameInlines.h"
#include "CellSize.h"
#include "ClonedArguments.h"
#include "DeferGCInlines.h"
#include "DirectArguments.h"
#include "ErrorConstructor.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "InternalFunction.h"
#include "Interpreter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSFunction.h"
#include "LLIntThunks.h"
#include "ObjectConstructor.h"
#include "OwedOutOfMemoryError.h"
#include "ProtoCallFrameInlines.h"
#include "VMEntryRecord.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include <algorithm>
#include <array>
#include <wtf/StackPointer.h>

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

static JSFunction* calleeOfStrictCode(JSGlobalObject* globalObject)
{
    return globalObject->arrayProtoValuesFunction();
}

static ClonedArguments* tryCreateArgumentsFast(Instance* instance, const ArgumentsObjectSource& source)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    Structure* structure = globalObject->clonedArgumentsStructure();
    if (!hasContiguous(structure->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->storedPrototypeObject()->needsSlowPutIndexing())
        return nullptr;
    if (structure->outOfLineCapacity() == initialOutOfLineCapacity) {
        instance->allocation.calleeOfStrictArguments = calleeOfStrictCode(globalObject);
        prepareInlineAllocation<ClonedArguments>(instance, InlineAllocation::Arguments, structure);
    }
    if (!canReadRestQuickly(source))
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
    vm.mutatorFence();
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
    vm.mutatorFence();
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateUnmappedArguments, JSCell*, (Instance* instance, uint32_t count, JSCell* rest, const EncodedJSValue* parameters))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateUnmappedArguments);
    OPERATION_RETURN(scope, createArguments(instance, createArgumentSource(count, rest, parameters, Entry::operationAOTCreateUnmappedArguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateMappedArguments, JSCell*, (Instance* instance, JSCell* callee, uint32_t count, JSCell* rest, const EncodedJSValue* parameters))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateMappedArguments);
    Closure* closure = cellArgument<Closure>(callee, Entry::operationAOTCreateMappedArguments);
    ArgumentsObjectSource source = createArgumentSource(count, rest, parameters, Entry::operationAOTCreateMappedArguments);
    unsigned numberOfParameters = functionLengthOf(closure->function());
    if (source.numberInFrame > numberOfParameters) [[unlikely]]
        crashOnWrongOperand(Entry::operationAOTCreateMappedArguments);
    unsigned capacity = std::max<unsigned>(count, numberOfParameters);
    instance->allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::MappedArguments)].header = cellHeaderFor(globalObject->directArgumentsStructure());
    DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), count, capacity);
    result->setCallee(vm, closure);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = argumentAt(globalObject, source, i);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        result->argument(DirectArgumentsOffset(i)).set(vm, result, value);
    }
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
    instance->vm->writeBarrier(object);
}

void throwNotAFunctionError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callee)
{
    throwException(globalObject, scope, createNotAFunctionError(globalObject, callee, StringView()));
}

static StringView nameOfCalleeAtCallSite(VM& vm)
{
    const void* returnAddress = CompiledFrameIterator::topFrameReturnAddress(vm);
    if (!returnAddress)
        return { };
    std::optional<FaultSite> site = Image::current()->faultSiteAt(static_cast<const uint8_t*>(returnAddress) - sizeOfNearCall);
    if (!site || site->kind != FaultSiteKind::NullCheck)
        return { };
    ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(site->throwSite[0]);
    if (static_cast<ThrowKind>(word.kind) != ThrowKind::NotAFunction)
        return { };
    uint32_t name = word.hasWideA ? site->throwSite[1] : word.narrowA();
    return name == ThrowSiteWord::none ? StringView() : Image::current()->text().view(StringID(name));
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
    auto* mapped = dynamicDowncast<DirectArguments>(object);
    if (mapped && mapped->overrodeThings())
        mapped = nullptr;
    uint64_t length;
    if (auto* array = dynamicDowncast<JSArray>(object)) [[likely]]
        length = array->length();
    else if (mapped)
        length = mapped->internalLength();
    else {
        length = toLength(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!fitsOnStack(vm, length)) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return false;
    }
    if (mapped) {
        for (uint32_t index = 0; index < length; ++index)
            arguments.append(mapped->getIndexQuickly(index));
    } else {
        for (uint64_t index = 0; index < length; ++index) {
            arguments.append(object->getIndex(globalObject, index));
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

class SuperCallScope {
    WTF_MAKE_NONCOPYABLE(SuperCallScope);
public:
    SuperCallScope(VM& vm, JSValue parent, JSValue newTarget)
        : m_state(vm.aotState())
    {
        if (!parent.isCell() || !newTarget.isCell())
            return;
        m_state.parentOfSuperCall = parent.asCell();
        m_state.newTargetOfSuperCall = newTarget.asCell();
    }

    ~SuperCallScope() { m_state.newTargetOfSuperCall = nullptr; }

private:
    VMState& m_state;
};

static JSValue createArrayOfHeir(JSGlobalObject* globalObject, VM& vm, JSObject* newTarget, unsigned length)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(globalObject, ArrayWithUndecided, newTarget);
    RETURN_IF_EXCEPTION(scope, { });
    auto* classObject = dynamicDowncast<JSFunction>(newTarget);
    Instance* instance = classObject ? Instance::owning(classObject) : nullptr;
    State* state = instance ? instance->state : nullptr;
    JSArray* array = state && state->objects ? state->objects->classes.tryCreateArrayOfHeir(vm, newTarget, structure, length) : JSArray::tryCreate(vm, structure, length);
    if (!array) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return array;
}

template<bool isParent>
static JSValue constructValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue newTarget, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<SuperCallScope> superCall;
    if constexpr (isParent)
        superCall.emplace(vm, callee, newTarget);
    auto constructData = JSC::getConstructData(callee);
    if (constructData.type == CallData::Type::None) {
        if (auto* function = dynamicDowncast<JSFunction>(callee); function && function->isHostFunction()) {
            scope.release();
            throwNotAConstructorErrorInFrameOf(globalObject, function, StringView { }, isParent);
            return { };
        }
        throwException(globalObject, scope, isParent ? createParentNotAConstructorError(globalObject, callee) : createNotAConstructorError(globalObject, callee, nameOfCalleeAtCallSite(vm)));
        return { };
    }
    if (auto* errorConstructor = isParent ? dynamicDowncast<ErrorConstructor>(callee) : nullptr; errorConstructor && errorConstructor->realm() == globalObject && newTarget.isObject() && newTarget != callee) {
        Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, asObject(newTarget), errorConstructor);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, ErrorInstance::create(globalObject, structure, arguments.at(0), arguments.at(1), nullptr, TypeNothing, ErrorType::Error, true, asObject(newTarget)));
    }
    if (isParent && callee == JSValue(globalObject->arrayConstructor()) && newTarget.isObject() && newTarget != callee) {
        if (arguments.size() > 1)
            RELEASE_AND_RETURN(scope, constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), arguments, newTarget));
        JSValue given = arguments.isEmpty() ? jsNumber(0) : arguments.at(0);
        if (given.isUInt32AsAnyInt() && given.asUInt32AsAnyInt() <= maxHoleArrayCapacity)
            RELEASE_AND_RETURN(scope, createArrayOfHeir(globalObject, vm, asObject(newTarget), given.asUInt32AsAnyInt()));
    }
    if constexpr (isParent) {
        if (callee == globalObject->objectConstructor() && newTarget.isObject() && newTarget != callee) {
            if (JSObject* object = Instance::of(globalObject)->state->objects->classes.tryCreateInstanceBelowObject(vm, asObject(newTarget)))
                RELEASE_AND_RETURN(scope, object);
        }
        RELEASE_AND_RETURN(scope, JSC::construct(globalObject, callee, constructData, arguments, newTarget));
    }
    RELEASE_AND_RETURN(scope, constructAllowingPrimitiveResult(globalObject, callee, constructData, arguments, newTarget));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue newTarget, uint32_t count, const EncodedJSValue* argumentValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConstruct);
    ArgList arguments(const_cast<EncodedJSValue*>(argumentValues), count);
    OPERATION_RETURN(scope, JSValue::encode(constructValue<false>(globalObject, vm, JSValue::decode(callee), JSValue::decode(newTarget), arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTApply, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTApply);
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, JSValue::decode(list));
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    JSValue function = JSValue::decode(callee);
    auto callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None) {
        throwException(globalObject, scope, createNotAFunctionError(globalObject, function, StringView()));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, callData, JSValue::decode(thisValue), arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTReadArgumentList, JSCell*, (Instance* instance, JSCell* list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReadArgumentList);
    MarkedArgumentBuffer arguments;
    bool didAppend = appendArrayLike(globalObject, vm, arguments, list);
    EXCEPTION_ASSERT(didAppend == !scope.exception());
    if (!didAppend) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    JSCellButterfly* words = JSCellButterfly::tryCreateFromArgList(vm, arguments);
    if (!words) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, words);
}

static JSArray* tryCopyUnmappedArguments(Instance* instance, JSValue arrayLike)
{
    VM& vm = *instance->vm;
    if (!arrayLike.isCell() || arrayLike.asCell()->structure() != instance->globalObject->clonedArgumentsStructure())
        return nullptr;
    JSObject* arguments = asObject(arrayLike);
    if (!hasContiguous(arguments->indexingType()))
        return nullptr;
    JSValue length = arguments->getDirect(clonedArgumentsLengthPropertyOffset);
    if (!length.isUInt32() || length.asUInt32() > arguments->butterfly()->publicLength())
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

static JSArray* tryCopyMappedArguments(Instance* instance, JSValue arrayLike)
{
    VM& vm = *instance->vm;
    auto* arguments = arrayLike.isCell() ? dynamicDowncast<DirectArguments>(arrayLike.asCell()) : nullptr;
    if (!arguments || arguments->overrodeThings() || instance->globalObject->isHavingABadTime())
        return nullptr;
    uint32_t count = arguments->internalLength();
    for (uint32_t i = 0; i < count; ++i) {
        if (!arguments->getIndexQuickly(i))
            return nullptr;
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* list = JSArray::tryCreateUninitializedRestricted(initializationScope, instance->globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
    if (!list)
        return nullptr;
    for (uint32_t i = 0; i < count; ++i)
        list->initializeIndexWithoutBarrier(initializationScope, i, arguments->getIndexQuickly(i), ArrayWithContiguous);
    ensureStillAliveHere(arguments);
    return list;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArgumentList, JSCell*, (Instance* instance, EncodedJSValue arrayLike))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArgumentList);
    if (JSArray* list = tryCopyUnmappedArguments(instance, JSValue::decode(arrayLike)))
        OPERATION_RETURN(scope, list);
    if (JSArray* list = tryCopyMappedArguments(instance, JSValue::decode(arrayLike)))
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
    if (count == 1 && JSValue::decode(parent) == JSValue(globalObject->arrayConstructor()) && JSValue::decode(newTarget).isCell() && !globalObject->isHavingABadTime()) {
        JSValue given = JSValue::decode(*argumentValues);
        State* state = instance->state;
        if (given.isUInt32() && given.asUInt32() <= maxHoleArrayCapacity && state && state->objects) {
            if (JSArray* array = state->objects->classes.tryCreateRememberedHeir(vm, JSValue::decode(parent).asCell(), JSValue::decode(newTarget).asCell(), given.asUInt32()))
                OPERATION_RETURN(scope, JSValue::encode(array));
        }
    }
    ArgList arguments(const_cast<EncodedJSValue*>(argumentValues), count);
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

JSC_DEFINE_JIT_OPERATION(operationAOTThrowBecauseParentMadeNoObject, EncodedJSValue, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowBecauseParentMadeNoObject);
    throwTypeError(globalObject, scope, "The constructor did not return an object"_s);
    OPERATION_RETURN(scope, encodedJSValue());
}

static JSValue calleeOfDecodedReference(Instance* instance, EncodedJSValue callee)
{
    JSValue value = valueOfDecodedReference(instance, static_cast<uint64_t>(callee));
    return value ? value : jsUndefined();
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowUnlessCallable, void, (Instance* instance, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowUnlessCallable);
    JSValue value = calleeOfDecodedReference(instance, callee);
    if (!value.isCallable())
        throwException(globalObject, scope, createNotAFunctionError(globalObject, value, nameOfCalleeAtCallSite(vm)));
    OPERATION_RETURN(scope);
}

static BuiltinID builtinThatForwardsCallToReceiver(JSValue callee)
{
    std::optional<FunctionID> function = functionOf(callee.asCell());
    if (!function)
        return { };
    std::optional<uint32_t> builtin = Image::current()->programTables().builtinImplementedBy(sourceFunctionOf(*function).index());
    if (!builtin || (BuiltinID(*builtin) != Builtins::FunctionPrototypeCall && BuiltinID(*builtin) != Builtins::FunctionPrototypeApply))
        return { };
    return BuiltinID(*builtin);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowUnlessForwardedCallCanBeMade, void, (Instance* instance, EncodedJSValue callee, EncodedJSValue receiver, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowUnlessForwardedCallCanBeMade);
    JSValue value = calleeOfDecodedReference(instance, callee);
    if (BuiltinID forwards = value.isCallable() ? builtinThatForwardsCallToReceiver(value) : BuiltinID()) {
        value = calleeOfDecodedReference(instance, receiver);
        JSValue applied = JSValue::decode(list);
        if (forwards == Builtins::FunctionPrototypeApply && (!value.isCallable() || (!applied.isUndefinedOrNull() && !applied.isObject()))) {
            MarkedArgumentBuffer read;
            appendArrayLike(globalObject, vm, read, applied);
            OPERATION_RETURN_IF_EXCEPTION(scope);
        }
    }
    if (!value.isCallable())
        throwException(globalObject, scope, createNotAFunctionError(globalObject, value, nameOfCalleeAtCallSite(vm)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowStackOverflowError);
    if (!throwOutOfMemoryErrorIfOwed(vm, globalObject, scope)) [[likely]]
        throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE UGPRPair lookupExceptionHandler(VM& vm, Instance* instance, CallerFrameAndPC* frameRecord)
{
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
    auto unwind = [&]() ALWAYS_INLINE_LAMBDA {
        return unwindRunOfThrow(vm, *instance, { callFrame->rawReturnPC(), callerStackPointer(callFrame), callFrame->callerFrame(), callFrame }, vm.topEntryFrame, *vmEntryRecord(vm.topEntryFrame));
    };
    CompiledUnwindResult found = isTermination ? CompiledUnwindResult { } : unwind();
    bool isHandled = found.status == CompiledUnwindStatus::FoundHandler;
    RELEASE_ASSERT(!isHandled || found.handler.code);
    bool takesException = found.handler.kind == CompiledHandlerKind::Finally;
    UGPRPair codeAndFrame = makeUGPRPair(std::bit_cast<UCPURegister>(found.handler.code), std::bit_cast<UCPURegister>(found.handler.framePointer));
    if (rethrownException && isHandled && !needsException) {
        if (!takesException)
            exceptions.thrownValue = JSValue::encode(rethrownException->value());
        return codeAndFrame;
    }
    if (isHandled && (exception || !(takesException || needsException))) [[likely]]
        return codeAndFrame;
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
                return codeAndFrame;
            stackIsUnobserved = isCaughtAboveCleanup(vm, found.siteOfHandler);
            if (stackIsUnobserved)
                return codeAndFrame;
        }
    }
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecord);
    if (!exception) {
        DeferGCForAWhile deferGC(vm);
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
        return codeAndFrame;
    unwindBeyondCompiledFrames(vm, std::bit_cast<CallFrame*>(frameRecord), found.status);
    return makeUGPRPair(0, 0);
}

static bool isCountedAsThrownByEffects(VM& vm)
{
    Exception* exception = vm.exceptionForInspection();
    if (!exception || vm.isTerminationException(exception))
        return false;
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    return !error || (!error->isOutOfMemoryError() && !error->isStackOverflowError());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM* vm, Instance* instance))
{
    if (uint64_t held = std::exchange(instance->effectsNet.callHeldToThrowNothing, 0); held && isCountedAsThrownByEffects(*vm)) [[unlikely]]
        operationAOTStopAtFalseEffects(instance, static_cast<uint32_t>(held >> 32 & INT32_MAX), static_cast<uint32_t>(held), static_cast<uint32_t>(Typed::PartOfEffects::Throws), static_cast<uint32_t>(instance->effectsNet.rowsOfCallHeldToThrowNothing), 0);
    return lookupExceptionHandler(*vm, instance, AOT_OPERATION_FRAME_RECORD());
}

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

static NEVER_INLINE JSValue enterWithManyArguments(VM& vm, const CallData& callData, JSObject* callee, JSValue thisSlot, JSCell* context, const ArgList& args)
{
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
        return enterWithManyArguments(vm, callData, callee, thisSlot, context, args);
    }
}

template<bool isConstruct>
static NEVER_INLINE JSValue enterFromEngine(VM& vm, JSObject* callee, const CallData& callData, JSValue thisSlot, JSCell* context, const ArgList& args)
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
    if (!vm.entryScope || !vm.isSafeToRecurseSoft() || vm.disallowVMEntryCount || args.size() > 6) [[unlikely]]
        return enterFromEngine<false>(vm, callee, callData, thisValue, context, args);
    ASSERT(!vm.isCollectorBusyOnCurrentThread());
    vm.clearLastException();
    JSValue result = enterWithArgumentList(vm, callData, callee, thisValue, context, args);
    vm.didEnterVM = true;
    return result;
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
