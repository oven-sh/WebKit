/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTClosure.h"
#include "AOTFunctionTables.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "CellSize.h"
#include "ClonedArguments.h"
#include "DirectArguments.h"
#include "GetterSetter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr unsigned argumentsCalleeAttributes = PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::Accessor;

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

// The first object of a realm gets its two properties one by one, which creates the Structures that are looked up here.
static const UnmappedArgumentsShape* findShapeOfUnmappedArguments(Instance* instance)
{
    State& state = *instance->state;
    UnmappedArgumentsShape& shape = state.builtinFamilies->unmappedArguments;
    Structure* initialStructure = instance->globalObject->clonedArgumentsStructure();
    if (shape.initialStructure == initialStructure) [[likely]]
        return &shape;

    VM& vm = *instance->vm;
    UnmappedArgumentsShape found { initialStructure, nullptr, invalidOffset, invalidOffset };
    found.structure = Structure::addPropertyTransitionToExistingStructure(initialStructure, vm.propertyNames->callee, argumentsCalleeAttributes, found.offsetOfCallee);
    if (found.structure)
        found.structure = Structure::addPropertyTransitionToExistingStructure(found.structure, vm.propertyNames->iteratorSymbol, static_cast<unsigned>(PropertyAttribute::DontEnum), found.offsetOfIterator);
    if (!found.structure)
        return nullptr;
    state.immortalCells->add(found.structure);
    shape = found;
    return &shape;
}

static ClonedArguments* tryCreateArgumentsFast(Instance* instance, const ArgumentsObjectSource& source)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    if (!canReadRestQuickly(source))
        return nullptr;
    const UnmappedArgumentsShape* shape = findShapeOfUnmappedArguments(instance);
    if (!shape)
        return nullptr;
    Structure* structure = shape->structure;
    if (!hasContiguous(structure->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->storedPrototypeObject()->needsSlowPutIndexing())
        return nullptr;
    JSValue callee = globalObject->throwTypeErrorArgumentsCalleeGetterSetter();
    JSValue iterator = globalObject->arrayProtoValuesFunction();
    ClonedArguments* result = ClonedArguments::createEmpty(vm, nullptr, structure, nullptr, source.count, nullptr);
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
    result->putDirectOffset(vm, shape->offsetOfCallee, callee);
    result->putDirectOffset(vm, shape->offsetOfIterator, iterator);
    return result;
}

static ClonedArguments* createArguments(Instance* instance, const ArgumentsObjectSource& source)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (ClonedArguments* result = tryCreateArgumentsFast(instance, source))
        return result;
    MarkedArgumentBuffer values;
    for (uint32_t i = 0; i < source.count; ++i) {
        values.append(argumentAt(globalObject, source, i));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    ClonedArguments* result = ClonedArguments::createEmpty(vm, globalObject, globalObject->clonedArgumentsStructure(), nullptr, source.count, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!result) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        result->putDirectIndex(globalObject, i, values.at(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    result->putDirectAccessor(globalObject, vm.propertyNames->callee, globalObject->throwTypeErrorArgumentsCalleeGetterSetter(), argumentsCalleeAttributes);
    result->putDirect(vm, vm.propertyNames->iteratorSymbol, globalObject->arrayProtoValuesFunction(), static_cast<unsigned>(PropertyAttribute::DontEnum));
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
