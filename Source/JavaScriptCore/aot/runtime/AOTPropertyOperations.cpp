/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeRecords.h"
#include "AOTSchemaEnums.h"
#include "AOTState.h"
#include "AOTTypeErrorMessages.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include <optional>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral homeObjectPrototypeIsNotObjectMessage = "The prototype of the home object is not an object"_s;

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetById);
    JSValue value = JSValue::decode(base);
    const Identifier& identifier = identifierAt(instance, name);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(value, identifier.string(), false));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    ClassTable& classes = instance->state->objects->classes;
    if (auto found = value.isObject() ? classes.findThroughLazyPrototypes(vm, asObject(value), identifier.impl(), false) : std::nullopt) {
        if (found->member) {
            JSValue member = found->member->holder->getDirect(found->member->offset);
            if (!found->member->isAccessor)
                OPERATION_RETURN(scope, JSValue::encode(member));
            OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<GetterSetter>(member.asCell())->callGetter(globalObject, value)));
        }
        if (!found->firstObjectAbove)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        PropertySlot slot(value, PropertySlot::InternalMethodType::Get);
        bool has = found->firstObjectAbove->getPropertySlot(globalObject, identifier, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(has ? slot.getValue(globalObject, identifier) : jsUndefined()));
    }
    OPERATION_RETURN(scope, JSValue::encode(value.get(globalObject, identifier)));
}

static void putToCompiledObject(Instance* instance, Object* object, const Identifier& identifier, JSValue value, PutPropertySlot& slot)
{
    const FieldData* field = object->fieldNamed(*instance->state, identifier);
    if (field && object->hasExtraProperties() && !object->read(*field))
        object->ensureStructureForExtraProperties(*instance->vm);
    Object::putWithKnownField(object, instance->globalObject, identifier, value, slot, field);
}

template<PutMode mode>
static void putById(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue base, uint32_t name, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = identifierAt(instance, name);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(base, identifier.string(), true));
        return;
    }
    PutPropertySlot slot(base, mode == PutMode::Strict);
    if (Object* object = compiledObjectOrNull(base))
        RELEASE_AND_RETURN(scope, putToCompiledObject(instance, object, identifier, value, slot));
    scope.release();
    base.put(globalObject, identifier, value, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutById);
    putById<PutMode::Strict>(instance, globalObject, vm, JSValue::decode(base), name, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdSloppy, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdSloppy);
    putById<PutMode::Sloppy>(instance, globalObject, vm, JSValue::decode(base), name, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

static size_t deleteFromObject(JSGlobalObject* globalObject, VM& vm, JSObject* object, PropertyName property, Typed::ECMAMode mode)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool deleted = JSCell::deleteProperty(object, globalObject, property);
    RETURN_IF_EXCEPTION(scope, 0);
    if (deleted)
        return 1;
    if (mode == Typed::ECMAMode::Strict)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    return 0;
}

static size_t deleteFromValue(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName property, Typed::ECMAMode mode)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    RELEASE_AND_RETURN(scope, deleteFromObject(globalObject, vm, object, property, mode));
}

static size_t deleteKeyFromValue(JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue key, Typed::ECMAMode mode)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    if (auto longKeyName = findNameEqualToLongString(object, key, false)) [[unlikely]] {
        if (!*longKeyName)
            return 1;
        RELEASE_AND_RETURN(scope, deleteFromObject(globalObject, vm, object, Identifier::fromUid(vm, *longKeyName), mode));
    }
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    RELEASE_AND_RETURN(scope, deleteFromObject(globalObject, vm, object, property, mode));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteById, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteById);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, vm, JSValue::decode(base), identifierAt(instance, name), Typed::ECMAMode::Strict));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByIdSloppy, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByIdSloppy);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, vm, JSValue::decode(base), identifierAt(instance, name), Typed::ECMAMode::Sloppy));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByVal, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByVal);
    OPERATION_RETURN(scope, deleteKeyFromValue(globalObject, vm, JSValue::decode(base), JSValue::decode(key), Typed::ECMAMode::Strict));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValSloppy, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByValSloppy);
    OPERATION_RETURN(scope, deleteKeyFromValue(globalObject, vm, JSValue::decode(base), JSValue::decode(key), Typed::ECMAMode::Sloppy));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetSuperBase, EncodedJSValue, (Instance* instance, EncodedJSValue home, uint32_t homeKind))
{
    constexpr Entry entry = Entry::operationAOTGetSuperBase;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSObject* homeObject = objectArgument(home, entry);
    switch (static_cast<Typed::HomeObjectKind>(homeKind)) {
    case Typed::HomeObjectKind::Object:
        break;
    case Typed::HomeObjectKind::ClassPrototype:
        homeObject = objectArgument(JSValue::encode(cellArgument<Closure>(homeObject, entry)->prototypeProperty(vm)), entry);
        break;
    }
    if (homeObject->structure()->typeInfo().overridesGetPrototype()) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, JSValue::encode(homeObject->getPrototypeDirect()));
}

static JSObject* checkSuperBase(JSGlobalObject* globalObject, VM& vm, JSValue base)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isObject()) {
        throwTypeError(globalObject, scope, homeObjectPrototypeIsNotObjectMessage);
        return nullptr;
    }
    return asObject(base);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetSuperPropertyById, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue receiver, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetSuperPropertyById);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    const Identifier& identifier = identifierAt(instance, name);
    ClassPrototype::ensureMembers(parent);
    Object::ensureStructureForExtraPropertiesIfNeeded(vm, parent);
    Structure* structureBefore = parent->structure();
    PropertySlot slot(JSValue::decode(receiver), PropertySlot::InternalMethodType::Get);
    bool has = parent->getPropertySlot(globalObject, identifier, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = has ? slot.getValue(globalObject, identifier) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (parent->structure() == structureBefore) {
        PropertyCaches& caches = *instance->state->propertyCaches;
        PropertyAnswer answer = classifyRead({ globalObject, parent, structureBefore, identifier.impl(), slot });
        if (answer.isCacheable())
            caches.cache({ structureBefore, caches.nameOf(name), SlotAccess::Read }, answer);
    }
    ensureStillAliveHere(structureBefore);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperGet, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue receiver))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperGet);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    PropertySlot slot(JSValue::decode(receiver), PropertySlot::InternalMethodType::Get);
    bool has = parent->getPropertySlot(globalObject, property, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(has ? slot.getValue(globalObject, property) : jsUndefined()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperSet, void, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue value, EncodedJSValue receiver, uint32_t mode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperSet);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(JSValue::decode(receiver), static_cast<Typed::ECMAMode>(mode) == Typed::ECMAMode::Strict);
    parent->methodTable()->put(parent, globalObject, property, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTScopeHasBinding, size_t, (Instance* instance, EncodedJSValue encodedScope, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTScopeHasBinding);
    JSObject* object = objectArgument(encodedScope, Entry::operationAOTScopeHasBinding);
    const Identifier& identifier = identifierAt(instance, name);
    bool hasBinding = object->hasProperty(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!hasBinding)
        OPERATION_RETURN(scope, false);
    JSValue unscopables = object->get(globalObject, vm.propertyNames->unscopablesSymbol);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!unscopables.isObject())
        OPERATION_RETURN(scope, true);
    JSValue blocked = asObject(unscopables)->get(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, !blocked.toBoolean(globalObject));
}

static JSValue getOwnDataProperty(VM& vm, JSObject* object, uint32_t key, const Identifier& name)
{
    if (std::optional<uint32_t> index = indexNamedBy(key, name))
        return object->canGetIndexQuickly(*index) ? object->getIndexQuickly(*index) : JSValue();
    if (isCompiledObject(object))
        return Object::tryGetOwnDataProperty(vm, object, name);
    unsigned attributes = 0;
    JSValue value = object->getDirect(vm, name, attributes);
    return attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue ? JSValue() : value;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetOwnDataProperty, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t key))
{
    constexpr Entry entry = Entry::operationAOTGetOwnDataProperty;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSValue value = getOwnDataProperty(vm, cellArgument<JSObject>(cell, entry), key, identifierAt(instance, key));
    OPERATION_RETURN(scope, JSValue::encode(value ? value : jsUndefined()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
