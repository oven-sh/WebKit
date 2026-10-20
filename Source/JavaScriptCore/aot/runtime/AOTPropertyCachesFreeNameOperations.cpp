/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "SymbolTableInlines.h"

namespace JSC { namespace AOT {

// The stub comes here for a site that holds an Uncacheable slot too, and then only the access is wanted.
static SiteCache siteToFill(SiteCache site)
{
    return site && site->get().kind != Slot::Kind::Uncacheable ? site : nullptr;
}

static bool canCachePropertiesOf(JSGlobalObject* globalObject)
{
    TypeInfo typeInfo = globalObject->structure()->typeInfo();
    return !typeInfo.prohibitsPropertyCaching() && !typeInfo.getOwnPropertySlotIsImpure() && !typeInfo.getOwnPropertySlotIsImpureForPropertyAbsence();
}

static bool isFixed(const PropertySlot& slot, const Identifier& identifier)
{
    constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    return (slot.attributes() & fixed) == fixed || identifier.isPrivateName();
}

static bool isConstantVariable(JSGlobalObject* globalObject, const Identifier& identifier)
{
    SymbolTableEntry::Fast entry = globalObject->symbolTable()->get(identifier.impl());
    return !entry.isNull() && (entry.isReadOnly() || identifier.isPrivateName());
}

static ChainWalk walkAboveGlobalObject(JSGlobalObject* globalObject, UniquedStringImpl* uid, ChainWalkMode mode = ChainWalkMode::MayAllocate)
{
    return walkChain({ globalObject, globalObject->structure(), globalObject, mode }, uid);
}

static PropertyAnswer uncacheableOnce(SlotAccess access)
{
    return PropertyAnswer::uncacheable(access, true);
}

static PropertyAnswer uncacheableAfter(ChainWalk&& walk)
{
    PropertyAnswer answer = PropertyAnswer::uncacheable(SlotAccess::Read, walk.didFlatten);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer constantAnswer(JSValue value, JSGlobalObject* changeableGlobalObject = nullptr)
{
    PropertyAnswer answer;
    answer.kind = PropertyAnswerKind::Constant;
    answer.constant = value;
    if (changeableGlobalObject)
        answer.changeableObjects.append(changeableGlobalObject);
    return answer;
}

static PropertyAnswer answerInStorage(PropertyAnswerKind kind, SlotAccess access, JSObject* holder, PropertyOffset offset)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.access = access;
    answer.holder = holder;
    answer.offset = offset;
    return answer;
}

static PropertyAnswer classifyLexicalRead(JSGlobalObject* globalObject, UniquedStringImpl* uid, JSValue value)
{
    if (globalObject->globalLexicalEnvironment()->isConstVariable(uid))
        return constantAnswer(value, globalObject);
    return PropertyAnswer::uncacheable(SlotAccess::Read);
}

static PropertyAnswer classifyAbsentRead(JSGlobalObject* globalObject, UniquedStringImpl* uid)
{
    if (!canCachePropertiesOf(globalObject))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    ChainWalk walk = walkAboveGlobalObject(globalObject, uid);
    if (walk.result != ChainWalkResult::Absent)
        return uncacheableAfter(WTF::move(walk));
    PropertyAnswer answer;
    answer.kind = PropertyAnswerKind::Absent;
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer classifyInheritedRead(JSGlobalObject* globalObject, UniquedStringImpl* uid, const PropertySlot& slot)
{
    ChainWalk walk = walkAboveGlobalObject(globalObject, uid);
    if (walk.result != ChainWalkResult::Found || walk.holder != slot.slotBase())
        return uncacheableAfter(WTF::move(walk));
    bool isValue = slot.isCacheableValue() && !(walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue);
    bool isGetter = slot.isCacheableGetter() && walk.attributes & PropertyAttribute::Accessor;
    if (!isValue && !isGetter)
        return uncacheableAfter(WTF::move(walk));
    PropertyAnswer answer = answerInStorage(isValue ? PropertyAnswerKind::Value : PropertyAnswerKind::Accessor, SlotAccess::Read, walk.holder, walk.offset);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer classifyOwnValueRead(Instance& instance, JSGlobalObject* globalObject, const Identifier& identifier, const PropertySlot& slot, JSValue value, SiteCache site)
{
    UniquedStringImpl* uid = identifier.impl();
    if (isFixed(slot, identifier) || globalObject->structure()->isImmutableProperty(uid))
        return constantAnswer(value);

    if (instance.state->propertyCaches->watchedValues().tryWatchConstant(*instance.vm, globalObject, uid, value, site))
        return constantAnswer(value, globalObject);
    return answerInStorage(PropertyAnswerKind::Value, SlotAccess::Read, globalObject, slot.cachedOffset());
}

static PropertyAnswer classifyFreeNameRead(Instance& instance, JSGlobalObject* globalObject, const Identifier& identifier, const PropertySlot& slot, JSValue value, SiteCache site)
{
    if (!canCachePropertiesOf(globalObject) || slot.isTaintedByOpaqueObject())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    if (slot.slotBase() != globalObject)
        return classifyInheritedRead(globalObject, identifier.impl(), slot);
    if (slot.isCacheableValue())
        return classifyOwnValueRead(instance, globalObject, identifier, slot, value, site);
    if (slot.isCacheableGetter())
        return answerInStorage(PropertyAnswerKind::Accessor, SlotAccess::Read, globalObject, slot.cachedOffset());
    if (slot.isValue() && isConstantVariable(globalObject, identifier))
        return constantAnswer(value);
    if (slot.isCustom() && isFixed(slot, identifier) && slot.attributes() & PropertyAttribute::CustomValue)
        return constantAnswer(value);
    return PropertyAnswer::uncacheable(SlotAccess::Read);
}

// The lookup may have created the property, and a getter it found has run by now, so the Structure is asked again and no accessor is filed.
static PropertyAnswer classifyReadForwardedTo(JSGlobalObject* globalObject, UniquedStringImpl* uid, const PropertySlot& slot)
{
    if (!canCachePropertiesOf(globalObject) || slot.isTaintedByOpaqueObject())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    unsigned attributes = 0;
    PropertyOffset offset = globalObject->structure()->get(globalObject->vm(), uid, attributes);
    if (isValidOffset(offset)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        return answerInStorage(PropertyAnswerKind::Value, SlotAccess::Read, globalObject, offset);
    }
    if (slot.isUnset())
        return classifyAbsentRead(globalObject, uid);
    if (slot.slotBase() == globalObject || slot.isAccessor())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    return classifyInheritedRead(globalObject, uid, slot);
}

PropertyAnswer classifyGlobalProxyRead(const ReadQuery& query)
{
    PropertyAnswer answer = classifyReadForwardedTo(query.globalObject, query.uid, query.slot);
    answer.changeableObjects.append(query.globalObject);
    return answer;
}

ChainWalk walkChainThroughGlobalProxy(JSGlobalObject* globalObject, const Slot& slot, UniquedStringImpl* uid)
{
    ChainWalk walk;
    if (slot.structureID.decode()->realm() != globalObject)
        return walk;
    if (slot.kind == Slot::Kind::Value && slot.holder == globalObject)
        walk.result = ChainWalkResult::Found;
    else
        walk = walkAboveGlobalObject(globalObject, uid, ChainWalkMode::AllocatesNothing);
    walk.changeableObjects.append(globalObject);
    return walk;
}

static JSValue getFreeName(Instance* instance, uint32_t key, SiteCache site, bool isForTypeOf)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    const Identifier& identifier = identifierAt(instance, key);
    UniquedStringImpl* uid = identifier.impl();
    site = siteToFill(site);

    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
        JSValue value = lexicalSlot.getValue(globalObject, identifier);
        if (value == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            return { };
        }
        if (site)
            caches.cacheFreeName(site, uid, SlotAccess::Read, classifyLexicalRead(globalObject, uid, value));
        return value;
    }

    PropertySlot slot(globalObject, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, identifier, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (!isForTypeOf) {
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
            return { };
        }
        if (site)
            caches.cacheFreeName(site, uid, SlotAccess::Read, classifyAbsentRead(globalObject, uid));
        return jsUndefined();
    }

    // The answer is filed before a getter runs, so that a change the getter makes drops it.
    bool callsGetter = slot.isAccessor();
    JSValue value;
    if (!callsGetter) {
        value = slot.getValue(globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (site)
        caches.cacheFreeName(site, uid, SlotAccess::Read, classifyFreeNameRead(*instance, globalObject, identifier, slot, value, site));
    if (callsGetter)
        RELEASE_AND_RETURN(scope, slot.getValue(globalObject, identifier));
    return value;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFreeName, EncodedJSValue, (Instance* instance, uint32_t key, SiteCache cache))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetFreeName);
    OPERATION_RETURN(scope, JSValue::encode(getFreeName(instance, key, cache, false)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFreeNameForTypeOf, EncodedJSValue, (Instance* instance, uint32_t key, SiteCache cache))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetFreeNameForTypeOf);
    OPERATION_RETURN(scope, JSValue::encode(getFreeName(instance, key, cache, true)));
}

static PropertyAnswer classifyFreeNameStore(VM& vm, JSGlobalObject* globalObject, UniquedStringImpl* uid, const PutPropertySlot& slot)
{
    Structure* structure = globalObject->structure();
    if (!canCachePropertiesOf(globalObject) || structure->isImmutableProperty(uid))
        return PropertyAnswer::uncacheable(SlotAccess::Store);
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset))
        return PropertyAnswer::uncacheable(SlotAccess::Store);
    if (attributes & PropertyAttribute::Accessor)
        return answerInStorage(PropertyAnswerKind::Accessor, SlotAccess::Store, globalObject, offset);
    if (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
        return PropertyAnswer::uncacheable(SlotAccess::Store);
    if (!slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || slot.base() != globalObject || slot.cachedOffset() != offset)
        return uncacheableOnce(SlotAccess::Store);
    return answerInStorage(PropertyAnswerKind::Value, SlotAccess::Store, globalObject, offset);
}

static void cacheFreeNameStore(Instance& instance, UniquedStringImpl* uid, SiteCache site, const PutPropertySlot& slot)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    PropertyCaches& caches = *instance.state->propertyCaches;
    PropertyAnswer answer = classifyFreeNameStore(vm, globalObject, uid, slot);
    if (answer.kind == PropertyAnswerKind::Value) {
        // The stub replaces the value without firing the replacement watchpoints of a later Structure, so the name is never a watched Constant again.
        caches.watchedValues().didCacheStore(uid);
        Structure* structure = globalObject->structure();
        if (structure->propertyAccessesAreCacheable())
            structure->didCachePropertyReplacement(vm, answer.offset);
    }
    caches.cacheFreeName(site, uid, SlotAccess::Store, answer);
}

template<bool isStrict>
static void setFreeName(Instance* instance, uint32_t key, SiteCache site, JSValue value)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = identifierAt(instance, key);

    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
        if (lexicalSlot.getValue(globalObject, identifier) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            return;
        }
        PutPropertySlot slot(lexical, true);
        scope.release();
        JSGlobalLexicalEnvironment::put(lexical, globalObject, identifier, value, slot);
        return;
    }
    if constexpr (isStrict) {
        bool exists = globalObject->hasProperty(globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, void());
        if (!exists) {
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
            return;
        }
    }

    Structure* structureBefore = globalObject->structure();
    unsigned lexicalBindingEpochBefore = globalObject->globalLexicalBindingEpoch();
    PutPropertySlot slot(globalObject, isStrict);
    globalObject->methodTable()->put(globalObject, globalObject, identifier, value, slot);
    RETURN_IF_EXCEPTION(scope, void());

    site = siteToFill(site);
    if (!site || globalObject->structure() != structureBefore || globalObject->globalLexicalBindingEpoch() != lexicalBindingEpochBefore)
        return;
    cacheFreeNameStore(*instance, identifier.impl(), site, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFreeName, void, (Instance* instance, uint32_t key, SiteCache cache, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetFreeName);
    setFreeName<true>(instance, key, cache, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFreeNameSloppy, void, (Instance* instance, uint32_t key, SiteCache cache, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetFreeNameSloppy);
    setFreeName<false>(instance, key, cache, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteFreeName, size_t, (Instance* instance, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteFreeName);
    const Identifier& identifier = identifierAt(instance, key);
    if (globalObject->globalLexicalEnvironment()->symbolTable()->contains(identifier.impl()))
        OPERATION_RETURN(scope, false);
    OPERATION_RETURN(scope, JSCell::deleteProperty(globalObject, globalObject, identifier));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
