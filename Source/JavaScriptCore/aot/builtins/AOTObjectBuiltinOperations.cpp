/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTPropertyListCache.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "InternalFieldTuple.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "ObjectInitializationScope.h"
#include "ObjectPrototype.h"
#include "ObjectPrototypeInlines.h"
#include <array>

namespace JSC { namespace AOT {

using Typed::OwnKeysKind;

namespace {

enum class OwnPropertyList : uint8_t { Values, Entries, FlatEntries };

} // anonymous namespace

static BuiltinID engineFunctionFor(OwnKeysKind kind)
{
    switch (kind) {
    case OwnKeysKind::Keys:
        return Builtins::ObjectKeys;
    case OwnKeysKind::Names:
        return Builtins::ObjectGetOwnPropertyNames;
    case OwnKeysKind::Symbols:
        return Builtins::ObjectGetOwnPropertySymbols;
    case OwnKeysKind::All:
        return Builtins::ReflectOwnKeys;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinID engineFunctionFor(OwnPropertyList list)
{
    return list == OwnPropertyList::Values ? Builtins::ObjectValues : Builtins::ObjectEntries;
}

static JSArray* tryCreateFlatEntriesArray(JSGlobalObject* globalObject, unsigned count, const Invocable<JSValue(unsigned)> auto& keyAt, const Invocable<JSValue(unsigned)> auto& valueAt)
{
    VM& vm = globalObject->vm();
    if (count > MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH / 2 || globalObject->isHavingABadTime())
        return nullptr;
    JSArray* entries = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count * 2);
    if (!entries)
        return nullptr;
    auto elements = entries->butterfly()->contiguous();
    for (unsigned index = 0; index < count; ++index) {
        JSValue value = valueAt(index);
        if (!value)
            return nullptr;
        elements.at(entries, index * 2).set(vm, entries, keyAt(index));
        elements.at(entries, index * 2 + 1).set(vm, entries, value);
    }
    return entries;
}

static void cacheKeyListByStructure(VM& vm, Object* compiled)
{
    DeferGC deferGC(vm);
    compiled->ensureStructureForExtraProperties(vm);
    if (!compiled->keysDependOnlyOnStructureAndOrderWord())
        return;
    JSCellButterfly* list = compiled->keysWithExtraProperties(vm);
    if (!list)
        return;
    uint32_t structureWord = compiled->structureID().bits() >> 1;
    unsafeMakeSpan(compiled->instance().objects.keyLists, numberOfKeyLists)[indexOfKeyList(structureWord, compiled->orderWord())] = { compiled->orderWord(), structureWord, list };
}

// The keys go from the table of the extra properties into the list, with no list of names in between.
static JSArray* tryListKeysAsCreated(JSGlobalObject* globalObject, Object& compiled, JSObject* extra, OwnKeysKind kind)
{
    VM& vm = globalObject->vm();
    if (kind == OwnKeysKind::Symbols || hasIndexedProperties(extra->indexingType()))
        return nullptr;
    validateKeyListing(vm, compiled);
    State& state = compiled.state();
    Object::KeyOrderStorage storage;
    size_t maxNumberOfKeys = compiled.keyOrder(state, storage).size() + extra->structure()->propertyTableInOrderOfAddition(vm)->size();
    if (maxNumberOfKeys >= MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH)
        return nullptr;
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), static_cast<unsigned>(maxNumberOfKeys));
    if (!list)
        return nullptr;
    auto elements = list->butterfly()->contiguous();
    unsigned numberOfKeys = 0;
    KeysListed listed = kind == OwnKeysKind::All ? KeysListed::StringsAndSymbols : KeysListed::Strings;
    forEachOwnKey(vm, state, compiled, listed, kind == OwnKeysKind::Keys ? DontEnumPropertiesMode::Exclude : DontEnumPropertiesMode::Include, [&](UniquedStringImpl* name) ALWAYS_INLINE_LAMBDA {
        JSCell* key = name->isSymbol() ? static_cast<JSCell*>(Symbol::create(vm, static_cast<SymbolImpl&>(*name))) : jsOwnedString(vm, String(name));
        elements.at(list, numberOfKeys++).set(vm, list, key);
    });
    list->butterfly()->setPublicLength(numberOfKeys);
    return list;
}

static JSArray* tryListOwnKeys(JSGlobalObject* globalObject, Object& compiled, OwnKeysKind kind)
{
    VM& vm = globalObject->vm();
    if (JSObject* extra = compiled.extraProperties()) {
        if (kind == OwnKeysKind::Keys) {
            cacheKeyListByStructure(vm, &compiled);
            if (JSArray* list = compiled.tryCreateKeysArrayWithExtraProperties(globalObject))
                return list;
        } else if (kind != OwnKeysKind::Symbols) {
            bool listsSymbols = kind == OwnKeysKind::All;
            compiled.ensureStructureForExtraProperties(vm);
            JSCellButterfly* keys = findKeptOwnKeys(compiled, listsSymbols);
            if (!keys)
                keys = PropertyWalk::tryGetCachedOwnKeys(vm, &compiled, listsSymbols ? PropertyWalk::Filter::StringsAndSymbols : PropertyWalk::Filter::Strings);
            if (keys)
                return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly());
            JSArray* list = tryListKeysAsCreated(globalObject, compiled, extra, kind);
            if (list)
                didListOwnKeys(vm, compiled, listsSymbols, list);
            return list;
        }
        return tryListKeysAsCreated(globalObject, compiled, extra, kind);
    }
    JSCellButterfly* keys = compiled.keys(vm, kind == OwnKeysKind::Symbols ? KeyListKind::Symbols : kind == OwnKeysKind::All ? KeyListKind::StringsAndSymbols : KeyListKind::Strings);
    if (!keys)
        return nullptr;
    return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly());
}

JSC_DEFINE_JIT_OPERATION(operationAOTOwnPropertyKeys, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t encodedKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTOwnPropertyKeys);
    JSValue value = JSValue::decode(encodedValue);
    OwnKeysKind kind = static_cast<OwnKeysKind>(encodedKind);
    JSObject* object;
    if (value.isObject()) [[likely]] {
        object = asObject(value);
        markPristineIfOriginal(*instance, object);
    } else {
        if (kind == OwnKeysKind::All || value.isUndefinedOrNull()) {
            JSValue list = callInEngine(instance, engineFunctionFor(kind), value);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            OPERATION_RETURN(scope, list.asCell());
        }
        object = value.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenForListing(vm, object);
    Object* compiled = instance->state->objects->layouts.compiledObject(object);
    if (compiled && !globalObject->isHavingABadTime()) {
        if (JSArray* keys = tryListOwnKeys(globalObject, *compiled, kind))
            OPERATION_RETURN(scope, keys);
    }
    switch (kind) {
    case OwnKeysKind::Keys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Exclude));
    case OwnKeysKind::Names:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Include));
    case OwnKeysKind::Symbols:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Symbols, DontEnumPropertiesMode::Include));
    case OwnKeysKind::All:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::StringsAndSymbols, DontEnumPropertiesMode::Include));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSArray* tryCreateValuesArray(JSGlobalObject* globalObject, unsigned count, const Invocable<JSValue(unsigned)> auto& valueAt)
{
    VM& vm = globalObject->vm();
    JSArray* values = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
    if (!values)
        return nullptr;
    auto elements = values->butterfly()->contiguous();
    for (unsigned index = 0; index < count; ++index) {
        JSValue value = valueAt(index);
        if (!value)
            return nullptr;
        elements.at(values, index).set(vm, values, value);
    }
    return values;
}

static JSArray* tryCreateEntry(VM& vm, Structure* structure, JSValue key, JSValue value)
{
    ObjectInitializationScope initializationScope(vm);
    JSArray* entry = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, structure, 2);
    if (!entry)
        return nullptr;
    entry->initializeIndex(initializationScope, 0, key);
    entry->initializeIndex(initializationScope, 1, value);
    return entry;
}

static JSArray* tryCreateEntriesArray(JSGlobalObject* globalObject, JSCellButterfly* keys, const Invocable<JSValue(unsigned)> auto& valueAt)
{
    VM& vm = globalObject->vm();
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    unsigned count = keys->length();
    JSArray* entries = JSArray::tryCreate(vm, structure, count);
    if (!entries)
        return nullptr;
    auto elements = entries->butterfly()->contiguous();
    for (unsigned index = 0; index < count; ++index) {
        JSValue value = valueAt(index);
        if (!value)
            return nullptr;
        JSArray* entry = tryCreateEntry(vm, structure, keys->get(index), value);
        if (!entry)
            return nullptr;
        elements.at(entries, index).set(vm, entries, entry);
    }
    ensureStillAliveHere(keys);
    return entries;
}

static JSArray* tryCreateList(JSGlobalObject* globalObject, OwnPropertyList list, unsigned count, JSCellButterfly* keys, const Invocable<JSValue(unsigned)> auto& valueAt)
{
    if (list == OwnPropertyList::Values)
        return tryCreateValuesArray(globalObject, count, valueAt);
    if (!keys || keys->length() != count)
        return nullptr;
    if (list == OwnPropertyList::FlatEntries) {
        JSArray* entries = tryCreateFlatEntriesArray(globalObject, count, [&](unsigned index) { return keys->get(index); }, valueAt);
        ensureStillAliveHere(keys);
        return entries;
    }
    return tryCreateEntriesArray(globalObject, keys, valueAt);
}

static JSArray* tryListOwnProperties(JSGlobalObject* globalObject, const Object& compiled, OwnPropertyList list)
{
    constexpr uint16_t flagsBarringListing = fieldIsNotEnumerable | fieldIsAccessor;
    VM& vm = globalObject->vm();
    bool listsKeys = list != OwnPropertyList::Values;
    if (!compiled.hasExtraProperties()) {
        std::optional<std::span<const FieldAsProperty>> fields = compiled.fieldsInKeyOrder();
        if (!fields)
            return nullptr;
        auto valueAt = [&](unsigned index) -> JSValue {
            const FieldAsProperty& property = (*fields)[index];
            if (property.field->flags & flagsBarringListing)
                return { };
            return compiled.readListedField(property);
        };
        return tryCreateList(globalObject, list, static_cast<unsigned>(fields->size()), listsKeys ? compiled.keys(vm) : nullptr, valueAt);
    }

    DeferGC deferGC(vm);
    const Vector<PropertyListEntry>* properties = compiled.fieldsAndExtraPropertiesInKeyOrder(vm);
    if (!properties)
        return nullptr;
    JSObject* extraProperties = compiled.extraProperties();
    auto valueAt = [&](unsigned index) -> JSValue {
        const PropertyListEntry& property = properties->at(index);
        if (property.field && property.field->flags & flagsBarringListing)
            return { };
        return compiled.readPropertyListEntry(property, extraProperties);
    };
    return tryCreateList(globalObject, list, static_cast<unsigned>(properties->size()), listsKeys ? compiled.keysWithExtraProperties(vm) : nullptr, valueAt);
}

static bool isListedWithoutRunningCode(JSObject* object)
{
    return !object->hasNonReifiedStaticProperties() && object->structure()->canPerformFastPropertyEnumerationCommon() && !object->canHaveExistingOwnIndexedProperties();
}

static JSArray* tryListFlatEntriesOfOrdinaryObject(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    Vector<UniquedStringImpl*, 16> names;
    MarkedArgumentBuffer values;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if (!(entry.attributes() & PropertyAttribute::DontEnum) && !entry.key()->isSymbol()) {
            names.append(entry.key());
            values.appendWithCrashOnOverflow(object->getDirect(entry.offset()));
        }
        return true;
    });
    JSCellButterfly* cachedKeys = structure->cachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings);
    if (cachedKeys && cachedKeys->length() != names.size())
        cachedKeys = nullptr;
    JSArray* entries = tryCreateFlatEntriesArray(globalObject, static_cast<unsigned>(names.size()), [&](unsigned index) -> JSValue {
        if (cachedKeys && asString(cachedKeys->get(index))->tryGetValueImpl() == names[index])
            return cachedKeys->get(index);
        return jsOwnedString(vm, names[index]);
    }, [&](unsigned index) {
        return values.at(index);
    });
    ensureStillAliveHere(object);
    return entries;
}

static JSValue listOwnProperties(Instance* instance, JSValue value, OwnPropertyList list)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    if (Object* compiled = instance->state->objects->layouts.compiledObject(value); compiled && !globalObject->isHavingABadTime()) {
        if (JSArray* array = tryListOwnProperties(globalObject, *compiled, list))
            return array;
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue listed;
    if (!value.isObject() || !isListedWithoutRunningCode(asObject(value)))
        listed = callInEngine(instance, engineFunctionFor(list), value);
    else if (JSArray* entries = list == OwnPropertyList::FlatEntries ? tryListFlatEntriesOfOrdinaryObject(globalObject, asObject(value)) : nullptr)
        return entries;
    else
        listed = list == OwnPropertyList::Values ? objectValues(vm, globalObject, value) : objectEntries(vm, globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (list != OwnPropertyList::FlatEntries)
        return listed;

    JSArray* pairs = uncheckedDowncast<JSArray>(listed);
    auto memberAt = [&](unsigned index, unsigned member) {
        return uncheckedDowncast<JSArray>(pairs->getDirectIndex(globalObject, index))->getDirectIndex(globalObject, member);
    };
    if (JSArray* entries = tryCreateFlatEntriesArray(globalObject, pairs->length(), [&](unsigned index) { return memberAt(index, 0); }, [&](unsigned index) { return memberAt(index, 1); }))
        return entries;
    JSArray* entries = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned index = 0; index < pairs->length(); ++index) {
        for (unsigned member : { 0u, 1u }) {
            entries->putDirectIndex(globalObject, index * 2 + member, memberAt(index, member));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return entries;
}

JSC_DEFINE_JIT_OPERATION(operationAOTObjectToStringOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    const SmallStrings& texts = instance->vm->smallStrings;
    if (value.isString())
        return { JSValue::encode(texts.objectStringString()), nullptr };
    if (value.isNumber())
        return { JSValue::encode(texts.objectNumberString()), nullptr };
    if (value.isBoolean())
        return { JSValue::encode(texts.objectBooleanString()), nullptr };
    if (value.isCell() && value.asCell()->type() == ProxyObjectType) [[unlikely]]
        return { encodedJSValue(), nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTObjectToStringOfAnyValue);
    OPERATION_RETURN(scope, JSValue::encode(objectPrototypeToString(globalObject, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTObjectValues, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTObjectValues);
    JSValue values = listOwnProperties(instance, JSValue::decode(encodedValue), OwnPropertyList::Values);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, values.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTObjectEntries, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTObjectEntries);
    JSValue entries = listOwnProperties(instance, JSValue::decode(encodedValue), OwnPropertyList::Entries);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, entries.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTPropertyIsEnumerable, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPropertyIsEnumerable);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    PropertyDescriptor descriptor;
    bool isOwn = object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, isOwn && descriptor.enumerable());
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetLiteralPrototype, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedPrototype, uint32_t refusesOtherValues))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetLiteralPrototype);
    JSObject* object = cellArgument<JSObject>(cell, Entry::operationAOTSetLiteralPrototype);
    JSValue prototype = JSValue::decode(encodedPrototype);
    if (!prototype.isObject() && !prototype.isNull()) {
        if (refusesOtherValues)
            throwTypeError(globalObject, scope, "Object prototype may only be an Object or null."_s);
        OPERATION_RETURN(scope);
    }
    if (Object* compiled = instance->state->objects->layouts.compiledObject(object)) {
        ObjectModel& objects = *instance->state->objects;
        const LayoutData& layout = compiled->compiledStructure().layout();
        if (layout.isClass() && prototype.isObject() && objects.classes.defineClassOfCreatedObjects(vm, objects.classes.classOfInstances(layout), asObject(prototype))) {
            objects.structures.forgetStructuresOf(objects.classes.classOfInstances(layout));
            compiled->setStructure(vm, objects.structures.objectStructure(vm, layout.shape));
            OPERATION_RETURN(scope);
        }
        if (compiled->getPrototypeDirect() != prototype)
            compiled->setPrototypeAtCreation(vm, prototype);
        OPERATION_RETURN(scope);
    }
    object->setPrototypeDirect(vm, prototype);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTObjectCreate, JSCell*, (Instance* instance, JSCell* prototypeOrNull))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTObjectCreate);
    DeferTerminationForAWhile deferTermination(vm);
    if (!prototypeOrNull) {
        Structure* structure = globalObject->nullPrototypeObjectStructure();
        prepareFinalObjectAllocation(instance, InlineAllocation::NullPrototypeObject, structure);
        OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
    }
    JSObject* prototype = cellArgument<JSObject>(prototypeOrNull, Entry::operationAOTObjectCreate);
    prepareFinalObjectAllocation(instance, InlineAllocation::DefaultFinalObject, globalObject->objectStructureForObjectConstructor());
    JSFinalObject* object = constructEmptyObject(globalObject, prototype);
    instance->state->builtinFamilies->cacheObjectCreate(prototype, object->structure());
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetOwnPropertyDescriptor, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetOwnPropertyDescriptor);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(callInEngine(instance, Builtins::ObjectGetOwnPropertyDescriptor, base, JSValue::decode(encodedKey))));
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(objectConstructorGetOwnPropertyDescriptor(globalObject, object, name)));
}

struct OwnPropertyFields {
    JSValue isPresent { jsBoolean(false) };
    JSValue value { jsUndefined() };
    JSValue writable { jsUndefined() };
    JSValue getter { jsUndefined() };
    JSValue setter { jsUndefined() };
    JSValue isEnumerable { jsBoolean(false) };
    JSValue isConfigurable { jsBoolean(false) };
};

static OwnPropertyFields ownPropertyFieldsOf(const PropertyDescriptor& descriptor)
{
    auto orUndefined = [](JSValue value) {
        return value ? value : jsUndefined();
    };
    return {
        .isPresent = jsBoolean(true),
        .value = orUndefined(descriptor.value()),
        .writable = descriptor.writablePresent() ? jsBoolean(descriptor.writable()) : jsUndefined(),
        .getter = descriptor.getterPresent() ? orUndefined(descriptor.getter()) : jsUndefined(),
        .setter = descriptor.setterPresent() ? orUndefined(descriptor.setter()) : jsUndefined(),
        .isEnumerable = jsBoolean(descriptor.enumerable()),
        .isConfigurable = jsBoolean(descriptor.configurable()),
    };
}

static void storeOwnPropertyFields(EncodedJSValue* results, const OwnPropertyFields& fields)
{
    std::array values { fields.isPresent, fields.value, fields.writable, fields.getter, fields.setter, fields.isEnumerable, fields.isConfigurable };
    auto words = unsafeMakeSpan(results, values.size());
    for (size_t i = 0; i < values.size(); ++i)
        words[i] = JSValue::encode(values[i]);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetOwnPropertyFields, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetOwnPropertyFields);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull()) {
            callInEngine(instance, Builtins::ObjectGetOwnPropertyDescriptor, base, JSValue::decode(encodedKey));
            OPERATION_RETURN_IF_EXCEPTION(scope);
            storeOwnPropertyFields(results, { });
            OPERATION_RETURN(scope);
        }
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (Object* compiled = isPreparedBeforeLookups(vm, name.impl()) ? nullptr : compiledObjectOrNull(object)) {
        unsigned attributes = 0;
        JSValue value = compiled->getDirectFromNativeCode(vm, name, &attributes);
        if (value && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
            storeOwnPropertyFields(results, {
                .isPresent = jsBoolean(true),
                .value = value,
                .writable = jsBoolean(!(attributes & PropertyAttribute::ReadOnly)),
                .isEnumerable = jsBoolean(!(attributes & PropertyAttribute::DontEnum)),
                .isConfigurable = jsBoolean(!(attributes & PropertyAttribute::DontDelete)),
            });
            OPERATION_RETURN(scope);
        }
    }
    PropertyDescriptor descriptor;
    bool isOwn = object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    storeOwnPropertyFields(results, isOwn ? ownPropertyFieldsOf(descriptor) : OwnPropertyFields { });
    OPERATION_RETURN(scope);
}

static bool hasOrdinaryOwnProperties(JSObject* object)
{
    return object->type() == FinalObjectType || object->inherits<Object>();
}

static bool mayChangeDescriptors(VM& vm, JSObject* objectPrototype)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    for (const Identifier* name : { &names.value, &names.writable, &names.get, &names.set, &names.enumerable, &names.configurable }) {
        if (isValidOffset(objectPrototype->structure()->get(vm, *name)))
            return true;
    }
    return false;
}

namespace {

struct PropertiesInSlots {
    void append(UniquedStringImpl* name, JSValue valueOrAccessor, unsigned attributesOfProperty)
    {
        names.append(name);
        attributes.append(attributesOfProperty);
        valuesOrAccessors.appendWithCrashOnOverflow(valueOrAccessor);
        arePlain &= !attributesOfProperty;
    }

    Vector<UniquedStringImpl*, 24> names;
    Vector<unsigned, 24> attributes;
    MarkedArgumentBuffer valuesOrAccessors;
    bool arePlain { true };
};

} // anonymous namespace

static bool tryReadBySlots(VM& vm, JSObject* cell, PropertiesInSlots& properties)
{
    Object* source = compiledObjectOrNull(cell);
    if (!source)
        return false;
    const LayoutData& layout = source->layout();
    if (source->hasExtraProperties() || layout.keysAreNeverEnumerated) {
        auto result = PropertyWalk::forEachOwnPropertyInline(vm, source, PropertyWalk::Filter::StringsAndSymbols, [&](const PropertyWalk::OwnProperty& property) {
            properties.append(property.name, property.value, property.attributes);
            return IterationStatus::Continue;
        });
        return result == PropertyWalk::Result::Completed;
    }

    State& state = source->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout.fields();
    auto readKeys = [&](bool symbols) {
        source->forEachFieldInKeyOrder(state, [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (field.isPrivate() || !(symbols ? layouts.hasSymbolKey(field) : layouts.hasStringKey(field)))
                return;
            if (FieldProperty property = findFieldProperty(vm, *source, field))
                properties.append(layouts.nameOf(vm, field), property.valueOrAccessor, property.attributes);
        });
    };
    readKeys(false);
    if (layout.hasFieldsWithSymbolKeys)
        readKeys(true);
    return true;
}

namespace {

class PropertyDefiner {
public:
    PropertyDefiner(VM& vm, JSGlobalObject* globalObject, JSObject* target)
        : m_vm(vm)
        , m_globalObject(globalObject)
        , m_target(target)
        , m_compiledTarget(compiledObjectOrNull(target))
    {
        if (m_compiledTarget && m_compiledTarget->orderWord() & orderWordIntegrityBits)
            m_compiledTarget = nullptr;
    }

    Object* compiledTargetThatRefusesNothing() const { return m_compiledTarget; }

    void define(PropertyName name, const PropertyDescriptor& descriptor)
    {
        if (m_compiledTarget && tryDefineWithoutValidation(name, descriptor))
            return;
        addNewNames();
        m_target->methodTable()->defineOwnProperty(m_target, m_globalObject, name, descriptor, true);
    }

    void addNewNames()
    {
        if (m_newNames.isEmpty())
            return;
        JSObject* extra = m_compiledTarget->ensureExtraProperties(m_vm);
        RELEASE_ASSERT(extra);
        for (UniquedStringImpl* name : m_newNames)
            m_compiledTarget->willAddExtraProperty(m_vm, name);
        extra->putOwnDataPropertyBatching(m_vm, m_newNames.mutableSpan().data(), m_newValues.data(), m_newNames.size());
        m_compiledTarget->didChangeExtraProperties(m_vm);
        didChangeOwnProperties(m_vm, *m_compiledTarget, VM::StructureChainIntegrityEvent::Add, nullptr);
        m_newNames.shrink(0);
        m_newValues.clear();
    }

private:
    bool tryDefineWithoutValidation(PropertyName name, const PropertyDescriptor& descriptor)
    {
        Object& target = *m_compiledTarget;
        unsigned attributes = descriptor.attributes();
        if (const FieldData* field = target.fieldNamed(target.state(), name)) {
            if (attributes || !descriptor.isDataDescriptor() || target.attributesOf(*field))
                return false;
            FieldProperty current = findFieldProperty(m_vm, target, *field);
            return current && current.isInSlot && target.tryStore(m_vm, *field, descriptor.value());
        }
        JSObject* extra = target.extraProperties();
        if (parseIndex(name) || name.isPrivateName() || (extra && isValidOffset(extra->structure()->get(m_vm, name))))
            return false;
        if (!attributes && descriptor.isDataDescriptor()) {
            m_newNames.append(name.uid());
            m_newValues.appendWithCrashOnOverflow(descriptor.value());
            return true;
        }
        addNewNames();
        if (!descriptor.isAccessorDescriptor())
            return target.tryDefineAbsentExtraProperty(m_vm, name, descriptor.value(), attributes & ~PropertyAttribute::Accessor);
        auto* accessor = GetterSetter::create(m_vm, m_globalObject, descriptor.getterObject(), descriptor.setterObject());
        return target.tryDefineAbsentExtraProperty(m_vm, name, accessor, (attributes & ~PropertyAttribute::ReadOnly) | PropertyAttribute::Accessor);
    }

    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSObject* m_target;
    Object* m_compiledTarget;
    Vector<UniquedStringImpl*, 16> m_newNames;
    MarkedArgumentBuffer m_newValues;
};

} // anonymous namespace

} } // namespace JSC::AOT

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__ownEntriesFlat(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedObject)
{
    return JSC::JSValue::encode(JSC::AOT::listOwnProperties(JSC::AOT::Instance::of(globalObject), JSC::JSValue::decode(encodedObject), JSC::AOT::OwnPropertyList::FlatEntries));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__describeUnlessOrdinary(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedSource)
{
    using namespace JSC;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* source = JSValue::decode(encodedSource).toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (AOT::hasOrdinaryOwnProperties(source)) [[likely]]
        return JSValue::encode(source);
    JSValue descriptors = objectConstructorGetOwnPropertyDescriptors(globalObject, source);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), descriptors, jsUndefined()));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__definePropertiesFrom(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedTarget, JSC::EncodedJSValue encodedDescribed, JSC::EncodedJSValue)
{
    using namespace JSC;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!JSValue::decode(encodedTarget).isObject()) {
        throwTypeError(globalObject, scope, DefinePropertyOnNonObjectError);
        return { };
    }
    JSObject* target = asObject(JSValue::decode(encodedTarget));
    JSObject* source = asObject(JSValue::decode(encodedDescribed));

    auto* describedElsewhere = dynamicDowncast<InternalFieldTuple>(source);
    if (describedElsewhere || !AOT::hasOrdinaryOwnProperties(target) || AOT::mayChangeDescriptors(vm, globalObject->objectPrototype())) [[unlikely]] {
        JSValue descriptors = describedElsewhere ? describedElsewhere->getInternalField(0) : objectConstructorGetOwnPropertyDescriptors(globalObject, source);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(AOT::callInEngine(AOT::Instance::of(globalObject), AOT::Builtins::ObjectDefineProperties, target, descriptors)));
    }

    EnsureStillAliveScope sourceScope(source);
    AOT::PropertyDefiner definer(vm, globalObject, target);

    AOT::PropertiesInSlots properties;
    if (AOT::tryReadBySlots(vm, source, properties)) {
        AOT::Object* compiledTarget = definer.compiledTargetThatRefusesNothing();
        if (compiledTarget && properties.arePlain && !compiledTarget->hasFlagsInOrderWord() && compiledTarget->tryDefineFields(vm, properties.names.span(), { properties.valuesOrAccessors.data(), properties.valuesOrAccessors.size() }))
            return JSValue::encode(target);
        for (size_t index = 0; index < properties.names.size(); ++index) {
            PropertyDescriptor descriptor;
            descriptor.setDescriptor(properties.valuesOrAccessors.at(index), properties.attributes[index]);
            definer.define(properties.names[index], descriptor);
            RETURN_IF_EXCEPTION(scope, { });
        }
        definer.addNewNames();
        return JSValue::encode(target);
    }

    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    source->methodTable()->getOwnPropertyNames(source, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& name : names) {
        PropertyDescriptor descriptor;
        bool isOwn = source->getOwnPropertyDescriptor(globalObject, name, descriptor);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isOwn)
            continue;
        definer.define(name, descriptor);
        RETURN_IF_EXCEPTION(scope, { });
    }
    definer.addNewNames();
    return JSValue::encode(target);
}

#endif // ENABLE(AOT)
