/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTFunctionNames.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "Error.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static bool canSkipDefineOwnProperty(VM& vm, JSObject* object, Structure* structure, PropertyName name)
{
    if (structure->hasImmutableProperties() || structure->hasListedImmutableProperties())
        return false;
    if (hasOrdinaryDefineForName(vm, object, structure, name.uid()))
        return true;
    auto* closure = dynamicDowncast<Closure>(object);
    return closure && structure->isStructureExtensible() && !parseIndex(name) && closure->hasOrdinaryDefineOwnProperty(vm, name);
}

void defineDataProperty(Instance* instance, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = *instance->vm;
    if (Object* compiled = compiledObjectOrNull(object)) {
        if (compiled->tryStoreExtraProperty(vm, name, value, { Object::ExtraPropertyStoreFlag::IsDefinition }).kind != Object::ExtraPropertyStore::NotHandled)
            return;
    }
    Structure* structure = object->structure();
    unsigned attributes = 0;
    if (canSkipDefineOwnProperty(vm, object, structure, name) && (!isValidOffset(structure->get(vm, name, attributes)) || !attributes))
        object->putDirect(vm, name, value, slot);
    else
        object->createDataProperty(instance->globalObject, name, value, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineById, void, (Instance* instance, EncodedJSValue encodedObject, uint32_t name, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTDefineById;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = objectArgument(encodedObject, entry);
    PutPropertySlot slot(object, true);
    defineDataProperty(instance, object, identifierAt(instance, name), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

// These are the arrays for which JSArray::defineOwnProperty() comes down to putDirectIndex(): a length that is read-only puts the array into sparse mode. The text of the index is never made.
static bool canDefineElementsDirectly(JSObject* object)
{
    return isJSArray(object) && !object->structure()->hasImmutableIndexedProperties() && !object->inSparseIndexingMode() && !isCopyOnWrite(object->indexingMode());
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineField, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue encodedKey, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTDefineField;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = objectArgument(encodedObject, entry);
    JSValue key = JSValue::decode(encodedKey);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index(); index && canDefineElementsDirectly(object)) {
        object->putDirectIndex(globalObject, *index, JSValue::decode(value), 0, PutDirectIndexShouldThrow);
        OPERATION_RETURN(scope);
    }
    bool isKeyUniqued = key.isSymbol() || atomOfKey(key);
    auto name = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, true);
    defineDataProperty(instance, object, name, JSValue::decode(value), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (isKeyUniqued)
        instance->state->propertyCaches->cache({ structureBefore, { name.impl(), std::nullopt }, SlotAccess::Define }, classifyStore({ globalObject, object, structureBefore, name.impl(), slot, PutMode::Define }));
    ensureStillAliveHere(structureBefore);
    OPERATION_RETURN(scope);
}

static bool tryDefineAbsentProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    if (!canSkipDefineOwnProperty(vm, object, structure, name))
        return false;
    bool isAccessor = bits.hasGet || bits.hasSet;
    unsigned attributes = (bits.isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum) | (bits.isConfigurable ? PropertyAttribute::None : PropertyAttribute::DontDelete);
    if (isAccessor)
        attributes |= PropertyAttribute::Accessor;
    else if (!bits.isWritable)
        attributes |= PropertyAttribute::ReadOnly;
    JSValue stored = bits.hasValue ? valueOrGetter : jsUndefined();
    if (isAccessor)
        stored = GetterSetter::create(vm, globalObject, bits.hasGet ? valueOrGetter.getObject() : nullptr, setter.getObject());
    if (object->tryPutDirectByExistingTransition(vm, name, stored, attributes))
        return true;
    if (isValidOffset(structure->get(vm, name)))
        return false;
    if (!isAccessor) {
        object->putDirect(vm, name, stored, attributes);
        return true;
    }
    return object->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(stored), attributes);
}

static JSObject* createDescriptorLiteral(JSGlobalObject* globalObject, JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    const CommonIdentifiers& names = *vm.propertyNames;
    JSObject* literal = constructEmptyObject(globalObject);
    if (bits.hasValue)
        literal->putDirect(vm, names.value, valueOrGetter);
    if (bits.hasWritable)
        literal->putDirect(vm, names.writable, jsBoolean(bits.isWritable));
    if (bits.hasEnumerable)
        literal->putDirect(vm, names.enumerable, jsBoolean(bits.isEnumerable));
    if (bits.hasConfigurable)
        literal->putDirect(vm, names.configurable, jsBoolean(bits.isConfigurable));
    if (bits.hasGet)
        literal->putDirect(vm, names.get, valueOrGetter);
    if (bits.hasSet)
        literal->putDirect(vm, names.set, setter);
    return literal;
}

static void defineOwnPropertyFromBits(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(!(bits.hasValue || bits.hasWritable) || !(bits.hasGet || bits.hasSet));
    PropertyDescriptor descriptor;
    if (globalObject->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        globalObject->tryInstallPropertyDescriptorFastPathWatchpoint();
    if (globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid() && globalObject->objectPrototypeChainIsSane()) [[likely]] {
        if (bits.hasEnumerable)
            descriptor.setEnumerable(bits.isEnumerable);
        if (bits.hasConfigurable)
            descriptor.setConfigurable(bits.isConfigurable);
        if (bits.hasValue)
            descriptor.setValue(valueOrGetter);
        if (bits.hasWritable)
            descriptor.setWritable(bits.isWritable);
        if (bits.hasGet) {
            if (!valueOrGetter.isUndefined() && !valueOrGetter.isCallable()) {
                throwTypeError(globalObject, scope, "Getter must be a function."_s);
                return;
            }
            descriptor.setGetter(valueOrGetter);
        }
        if (bits.hasSet) {
            if (!setter.isUndefined() && !setter.isCallable()) {
                throwTypeError(globalObject, scope, "Setter must be a function."_s);
                return;
            }
            descriptor.setSetter(setter);
        }
        if (tryDefineAbsentProperty(globalObject, object, name, valueOrGetter, setter, bits))
            return;
    } else {
        bool isDescriptor = toPropertyDescriptor(globalObject, createDescriptorLiteral(globalObject, valueOrGetter, setter, bits), descriptor);
        EXCEPTION_ASSERT(!scope.exception() == isDescriptor);
        if (!isDescriptor)
            return;
    }
    scope.release();
    object->methodTable()->defineOwnProperty(object, globalObject, name, descriptor, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineOwnProperty, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue key, EncodedJSValue valueOrGetter, EncodedJSValue setter, uint32_t descriptorBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefineOwnProperty);
    JSValue object = JSValue::decode(encodedObject);
    if (!object.isObject()) {
        throwTypeError(globalObject, scope, "Properties can only be defined on Objects."_s);
        OPERATION_RETURN(scope);
    }
    auto name = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    defineOwnPropertyFromBits(globalObject, asObject(object), name, JSValue::decode(valueOrGetter), JSValue::decode(setter), std::bit_cast<PropertyDescriptorBits>(descriptorBits));
    OPERATION_RETURN(scope);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, PropertyName property, JSValue function, bool isEnumerable, bool isGetter)
{
    VM& vm = globalObject->vm();
    if (Structure* structure = object->structure(); canSkipDefineOwnProperty(vm, object, structure, property)) {
        unsigned accessorAttributes = PropertyAttribute::Accessor | (isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum);
        unsigned attributes;
        PropertyOffset offset = structure->get(vm, property, attributes);
        JSObject* getter = isGetter ? asObject(function) : nullptr;
        JSObject* setter = isGetter ? nullptr : asObject(function);
        bool isOtherHalf = isValidOffset(offset) && attributes == accessorAttributes;
        if (isOtherHalf) {
            auto* current = uncheckedDowncast<GetterSetter>(object->getDirect(offset));
            if (isGetter)
                setter = current->isSetterNull() ? nullptr : current->setter();
            else
                getter = current->isGetterNull() ? nullptr : current->getter();
        }
        if ((isOtherHalf || !isValidOffset(offset)) && object->putDirectNonIndexAccessor(vm, property, GetterSetter::create(vm, globalObject, getter, setter), accessorAttributes))
            return;
    }
    PropertyDescriptor descriptor;
    if (isGetter)
        descriptor.setGetter(function);
    else
        descriptor.setSetter(function);
    descriptor.setEnumerable(isEnumerable);
    descriptor.setConfigurable(true);
    object->methodTable()->defineOwnProperty(object, globalObject, property, descriptor, true);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, JSValue key, JSValue function, uint32_t accessorFlags)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    defineAccessor(globalObject, object, property, function, accessorFlags & accessorIsEnumerable, accessorFlags & accessorIsGetter);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessor, void, (Instance* instance, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function, uint32_t accessorFlags))
{
    constexpr Entry entry = Entry::operationAOTDefineAccessor;
    AOT_OPERATION_BEGIN(instance, entry);
    defineAccessor(globalObject, objectArgument(object, entry), JSValue::decode(key), JSValue::decode(function), accessorFlags);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessors, void, (Instance* instance, JSCell* cell, uint32_t count, uint32_t getters, uint32_t enumerable, const EncodedJSValue* keysAndFunctions))
{
    constexpr Entry entry = Entry::operationAOTDefineAccessors;
    constexpr uint32_t maskWidth = 32;
    constexpr size_t wordsPerAccessor = sizeof(DefinedAccessor) / sizeof(EncodedJSValue);
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = cellArgument<JSObject>(cell, entry);
    if (count > maskWidth) [[unlikely]]
        crashOnWrongOperand(entry);
    auto accessors = spanReinterpretCast<const DefinedAccessor>(unsafeMakeSpan(keysAndFunctions, count * wordsPerAccessor));
    for (uint32_t index = 0; index < count; ++index) {
        const DefinedAccessor& accessor = accessors[index];
        uint32_t accessorFlags = (getters >> index & 1 ? accessorIsGetter : 0) | (enumerable >> index & 1 ? accessorIsEnumerable : 0);
        defineAccessor(globalObject, object, JSValue::decode(accessor.key), JSValue::decode(accessor.function), accessorFlags);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    if (Object* compiled = compiledObjectOrNull(object))
        compiled->ensureStructureForExtraProperties(vm);
    OPERATION_RETURN(scope);
}

static void defineMethod(JSGlobalObject* globalObject, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    switch (kind) {
    case MethodKind::Method: {
        constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
        if (Structure* structure = holder->structure(); canSkipDefineOwnProperty(vm, holder, structure, key) && !isValidOffset(structure->get(vm, key))) {
            holder->putDirect(vm, key, function, attributes);
            return;
        }
        holder->methodTable()->defineOwnProperty(holder, globalObject, key, PropertyDescriptor(function, attributes), true);
        return;
    }
    case MethodKind::Getter:
    case MethodKind::Setter:
        defineAccessor(globalObject, holder, key, function, false, kind == MethodKind::Getter);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineMethod, void, (Instance* instance, JSCell* classObject, EncodedJSValue property, JSCell* function, uint32_t sideAndKind))
{
    constexpr Entry entry = Entry::operationAOTDefineMethod;
    AOT_OPERATION_BEGIN(instance, entry);
    Closure* closure = cellArgument<Closure>(classObject, entry);
    const LayoutData* classLayout = closure->classData();
    if (!classLayout) [[unlikely]]
        crashOnWrongOperand(entry);
    DefinedMethodBits bits = DefinedMethodBits::decode(sideAndKind);
    ClassSide side = static_cast<ClassSide>(bits.side);
    JSObject* holder = side == ClassSide::Static ? closure : asObject(closure->prototypeProperty(vm));
    ClassPrototype::ensureMembers(holder);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    defineMethod(globalObject, holder, key, cellArgument<JSObject>(function, entry), static_cast<MethodKind>(bits.kind));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    instance->state->objects->classes.didDefineMember(vm, *classLayout, side, holder, key.impl());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFunctionName, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedName, uint32_t methodKind))
{
    constexpr Entry entry = Entry::operationAOTSetFunctionName;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue name = JSValue::decode(encodedName);
    RELEASE_ASSERT(name.isString() || name.isSymbol());
    auto key = name.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    std::optional<FunctionID> function = functionOf(cell);
    if (!function) [[unlikely]]
        crashOnWrongOperand(entry);
    setComputedFunctionName(vm, asObject(cell), key, static_cast<MethodKind>(methodKind));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
