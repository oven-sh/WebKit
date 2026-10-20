/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTState.h"
#include "AOTTypeErrorMessages.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static OperationReturnType<EncodedJSValue> getByVal(JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue property)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(globalObject, base, property, false));
        OPERATION_RETURN(scope, encodedJSValue());
    }

    if (std::optional<uint32_t> index = indexOfKey(property)) {
        uint32_t i = *index;
        if (isJSString(base)) {
            if (asString(base)->canGetIndex(i))
                OPERATION_RETURN(scope, JSValue::encode(asString(base)->getIndex(globalObject, i)));
        } else if (base.isObject()) {
            JSObject* object = asObject(base);
            if (JSValue result = object->tryGetIndexQuickly(i))
                OPERATION_RETURN(scope, JSValue::encode(result));
            Structure* structure = object->structure();
            if (structure->realm() == globalObject && globalObject->isOriginalArrayStructure(structure) && !hasAnyArrayStorage(structure->indexingType()) && globalObject->arrayPrototypeChainIsSane())
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, i)));
    }

    if (base.isObject() && property.isString()) [[likely]] {
        auto existingAtomString = asString(property)->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (existingAtomString) {
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, Identifier::fromUid(vm, existingAtomString.data), slot)));
        }
    }

    if (base.isObject() && property.isSymbol()) {
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, Identifier::fromUid(asSymbol(property)->privateName()), slot)));
    }

    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByVal);
    scope.release();
    return getByVal(globalObject, vm, JSValue::decode(encodedBase), JSValue::decode(encodedProperty));
}

static OperationReturnType<void> putByVal(JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue property, JSValue value, bool isStrict)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(globalObject, base, property, true));
        OPERATION_RETURN(scope);
    }

    if (std::optional<uint32_t> index = indexOfKey(property)) {
        uint32_t i = *index;
        if (base.isObject()) {
            JSObject* object = asObject(base);
            if (object->trySetIndexQuickly(vm, i, value))
                OPERATION_RETURN(scope);
            scope.release();
            object->methodTable()->putByIndex(object, globalObject, i, value, isStrict);
            OPERATION_RETURN(scope);
        }
        scope.release();
        base.putByIndex(globalObject, i, value, isStrict);
        OPERATION_RETURN(scope);
    }

    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(base, isStrict);
    base.put(globalObject, key, value, slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByVal);
    scope.release();
    return putByVal(globalObject, vm, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), isStrict);
}

struct PendingAccess {
    JSObject* object;
    Structure* structureBefore;
    UniquedStringImpl* uid;
};

static PendingAccess prepareAccess(VM& vm, JSObject* object, UniquedStringImpl* uid)
{
    Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
    return { object, object->structure(), uid };
}

static CachedAccess cachedAccess(const PendingAccess& pending, SlotAccess access)
{
    return { pending.structureBefore, { pending.uid, std::nullopt }, access };
}

static OperationReturnType<EncodedJSValue> getFromPrimitiveAndCache(Instance* instance, JSValue base, const Identifier& name)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    UniquedStringImpl* uid = name.impl();
    Structure* structureBefore = structureForPrimitive(globalObject, base);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    JSValue result = base.get(globalObject, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    PropertyAnswer answer = classifyRead({ globalObject, base, structureBefore, uid, slot });
    instance->state->propertyCaches->cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Read }, answer);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static OperationReturnType<EncodedJSValue> getFromObjectAndCache(Instance* instance, JSObject* object, const Identifier& name)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    PendingAccess pending = prepareAccess(vm, object, name.impl());
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);

    JSObject* lookupStart = object;
    LazyPrototypeRead lazy = classifyReadThroughLazyPrototypes(globalObject, object, pending.uid);
    switch (lazy.result) {
    case LazyPrototypeReadResult::Answered:
        caches.cache(cachedAccess(pending, SlotAccess::Read), lazy.answer);
        OPERATION_RETURN(scope, JSValue::encode(readThroughAnswer(globalObject, object, lazy.answer)));
    case LazyPrototypeReadResult::ContinuesAbove:
        lookupStart = lazy.firstObjectAbove;
        break;
    case LazyPrototypeReadResult::NotApplicable:
        break;
    }

    bool isFound = lookupStart->getPropertySlot(globalObject, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = isFound ? slot.getValue(globalObject, name) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    caches.cache(cachedAccess(pending, SlotAccess::Read), classifyRead({ globalObject, pending.object, pending.structureBefore, pending.uid, slot }));
    ensureStillAliveHere(pending.structureBefore);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (auto longKeyName = findNameEqualToLongString(base, property)) [[unlikely]] {
        if (!*longKeyName)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        OPERATION_RETURN(scope, JSValue::encode(asObject(base)->get(globalObject, Identifier::fromUid(vm, *longKeyName))));
    }
    // The keyed stubs probe the hashed table only for a receiver that is a cell.
    if (!base.isCell()) {
        scope.release();
        return getByVal(globalObject, vm, base, property);
    }

    auto name = nameForKeyedCache(globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!base.isObject()) {
        scope.release();
        if (name)
            return getFromPrimitiveAndCache(instance, base, *name);
        return getByVal(globalObject, vm, base, property);
    }

    JSObject* object = asObject(base);
    if (!name && property.isString()) {
        if (object->structure()->isCompiledStructure()) {
            Identifier identifier = asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*identifier.impl()))
                name = WTF::move(identifier);
        } else {
            auto text = asString(property)->value(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*text.data.impl()) && classifyChainLookups(object) == ChainLookups::Ordinary)
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    scope.release();
    if (!name)
        return getByVal(globalObject, vm, base, property);
    return getFromObjectAndCache(instance, object, *name);
}

static bool tryStoreExtraProperty(Instance* instance, Object& object, PendingAccess& pending, JSValue value, PutMode mode, const PutPropertySlot& slot)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = *instance->vm;
    OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
    bool isRepeated = object.repeatsExtraPropertyStore(vm, pending.uid);
    if (isRepeated) {
        object.ensureStructureForExtraProperties(vm);
        flags.add(Flag::NamesExtraPropertiesStructure);
    }
    pending.structureBefore = object.structure();
    if (object.tryStoreExtraProperty(vm, pending.uid, value, flags).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    if (!isRepeated)
        return true;
    PropertyAnswer answer = classifyStore({ instance->globalObject, pending.object, pending.structureBefore, pending.uid, slot, mode });
    instance->state->propertyCaches->cache(cachedAccess(pending, accessFor(mode)), answer);
    return true;
}

static OperationReturnType<void> putByValAndCache(Instance* instance, JSGlobalObject* globalObject, JSValue base, JSValue property, JSValue value, PutMode mode)
{
    ASSERT(mode != PutMode::Define);
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isStrict = mode == PutMode::Strict;
    if (auto longKeyName = findNameEqualToLongString(base, property); longKeyName && *longKeyName) [[unlikely]] {
        PutPropertySlot slot(base, isStrict);
        base.put(globalObject, Identifier::fromUid(vm, *longKeyName), value, slot);
        OPERATION_RETURN(scope);
    }
    if (!base.isObject()) {
        scope.release();
        return putByVal(globalObject, vm, base, property, value, isStrict);
    }

    std::optional<Identifier> name;
    if (property.isString()) {
        Identifier stringIdentifier = asString(property)->toIdentifier(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (!parseIndex(*stringIdentifier.impl()))
            name = WTF::move(stringIdentifier);
    } else
        name = nameForKeyedCache(globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!name) {
        scope.release();
        return putByVal(globalObject, vm, base, property, value, isStrict);
    }

    JSObject* object = asObject(base);
    PendingAccess pending { object, nullptr, name->impl() };
    PutPropertySlot slot(base, isStrict);
    Object* compiledObject = compiledObjectOrNull(base);
    const FieldData* field = compiledObject ? compiledObject->fieldNamed(compiledObject->state(), *name) : nullptr;
    if (compiledObject && !field && tryStoreExtraProperty(instance, *compiledObject, pending, value, mode, slot))
        OPERATION_RETURN(scope);

    pending = prepareAccess(vm, object, name->impl());
    bool repeatsAddition = compiledObject || repeatsPropertyAddition(pending.structureBefore, pending.uid);
    if (compiledObject)
        Object::putWithKnownField(object, globalObject, *name, value, slot, field);
    else
        base.put(globalObject, *name, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!repeatsAddition && slot.type() == PutPropertySlot::NewProperty)
        OPERATION_RETURN(scope);

    PropertyAnswer answer = classifyStore({ globalObject, pending.object, pending.structureBefore, pending.uid, slot, mode });
    instance->state->propertyCaches->cache(cachedAccess(pending, accessFor(mode)), answer);
    ensureStillAliveHere(pending.structureBefore);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByValAndCache);
    scope.release();
    return putByValAndCache(instance, globalObject, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), PutMode::Strict);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCacheSloppy, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByValAndCacheSloppy);
    scope.release();
    return putByValAndCache(instance, globalObject, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), PutMode::Sloppy);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
