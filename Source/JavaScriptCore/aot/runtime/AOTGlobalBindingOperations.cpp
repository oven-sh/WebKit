/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSGlobalProxy.h"
#include "SymbolTableInlines.h"
#include <bit>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

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

static bool isConstantVariable(const SymbolTableEntry::Fast& entry, const Identifier& identifier)
{
    return !entry.isNull() && (entry.isReadOnly() || identifier.isPrivateName());
}

static ChainWalk walkAboveGlobalObject(JSGlobalObject* globalObject, UniquedStringImpl* uid, ChainWalkMode mode = ChainWalkMode::MayAllocate)
{
    return walkChain({ globalObject, globalObject->structure(), globalObject, mode }, uid);
}

static ALWAYS_INLINE bool putGlobalName(JSGlobalObject* globalObject, const Identifier& identifier, JSValue value, PutPropertySlot& slot, const Invocable<void()> auto& didNotFile)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    if (SymbolTableEntry::Fast entry = lexical->symbolTable()->get(identifier.impl()); !entry.isNull()) {
        didNotFile();
        if (!lexical->variableAt(entry.scopeOffset()).get()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            return false;
        }
        PutPropertySlot lexicalSlot(lexical, true);
        scope.release();
        JSGlobalLexicalEnvironment::put(lexical, globalObject, identifier, value, lexicalSlot);
        return false;
    }
    if (slot.isStrictMode()) {
        bool exists = globalObject->hasProperty(globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, false);
        if (!exists) {
            didNotFile();
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
            return false;
        }
    }

    Structure* structureBefore = globalObject->structure();
    unsigned lexicalBindingEpochBefore = globalObject->globalLexicalBindingEpoch();
    globalObject->methodTable()->put(globalObject, globalObject, identifier, value, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (globalObject->structure() == structureBefore && globalObject->globalLexicalBindingEpoch() == lexicalBindingEpochBefore)
        return true;
    didNotFile();
    return false;
}

static SiteCache siteToFill(SiteCache site)
{
    return site && site->get().kind != Slot::Kind::Uncacheable ? site : nullptr;
}

static PropertyAnswer constantAnswer(JSValue value, JSGlobalObject* changeableGlobalObject = nullptr)
{
    PropertyAnswer answer { .kind = PropertyAnswerKind::Constant, .constant = value };
    if (changeableGlobalObject)
        answer.changeableObjects.append(changeableGlobalObject);
    return answer;
}

static PropertyAnswer answerInStorage(PropertyAnswerKind kind, SlotAccess access, JSObject* holder, PropertyOffset offset)
{
    return { .kind = kind, .access = access, .offset = offset, .holder = holder };
}

static PropertyAnswer classifyAbsentRead(JSGlobalObject* globalObject, UniquedStringImpl* uid)
{
    if (!canCachePropertiesOf(globalObject))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    ChainWalk walk = walkAboveGlobalObject(globalObject, uid);
    if (walk.result != ChainWalkResult::Absent)
        return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    return { .kind = PropertyAnswerKind::Absent, .changeableObjects = WTF::move(walk.changeableObjects) };
}

static PropertyAnswer classifyInheritedRead(JSGlobalObject* globalObject, UniquedStringImpl* uid, const PropertySlot& slot)
{
    ChainWalk walk = walkAboveGlobalObject(globalObject, uid);
    bool isValue = slot.isCacheableValue() && !(walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue);
    bool isGetter = slot.isCacheableGetter() && walk.attributes & PropertyAttribute::Accessor;
    if (walk.result != ChainWalkResult::Found || walk.holder != slot.slotBase() || (!isValue && !isGetter))
        return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    return PropertyAnswer::above(isValue ? PropertyAnswerKind::Value : PropertyAnswerKind::Accessor, SlotAccess::Read, WTF::move(walk));
}

static PropertyAnswer classifyFreeNameRead(Instance& instance, const Identifier& identifier, const PropertySlot& slot, JSValue value, SiteCache site)
{
    JSGlobalObject* globalObject = instance.globalObject;
    UniquedStringImpl* uid = identifier.impl();
    if (!canCachePropertiesOf(globalObject) || slot.isTaintedByOpaqueObject())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    if (slot.slotBase() != globalObject)
        return classifyInheritedRead(globalObject, uid, slot);
    if (slot.isCacheableValue()) {
        if (isFixed(slot, identifier) || globalObject->structure()->isImmutablePropertyForReader(uid))
            return constantAnswer(value);
        if (instance.state->propertyCaches->watchedValues().tryWatchConstant(*instance.vm, globalObject, uid, value, site))
            return constantAnswer(value, globalObject);
        return answerInStorage(PropertyAnswerKind::Value, SlotAccess::Read, globalObject, slot.cachedOffset());
    }
    if (slot.isCacheableGetter())
        return answerInStorage(PropertyAnswerKind::Accessor, SlotAccess::Read, globalObject, slot.cachedOffset());
    if (slot.isValue() && isConstantVariable(globalObject->symbolTable()->get(uid), identifier))
        return constantAnswer(value);
    if (slot.isCustom() && isFixed(slot, identifier) && slot.attributes() & PropertyAttribute::CustomValue)
        return constantAnswer(value);
    return PropertyAnswer::uncacheable(SlotAccess::Read);
}

PropertyAnswer classifyGlobalProxyRead(const ReadQuery& query)
{
    JSGlobalObject* globalObject = query.globalObject;
    auto classify = [&] {
        if (!canCachePropertiesOf(globalObject) || query.slot.isTaintedByOpaqueObject())
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        unsigned attributes = 0;
        PropertyOffset offset = globalObject->structure()->get(globalObject->vm(), query.uid, attributes);
        if (isValidOffset(offset))
            return attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue ? PropertyAnswer::uncacheable(SlotAccess::Read) : answerInStorage(PropertyAnswerKind::Value, SlotAccess::Read, globalObject, offset);
        if (query.slot.isUnset())
            return classifyAbsentRead(globalObject, query.uid);
        if (query.slot.slotBase() == globalObject || query.slot.isAccessor())
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        return classifyInheritedRead(globalObject, query.uid, query.slot);
    };
    PropertyAnswer answer = classify();
    answer.changeableObjects.append(globalObject);
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
    if (SymbolTableEntry::Fast entry = lexical->symbolTable()->get(uid); !entry.isNull()) {
        JSValue value = lexical->variableAt(entry.scopeOffset()).get();
        if (!value)
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
        else if (site)
            caches.cacheFreeName(site, uid, SlotAccess::Read, entry.isReadOnly() ? constantAnswer(value, globalObject) : PropertyAnswer::uncacheable(SlotAccess::Read));
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
        caches.cacheFreeName(site, uid, SlotAccess::Read, classifyFreeNameRead(*instance, identifier, slot, value, site));
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
        return PropertyAnswer::uncacheable(SlotAccess::Store, true);
    return answerInStorage(PropertyAnswerKind::Value, SlotAccess::Store, globalObject, offset);
}

static ALWAYS_INLINE void setFreeName(Instance* instance, uint32_t key, SiteCache site, JSValue value, bool isStrict)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = identifierAt(instance, key);
    PutPropertySlot slot(globalObject, isStrict);
    bool mayBeFiled = putGlobalName(globalObject, identifier, value, slot, [] { });
    RETURN_IF_EXCEPTION(scope, void());
    site = siteToFill(site);
    if (!mayBeFiled || !site)
        return;

    UniquedStringImpl* uid = identifier.impl();
    PropertyCaches& caches = *instance->state->propertyCaches;
    PropertyAnswer answer = classifyFreeNameStore(vm, globalObject, uid, slot);
    if (answer.kind == PropertyAnswerKind::Value) {
        caches.watchedValues().didCacheStore(uid);
        Structure* structure = globalObject->structure();
        if (structure->propertyAccessesAreCacheable())
            structure->didCachePropertyReplacement(vm, answer.offset);
    }
    caches.cacheFreeName(site, uid, SlotAccess::Store, answer);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFreeName, void, (Instance* instance, uint32_t key, SiteCache cache, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetFreeName);
    setFreeName(instance, key, cache, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFreeNameSloppy, void, (Instance* instance, uint32_t key, SiteCache cache, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetFreeNameSloppy);
    setFreeName(instance, key, cache, JSValue::decode(value), false);
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

namespace {

struct BindingAccess {
    UniquedStringImpl* uid() const { return name.impl(); }
    void didNotFile() const { count(instance, RuntimeCounterID::GlobalBindingNotFiled); }

    Instance& instance;
    VM& vm;
    JSGlobalObject* globalObject;
    GlobalBindings& bindings;
    uint32_t number;
    Identifier name;
};

} // anonymous namespace

static ALWAYS_INLINE BindingAccess bindingAccess(Instance* instance, uint32_t number)
{
    GlobalBindings& bindings = *instance->state->globalBindings;
    return { *instance, *instance->vm, instance->globalObject, bindings, number, bindings.nameOf(number) };
}

static bool structureSaysWherePropertiesAre(const BindingAccess& access)
{
    Structure* structure = access.globalObject->structure();
    if (structure->propertyAccessesAreCacheable())
        return true;
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore())
        access.globalObject->flattenDictionaryObject(access.vm);
    return false;
}

static int64_t locationOfVariable(JSSegmentedVariableObject* scope, const SymbolTableEntry::Fast& entry)
{
    return std::bit_cast<int64_t>(scope->variableAt(entry.scopeOffset()).slot());
}

static int64_t locationOfProperty(JSGlobalObject* globalObject, PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return std::bit_cast<int64_t>(globalObject->locationForOffset(offset));
    return offsetInButterfly(offset) * static_cast<int64_t>(sizeof(EncodedJSValue));
}

static EncodedJSValue* filedLocationOrNull(Instance* instance, uint32_t number, bool seesScriptBindings)
{
    const GlobalBinding& record = instance->state->globalBindings->at(number);
    JSGlobalObject* globalObject = instance->globalObject;
    if (record.structureID != globalObject->structureID().bits() || (seesScriptBindings && record.lexicalBindingEpoch != globalObject->globalLexicalBindingEpoch()))
        return nullptr;
    int64_t base = record.location < 0 ? std::bit_cast<int64_t>(globalObject->butterfly()) : 0;
    return std::bit_cast<EncodedJSValue*>(base + record.location);
}

static bool tryStoreAtFiledLocation(Instance* instance, uint32_t number, bool seesScriptBindings, JSValue value)
{
    EncodedJSValue* location = filedLocationOrNull(instance, number, seesScriptBindings);
    if (!location)
        return false;
    *location = JSValue::encode(value);
    instance->vm->writeBarrier(instance->globalObject, value);
    return true;
}

static bool tryFileRead(const BindingAccess& access, const PropertySlot& slot, JSValue value)
{
    JSGlobalObject* globalObject = access.globalObject;
    if (!canCachePropertiesOf(globalObject) || slot.isTaintedByOpaqueObject() || slot.slotBase() != globalObject || !value || !slot.isValue())
        return false;
    int64_t location;
    const bool filesConstants = !access.bindings.filesLocationAlone(access.number);
    if (slot.isCacheableValue()) {
        if (filesConstants && (isFixed(slot, access.name) || globalObject->structure()->isImmutablePropertyForReader(access.uid())))
            return access.bindings.fileConstant(access.vm, access.number, nullptr, value);
        if (filesConstants && access.bindings.fileConstant(access.vm, access.number, globalObject, value))
            return true;
        if (!structureSaysWherePropertiesAre(access))
            return false;
        location = locationOfProperty(globalObject, slot.cachedOffset());
    } else {
        SymbolTableEntry::Fast entry = globalObject->symbolTable()->get(access.uid());
        if (entry.isNull())
            return false;
        if (filesConstants && isConstantVariable(entry, access.name))
            return access.bindings.fileConstant(access.vm, access.number, nullptr, value);
        location = locationOfVariable(globalObject, entry);
    }
    access.bindings.fileLocation(access.number, globalObject->structure(), location);
    return true;
}

static bool tryFileAbsence(const BindingAccess& access)
{
    if (!canCachePropertiesOf(access.globalObject))
        return false;
    ChainWalk walk = walkAboveGlobalObject(access.globalObject, access.uid());
    if (walk.result != ChainWalkResult::Absent)
        return false;
    access.bindings.fileAbsence(access.vm, access.number, walk.changeableObjects.span());
    return true;
}

static ALWAYS_INLINE JSValue getGlobalBinding(Instance* instance, uint32_t number, bool seesScriptBindings)
{
    if (EncodedJSValue* location = filedLocationOrNull(instance, number, seesScriptBindings)) {
        if (JSValue value = JSValue::decode(*location))
            return value;
    }
    BindingAccess access = bindingAccess(instance, number);
    ASSERT(access.bindings.seesScriptBindings(number) == seesScriptBindings);
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    const Identifier& identifier = access.name;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* receiver = globalObject;
    if (seesScriptBindings) {
        JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
        if (SymbolTableEntry::Fast entry = lexical->symbolTable()->get(access.uid()); !entry.isNull()) {
            JSValue value = lexical->variableAt(entry.scopeOffset()).get();
            if (!value)
                throwException(globalObject, scope, createTDZError(globalObject, identifier));
            else if (entry.isReadOnly() && vm.globalConstRedeclarationShouldThrow() && !vm.allowRedeclaringSymbols() && !access.bindings.filesLocationAlone(number))
                access.bindings.fileConstant(vm, number, nullptr, value);
            else
                access.bindings.fileLocation(number, globalObject->structure(), locationOfVariable(lexical, entry));
            return value;
        }
    } else {
        receiver = globalObject->globalThis();
        bool forwardsToGlobalObject = receiver == globalObject || (receiver->classInfo() == JSGlobalProxy::info() && uncheckedDowncast<JSGlobalProxy>(receiver)->target() == globalObject);
        if (!forwardsToGlobalObject) [[unlikely]] {
            access.didNotFile();
            RELEASE_AND_RETURN(scope, JSValue(receiver).get(globalObject, identifier));
        }
    }

    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, identifier, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (access.bindings.answersUndefinedWhereAbsent(number)) {
            if (!tryFileAbsence(access))
                access.didNotFile();
            return jsUndefined();
        }
        access.didNotFile();
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
        return { };
    }

    if (slot.isAccessor()) {
        access.didNotFile();
        RELEASE_AND_RETURN(scope, slot.getValue(globalObject, identifier));
    }
    JSValue value = slot.getValue(globalObject, identifier);
    RETURN_IF_EXCEPTION(scope, { });
    if (!tryFileRead(access, slot, value))
        access.didNotFile();
    return value;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetGlobalBinding, EncodedJSValue, (Instance* instance, uint32_t binding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetGlobalBinding);
    OPERATION_RETURN(scope, JSValue::encode(getGlobalBinding(instance, binding, true)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetGlobalObjectBinding, EncodedJSValue, (Instance* instance, uint32_t binding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetGlobalObjectBinding);
    OPERATION_RETURN(scope, JSValue::encode(getGlobalBinding(instance, binding, false)));
}

static bool tryFileStore(const BindingAccess& access, const PutPropertySlot& slot)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    if (!canCachePropertiesOf(globalObject) || globalObject->structure()->isImmutableProperty(access.uid()) || !structureSaysWherePropertiesAre(access))
        return false;
    Structure* structure = globalObject->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, access.uid(), attributes);
    bool isWritableDataProperty = isValidOffset(offset) && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
    if (!isWritableDataProperty || !slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || slot.base() != globalObject || slot.cachedOffset() != offset)
        return false;
    structure->didCachePropertyReplacement(vm, offset);
    access.bindings.didFileStore(access.number);
    access.bindings.fileLocation(access.number, structure, locationOfProperty(globalObject, offset));
    return true;
}

static ALWAYS_INLINE void setGlobalBinding(Instance* instance, uint32_t number, JSValue value, bool isStrict)
{
    if (tryStoreAtFiledLocation(instance, number, true, value))
        return;
    BindingAccess access = bindingAccess(instance, number);
    ASSERT(access.bindings.seesScriptBindings(number));
    auto scope = DECLARE_THROW_SCOPE(access.vm);
    PutPropertySlot slot(access.globalObject, isStrict);
    bool mayBeFiled = putGlobalName(access.globalObject, access.name, value, slot, [&] { access.didNotFile(); });
    RETURN_IF_EXCEPTION(scope, void());
    if (mayBeFiled && !tryFileStore(access, slot))
        access.didNotFile();
}

static ALWAYS_INLINE void setGlobalObjectBinding(Instance* instance, uint32_t number, JSValue value, bool isStrict)
{
    if (tryStoreAtFiledLocation(instance, number, false, value))
        return;
    BindingAccess access = bindingAccess(instance, number);
    ASSERT(!access.bindings.seesScriptBindings(number));
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(access.vm);

    Structure* structureBefore = globalObject->structure();
    JSObject* receiver = globalObject->globalThis();
    PutPropertySlot slot(receiver, isStrict);
    receiver->methodTable()->put(receiver, globalObject, access.name, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    if (globalObject->structure() != structureBefore || !tryFileStore(access, slot))
        access.didNotFile();
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalBinding);
    setGlobalBinding(instance, binding, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalBindingSloppy, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalBindingSloppy);
    setGlobalBinding(instance, binding, JSValue::decode(value), false);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalObjectBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalObjectBinding);
    setGlobalObjectBinding(instance, binding, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalObjectBindingSloppy, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalObjectBindingSloppy);
    setGlobalObjectBinding(instance, binding, JSValue::decode(value), false);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTValidateGlobalBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    JSGlobalObject* globalObject = instance->globalObject;
    GlobalBindings& bindings = *instance->state->globalBindings;
    Identifier identifier = bindings.nameOf(binding);
    JSValue answered = JSValue::decode(value);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    SymbolTableEntry::Fast entry = bindings.seesScriptBindings(binding) ? lexical->symbolTable()->get(identifier.impl()) : SymbolTableEntry::Fast();
    JSValue found;
    PropertySlot own(globalObject, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!entry.isNull())
        found = lexical->variableAt(entry.scopeOffset()).get();
    else if (globalObject->methodTable()->getOwnPropertySlot(globalObject, globalObject, identifier, own) && own.isValue())
        found = own.getValue(globalObject, identifier);
    if (found == answered)
        return;
    PropertySlot inherited(globalObject, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!found && answered.isUndefined() && bindings.answersUndefinedWhereAbsent(binding) && !globalObject->getPropertySlot(globalObject, identifier, inherited) && !inherited.isTaintedByOpaqueObject())
        return;
    dataLogLn("AOT: the record of the global name ", identifier.string(), " answers with another value than a lookup finds");
    crashOnValidationFailure(vm, { ValidationFailureKind::LookupDiffers, { }, { }, answered });
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(GlobalBindings);

GlobalBindings::GlobalBindings(State& state)
    : m_state(state)
    , m_records(unsafeMakeSpan(state.instance.runtime.counters.globalBindings, state.tables.globalBindings().size()))
    , m_names(state.tables.globalBindings())
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
{
    RELEASE_ASSERT(m_records.data());
    state.caches->add(CacheKind::WeakByKey, "GlobalBindings"_s, *this);
}

Identifier GlobalBindings::nameOf(uint32_t number) const
{
    RELEASE_ASSERT(number < m_names.size());
    return identifiersOf(m_state.instance).identifier(identifierNumberOf(number));
}

bool GlobalBindings::fileConstant(VM& vm, uint32_t number, JSObject* holderToWatch, JSValue value)
{
    RELEASE_ASSERT(value);
    RELEASE_ASSERT_WITH_MESSAGE(!m_isStoredThrough.get(number), "A store through a global binding has a record of its own.");
    if (!holderToWatch)
        m_watches.remove(number);
    else {
        // A free-name site with a cached store compares no Structure and fires no watchpoint, and each later Structure of the holder starts with a valid replacement set.
        if (m_names[number] & isStoredByNameFlag || m_identifiersStoredThrough.contains(identifierNumberOf(number)) || !holderToWatch->structure()->propertyAccessesAreCacheable())
            return false;
        auto isValue = [&](JSValue held) { return held == value; };
        auto watch = PropertyValueWatchpoint::tryWatch(vm, holderToWatch, nameOf(number).impl(), isValue, [](PropertyValueWatchpoint& fired) {
            auto& bindings = fired.owner<GlobalBindings>();
            bindings.at(fired.number()).constant = 0;
            bindings.m_writtenSinceCollection.recordAll();
        }, this, number);
        if (!watch)
            return false;
        m_watches.set(number, WTF::move(watch));
    }
    // The global object or its lexical environment holds the value for as long as it stands here, so the record is no root.
    setConstant(number, value, false);
    return true;
}

void GlobalBindings::setConstant(uint32_t number, JSValue value, bool isAbsence)
{
    m_isFiledAsAbsent.set(number, isAbsence);
    at(number).constant = JSValue::encode(value);
    if (!m_isListedAsConstant.set(number))
        m_filedConstants.append(number);
}

void GlobalBindings::fileAbsence(VM& vm, uint32_t number, std::span<JSObject* const> objectsAboveGlobalObject)
{
    RELEASE_ASSERT(answersUndefinedWhereAbsent(number));
    ChainObjects objects;
    objects.append(m_state.instance.globalObject);
    objects.append(objectsAboveGlobalObject);
    // Only an object that may be a prototype reports its changes. The global object becomes one here, which costs it a Structure.
    for (JSObject* object : objects) {
        if (object->mayBePrototype()) [[likely]]
            continue;
        DeferGCForAWhile deferGC(vm);
        object->didBecomePrototype(vm);
    }
    m_state.chainDependents->add(objects.span(), { nameOf(number).impl(), number, m_chainDependentOwner });
    m_watches.remove(number);
    setConstant(number, jsUndefined(), true);
    // A read that looks at the location alone finds the answer in the record: any new property changes the Structure, and a change above the global object empties the word.
    if (filesLocationAlone(number))
        fileLocation(number, m_state.instance.globalObject->structure(), std::bit_cast<int64_t>(&at(number).constant));
}

void GlobalBindings::dropChainDependent(VM&, uint32_t number, UniquedStringImpl*)
{
    if (m_isFiledAsAbsent.clear(number))
        at(number).constant = 0;
}

void GlobalBindings::fileLocation(uint32_t number, Structure* structure, int64_t location)
{
    RELEASE_ASSERT(location);
    GlobalBinding& record = at(number);
    if (!record.structureID)
        m_filedLocations.append(number);
    record.structureID = structure->id().bits();
    record.lexicalBindingEpoch = m_state.instance.globalObject->globalLexicalBindingEpoch();
    record.location = location;
    m_writtenSinceCollection.recordAll();
}

void GlobalBindings::didFileStore(uint32_t number)
{
    RELEASE_ASSERT(!at(number).constant);
    m_isStoredThrough.set(number);
    m_identifiersStoredThrough.add(identifierNumberOf(number));
}

void GlobalBindings::dropConstants()
{
    for (uint32_t number : m_filedConstants) {
        at(number).constant = 0;
        m_isListedAsConstant.quickClear(number);
        m_isFiledAsAbsent.clear(number);
    }
    m_filedConstants.shrink(0);
}

static void forgetLocation(GlobalBinding& record)
{
    record.structureID = 0;
    record.lexicalBindingEpoch = 0;
    record.location = 0;
}

void GlobalBindings::dropLocations()
{
    for (uint32_t number : m_filedLocations)
        forgetLocation(at(number));
    m_filedLocations.shrink(0);
}

// The number of a dead Structure is given to a later one, which the global object may get.
size_t GlobalBindings::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRemovedEntries = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        numberOfRemovedEntries += m_filedLocations.removeAllMatching([&](uint32_t number) {
            GlobalBinding& record = at(number);
            if (vm.heap.isMarked(std::bit_cast<StructureID>(record.structureID).decode()))
                return false;
            forgetLocation(record);
            return true;
        });
        m_watches.removeIf([&](auto& entry) {
            numberOfRemovedEntries += entry.value->hasFired();
            return entry.value->hasFired();
        });
    });
    return numberOfRemovedEntries;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
