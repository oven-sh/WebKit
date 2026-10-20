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
#include "AOTPropertyCachesNamedAccess.h"
#include "AOTRuntimeFunctions.h"
#include "AOTState.h"
#include "AOTTypeErrorMessages.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static NamedAccess namedAccess(Instance* instance, uint32_t key, SiteCache site)
{
    return { instance, instance->globalObject, *instance->vm, *instance->state->propertyCaches, identifierAt(instance, key), key, site };
}

static CachedAccess cachedAccess(const NamedAccess& access, Structure* structure, SlotAccess slotAccess)
{
    return { structure, { access.ident.impl(), access.number }, slotAccess, access.site };
}

// Only the place of a property that a cacheable dictionary holds is filed under such a Structure. Every other read by name comes here each time, and the lookup that classifies nothing answers it.
static JSValue tryGetFromPermanentDictionary(Instance* instance, JSValue base, UniquedStringImpl* uid)
{
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return { };
    VM& vm = *instance->vm;
    Structure* structure = base.asCell()->structure();
    if (!structure->isDictionary() || !structure->hasBeenFlattenedBefore() || isNeverCachedName(vm, uid))
        return { };
    if (!structure->isUncacheableDictionary() && isValidOffset(structure->get(vm, uid)))
        return { };
    return JSValue::decode(operationAOTGetGeneric(instance, base.asCell(), uid));
}

static bool tryGetFromOrdinaryObject(JSGlobalObject* globalObject, JSValue base, UniquedStringImpl* uid, PropertySlot& slot, JSValue& result)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || isNeverCachedName(vm, uid))
        return false;
    JSObject* object = asObject(base.asCell());
    Structure* structure = object->structure();
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot())
        return false;
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return false;
    result = object->getDirect(offset);
    slot.setValue(object, attributes, result, offset);
    return true;
}

JSValue getAndCache(const NamedAccess& access, JSValue base)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    if (base.isCell())
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, base.asCell());
    Structure* structureBefore = structureForPrimitive(globalObject, base);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);

    LazyPrototypeRead lazy;
    if (base.isObject())
        lazy = classifyReadThroughLazyPrototypes(globalObject, asObject(base), uid);

    JSValue result;
    switch (lazy.result) {
    case LazyPrototypeReadResult::Answered:
        access.caches.cache(cachedAccess(access, structureBefore, SlotAccess::Read), lazy.answer);
        RELEASE_AND_RETURN(scope, readThroughAnswer(globalObject, base, lazy.answer));
    case LazyPrototypeReadResult::ContinuesAbove:
        result = JSValue(lazy.firstObjectAbove).get(globalObject, ident, slot);
        break;
    case LazyPrototypeReadResult::NotApplicable:
        if (!tryGetFromOrdinaryObject(globalObject, base, uid, slot, result))
            result = base.get(globalObject, ident, slot);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });

    access.caches.cache(cachedAccess(access, structureBefore, SlotAccess::Read), classifyRead({ globalObject, base, structureBefore, uid, slot }));
    ensureStillAliveHere(structureBefore);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t key, SiteCache cache))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByIdAndCache);
    NamedAccess access = namedAccess(instance, key, cache);
    JSValue base = JSValue::decode(encodedBase);
    if (JSValue value = tryGetFromPermanentDictionary(instance, base, access.ident.impl()))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(base, access.ident.string(), false));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(getAndCache(access, base)));
}

static bool tryStoreToPermanentDictionary(const NamedAccess& access, JSObject* object, JSValue value, PutMode mode, PutPropertySlot& slot)
{
    VM& vm = access.vm;
    UniquedStringImpl* uid = access.ident.impl();
    Structure* structure = object->structure();
    if (flattenOnce(vm, object))
        return false;
    unsigned attributes = 0;
    PropertyOffset offset = object->type() == FinalObjectType ? structure->get(vm, uid, attributes) : invalidOffset;
    if (isValidOffset(offset) && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) && !structure->isImmutableProperty(uid)) {
        structure->didReplaceProperty(offset);
        object->putDirectOffset(vm, offset, value);
        if (!structure->isUncacheableDictionary()) {
            slot.setExistingProperty(object, offset);
            access.caches.cache(cachedAccess(access, structure, accessFor(mode)), classifyStore({ access.globalObject, object, structure, uid, slot, mode }));
        }
        return true;
    }
    JSValue(object).putInline(access.globalObject, access.ident, value, slot);
    return true;
}

static bool tryStoreExtraProperty(const NamedAccess& access, Object& object, JSValue value, PutMode mode, Structure*& structureBefore, const PutPropertySlot& slot)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = access.vm;
    OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
    bool isRepeated = object.repeatsExtraPropertyStore(vm, access.ident);
    if (isRepeated) {
        object.ensureStructureForExtraProperties(vm);
        flags.add(Flag::NamesExtraPropertiesStructure);
    }
    structureBefore = object.structure();
    if (mode == PutMode::Define)
        flags.add(Flag::IsDefinition);
    if (object.tryStoreExtraProperty(vm, access.ident, value, flags).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    if (!isRepeated)
        return true;
    PropertyAnswer answer = classifyStore({ access.globalObject, &object, structureBefore, access.ident.impl(), slot, mode });
    access.caches.cache(cachedAccess(access, structureBefore, accessFor(mode)), answer);
    return true;
}

static void primeFieldRead(const NamedAccess& access, Object& object, Structure* structure)
{
    UniquedStringImpl* uid = access.ident.impl();
    PropertySlot unsetSlot(&object, PropertySlot::InternalMethodType::Get);
    PropertyAnswer answer = classifyCompiledObjectRead({ access.globalObject, &object, structure, uid, unsetSlot }, object);
    access.caches.cache({ structure, { uid, access.number }, SlotAccess::Read, nullptr }, answer);
}

void putAndCache(const NamedAccess& access, JSCell* cell, JSValue value, PutMode mode)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    JSValue base = cell;
    PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
    if (mode != PutMode::Define && cell->isObject() && cell->structure()->isDictionary() && !cell->structure()->isCompiledStructure()) {
        if (tryStoreToPermanentDictionary(access, asObject(cell), value, mode, slot))
            return;
    }

    Structure* structureBefore = nullptr;
    Object* object = compiledObjectOrNull(base);
    const FieldData* field = object ? object->fieldNamed(object->state(), ident) : nullptr;
    if (object && !field && tryStoreExtraProperty(access, *object, value, mode, structureBefore, slot))
        return;

    Object::ensureStructureForExtraPropertiesIfNeeded(vm, cell);
    structureBefore = cell->structure();
    bool repeatsAddition = object || !cell->isObject() || repeatsPropertyAddition(structureBefore, uid);
    if (mode == PutMode::Define && cell->isObject())
        defineDataProperty(access.instance, asObject(cell), ident, value, slot);
    else if (object)
        Object::putWithKnownField(cell, globalObject, ident, value, slot, field);
    else
        base.putInline(globalObject, ident, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    if (!cell->isObject() || (!repeatsAddition && slot.type() == PutPropertySlot::NewProperty))
        return;

    PropertyAnswer answer = classifyStore({ globalObject, asObject(cell), structureBefore, uid, slot, mode });
    access.caches.cache(cachedAccess(access, structureBefore, accessFor(mode)), answer);
    bool isStoreToField = answer.kind == PropertyAnswerKind::Field || answer.kind == PropertyAnswerKind::FieldCreation;
    if (object && isStoreToField && mode != PutMode::Define)
        primeFieldRead(access, *object, structureBefore);
    ensureStillAliveHere(structureBefore);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t key, SiteCache cache, uint32_t putMode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdAndCache);
    NamedAccess access = namedAccess(instance, key, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    PutMode mode = static_cast<PutMode>(putMode);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(base, access.ident.string(), true));
        OPERATION_RETURN(scope);
    }
    if (!base.isCell()) {
        PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
        base.putInline(globalObject, access.ident, value, slot);
        OPERATION_RETURN(scope);
    }
    putAndCache(access, base.asCell(), value, mode);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillSiteCache, void, (Instance* instance, JSCell* base, uint32_t key, SiteCache cache, uint32_t access))
{
    instance->state->propertyCaches->fillSiteCacheFromTables(cache, base, key, static_cast<SlotAccess>(access));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
