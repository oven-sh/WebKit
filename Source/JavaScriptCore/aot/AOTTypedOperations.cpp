/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInlineCaches.h"
#include "AOTLockedOperation.h"
#include "AOTModuleEvaluation.h"
#include "AOTOperationHelpers.h"
#include "AOTOperationsObjects.h"
#include "AOTRuntime.h"
#include "AOTSlotWatchpoint.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "AOTTypedType.h"
#include "ArrayPrototype.h"
#include "AbstractModuleRecord.h"
#include "AsyncContextSwapScope.h"
#include "BuiltinNames.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSModuleRecord.h"
#include "ClonedArguments.h"
#include "Completion.h"
#include "DirectEvalExecutable.h"
#include "ErrorConstructor.h"
#include "Interpreter.h"
#include "DeferTermination.h"
#include "DirectArguments.h"
#include "GeneratorPrototype.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSIteratorPrototype.h"
#include "CustomGetterSetter.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCellButterfly.h"
#include "JSONObject.h"
#include "MarkedBlockCage.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSMapInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleRegistryEntry.h"
#include "JSTypedArrays.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "JSGlobalObjectFunctions.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPropertyConditionSet.h"
#include "ObjectPrototype.h"
#include "ProxyObject.h"
#include "Operations.h"
#include "TopExceptionScope.h"
#include "TypeofType.h"
#include "VMInlines.h"
#include "VMTrapsInlines.h"
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>
#include <wtf/StackPointer.h>

namespace JSC { namespace AOT {

using Typed::State;

static ALWAYS_INLINE const Identifier& nameAt(Instance* instance, uint32_t index)
{
    return instance->typed->name(*instance->vm, index);
}

static ALWAYS_INLINE std::optional<uint32_t> indexNamedBy(Instance* instance, uint32_t number, const Identifier& name)
{
    if (!(instance->typed->data.stringFacts(number) & Typed::Data::stringStartsWithDigit)) [[likely]]
        return std::nullopt;
    return parseIndex(name);
}

static void fill(MarkedArgumentBuffer& buffer, uint32_t count, const EncodedJSValue* values)
{
    for (uint32_t i = 0; i < count; ++i)
        buffer.append(JSValue::decode(values[i]));
}

static void fillGlobalPropertyCache(VM& vm, Instance* instance, JSGlobalObject* globalObject, uint32_t cacheNumber, int64_t location, JSValue value = { })
{
    Structure* structure = globalObject->structure();
    instance->didFillGlobalPropertyCache();
    if (value) {
        instance->typed->globalPropertyCaches()[cacheNumber - 1] = { structure->id().bits(), globalObject->globalLexicalBindingEpoch(), 0, JSValue::encode(value) };
        instance->didStoreRootInStatics(instance->typed->data.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(Typed::GlobalPropertyCache) + OBJECT_OFFSETOF(Typed::GlobalPropertyCache, value));
        return;
    }
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore()) {
        globalObject->flattenDictionaryObject(vm);
        return;
    }
    if (!structure->propertyAccessesAreCacheable())
        return;
    Typed::GlobalPropertyCache& cache = instance->typed->globalPropertyCaches()[cacheNumber - 1];
    cache = { structure->id().bits(), globalObject->globalLexicalBindingEpoch(), location, JSValue::encode(JSValue()) };
}

static int64_t locationInButterfly(PropertyOffset offset)
{
    return offsetInButterfly(offset) * static_cast<int64_t>(sizeof(EncodedJSValue));
}

struct ModuleScopeVariable {
    JSLexicalEnvironment* environment { nullptr };
    WriteBarrierBase<Unknown>* variable { nullptr };
};

static ModuleScopeVariable findInModuleScope(Instance* instance, JSGlobalLexicalEnvironment* lexical, const Identifier& name)
{
    for (JSScope* scope = instance->loader()->moduleScope(); scope != lexical; scope = scope->next()) {
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scope);
        if (auto entry = environment->symbolTable()->get(name.impl()); !entry.isNull())
            return { environment, &environment->variableAt(entry.scopeOffset()) };
    }
    return { };
}

static constexpr ASCIILiteral graphVariablePrefix = "graph "_s;

static ALWAYS_INLINE const Identifier& freeNameAt(Instance* instance, VM& vm, uint32_t index, Identifier& withoutPrefix)
{
    const Identifier& written = nameAt(instance, index);
    if (!instance->typed->data.mayRunInMultipleInstances || !written.string().startsWith(graphVariablePrefix)) [[likely]]
        return written;
    withoutPrefix = Identifier::fromString(vm, written.string().string().substring(graphVariablePrefix.length()));
    return withoutPrefix;
}

enum class FreeVariableAccess : uint8_t { Read, TypeOf, GlobalThisProperty };

static JSValue getFreeVariable(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t nameIndex, uint32_t cacheNumber, FreeVariableAccess how)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (cacheNumber) {
        auto& cache = instance->typed->globalPropertyCaches()[cacheNumber - 1];
        if (cache.value)
            return JSValue::decode(cache.value);
        if (cache.location && cache.structureID == globalObject->structureID().bits() && cache.globalLexicalBindingEpoch == globalObject->globalLexicalBindingEpoch())
            return JSValue::decode(*std::bit_cast<EncodedJSValue*>((cache.location < 0 ? std::bit_cast<int64_t>(globalObject->butterfly()) : 0) + cache.location));
    }
    Identifier withoutPrefix;
    const Identifier& name = freeNameAt(instance, vm, nameIndex, withoutPrefix);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    if (how != FreeVariableAccess::GlobalThisProperty) {
        if (auto found = findInModuleScope(instance, lexical, name); found.variable) {
            if (cacheNumber)
                fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, std::bit_cast<int64_t>(found.variable->slot()));
            return found.variable->get();
        }
    }
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (how != FreeVariableAccess::GlobalThisProperty && JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, name, lexicalSlot)) {
        JSValue value = lexicalSlot.getValue(globalObject, name);
        if (value == jsTDZValue())
            throwException(globalObject, scope, createTDZError(globalObject, name));
        else if (auto entry = lexical->symbolTable()->get(name.impl()); cacheNumber && !entry.isNull())
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, std::bit_cast<int64_t>(lexical->variableAt(entry.scopeOffset()).slot()), entry.isReadOnly() ? value : JSValue());
        return value;
    }
    PropertySlot slot(how == FreeVariableAccess::GlobalThisProperty ? globalObject->globalThis() : globalObject, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (how == FreeVariableAccess::Read)
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, name));
        return jsUndefined();
    }
    JSValue value = slot.getValue(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!cacheNumber || slot.slotBase() != globalObject)
        return value;
    constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    bool isEnginePrivateName = instance->typed->data.stringFacts(nameIndex) & Typed::Data::stringIsEnginePrivateName;
    bool isFixed = (slot.attributes() & fixed) == fixed || isEnginePrivateName;
    if (slot.isCacheableValue()) {
        if (isFixed || instance->typed->trustGlobalProperty(vm, globalObject, cacheNumber, name.impl(), value))
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, 0, value);
        else if (isOutOfLineOffset(slot.cachedOffset()))
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, locationInButterfly(slot.cachedOffset()));
    } else if (slot.isValue()) {
        if (auto entry = globalObject->symbolTable()->get(name.impl()); !entry.isNull())
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, std::bit_cast<int64_t>(globalObject->variableAt(entry.scopeOffset()).slot()), entry.isReadOnly() || isEnginePrivateName ? value : JSValue());
    } else if (slot.isCustom() && isFixed && slot.attributes() & PropertyAttribute::CustomValue)
        fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, 0, value);
    return value;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFreeVariable, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFreeVariable(instance, globalObject, vm, name, cacheNumber, FreeVariableAccess::Read)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedValidateGlobalPermanently, void, (Instance* instance, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue current = getFreeVariable(instance, globalObject, vm, name, 0, FreeVariableAccess::Read);
    RELEASE_ASSERT_WITH_MESSAGE(!scope.exception() && current == JSValue::decode(value), "AOT: a free name that was taken to have one value for good has another");
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFreeVariableForTypeOf, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFreeVariable(instance, globalObject, vm, name, cacheNumber, FreeVariableAccess::TypeOf)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetGlobalThisProperty, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFreeVariable(instance, globalObject, vm, name, cacheNumber, FreeVariableAccess::GlobalThisProperty)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLookUpGlobalMember, void, (Instance* instance, EncodedJSValue encodedHolder, uint32_t global, uint32_t name, uint32_t cacheNumber))
{
    static constexpr uint32_t maxAttempts = 8;
    JSValue holder = JSValue::decode(encodedHolder);
    if (holder.isObject() && instance->typed->trustGlobalMember(*instance->vm, instance->globalObject, cacheNumber, nameAt(instance, global).impl(), asObject(holder), nameAt(instance, name).impl()))
        return;
    Typed::GlobalPropertyCache& cache = instance->typed->globalPropertyCaches()[cacheNumber - 1];
    if (++cache.globalLexicalBindingEpoch == maxAttempts)
        cache.location = 1;
}

template<bool isStrict>
static ALWAYS_INLINE OperationReturnType<void> setFreeVariable(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t name, uint32_t cacheNumber, EncodedJSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier withoutPrefix;
    const Identifier& identifier = freeNameAt(instance, vm, name, withoutPrefix);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    if (auto found = findInModuleScope(instance, lexical, identifier); found.variable) {
        found.variable->set(vm, found.environment, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
        if (lexicalSlot.getValue(globalObject, identifier) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            OPERATION_RETURN(scope);
        }
        PutPropertySlot slot(lexical, true);
        JSGlobalLexicalEnvironment::put(lexical, globalObject, identifier, JSValue::decode(value), slot);
        OPERATION_RETURN(scope);
    }
    bool exists = !isStrict || globalObject->hasProperty(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!exists) {
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
        OPERATION_RETURN(scope);
    }
    Structure* structure = globalObject->structure();
    PutPropertySlot slot(globalObject, isStrict);
    globalObject->methodTable()->put(globalObject, globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (cacheNumber && slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == globalObject && globalObject->structure() == structure && isOutOfLineOffset(slot.cachedOffset())) {
        if (structure->propertyAccessesAreCacheable())
            structure->didCachePropertyReplacement(vm, slot.cachedOffset());
        fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, locationInButterfly(slot.cachedOffset()));
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetFreeVariable, void, (Instance* instance, uint32_t name, uint32_t cacheNumber, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    return setFreeVariable<true>(instance, globalObject, vm, name, cacheNumber, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetFreeVariableSloppy, void, (Instance* instance, uint32_t name, uint32_t cacheNumber, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    return setFreeVariable<false>(instance, globalObject, vm, name, cacheNumber, value);
}

using PrivateElement = Typed::Object::PrivateElement;

static JSValue privateGet(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PrivateElement element = Typed::Object::findPrivateElement(vm, object, name);
    switch (element.kind) {
    case PrivateElement::Kind::None:
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return { };
    case PrivateElement::Kind::Field:
    case PrivateElement::Kind::Method:
        return element.value;
    case PrivateElement::Kind::Accessor:
        if (!element.getter) {
            throwTypeError(globalObject, scope, "Trying to access an undefined private getter"_s);
            return { };
        }
        RELEASE_AND_RETURN(scope, call(globalObject, element.getter, JSC::getCallData(element.getter), base, ArgList()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void privateSet(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PrivateElement element = Typed::Object::findPrivateElement(vm, object, name);
    switch (element.kind) {
    case PrivateElement::Kind::None:
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return;
    case PrivateElement::Kind::Field: {
        PutPropertySlot slot(object, true);
        scope.release();
        if (dynamicDowncast<Typed::Object>(object))
            Typed::Object::put(object, globalObject, name, value, slot);
        else
            object->setPrivateField(globalObject, name, value, slot);
        return;
    }
    case PrivateElement::Kind::Method:
        throwTypeError(globalObject, scope, "Trying to assign to a private method"_s);
        return;
    case PrivateElement::Kind::Accessor: {
        if (!element.setter) {
            throwTypeError(globalObject, scope, "Trying to access an undefined private setter"_s);
            return;
        }
        MarkedArgumentBuffer arguments;
        arguments.append(value);
        scope.release();
        call(globalObject, element.setter, JSC::getCallData(element.setter), base, arguments);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool privateHas(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        return false;
    }
    return Typed::Object::findPrivateElement(vm, asObject(base), name).kind != PrivateElement::Kind::None;
}

static void privateAdd(JSGlobalObject* globalObject, VM& vm, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isCompiled = !!dynamicDowncast<Typed::Object>(object);
    if (!isCompiled && !attributes) {
        PutPropertySlot slot(object, true);
        RELEASE_AND_RETURN(scope, object->definePrivateField(globalObject, name, value, slot));
    }
    if (Typed::Object::findPrivateElement(vm, object, name).kind != PrivateElement::Kind::None) {
        throwException(globalObject, scope, attributes ? createReinstallPrivateMethodError(globalObject) : createRedefinedPrivateNameError(globalObject));
        return;
    }
    if (isCompiled) {
        PropertyDescriptor descriptor;
        descriptor.setDescriptor(value, attributes);
        scope.release();
        Typed::Object::defineOwnProperty(object, globalObject, name, descriptor, true);
        return;
    }
    if (attributes & PropertyAttribute::Accessor)
        object->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(value), attributes);
    else
        object->putDirect(vm, name, value, attributes);
}

static ALWAYS_INLINE bool isClassPrivateName(Instance* instance, const Identifier& identifier, uint32_t name)
{
    return identifier.isPrivateName() && instance->typed->data.stringFacts(name) & Typed::Data::stringIsPrivateName;
}

static void cachePrivateElement(Instance* instance, VM& vm, JSValue base, Symbol* name)
{
    if (!base.isObject())
        return;
    Structure* structure = asObject(base)->structure();
    Slot& read = instance->keyedReadFor(structure->id(), name).slot;
    unsigned attributes = 0;
    PropertyOffset offset = structure->compiledLayout() ? invalidOffset : structure->get(vm, PropertyName(name->privateName()), attributes);
    bool isValue = isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue);
    cacheKeyedAccess(read, structure, name, isValue ? std::optional(offset) : std::nullopt);
    if (!isValidOffset(offset) && !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary())
        read.offset = Instance::keyIsAbsent;
    bool isField = isValue && !attributes;
    if (isField && !(read.offset & Slot::isGetter)) {
        read.offset &= ~Instance::keyMayNotBeEvicted;
        structure->didCachePropertyReplacement(vm, offset);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateGet, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(object), PropertyName(symbol->privateName()))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateSet, void, (Instance* instance, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    privateSet(globalObject, vm, JSValue::decode(object), PropertyName(symbol->privateName()), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

static void cachePrivateElementAtSite(Instance* instance, VM& vm, CallFrame* callFrame, Slot* cache, JSValue base, Symbol* name, bool isStore)
{
    if (SharedData::contains(cache))
        return;
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->pointer = nullptr;
    if (attempts == Slot::attemptsMask) {
        cache->offset = Slot::attemptsMask | Slot::isGetter;
        return;
    }
    attempts += 1u << Slot::attemptsShift;
    cache->offset = attempts;
    if (!base.isObject())
        return;
    Structure* structure = asObject(base)->structure();
    if (structure->compiledLayout() || structure->isDictionary() || !structure->propertyAccessesAreCacheable())
        return;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, PropertyName(name->privateName()), attributes);
    if (!isValidOffset(offset) || !isInlineOffset(offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || (isStore && attributes))
        return;
    auto location = propertyLocation(offset);
    if (!location || *location & ~Slot::directLocationMask)
        return;
    if (isStore)
        structure->didCachePropertyReplacement(vm, offset);
    cache->offset = *location | attempts;
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    didFillSlot(vm, dataWithSlot(instance, callFrame, cache));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateGetAtSite, EncodedJSValue, (Instance* instance, Slot* cache, EncodedJSValue object, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElementAtSite(instance, vm, callFrame, cache, JSValue::decode(object), symbol, false);
    OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(object), PropertyName(symbol->privateName()))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateSetAtSite, void, (Instance* instance, Slot* cache, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElementAtSite(instance, vm, callFrame, cache, JSValue::decode(object), symbol, true);
    privateSet(globalObject, vm, JSValue::decode(object), PropertyName(symbol->privateName()), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateHas, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(privateHas(globalObject, vm, JSValue::decode(object), PropertyName(symbol->privateName())))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateFieldAdd, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    JSObject* object = asObject(JSValue::decode(encodedObject));
    Structure* before = object->structure();
    if (!before->isDictionary() && !before->compiledLayout()) {
        PropertyOffset offset = invalidOffset;
        Structure* after = Structure::addPropertyTransitionToExistingStructure(before, PropertyName(symbol->privateName()), 0, offset);
        if (after && after->outOfLineCapacity() != before->outOfLineCapacity()) {
            Butterfly* butterfly = object->allocateMoreOutOfLineStorage(vm, before->outOfLineCapacity(), after->outOfLineCapacity());
            object->nukeStructureAndSetButterfly(vm, before->id(), butterfly);
            object->putDirectOffset(vm, offset, JSValue::decode(value));
            object->setStructure(vm, after);
            ensureStillAliveHere(before);
            ensureStillAliveHere(after);
            OPERATION_RETURN(scope);
        }
    }
    privateAdd(globalObject, vm, object, PropertyName(symbol->privateName()), JSValue::decode(value), 0);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    cacheKeyedDefinition(instance, vm, before, object->structure(), symbol, &symbol->privateName()->uid());
    OPERATION_RETURN(scope);
}

static void privateMethodsAdd(JSGlobalObject* globalObject, VM& vm, JSObject* object, JSObject* holder)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<RefPtr<UniquedStringImpl>, 4> names;
    holder->structure()->forEachProperty(vm, [&](const auto& entry) {
        names.append(entry.key());
        return true;
    });
    for (auto& name : names) {
        unsigned attributes;
        JSValue value = holder->getDirect(vm, name.get(), attributes);
        privateAdd(globalObject, vm, object, name.get(), value, attributes);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateMethodsAdd, EncodedJSValue, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue encodedHolder))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = asObject(JSValue::decode(encodedObject));
    JSObject* holder = asObject(JSValue::decode(encodedHolder));
    Structure* before = object->structure();
    privateMethodsAdd(globalObject, vm, object, holder);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    Structure* after = object->structure();
    Structure* holderStructure = holder->structure();
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype();
    };
    if (!isPlain(before) || !isPlain(after) || !isPlain(holderStructure) || holder->type() != FinalObjectType || before->typeInfoBlob() != after->typeInfoBlob())
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    PropertyOffset first = Instance::offsetAfter(before->maxOffset(), before->inlineCapacity());
    bool isInline = isInlineOffset(first);
    unsigned capacityBefore = before->outOfLineCapacity();
    unsigned capacityAfter = after->outOfLineCapacity();
    if (capacityAfter != capacityBefore && (isInline || !hasOnlyNamedOutOfLineStorage(before, after) || capacityAfter * sizeof(EncodedJSValue) > MarkedSpace::largeCutoff))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    unsigned count = 0;
    bool areAdjacent = true;
    holderStructure->forEachProperty(vm, [&](const auto& entry) {
        areAdjacent &= isInlineOffset(entry.offset()) && after->get(vm, entry.key()) == first + entry.offset();
        ++count;
        return true;
    });
    if (!areAdjacent || !count || holderStructure->maxOffset() != static_cast<PropertyOffset>(count - 1) || after->maxOffset() != static_cast<PropertyOffset>(first + count - 1) || (isInline && first + count > before->inlineCapacity()))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    auto& entry = instance->keyedAdditionFor(before->id(), holder);
    cacheKeyedAccess(entry.slot, before, holder, first);
    entry.newStructureID = after->id();
    entry.epoch = 0;
    entry.newStorageCapacity = capacityAfter != capacityBefore ? capacityAfter : 0;
    entry.holderValueCount = count;
    OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByName, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (isClassPrivateName(instance, identifier, name)) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(base), identifier)));
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), false));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    if (auto found = JSValue::decode(base).isObject() ? instance->typed->findThroughLazyPrototypesByString(vm, asObject(JSValue::decode(base)), identifier.impl(), name + 1) : std::nullopt) {
        if (found->member) {
            JSValue member = found->member->holder->getDirect(found->member->offset);
            if (!found->member->isAccessor)
                OPERATION_RETURN(scope, JSValue::encode(member));
            OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<GetterSetter>(member.asCell())->callGetter(globalObject, JSValue::decode(base))));
        }
        if (!found->firstObjectAbove)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        PropertySlot slot(JSValue::decode(base), PropertySlot::InternalMethodType::Get);
        bool has = found->firstObjectAbove->getPropertySlot(globalObject, identifier, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(has ? slot.getValue(globalObject, identifier) : jsUndefined()));
    }
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(base).get(globalObject, identifier)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetSuperPropertyByName, EncodedJSValue, (Instance* instance, EncodedJSValue home, EncodedJSValue receiver, uint32_t nameInTable))
{
    AOT_OPERATION_BEGIN(instance);
    RELEASE_ASSERT(JSValue::decode(home).isObject());
    JSValue homeObjectPrototype = asObject(JSValue::decode(home))->getPrototype(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!homeObjectPrototype.isObject()) {
        throwTypeError(globalObject, scope, "The prototype of the home object is not an object"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSObject* parent = asObject(homeObjectPrototype);
    const Identifier& name = identifierAt(instance, callFrame, PropertyLookupTable::identifierIndexForName(nameInTable));
    Typed::ClassPrototype::look(parent);
    Structure* structureBefore = parent->structure();
    PropertySlot slot(JSValue::decode(receiver), PropertySlot::InternalMethodType::Get);
    JSValue result = getByIdAndFillMegamorphicCache(globalObject, parent, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    bool didReadLazyPrototype = std::exchange(instance->typed->didGetDeclaredMember, false);
    auto* entry = !didReadLazyPrototype && parent->structure() == structureBefore && vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), name.impl()) : nullptr;
    if (!entry)
        OPERATION_RETURN(scope, JSValue::encode(result));
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    UniquedStringImpl* prototypeDependentUID = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : name.impl();
    if (!entry->m_holder)
        table.setAbsent(structureBefore, nameInTable, prototypeDependentUID);
    else if (entry->m_holder == JSCell::seenMultipleCalleeObjects())
        table.setOwnOffset(structureBefore, nameInTable, entry->m_offset, false);
    else
        table.setInherited(structureBefore, nameInTable, entry->m_holder, entry->m_offset, prototypeDependentUID);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutLazyClosure, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t function, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    instance->typed->putLazyClosure(globalObject, JSValue::decode(encodedBase), nameAt(instance, name), function);
    OPERATION_RETURN(scope);
}

static Typed::Object* asTypedObject(JSValue value)
{
    const Typed::LayoutData* layout = value.isCell() ? value.asCell()->structure()->compiledLayout() : nullptr;
    return layout && Typed::isObjectLayout(layout->kind) ? uncheckedDowncast<Typed::Object>(value.asCell()) : nullptr;
}

static void putToTypedObject(Instance* instance, JSGlobalObject* globalObject, Typed::Object* object, const Identifier& name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const Typed::FieldData* field = object->fieldNamed(state, name);
    if (field && object->hasExtraProperties() && !object->read(*field))
        object->specializeStructureForExtraProperties(vm);
    if (field && state.tryCreateFieldWithCachedTransition(vm, object, *field, value))
        return;
    Structure* structureBefore = object->structure();
    uint64_t wordsBefore = object->orderAndShapeWords();
    Typed::Object::putWithKnownField(object, globalObject, name, value, slot, field);
    RETURN_IF_EXCEPTION(scope, void());
    if (field && object->structure() == structureBefore && Typed::Object::didCreateOneField(static_cast<uint32_t>(wordsBefore), object->orderWord()) && Typed::Object::tryGetOwnData(vm, object, name))
        recordFieldCreation(instance, object, wordsBefore, name);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (isClassPrivateName(instance, identifier, name)) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), true);
    if (Typed::Object* object = asTypedObject(JSValue::decode(base)))
        putToTypedObject(instance, globalObject, object, identifier, JSValue::decode(value), slot);
    else
        JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowIfReadOnly, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(object->state(), nameAt(instance, name));
    if (field && object->attributesOf(*field) & PropertyAttribute::ReadOnly)
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByNameSloppy, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), false);
    if (Typed::Object* object = asTypedObject(JSValue::decode(base)))
        putToTypedObject(instance, globalObject, object, identifier, JSValue::decode(value), slot);
    else
        JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetStaticField, EncodedJSValue, (Instance* instance, uint32_t ofClass, uint32_t field))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    const Typed::LayoutData& layout = *state.classWithIndex(ofClass);
    JSValue classObject = state.closure(vm, layout.classData().classConstructor);
    OPERATION_RETURN(scope, JSValue::encode(classObject.get(globalObject, state.name(vm, layout.staticFields()[field].name))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetStaticField, void, (Instance* instance, uint32_t ofClass, uint32_t field, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    const Typed::LayoutData& layout = *state.classWithIndex(ofClass);
    JSValue classObject = state.closure(vm, layout.classData().classConstructor);
    PutPropertySlot slot(classObject, true);
    classObject.put(globalObject, state.name(vm, layout.staticFields()[field].name), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByNameInThisStructure, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (tryStoreIntoThisStructure(globalObject, nullptr, nullptr, JSValue::decode(base), identifier, JSValue::decode(value)))
        OPERATION_RETURN(scope);
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), true);
    JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

static void createDataProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value)
{
    Structure* structure = object->structure();
    unsigned attributes = 0;
    if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind)) {
        PropertyOffset offset;
        if (uncheckedDowncast<Typed::Object>(object)->tryStoreExtraProperty(globalObject->vm(), name, value, offset, false, true) != Typed::Object::ExtraPropertyStore::NotHandled)
            return;
    }
    if (object->type() == FinalObjectType && !structure->compiledLayout() && structure->isStructureExtensible() && !parseIndex(name) && (structure->get(globalObject->vm(), name, attributes) == invalidOffset || !attributes))
        object->putDirect(globalObject->vm(), name, value);
    else
        object->createDataProperty(globalObject, name, value, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineByKey, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue encodedKey, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = asObject(JSValue::decode(encodedObject));
    JSValue key = JSValue::decode(encodedKey);
    bool isAlreadyPropertyName = key.isSymbol() || (key.isString() && asString(key)->tryGetValueImpl() && asString(key)->tryGetValueImpl()->isAtom());
    auto name = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* before = object->structure();
    if (const Typed::LayoutData* layout = before->compiledLayout(); layout && Typed::isObjectLayout(layout->kind) && isAlreadyPropertyName) {
        auto* typedObject = uncheckedDowncast<Typed::Object>(object);
        bool hadNoExtraProperties = !typedObject->extraProperties();
        auto* place = hadNoExtraProperties || before->extraPropertiesStructureID() ? instance->freeKeyedAdditionEntry(before->id(), key.asCell()) : nullptr;
        bool wasSeenBefore = place && place->slot.structureID == before->id() && place->slot.pointer == key.asCell() && place->epoch >= Instance::keyedExtraPropertyAdditionSeenOnce;
        PropertyOffset offset;
        auto how = typedObject->tryStoreExtraProperty(vm, name, JSValue::decode(value), offset, false, false, nullptr, wasSeenBefore);
        if (how == Typed::Object::ExtraPropertyStore::Added && wasSeenBefore)
            cacheKeyedExtraPropertyAddition(instance, before, typedObject->structure(), key.asCell(), offset, hadNoExtraProperties);
        else if (how == Typed::Object::ExtraPropertyStore::Added && place)
            recordKeyedExtraPropertyAddition(*place, before, key.asCell());
        if (how == Typed::Object::ExtraPropertyStore::Added)
            OPERATION_RETURN(scope);
    }
    createDataProperty(globalObject, object, name, JSValue::decode(value));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (isAlreadyPropertyName)
        cacheKeyedDefinition(instance, vm, before, object->structure(), key.asCell(), name.impl());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (isClassPrivateName(instance, identifier, name)) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    asObject(JSValue::decode(base))->createDataProperty(globalObject, identifier, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

static bool tryDefineAbsentProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, uint32_t literal)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    bool isNonClassClosure = object->type() == JSFunctionType && object->classInfo() == Typed::Closure::info() && !uncheckedDowncast<Typed::Closure>(object)->classData() && uncheckedDowncast<Typed::Closure>(object)->kind() != Typed::Format::FunctionKind::Bound
        && name != vm.propertyNames->prototype && name != vm.propertyNames->name && name != vm.propertyNames->length;
    if ((object->type() != FinalObjectType && !isNonClassClosure) || structure->compiledLayout() || !structure->isStructureExtensible() || structure->hasImmutableProperties() || structure->hasImmutableShape()
        || structure->inheritorsMayOverrideReadOnlyProperties() || structure->typeInfo().wouldHaveFixedProperties() || parseIndex(name))
        return false;
    bool isAccessor = literal & (DescriptorLiteral::hasGet | DescriptorLiteral::hasSet);
    unsigned attributes = (literal & DescriptorLiteral::isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum) | (literal & DescriptorLiteral::isConfigurable ? PropertyAttribute::None : PropertyAttribute::DontDelete);
    if (isAccessor)
        attributes |= PropertyAttribute::Accessor;
    else if (!(literal & DescriptorLiteral::isWritable))
        attributes |= PropertyAttribute::ReadOnly;
    JSValue stored = literal & DescriptorLiteral::hasValue ? valueOrGetter : jsUndefined();
    if (isAccessor)
        stored = GetterSetter::create(vm, globalObject, literal & DescriptorLiteral::hasGet ? valueOrGetter.getObject() : nullptr, setter.getObject());
    PropertyOffset offset;
    Structure* next = structure->isDictionary() ? nullptr : Structure::addPropertyTransitionToExistingStructure(structure, name, attributes, offset);
    if (next) {
        if (structure->outOfLineCapacity() != next->outOfLineCapacity())
            object->nukeStructureAndSetButterfly(vm, structure->id(), object->allocateMoreOutOfLineStorage(vm, structure->outOfLineCapacity(), next->outOfLineCapacity()));
        object->putDirectOffset(vm, offset, stored);
        object->setStructure(vm, next);
        if (isAccessor)
            next->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(name == vm.propertyNames->underscoreProto);
        if (structure->mayBePrototype())
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add, object, name.uid());
        return true;
    }
    if (isValidOffset(structure->get(vm, name)))
        return false;
    if (!isAccessor) {
        object->putDirect(vm, name, stored, attributes);
        return true;
    }
    return object->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(stored), attributes);
}

static bool defineOwnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, uint32_t literal)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor descriptor;
    if (globalObject->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        globalObject->tryInstallPropertyDescriptorFastPathWatchpoint();
    if (globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid() && globalObject->objectPrototypeChainIsSane()) [[likely]] {
        if (literal & DescriptorLiteral::hasEnumerable)
            descriptor.setEnumerable(literal & DescriptorLiteral::isEnumerable);
        if (literal & DescriptorLiteral::hasConfigurable)
            descriptor.setConfigurable(literal & DescriptorLiteral::isConfigurable);
        if (literal & DescriptorLiteral::hasValue)
            descriptor.setValue(valueOrGetter);
        if (literal & DescriptorLiteral::hasWritable)
            descriptor.setWritable(literal & DescriptorLiteral::isWritable);
        if (literal & DescriptorLiteral::hasGet) {
            if (!valueOrGetter.isUndefined() && !valueOrGetter.isCallable()) {
                throwTypeError(globalObject, scope, "Getter must be a function."_s);
                return false;
            }
            descriptor.setGetter(valueOrGetter);
        }
        if (literal & DescriptorLiteral::hasSet) {
            if (!setter.isUndefined() && !setter.isCallable()) {
                throwTypeError(globalObject, scope, "Setter must be a function."_s);
                return false;
            }
            descriptor.setSetter(setter);
        }
        if (tryDefineAbsentProperty(globalObject, object, name, valueOrGetter, setter, literal))
            return true;
    } else {
        JSObject* description = constructEmptyObject(globalObject);
        uint32_t stored = literal >> DescriptorLiteral::storedFieldShift ? 1u << ((literal >> DescriptorLiteral::storedFieldShift) - 1) : 0;
        for (bool stores : { false, true }) {
            auto addField = [&](uint32_t which, const Identifier& field, JSValue fieldValue) {
                if (!(literal & which) || (stored == which) != stores)
                    return;
                if (!stores) {
                    description->putDirect(vm, field, fieldValue);
                    return;
                }
                PutPropertySlot slot(description, true);
                JSValue(description).put(globalObject, field, fieldValue, slot);
            };
            addField(DescriptorLiteral::hasValue, vm.propertyNames->value, valueOrGetter);
            addField(DescriptorLiteral::hasWritable, vm.propertyNames->writable, jsBoolean(literal & DescriptorLiteral::isWritable));
            addField(DescriptorLiteral::hasEnumerable, vm.propertyNames->enumerable, jsBoolean(literal & DescriptorLiteral::isEnumerable));
            addField(DescriptorLiteral::hasConfigurable, vm.propertyNames->configurable, jsBoolean(literal & DescriptorLiteral::isConfigurable));
            addField(DescriptorLiteral::hasGet, vm.propertyNames->get, valueOrGetter);
            addField(DescriptorLiteral::hasSet, vm.propertyNames->set, setter);
            RETURN_IF_EXCEPTION(scope, false);
        }
        bool isDescriptor = toPropertyDescriptor(globalObject, description, descriptor);
        EXCEPTION_ASSERT(!scope.exception() == isDescriptor);
        if (!isDescriptor)
            return false;
    }
    object->methodTable()->defineOwnProperty(object, globalObject, name, descriptor, true);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineOwnProperty, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue key, EncodedJSValue valueOrGetter, EncodedJSValue setter, uint32_t literal))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue object = JSValue::decode(encodedObject);
    if (!object.isObject()) {
        throwTypeError(globalObject, scope, "Properties can only be defined on Objects."_s);
        OPERATION_RETURN(scope);
    }
    auto name = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    defineOwnProperty(globalObject, asObject(object), name, JSValue::decode(valueOrGetter), JSValue::decode(setter), literal);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineOwnPropertyAtSite, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue value, uint32_t identifier, Slot* cache, uint32_t literal))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    JSValue base = JSValue::decode(encodedObject);
    if (!base.isObject()) {
        throwTypeError(globalObject, scope, "Properties can only be defined on Objects."_s);
        OPERATION_RETURN(scope);
    }
    JSObject* object = asObject(base);
    const Identifier& name = identifierAt(instance, callFrame, identifier);
    Structure* before = object->structure();
    bool isDefined = defineOwnProperty(globalObject, object, name, JSValue::decode(value), jsUndefined(), literal);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDefined || !globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid())
        OPERATION_RETURN(scope);
    Vector<UniquedStringImpl*, 6> absent;
    auto isAbsentUnless = [&](uint32_t has, const Identifier& nameOfDescriptor) {
        if (!(literal & has))
            absent.append(nameOfDescriptor.impl());
    };
    isAbsentUnless(DescriptorLiteral::hasValue, vm.propertyNames->value);
    isAbsentUnless(DescriptorLiteral::hasWritable, vm.propertyNames->writable);
    isAbsentUnless(DescriptorLiteral::hasEnumerable, vm.propertyNames->enumerable);
    isAbsentUnless(DescriptorLiteral::hasConfigurable, vm.propertyNames->configurable);
    isAbsentUnless(DescriptorLiteral::hasGet, vm.propertyNames->get);
    isAbsentUnless(DescriptorLiteral::hasSet, vm.propertyNames->set);
    cacheDefinedProperty(globalObject, callerData(instance, callFrame), cache, object, before, name.impl(), absent.span());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasName, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    const Identifier& identifier = nameAt(instance, name);
    if (isClassPrivateName(instance, identifier, name)) [[unlikely]]
        OPERATION_RETURN(scope, privateHas(globalObject, vm, value, identifier));
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, asObject(value)->hasProperty(globalObject, identifier));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByName, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool deleted = JSCell::deleteProperty(object, globalObject, nameAt(instance, name));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted)
        throwTypeError(globalObject, scope, object->recordIfOnlyLockRejects(vm, nameAt(instance, name).impl(), false, true) ? AOT::lockedOperationErrorMessage(vm, object, AOT::LockedOperation::Delete, nameAt(instance, name).impl()) : String(UnableToDeletePropertyError));
    OPERATION_RETURN(scope, deleted);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByKey, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue baseValue = JSValue::decode(base);
    JSValue keyValue = JSValue::decode(key);
    if (std::optional<uint32_t> index = keyValue.tryGetAsUint32Index())
        OPERATION_RETURN(scope, JSValue::encode(baseValue.get(globalObject, *index)));
    baseValue.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = keyValue.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(baseValue.get(globalObject, property)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByKey, void, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue baseValue = JSValue::decode(base);
    JSValue keyValue = JSValue::decode(key);
    if (std::optional<uint32_t> index = keyValue.tryGetAsUint32Index()) {
        if (baseValue.isCell() && baseValue.asCell()->type() == ArrayType)
            putArrayIndex(globalObject, uncheckedDowncast<JSArray>(baseValue.asCell()), *index, JSValue::decode(value), true);
        else
            baseValue.putByIndex(globalObject, *index, JSValue::decode(value), true);
        OPERATION_RETURN(scope);
    }
    baseValue.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    auto property = keyValue.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(baseValue, true);
    baseValue.put(globalObject, property, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasKey, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, 0);
    }
    if (std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index())
        OPERATION_RETURN(scope, asObject(value)->hasProperty(globalObject, *index));
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, asObject(value)->hasProperty(globalObject, property));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedElementOrEmpty, EncodedJSValue, (Instance* instance, EncodedJSValue base, int64_t index, EncodedJSValue ifAbsent))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    bool checksPresenceFirst = !JSValue::decode(ifAbsent);
    if (index >= 0 && index <= static_cast<int64_t>(MAX_ARRAY_INDEX)) {
        if (!checksPresenceFirst)
            OPERATION_RETURN(scope, JSValue::encode(value.get(globalObject, static_cast<unsigned>(index))));
        if (!value.isObject()) {
            throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        bool has = asObject(value)->hasProperty(globalObject, static_cast<unsigned>(index));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!has)
            OPERATION_RETURN(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(asObject(value)->get(globalObject, static_cast<unsigned>(index))));
    }
    Identifier name = Identifier::from(vm, static_cast<double>(index));
    if (!checksPresenceFirst)
        OPERATION_RETURN(scope, JSValue::encode(value.get(globalObject, name)));
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    bool has = asObject(value)->hasProperty(globalObject, name);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!has)
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(asObject(value)->get(globalObject, name)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByKey, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool deleted = JSCell::deleteProperty(object, globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted)
        throwTypeError(globalObject, scope, object->recordIfOnlyLockRejects(vm, property.impl(), false, true) ? AOT::lockedOperationErrorMessage(vm, object, AOT::LockedOperation::Delete, property.impl()) : String(UnableToDeletePropertyError));
    OPERATION_RETURN(scope, deleted);
}

static void throwIsNotAFunction(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    String description = errorDescriptionForValue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    throwTypeError(globalObject, scope, makeString(description, " is not a function"_s));
}

static JSValue callValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue thisValue, uint32_t count, const EncodedJSValue* values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(callee);
    if (callData.type == CallData::Type::None) {
        throwIsNotAFunction(globalObject, scope, callee);
        return { };
    }
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    RELEASE_AND_RETURN(scope, call(globalObject, callee, callData, thisValue, arguments));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCall, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, JSValue::decode(callee), JSValue::decode(thisValue), count, values)));
}

static void throwNotAFunction(JSGlobalObject* globalObject, ThrowScope& scope, const Identifier& name, JSValue callee)
{
    throwTypeError(globalObject, scope, makeString(name.string(), " is not a function, it is "_s, State::describe(callee)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowUnlessCallable, void, (Instance* instance, EncodedJSValue callee, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uint64_t bits = callee;
    if (uint64_t offsetInCage = bits - MarkedBlockCage::base(); bits >= FaultingAccessInImage::reach && offsetInCage <= static_cast<uint64_t>(Typed::undefinedCell32) << MarkedBlockCage::shift)
        bits = offsetInCage >> MarkedBlockCage::shift;
    JSValue value = JSValue::decode(bits);
    if (!value)
        value = jsUndefined();
    if (value.isCallable())
        OPERATION_RETURN(scope);
    if (name == UINT32_MAX)
        throwIsNotAFunction(globalObject, scope, value);
    else
        throwNotAFunction(globalObject, scope, nameAt(instance, name), value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowMethodIsNotAFunction, void, (Instance* instance, EncodedJSValue callee, uint32_t siteIdentifier))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwNotAFunction(globalObject, scope, identifierAt(instance, callFrame, siteIdentifier), JSValue::decode(callee));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowUnlessMethodIsCallable, void, (Instance* instance, EncodedJSValue callee, uint32_t siteIdentifier))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!JSValue::decode(callee).isCallable())
        throwNotAFunction(globalObject, scope, identifierAt(instance, callFrame, siteIdentifier), JSValue::decode(callee));
    OPERATION_RETURN(scope);
}

static void forEachInSpread(JSGlobalObject*, JSValue iterable, const ScopedLambda<void(JSValue)>&);

static JSValue hostRequireFunction(JSGlobalObject* globalObject, Instance* instance)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = *instance->typed;
    if (!state.importMeta) {
        auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
        JSModuleLoader* ownLoader = instance->loader() ? instance->loader() : globalObject->moduleLoader();
        state.importMeta = ownLoader->createImportMetaProperties(globalObject, identifierToJSValue(vm, state.importerKey(vm)), importer, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, state.importMeta->get(globalObject, Identifier::fromString(vm, "require"_s)));
}

static JSValue requireFromHost(JSGlobalObject* globalObject, Instance* instance, JSValue specifier)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue require = hostRequireFunction(globalObject, instance);
    RETURN_IF_EXCEPTION(scope, { });
    auto callData = JSC::getCallData(require);
    if (callData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot find module '"_s, specifier.toWTFString(globalObject), "': require() is not available here"_s)));
        return { };
    }
    MarkedArgumentBuffer arguments;
    arguments.append(specifier);
    RELEASE_AND_RETURN(scope, call(globalObject, require, callData, jsUndefined(), arguments));
}

static JSObject* toESM(JSGlobalObject* globalObject, Instance* instance, JSValue exported, bool isNodeMode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* source = exported.getObject();
    JSObject* result = constructEmptyObject(globalObject);
    if (source) {
        JSValue prototype = source->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result = prototype.isObject() ? constructEmptyObject(globalObject, asObject(prototype)) : constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
        result->convertToDictionary(vm);
        result->putDirect(vm, vm.propertyNames->builtinNames().starNamespacePrivateName(), source, PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete | PropertyAttribute::DontEnum);
        RELEASE_ASSERT(!result->getDirectOffset(vm, vm.propertyNames->builtinNames().starNamespacePrivateName()));
    } else if (!exported.isUndefinedOrNull())
        result = constructEmptyObject(globalObject, exported.synthesizePrototype(globalObject));
    bool hasDefault = isNodeMode || !source;
    if (!hasDefault) {
        JSValue marker = source->get(globalObject, Identifier::fromString(vm, "__esModule"_s));
        RETURN_IF_EXCEPTION(scope, nullptr);
        hasDefault = !marker.toBoolean(globalObject);
        if (!hasDefault) {
            hasDefault = !source->hasOwnProperty(globalObject, vm.propertyNames->defaultKeyword);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    if (hasDefault)
        result->putDirect(vm, vm.propertyNames->defaultKeyword, exported);
    if (!source)
        return result;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    source->methodTable()->getOwnPropertyNames(source, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& name : names) {
        bool isAlreadyAvailable = result->hasOwnProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (isAlreadyAvailable)
            continue;
        if (!parseIndex(name)) {
            result->putDirectCustomAccessor(vm, name, instance->typed->liveBindingAccessor(vm), PropertyAttribute::CustomValue | PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete);
            continue;
        }
        MarkedArgumentBuffer boundArguments;
        boundArguments.append(identifierToJSValue(vm, name));
        JSObject* getter = JSBoundFunction::create(vm, globalObject, instance->typed->liveBindingGetter(vm), source, boundArguments, 0, nullptr, SourceTaintedOrigin::Untainted);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result->putDirectAccessor(globalObject, name, GetterSetter::create(vm, globalObject, getter, nullptr), PropertyAttribute::Accessor | PropertyAttribute::DontDelete);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFailModuleCycle, void, (Instance* instance, uint64_t failed, uint32_t numberOfRows, EncodedJSValue error))
{
    if (instance->moduleEvaluationDepth > 1)
        return;
    auto& data = instance->typed->data;
    const uint32_t* words = data.at<uint32_t>(data.moduleEvaluationsOffset);
    constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    auto forEachCalledModule = [&](uint32_t row, auto&& functor) {
        uint32_t start = words[numberOfRows + row];
        for (uint32_t cursor = start + 2; cursor != words[start + 1]; cursor += 2) {
            if (words[cursor] < none - 1)
                functor(words[cursor + 1]);
        }
    };
    auto modulesReachedBy = [&](auto&& forEachNext) {
        BitVector reached(numberOfRows);
        Vector<uint32_t, 16> worklist;
        reached.quickSet(failed);
        worklist.append(static_cast<uint32_t>(failed));
        while (!worklist.isEmpty()) {
            forEachNext(worklist.takeLast(), [&](uint32_t next) {
                if (!reached.quickSet(next))
                    worklist.append(next);
            });
        }
        return reached;
    };
    Vector<Vector<uint32_t, 2>> callers(numberOfRows);
    for (size_t row : modulesReachedBy(forEachCalledModule)) {
        forEachCalledModule(row, [&](uint32_t callee) {
            callers[callee].append(static_cast<uint32_t>(row));
        });
    }
    BitVector cycle = modulesReachedBy([&](uint32_t row, auto&& functor) {
        for (uint32_t caller : callers[row])
            functor(caller);
    });
    auto put = [&](uint32_t offset, EncodedJSValue value) {
        *reinterpret_cast<EncodedJSValue*>(instance->typedStatics + offset) = value;
        instance->didStoreRootInStatics(offset);
    };
    for (size_t row : cycle) {
        uint32_t errorAt = words[words[numberOfRows + row]];
        if (errorAt == none || instance->typedStatics[words[row]] != static_cast<uint8_t>(JSValue::encode(jsBoolean(true))))
            continue;
        instance->typedStatics[words[row]] = static_cast<uint8_t>(JSValue::encode(jsBoolean(false)));
        put(errorAt, error);
    }
}

static EncodedJSValue createModuleNamespace(Instance* instance, std::span<const uint32_t> nameIndicesOf, std::span<const EncodedJSValue> values)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = values.size();
    auto argument = [&](unsigned index) {
        return index < count ? JSValue::decode(values[index]) : jsUndefined();
    };
    bool isDeferred = argument(count - 1).isNull();
    count -= isDeferred;
    bool hasFlag = argument(count - 1).isBoolean();
    bool hasHostPrototype = hasFlag && argument(count - 1).isTrue();
    unsigned end = count - hasFlag;
    JSObject* star = end > 1 ? argument(end - 1).getObject() : nullptr;
    end -= !!star;
    RELEASE_ASSERT(nameIndicesOf.empty() || end == 1);
    unsigned numberOfNames = nameIndicesOf.empty() ? end - 1 : nameIndicesOf.size();
    auto nameAt = [&](unsigned index) {
        return nameIndicesOf.empty() ? asString(argument(index + 1)) : instance->typed->string(vm, nameIndicesOf[index]);
    };
    bool hasLazyNames = !nameIndicesOf.empty();
    auto* names = hasLazyNames ? nullptr : JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, numberOfNames);
    for (unsigned i = 0; !hasLazyNames && i < numberOfNames; ++i) {
        nameAt(i)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
        RELEASE_ASSERT(!i || codePointCompare(StringView(*nameAt(i - 1)->tryGetValueImpl()), StringView(*nameAt(i)->tryGetValueImpl())) < 0);
        names->setIndex(vm, i, nameAt(i));
    }
    JSCellButterfly* starExportNames = nullptr;
    if (star) {
        PropertyNameArrayBuilder found(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        star->methodTable()->getOwnPropertyNames(star, globalObject, found, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
        Vector<Identifier> brought;
        for (auto& name : found) {
            bool isOwn = name == vm.propertyNames->defaultKeyword || (hasLazyNames && Typed::ModuleNamespace::indexOfName(instance->typed->data, nameIndicesOf, name.impl()) != Typed::ModuleNamespace::noExport);
            for (unsigned i = 0; !hasLazyNames && i < numberOfNames && !isOwn; ++i)
                isOwn = nameAt(i)->tryGetValueImpl() == name.impl();
            if (!isOwn)
                brought.append(name);
        }
        std::sort(brought.begin(), brought.end(), [](const Identifier& a, const Identifier& b) {
            return codePointCompare(StringView(*a.impl()), StringView(*b.impl())) < 0;
        });
        starExportNames = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, brought.size());
        for (unsigned i = 0; i < brought.size(); ++i) {
            JSString* text = jsString(vm, brought[i].string());
            text->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, encodedJSValue());
            starExportNames->setIndex(vm, i, text);
        }
    }
    Structure* structure = instance->typed->structureForNewModuleNamespace(vm, hasHostPrototype);
    if (hasLazyNames)
        return JSValue::encode(Typed::ModuleNamespace::createWithLazyNames(vm, structure, asObject(argument(0)), nameIndicesOf, star, starExportNames, isDeferred));
    return JSValue::encode(Typed::ModuleNamespace::create(vm, structure, asObject(argument(0)), names, star, starExportNames, isDeferred));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMakeModuleNamespace, EncodedJSValue, (Instance* instance, uint32_t numberOfNames, uint32_t rowOfNames, uint32_t count, const EncodedJSValue* readerAndTrailingValues))
{
    AOT_OPERATION_BEGIN(instance);
    const uint32_t* nameIndicesOf = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    OPERATION_RETURN(scope, createModuleNamespace(instance, std::span { nameIndicesOf, numberOfNames }, std::span { readerAndTrailingValues, count }));
}

static EncodedJSValue externalModuleNamespace(JSGlobalObject* globalObject, Instance* instance, JSModuleLoader* loader, const Identifier& name, RefPtr<ScriptFetchParameters>&& parameters)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier key = loader->resolve(globalObject, name, instance->typed->importerKey(vm), nullptr, true);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    bool isEvaluating = false;
    std::optional<ScriptFetchParameters::Type> type;
    if (parameters)
        type = parameters->type();
    auto evaluatedRecord = [&]() -> AbstractModuleRecord* {
        ModuleRegistryEntry* entry = type ? loader->getRegisteredMayBeNull(key, *type) : loader->registryEntry(key);
        AbstractModuleRecord* record = entry ? entry->record() : nullptr;
        if (!record)
            return nullptr;
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record)) {
            isEvaluating = cyclic->status() == CyclicModuleRecord::Status::Evaluating;
            bool hasRun = cyclic->status() == CyclicModuleRecord::Status::Evaluated || (isEvaluating && !cyclic->hasTLA());
            return hasRun && !cyclic->evaluationError() ? record : nullptr;
        }
        return record->moduleEnvironmentMayBeNull() ? record : nullptr;
    };
    AbstractModuleRecord* imported = evaluatedRecord();
    if (!imported && !isEvaluating) {
        JSPromise* promise = loader->loadModuleSync(globalObject, key, WTF::move(parameters), nullptr);
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
        promise->markAsHandled();
        if (promise->status() == JSPromise::Status::Rejected) {
            throwException(globalObject, scope, promise->result());
            return encodedJSValue();
        }
        imported = evaluatedRecord();
    }
    if (!imported) {
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("The module \""_s, key.string(), "\" is not compiled with the program and cannot be loaded synchronously: use \"await import()\" instead"_s)));
        return encodedJSValue();
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(imported->getModuleNamespace(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExternalModule, EncodedJSValue, (Instance* instance, uint32_t specifier))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    AbstractModuleRecord* importer = instance->typed->moduleRecord;
    const Identifier& name = nameAt(instance, specifier);
    AbstractModuleRecord* imported = importer ? importer->hostResolveImportedModule(globalObject, name, ScriptFetchParameters::Type::JavaScript) : nullptr;
    if (importer && !imported) {
        String other = name.string().startsWith("node:"_s) ? name.string().string().substring(5) : makeString("node:"_s, name.string());
        imported = importer->hostResolveImportedModule(globalObject, Identifier::fromString(vm, other), ScriptFetchParameters::Type::JavaScript);
    }
    if (imported)
        OPERATION_RETURN(scope, JSValue::encode(imported->getModuleNamespace(globalObject)));
    JSModuleLoader* loader = importer ? importer->moduleLoader() : globalObject->moduleLoader();
    OPERATION_RETURN(scope, externalModuleNamespace(globalObject, instance, loader, name, nullptr));
}

static constexpr unsigned argumentsCalleeAttributes = PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::Accessor;

static ClonedArguments* tryCreateArgumentsFast(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSArray* list, unsigned length)
{
    IndexingType type = list->indexingType();
    if ((!hasContiguous(type) && !hasInt32(type)) || containsHole(list->butterfly()->contiguous().data(), length))
        return nullptr;
    PropertyOffset offsetOfCallee;
    PropertyOffset offsetOfIterator;
    Structure* structure = Structure::addPropertyTransitionToExistingStructure(globalObject->clonedArgumentsStructure(), vm.propertyNames->callee, argumentsCalleeAttributes, offsetOfCallee);
    if (structure)
        structure = Structure::addPropertyTransitionToExistingStructure(structure, vm.propertyNames->iteratorSymbol, static_cast<unsigned>(PropertyAttribute::DontEnum), offsetOfIterator);
    if (!structure || !hasContiguous(structure->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->storedPrototypeObject()->needsSlowPutIndexing())
        return nullptr;
    JSValue callee = globalObject->throwTypeErrorArgumentsCalleeGetterSetter();
    JSValue iterator = globalObject->arrayProtoValuesFunction();
    ClonedArguments* result = ClonedArguments::createEmpty(vm, nullptr, structure, nullptr, length, nullptr);
    if (!result)
        return nullptr;
    gcSafeMemcpy(result->butterfly()->contiguous().data(), list->butterfly()->contiguous().data(), length * sizeof(JSValue));
    result->putDirectOffset(vm, offsetOfCallee, callee);
    result->putDirectOffset(vm, offsetOfIterator, iterator);
    if (!instance->argumentsStructure && globalObject == instance->globalObject && offsetOfCallee == firstOutOfLineOffset + 1 && offsetOfIterator == firstOutOfLineOffset + 2 && structure->outOfLineCapacity() == initialOutOfLineCapacity) {
        instance->argumentsStructure = structure;
        instance->argumentsCallee = callee.asCell();
        instance->argumentsIterator = iterator.asCell();
        prepareInlineAllocation<ClonedArguments>(instance, Instance::InlineAllocation::Arguments, structure);
    }
    return result;
}

static void recordDisposeError(JSGlobalObject* globalObject, JSArray* stack, JSValue error)
{
    VM& vm = globalObject->vm();
    Identifier hasErrorName = Identifier::fromString(vm, "hasError"_s);
    Identifier errorName = Identifier::fromString(vm, "error"_s);
    JSValue hasError = stack->getDirect(vm, hasErrorName);
    if (hasError && hasError.isTrue()) {
        JSValue suppressed = stack->getDirect(vm, errorName);
        JSObject* wrapped = ErrorInstance::create(vm, globalObject->errorStructure(ErrorType::SuppressedError), String(), JSValue(), nullptr, TypeNothing, ErrorType::SuppressedError, false);
        wrapped->putDirect(vm, vm.propertyNames->error, error, static_cast<unsigned>(PropertyAttribute::DontEnum));
        wrapped->putDirect(vm, vm.propertyNames->suppressed, suppressed, static_cast<unsigned>(PropertyAttribute::DontEnum));
        error = wrapped;
    }
    stack->putDirect(vm, hasErrorName, jsBoolean(true));
    stack->putDirect(vm, errorName, error);
}

static NEVER_INLINE JSValue disposeNextResource(JSGlobalObject* globalObject, JSArray* stack)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int32_t needsAwait = 1;
    constexpr int32_t hasAwaited = 2;
    unsigned length = stack->length();
    int32_t awaits = 0;
    if (length % 3) {
        awaits = stack->pop(globalObject).asInt32();
        RETURN_IF_EXCEPTION(scope, { });
        --length;
    }
    auto leaveWith = [&](int32_t noted, JSValue result) {
        if (noted) {
            stack->push(globalObject, jsNumber(noted));
            RETURN_IF_EXCEPTION(scope, JSValue());
        }
        return result;
    };
    if (awaits == needsAwait) {
        JSValue kindOfNext = length ? stack->getIndex(globalObject, length - 1) : jsBoolean(false);
        RETURN_IF_EXCEPTION(scope, { });
        if (kindOfNext.isFalse())
            return jsUndefined();
    }
    if (!length)
        return stack;
    JSValue kind = stack->pop(globalObject);
    scope.assertNoException();
    JSValue method = stack->pop(globalObject);
    scope.assertNoException();
    JSValue resource = stack->pop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isUndefined())
        return leaveWith(awaits | needsAwait, jsNull());
    bool isPlainMethodUnderAwait = kind.isNumber();
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue result = call(globalObject, method, JSC::getCallData(method), resource, ArgList());
    if (Exception* exception = catchScope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!catchScope.clearExceptionExceptTermination())
            return { };
        recordDisposeError(globalObject, stack, error);
        if (!isPlainMethodUnderAwait)
            return leaveWith(awaits, jsNull());
    }
    if (kind.isFalse())
        return leaveWith(awaits, jsNull());
    return leaveWith(awaits | hasAwaited, isPlainMethodUnderAwait || result.isNull() ? jsUndefined() : result);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, PropertyName property, JSValue function, bool isEnumerable, bool isGetter)
{
    if (Structure* structure = object->structure(); object->type() == FinalObjectType && structure->isStructureExtensible() && !parseIndex(property)) {
        VM& vm = globalObject->vm();
        unsigned wanted = PropertyAttribute::Accessor | (isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum);
        unsigned attributes;
        PropertyOffset offset = structure->get(vm, property, attributes);
        JSObject* getter = isGetter ? asObject(function) : nullptr;
        JSObject* setter = isGetter ? nullptr : asObject(function);
        bool isOtherHalf = isValidOffset(offset) && attributes == wanted;
        if (isOtherHalf) {
            auto* current = uncheckedDowncast<GetterSetter>(object->getDirect(offset));
            if (isGetter)
                setter = current->isSetterNull() ? nullptr : current->setter();
            else
                getter = current->isGetterNull() ? nullptr : current->getter();
        }
        if ((isOtherHalf || !isValidOffset(offset)) && object->putDirectNonIndexAccessor(vm, property, GetterSetter::create(vm, globalObject, getter, setter), wanted))
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

static void defineAccessor(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue function, bool isEnumerable, bool isGetter, JSValue position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    defineAccessor(globalObject, asObject(object), property, function, isEnumerable, isGetter);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto* compiled = dynamicDowncast<Typed::Object>(object); compiled && position.isUInt32())
        compiled->positionExtraKey(compiled->state(), property, position.asUInt32());
}

static void defineEnumerableAccessorAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue object, JSValue key, JSValue function, bool isGetter)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!instance->nullGetterFunction) {
        instance->nullGetterFunction = globalObject->nullGetterFunction();
        instance->nullSetterFunction = globalObject->nullSetterFunction();
        prepareInlineAllocation<GetterSetter>(instance, Instance::InlineAllocation::GetterSetter, vm.getterSetterStructure.get());
    }
    Structure* before = asObject(object)->structure();
    defineAccessor(globalObject, object, key, function, true, isGetter, jsUndefined());
    RETURN_IF_EXCEPTION(scope, void());
    StringImpl* name = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    if (name && name->isAtom())
        cacheKeyedDefinition(instance, vm, before, asObject(object)->structure(), key.asCell(), static_cast<UniquedStringImpl*>(name), static_cast<unsigned>(PropertyAttribute::Accessor), isGetter ? Instance::keyedAdditionIsGetter : Instance::keyedAdditionIsSetter);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineGetterByKey, void, (Instance* instance, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    defineEnumerableAccessorAndCache(instance, globalObject, vm, JSValue::decode(object), JSValue::decode(key), JSValue::decode(function), true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineSetterByKey, void, (Instance* instance, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    defineEnumerableAccessorAndCache(instance, globalObject, vm, JSValue::decode(object), JSValue::decode(key), JSValue::decode(function), false);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineAccessors, void, (Instance* instance, JSCell* object, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Vector<std::pair<UniquedStringImpl*, uint32_t>, 16> namesAndPositions;
    JSObject* extra = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        auto value = [&](unsigned which) {
            return JSValue::decode(values[4 * i + which]);
        };
        const Identifier& name = nameAt(instance, value(0).asUInt32());
        auto* compiled = dynamicDowncast<Typed::Object>(object);
        bool isDefinedOnExtra = compiled && compiled->mayDefineAccessorOnExtraProperties(vm, compiled->state(), name, extra);
        defineAccessor(globalObject, isDefinedOnExtra ? extra : asObject(object), name, value(1), value(2).asInt32() & 2, value(2).asInt32() & 1);
        if (isDefinedOnExtra)
            compiled->didChangeExtraProperties(vm);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        namesAndPositions.append({ name.impl(), value(3).asUInt32() });
    }
    if (auto* compiled = dynamicDowncast<Typed::Object>(object)) {
        compiled->specializeStructureForExtraProperties(vm);
        compiled->positionExtraKeys(compiled->state(), namesAndPositions.span());
    }
    OPERATION_RETURN(scope);
}

static JSValue constructWith(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSCell* cell, uint32_t count, const EncodedJSValue* values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    auto* closure = dynamicDowncast<Typed::Closure>(callee);
    if (!closure || !closure->createsFixedLayoutObjects() || closure->structure()->aotInstance()->typed->rejectsFreshObject(closure->functionIndex(), uncheckedDowncast<Typed::Object>(cell)->shape())) {
        auto constructData = JSC::getConstructData(callee);
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, makeString(State::describe(callee), " is not a constructor"_s));
            return { };
        }
        RELEASE_AND_RETURN(scope, construct(globalObject, callee, constructData, arguments));
    }
    closure->structure()->aotInstance()->typed->registerConstructorClosure(closure);
    JSValue prototype = closure->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    object->setPrototypeAtCreation(vm, prototype.isObject() ? asObject(prototype) : closure->realm()->objectPrototype());
    JSValue result = closure->callAsConstructor(globalObject, object, closure, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    return result.isObject() ? result : JSValue(object);
}

static NEVER_INLINE bool appendBoxedDoubles(JSGlobalObject* globalObject, VM& vm, JSArray* target, const double* numbers, unsigned length)
{
    Vector<EncodedJSValue, 16> boxed(length, [&](size_t i) {
        return JSValue::encode(JSValue(JSValue::EncodeAsDouble, numbers[i]));
    });
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, boxed.span());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstructWith, EncodedJSValue, (Instance* instance, EncodedJSValue callee, JSCell* object, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, JSValue::decode(callee), object, count, values)));
}

static const char* nameOfElementsForCounters(JSArray* array)
{
    if (array->type() == DerivedArrayType)
        return "an heir of Array";
    if (array->classInfo() != JSArray::info())
        return array->classInfo()->className.characters();
    IndexingType type = array->indexingType();
    bool isShared = isCopyOnWrite(array->indexingMode());
    if (hasInt32(type))
        return isShared ? "int32, copy on write" : "int32";
    if (hasDouble(type))
        return isShared ? "doubles, copy on write" : "doubles";
    if (hasContiguous(type))
        return isShared ? "contiguous, copy on write" : "contiguous";
    return hasUndecided(type) ? "undecided" : hasAnyArrayStorage(type) ? "array storage" : "no elements";
}

static const char* arrayIterationReasonForCounters(JSArray* source)
{
    if (source->type() == DerivedArrayType)
        return "an heir of Array";
    if (source->classInfo() != JSArray::info())
        return "another class";
    if (!source->isIteratorProtocolFastAndNonObservable())
        return "the iteration can be observed";
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type))
        return containsHole(source->butterfly()->contiguousDouble().data(), length) ? "a hole or a NaN" : "the copy was declined";
    if (hasInt32(type) || hasContiguous(type))
        return containsHole(source->butterfly()->contiguous().data(), length) ? "a hole" : "the copy was declined";
    return "elements of another kind";
}

static NEVER_INLINE void countSpreadForCounters(Instance* instance, JSArray* target, JSValue source)
{
    auto* list = dynamicDowncast<JSArray>(source);
    countOperationNamed(instance, "AppendSpread:source", list ? nameOfElementsForCounters(list) : source.isCell() ? source.asCell()->classInfo()->className.characters() : "primitive");
    countOperationNamed(instance, "AppendSpread:target", nameOfElementsForCounters(target));
    if (!list)
        return;
    unsigned length = list->length();
    countOperationNamed(instance, "AppendSpread:length", !length ? "0" : length == 1 ? "1" : length <= 4 ? "2 to 4" : length <= 16 ? "5 to 16" : length <= 64 ? "17 to 64" : "over 64");
}

static NEVER_INLINE bool hasDefaultArrayIteration(JSArray* array)
{
    JSGlobalObject* globalObject = array->realm();
    VM& vm = globalObject->vm();
    if (!vm.useImmutableIntrinsics || !globalObject->isArrayPrototypeIteratorProtocolFastAndNonObservable())
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = array->getPropertySlot(globalObject, vm.propertyNames->iteratorSymbol, slot);
    scope.assertNoException();
    return isFound && !slot.isTaintedByOpaqueObject() && slot.isValue() && slot.slotBase() == globalObject->arrayPrototype();
}

static bool tryAppendLongArray(VM& vm, JSArray* target, JSArray* source)
{
    IndexingType shape = target->indexingMode() & IndexingShapeAndWritabilityMask;
    IndexingType sourceShape = source->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return false;
    if (sourceShape != shape && (shape != ContiguousShape || sourceShape != Int32Shape))
        return false;
    if (target->classInfo() != JSArray::info() || target->structure()->outOfLineCapacity() || !target->isStructureExtensible())
        return false;
    uint64_t length = target->butterfly()->publicLength();
    uint64_t count = source->butterfly()->publicLength();
    if (length + count > MAX_STORAGE_VECTOR_LENGTH)
        return false;
    unsigned newLength = length + count;
    if (newLength > target->butterfly()->vectorLength()) {
        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), newLength);
        void* base = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        if (!base) [[unlikely]]
            return false;
        Butterfly* grown = Butterfly::fromBase(base, 0, 0);
        uint64_t* elements = grown->indexingPayload<uint64_t>();
        memcpy(elements, target->butterfly()->indexingPayload<uint64_t>(), length * sizeof(uint64_t));
        memcpy(elements + length, source->butterfly()->indexingPayload<uint64_t>(), count * sizeof(uint64_t));
        uint64_t hole = shape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
        for (unsigned i = newLength; i < vectorLength; ++i)
            elements[i] = hole;
        grown->setVectorLength(vectorLength);
        grown->setPublicLength(newLength);
        target->setButterfly(vm, grown);
    } else {
        gcSafeMemcpy(target->butterfly()->indexingPayload<uint64_t>() + length, source->butterfly()->indexingPayload<uint64_t>(), count * sizeof(uint64_t));
        WTF::storeStoreFence();
        target->butterfly()->setPublicLength(newLength);
    }
    if (shape == ContiguousShape)
        vm.writeBarrier(target);
    return true;
}

static bool tryAppendArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSValue iterable)
{
    auto* source = dynamicDowncast<JSArray>(iterable);
    if (!source || source->classInfo() != JSArray::info() || (!source->isIteratorProtocolFastAndNonObservable() && !hasDefaultArrayIteration(source)))
        return false;
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type)) {
        if (containsHole(source->butterfly()->contiguousDouble().data(), length))
            return false;
        if (hasContiguous(target->indexingType()) && length < MIN_SPARSE_ARRAY_INDEX && target->length() < MIN_SPARSE_ARRAY_INDEX - length)
            return appendBoxedDoubles(globalObject, vm, target, source->butterfly()->contiguousDouble().data(), length);
    } else if (hasInt32(type) || hasContiguous(type)) {
        if (containsHole(source->butterfly()->contiguous().data(), length))
            return false;
    } else
        return false;
    if (length >= MIN_SPARSE_ARRAY_INDEX - std::min(target->length(), MIN_SPARSE_ARRAY_INDEX) && tryAppendLongArray(vm, target, source))
        return true;
    return target->appendMemcpy(globalObject, vm, target->length(), source);
}

static bool tryAppendTypedArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArrayBufferView* source)
{
    if (!source->isIteratorProtocolFastAndNonObservable())
        return false;
    size_t count = source->length();
    if (!count)
        return !source->isDetached() && !source->isOutOfBounds();
    if (count > MAX_STORAGE_VECTOR_LENGTH || !source->canGetIndexQuicklyForTypedArray(count - 1))
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (size_t i = 0; i < count; ++i) {
        target->push(globalObject, source->getIndexQuicklyForTypedArray(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static Structure* emptyLiteralStructureForCopy(JSGlobalObject* globalObject, VM& vm, unsigned inlineCapacity)
{
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    return profile.structure();
}

static Instance::CloneAndExtendPlan planCloneAndExtend(Instance*, JSGlobalObject*, VM&, Structure* source, uint64_t more);
static JSFinalObject* copyWithAllPropertiesInline(VM&, JSObject*, Structure*, Structure* ofCopy);

static void cacheSpreadCopy(Instance* instance, VM& vm, JSObject* object, Structure* structure, uint64_t more, const Instance::CloneAndExtendPlan& plan)
{
    Structure* ofCopy = plan.structure;
    if (!ofCopy || object->butterfly() || structure->outOfLineCapacity() || ofCopy->outOfLineCapacity())
        return;
    Instance::SpreadCopyCacheEntry* pair = &instance->spreadCopyCache[Instance::spreadCopyCacheIndex(structure->id().bits(), more)];
    auto canBeEvicted = [&](const Instance::SpreadCopyCacheEntry& place) {
        return !place.structureID || (place.structureID == structure->id().bits() && place.more == more);
    };
    Instance::SpreadCopyCacheEntry* place = canBeEvicted(pair[0]) ? &pair[0] : canBeEvicted(pair[1]) ? &pair[1] : nullptr;
    if (!place) {
        instance->keyedTableRandomState = instance->keyedTableRandomState * 1664525u + 1013904223u;
        if (instance->keyedTableRandomState >> 30)
            return;
        place = &pair[instance->keyedTableRandomState >> 29 & 1];
    }
    unsigned copied = ofCopy->inlineCapacity() == structure->inlineCapacity() ? structure->inlineCapacity() : structure->inlineSize();
    auto placeOf = [](PropertyOffset offset) {
        return static_cast<uint8_t>(isValidOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset : 0);
    };
    static_assert(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + JSFinalObject::maxInlineCapacity <= UINT8_MAX);
    *place = {
        more,
        static_cast<uint64_t>(ofCopy->typeInfoBlob()) << 32 | ofCopy->id().bits(),
        subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(ofCopy->inlineCapacity()), AllocatorForMode::EnsureAllocator).localAllocator(),
        structure->id().bits(),
        static_cast<uint8_t>(copied),
        static_cast<uint8_t>(ofCopy->inlineCapacity() - copied),
        { placeOf(plan.offsets[0]), placeOf(plan.offsets[1]) },
    };
    instance->spreadCopyCacheIsDirty = true;
}

static NEVER_INLINE void cacheCopyOfRecordWithExtraProperties(Instance* instance, JSGlobalObject* globalObject, VM& vm, Typed::Object& record)
{
    Typed::State& state = *instance->typed;
    Structure* structure = record.structure();
    const Typed::LayoutData& layout = record.layout();
    size_t offset = layout.offsetOfExtraProperties();
    StructureID named = structure->extraPropertiesStructureID();
    if (!named || !offset || layout.hasExtension || state.isCounting() || state.data.isCompiledForAudit || structure->storedPrototype() != JSValue(globalObject->objectPrototype()))
        return;
    Structure* ofExtraProperties = named.decode();
    if (ofExtraProperties->outOfLineCapacity() || ofExtraProperties->typeInfoBlob() != JSFinalObject::defaultTypeInfoBlob() || !ofExtraProperties->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(ofExtraProperties))
        return;
    uint64_t narrowNumbers = 0;
    for (auto& field : std::span { layout.fields(), layout.numberOfFields }) {
        if (field.flags & Typed::fieldHoldsLazyClosure)
            return;
        if (field.store() != Typed::Store::Int32OrNumberBox)
            continue;
        size_t place = (field.offset - Typed::offsetOfFirstField) / sizeof(int32_t);
        if (place >= 64)
            return;
        narrowNumbers |= 1ull << place;
    }
    size_t size = layout.sizeWithSpecializationCapacity();
    size_t copied = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(std::max<size_t>(layout.size, offset + Typed::sizeOfExtraPropertiesWord));
    size_t cleared = std::max(WTF::roundUpToMultipleOf<sizeof(uint64_t)>(size), copied) - copied;
    void* allocator = vm.cellSpace().allocatorFor(std::max(size, copied), AllocatorForMode::EnsureAllocator).localAllocator();
    if (!allocator || copied / sizeof(uint64_t) - 1 > UINT8_MAX || cleared / sizeof(uint64_t) > UINT8_MAX)
        return;
    Instance::SpreadCopyCacheEntry* pair = &instance->spreadCopyCache[Instance::spreadCopyCacheIndex(structure->id().bits(), 0)];
    auto canBeTaken = [&](const Instance::SpreadCopyCacheEntry& place) {
        return !place.structureID || place.structureID == structure->id().bits();
    };
    Instance::SpreadCopyCacheEntry* place = canBeTaken(pair[0]) ? &pair[0] : canBeTaken(pair[1]) ? &pair[1] : nullptr;
    if (!place) {
        instance->keyedTableRandomState = instance->keyedTableRandomState * 1664525u + 1013904223u;
        if (instance->keyedTableRandomState >> 30)
            return;
        place = &pair[instance->keyedTableRandomState >> 29 & 1];
    }
    *place = {
        narrowNumbers,
        static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits(),
        allocator,
        structure->id().bits(),
        static_cast<uint8_t>(copied / sizeof(uint64_t) - 1),
        static_cast<uint8_t>(cleared / sizeof(uint64_t)),
        { 0, 0 },
    };
    instance->spreadCopyCacheIsDirty = true;
}

static ALWAYS_INLINE JSObject* cloneObjectInline(Instance* instance, JSGlobalObject* globalObject, VM& vm, EncodedJSValue source, bool keepsLayout)
{
    if (Typed::Object* record = keepsLayout ? instance->typed->compiledObject(JSValue::decode(source)) : nullptr; record && record->layout().kind == Typed::LayoutKind::Record) {
        if (record->hasExtraProperties())
            cacheCopyOfRecordWithExtraProperties(instance, globalObject, vm, *record);
        return record->clone(globalObject);
    }
    if (JSValue value = JSValue::decode(source); value.isCell() && value.asCell()->type() == FinalObjectType && !asObject(value)->butterfly()) {
        Structure* structure = value.asCell()->structure();
        Instance::CloneAndExtendPlan plan;
        if (auto* known = instance->cloneAndExtendPlan(structure, 0))
            plan = *known;
        else {
            DeferGC deferGC(vm);
            plan = *instance->setCloneAndExtendPlan(structure, 0, planCloneAndExtend(instance, globalObject, vm, structure, 0));
        }
        cacheSpreadCopy(instance, vm, asObject(value), structure, 0, plan);
        if (Structure* ofCopy = plan.structure; ofCopy && structure->storedPrototype() != JSValue(globalObject->objectPrototype())) {
            if (ofCopy->inlineCapacity() != structure->inlineCapacity())
                return copyWithAllPropertiesInline(vm, asObject(value), structure, ofCopy);
            return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, ofCopy, nullptr, asObject(value)->inlineStorage());
        }
    }
    return cloneObjectForSpread(globalObject, JSValue::decode(source));
}

[[maybe_unused]] static NEVER_INLINE JSObject* cloneObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, EncodedJSValue source, bool keepsLayout)
{
    return cloneObjectInline(instance, globalObject, vm, source, keepsLayout);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObject, JSCell*, (Instance* instance, EncodedJSValue source, uint32_t keepsLayout))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, cloneObjectInline(instance, globalObject, vm, source, keepsLayout));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObjectWithSubtypeCheck, JSCell*, (Instance* instance, EncodedJSValue source, uint32_t supertypeShape, uint32_t lastSubtypeShape, uint32_t subtypeShapeCount))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Typed::State& state = *instance->typed;
    Typed::Object* record = state.compiledObject(JSValue::decode(source));
    bool isSubtype = record && lastSubtypeShape - record->shape() < subtypeShapeCount;
    if (record && !isSubtype) {
        if (Typed::Object* copy = record->tryCopyAs(globalObject, state.layoutForShape(supertypeShape), Typed::CopyNamesKind::StoredNext, { }))
            OPERATION_RETURN(scope, copy);
    }
    OPERATION_RETURN(scope, cloneObjectInline(instance, globalObject, vm, source, isSubtype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObjectAs, JSCell*, (Instance* instance, EncodedJSValue source, uint64_t target))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSValue value = JSValue::decode(source); value.isCell() && value.asCell()->type() == FinalObjectType && !asObject(value)->butterfly()) {
        if (Typed::Object* copy = instance->typed->tryCloneAs(vm, asObject(value), target))
            OPERATION_RETURN(scope, copy);
    }
    OPERATION_RETURN(scope, cloneObject(instance, globalObject, vm, source, false));
}

static PropertyOffset offsetOfPropertyInOrder(Structure* structure, size_t place)
{
    size_t inlineCapacity = structure->inlineCapacity();
    return static_cast<PropertyOffset>(place < inlineCapacity ? place : firstOutOfLineOffset + (place - inlineCapacity));
}

static Instance::CloneAndExtendPlan planCloneAndExtend(Instance* instance, JSGlobalObject* globalObject, VM& vm, Structure* source, uint64_t more)
{
    if (!source->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(source, false) || (!source->didTransition() && isValidOffset(source->maxOffset())))
        return { };
    if (source->realm() != globalObject)
        return { };
    bool hasObjectPrototype = source->storedPrototype() == JSValue(globalObject->objectPrototype());
    std::array<UniquedStringImpl*, 2> defined { nullptr, nullptr };
    unsigned needed = more >> 48;
    for (unsigned i = 0; i < 2; ++i) {
        if (uint32_t number = (more >> (24 * i)) & 0xffffff) {
            defined[i] = nameAt(instance, number - 1).impl();
            if (parseIndex(*defined[i]))
                return { };
            needed += !isValidOffset(source->get(vm, defined[i])) && defined[i] != defined[0];
        }
    }
    needed += defined[0] && !isValidOffset(source->get(vm, defined[0]));
    Vector<UniquedStringImpl*, 16> names;
    bool isInOrder = true;
    source->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        isInOrder &= !entry.attributes() && entry.offset() == offsetOfPropertyInOrder(source, names.size()) && !(entry.key()->isSymbol() && static_cast<SymbolImpl*>(entry.key())->isPrivate());
        names.append(entry.key());
        return isInOrder;
    });
    if (!source->isQuickPropertyAccessAllowedForEnumeration() && (!isInOrder || names.size() != source->inlineSize() + source->outOfLineSize()))
        return { };
    needed += names.size();
    Instance::CloneAndExtendPlan result { hasObjectPrototype ? source : nullptr, { } };
    if (isInOrder && (needed > source->inlineCapacity() || !hasObjectPrototype) && needed <= JSFinalObject::maxInlineCapacity) {
        size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(needed));
        unsigned inlineCapacity = std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
        Structure* empty = emptyLiteralStructureForCopy(globalObject, vm, inlineCapacity);
        if (Structure* wider = names.isEmpty() ? empty : instance->literalStructure(empty, names.span()))
            result.structure = wider;
    }
    if (!result.structure)
        return { };
    result.offsets = { invalidOffset, invalidOffset };
    for (unsigned i = 0; i < 2 && defined[i]; ++i) {
        PropertyOffset offset = result.structure->get(vm, defined[i]);
        if (!isValidOffset(offset)) {
            Structure* next = Structure::addPropertyTransitionToExistingStructure(result.structure, defined[i], 0, offset);
            if (!next) {
                DeferredStructureTransitionWatchpointFire deferred(vm, result.structure);
                next = Structure::addNewPropertyTransition(vm, result.structure, defined[i], 0, offset, PutPropertySlot::UnknownContext, &deferred);
            }
            if (storageAfterAddition(result.structure, next, offset) != StorageAfterAddition::Stays)
                return { };
            result.structure = next;
        }
        result.offsets[i] = offset;
    }
    return result;
}

static JSObject* tryCopyExcluding(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* source, std::span<const EncodedJSValue> keys)
{
    Structure* structure = source->structure();
    if (structure->isDictionary())
        return nullptr;
    Vector<UniquedStringImpl*, 8> excluded;
    for (EncodedJSValue encodedKey : keys) {
        JSValue key = JSValue::decode(encodedKey);
        const StringImpl* impl = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
        if (!impl || !impl->isAtom())
            return nullptr;
        excluded.append(static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl)));
    }
    DeferGC deferGC(vm);
    auto* plan = instance->copyExcluding(structure, excluded.span());
    if (!plan) {
        Instance::CopyExcluding made;
        made.excluded.appendRange(excluded.begin(), excluded.end());
        bool isEligible = structure->canPerformFastPropertyEnumerationCommon() && checkStructureForClone(structure)
            && structure->realm() == globalObject && structure->storedPrototype() == JSValue(globalObject->objectPrototype());
        if (isEligible) {
            Vector<UniquedStringImpl*, 16> names;
            structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
                if (!excluded.contains(entry.key())) {
                    names.append(entry.key());
                    made.offsets.append(entry.offset());
                }
                return true;
            });
            if (names.size() <= JSFinalObject::maxInlineCapacity) {
                Structure* empty = emptyLiteralStructureForCopy(globalObject, vm, std::max<unsigned>(names.size(), JSFinalObject::defaultInlineCapacity));
                made.structure = names.isEmpty() ? empty : instance->literalStructure(empty, names.span());
            }
        }
        plan = instance->setCopyExcluding(structure, WTF::move(made));
    }
    if (!plan->structure)
        return nullptr;
    JSFinalObject* rest = JSFinalObject::create(vm, plan->structure);
    auto* to = rest->inlineStorageUnsafe();
    for (unsigned i = 0; i < plan->offsets.size(); ++i)
        to[i].setWithoutWriteBarrier(source->getDirect(plan->offsets[i]));
    return rest;
}

static JSFinalObject* copyWithAllPropertiesInline(VM& vm, JSObject* object, Structure* structure, Structure* ofCopy)
{
    JSFinalObject* copy = JSFinalObject::create(vm, ofCopy);
    auto* to = copy->inlineStorageUnsafe();
    auto* from = object->inlineStorageUnsafe();
    size_t inlineSize = structure->inlineSize();
    for (size_t place = 0; place < inlineSize; ++place)
        to[place].setWithoutWriteBarrier(from[place].get());
    size_t outOfLineSize = structure->outOfLineSize();
    for (size_t place = 0; place < outOfLineSize; ++place)
        to[inlineSize + place].setWithoutWriteBarrier(object->getDirect(static_cast<PropertyOffset>(firstOutOfLineOffset + place)));
    return copy;
}

static JSObject* withAllPropertiesInline(Instance* instance, JSObject* object)
{
    Structure* structure = object->structure();
    if (object->type() != FinalObjectType || !structure->outOfLineSize() || hasIndexedProperties(structure->indexingType()))
        return object;
    JSGlobalObject* globalObject = instance->globalObject;
    if (structure->storedPrototype() != JSValue(globalObject->objectPrototype()))
        return object;
    VM& vm = globalObject->vm();
    Instance::CloneAndExtendPlan plan;
    if (auto* known = instance->cloneAndExtendPlan(structure, 0))
        plan = *known;
    else {
        DeferGC deferGC(vm);
        plan = *instance->setCloneAndExtendPlan(structure, 0, planCloneAndExtend(instance, globalObject, vm, structure, 0));
    }
    if (!plan.structure || plan.structure->inlineCapacity() == structure->inlineCapacity())
        return object;
    return copyWithAllPropertiesInline(vm, object, structure, plan.structure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFinishObjectLiteral, JSCell*, (Instance* instance, JSCell* object))
{
    return withAllPropertiesInline(instance, asObject(object));
}

JSObject* withAllPropertiesInline(JSGlobalObject* globalObject, JSObject* object)
{
    Instance* instance = globalObject->aotInstance();
    return instance ? withAllPropertiesInline(instance, object) : object;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObjectAndExtend, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint64_t more, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue source = JSValue::decode(encodedSource);
    std::array<JSValue, 2> values { JSValue::decode(first), JSValue::decode(second) };
    if (source.isCell() && source.asCell()->type() == FinalObjectType) {
        JSObject* object = asObject(source);
        Structure* structure = object->structure();
        Instance::CloneAndExtendPlan plan;
        if (auto* known = instance->cloneAndExtendPlan(structure, more))
            plan = *known;
        else {
            DeferGC deferGC(vm);
            plan = *instance->setCloneAndExtendPlan(structure, more, planCloneAndExtend(instance, globalObject, vm, structure, more));
        }
        cacheSpreadCopy(instance, vm, object, structure, more, plan);
        if (Structure* ofCopy = plan.structure) {
            auto offsets = plan.offsets;
            JSFinalObject* copy;
            if (ofCopy->inlineCapacity() != structure->inlineCapacity())
                copy = copyWithAllPropertiesInline(vm, object, structure, ofCopy);
            else if (unsigned propertyCapacity = object->butterfly() ? structure->outOfLineCapacity() : 0) {
                DeferGC deferGC(vm);
                Butterfly* butterfly = Butterfly::createUninitialized(vm, nullptr, 0, propertyCapacity, false, 0);
                memcpy(butterfly->propertyStorage() - propertyCapacity, object->butterfly()->propertyStorage() - propertyCapacity, propertyCapacity * sizeof(EncodedJSValue));
                copy = JSFinalObject::createWithButterflyCopyingInlineStorage(vm, ofCopy, butterfly, object->inlineStorage());
            } else
                copy = JSFinalObject::createWithButterflyCopyingInlineStorage(vm, ofCopy, nullptr, object->inlineStorage());
            for (unsigned i = 0; i < 2 && isValidOffset(offsets[i]); ++i)
                copy->locationForOffset(offsets[i])->setWithoutWriteBarrier(values[i]);
            OPERATION_RETURN(scope, copy);
        }
    } else if (const Typed::LayoutData* layout = source.isObject() ? source.asCell()->structure()->compiledLayout() : nullptr; layout && Typed::isObjectLayout(layout->kind)) {
        auto* record = uncheckedDowncast<Typed::Object>(source.asCell());
        if (Structure* structure = record->plainCopyStructure(globalObject)) {
            Instance::CloneAndExtendPlan plan;
            if (auto* known = instance->cloneAndExtendPlan(structure, more))
                plan = *known;
            else {
                DeferGC deferGC(vm);
                plan = *instance->setCloneAndExtendPlan(structure, more, planCloneAndExtend(instance, globalObject, vm, structure, more));
            }
            if (JSFinalObject* copy = plan.structure && !plan.structure->outOfLineCapacity() ? record->tryCopyAsPlainObjectWithCapacity(globalObject, plan.structure) : nullptr) {
                for (unsigned i = 0; i < 2 && isValidOffset(plan.offsets[i]); ++i)
                    copy->putDirectOffset(vm, plan.offsets[i], values[i]);
                OPERATION_RETURN(scope, copy);
            }
        }
    }
    JSObject* copy = cloneObjectForSpread(globalObject, source);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    for (unsigned i = 0; i < 2; ++i) {
        if (uint32_t number = (more >> (24 * i)) & 0xffffff) {
            createDataProperty(globalObject, copy, nameAt(instance, number - 1), values[i]);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObjectAndExtendWithNumber, JSCell*, (Instance* instance, EncodedJSValue source, uint64_t more, double first))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* copy = cloneObjectForSpread(globalObject, JSValue::decode(source));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    createDataProperty(globalObject, copy, nameAt(instance, (more & 0xffffff) - 1), jsNumber(purifyNaN(first)));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryCopyRecordAs, JSCell*, (Instance* instance, uint32_t shape, uint32_t namesKind, uint32_t count, const EncodedJSValue* values))
{
    Typed::State& state = *instance->typed;
    Typed::Object* record = state.compiledObject(JSValue::decode(values[0]));
    return record ? record->tryCopyAs(instance->globalObject, state.layoutForShape(shape), static_cast<Typed::CopyNamesKind>(namesKind), std::span { values + 1, count - 1 }) : nullptr;
}

static Instance::CloneAndExtendPlan planExtraPropertiesCopy(Instance* instance, JSGlobalObject* globalObject, VM& vm, const Typed::LayoutData& layout, Structure* source)
{
    if (!source->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(source) || source->realm() != globalObject)
        return { };
    Typed::State& state = *instance->typed;
    Vector<UniquedStringImpl*, 16> names;
    bool isEligible = true;
    source->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        isEligible &= !entry.attributes() && entry.offset() == offsetOfPropertyInOrder(source, names.size()) && !entry.key()->isSymbol()
            && entry.key() != vm.propertyNames->underscoreProto.impl() && !state.field(vm, layout, entry.key());
        names.append(entry.key());
        return isEligible;
    });
    if (!isEligible || names.size() > JSFinalObject::maxInlineCapacity)
        return { };
    if (names.isEmpty())
        return { globalObject->nullPrototypeObjectStructure(), { } };
    Structure* empty = names.size() <= JSFinalObject::defaultInlineCapacity ? globalObject->nullPrototypeObjectStructure() : JSFinalObject::createStructure(vm, globalObject, jsNull(), names.size());
    return { instance->literalStructure(empty, names.span()), { } };
}

static Typed::Object* tryCopyRecordWithExtraProperties(Instance* instance, const Typed::LayoutData& layout, Typed::Object& record, JSValue more)
{
    if (!more.isCell() || more.asCell()->type() != FinalObjectType)
        return nullptr;
    constexpr uint64_t extraPropertiesTag = 0xffffull << 48;
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    JSObject* source = asObject(more);
    Structure* structure = source->structure();
    DeferGC deferGC(vm);
    Instance::CloneAndExtendPlan plan;
    if (auto* known = instance->cloneAndExtendPlan(structure, extraPropertiesTag | layout.shape))
        plan = *known;
    else
        plan = *instance->setCloneAndExtendPlan(structure, extraPropertiesTag | layout.shape, planExtraPropertiesCopy(instance, globalObject, vm, layout, structure));
    if (!plan.structure)
        return nullptr;
    Typed::Object* copy = record.tryCopyAs(globalObject, layout, Typed::CopyNamesKind::Excluded, { });
    if (!copy || plan.structure == globalObject->nullPrototypeObjectStructure())
        return copy;
    copy->setExtraProperties(vm, copyWithAllPropertiesInline(vm, source, structure, plan.structure));
    return copy;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryMergeRecordsAs, JSCell*, (Instance* instance, uint32_t shape, EncodedJSValue first, EncodedJSValue second))
{
    Typed::State& state = *instance->typed;
    Typed::Object* one = state.compiledObject(JSValue::decode(first));
    Typed::Object* other = state.compiledObject(JSValue::decode(second));
    if (one && !other)
        return tryCopyRecordWithExtraProperties(instance, state.layoutForShape(shape), *one, JSValue::decode(second));
    return one && other ? Typed::Object::tryMergeAs(instance->globalObject, state.layoutForShape(shape), *one, *other) : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryCopyRecordAsAt, JSCell*, (Instance* instance, Slot* cache, uint32_t firstShape, uint32_t secondShape, uint32_t shape, uint32_t namesKind, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance);
    Typed::State& state = *instance->typed;
    Typed::Object* record = state.compiledObject(JSValue::decode(values[0]));
    if (!record && !JSValue::decode(values[0]).isCell() && static_cast<Typed::CopyNamesKind>(namesKind) == Typed::CopyNamesKind::StoredNext) {
        Typed::Object* made = Typed::Object::tryCreateAs(globalObject, state, state.layoutForShape(shape), std::span { values + 1, count - 1 });
        if (made && !SharedData::contains(cache) && !(made->orderWord() & ~Typed::orderAndCountMask) && Typed::orderOf(made->orderWord()) != Typed::keysAreInLayoutOrder) {
            Slot& slot = cache[!!firstShape + !!secondShape + 1];
            slot.offset = made->orderWord();
            slot.pointer = std::bit_cast<void*>(static_cast<uintptr_t>(1));
        }
        OPERATION_RETURN(scope, made);
    }
    if (!record)
        OPERATION_RETURN(scope, nullptr);
    uint64_t words = record->orderAndShapeWords();
    uint32_t recordShape = record->layout().shape;
    bool neverCopyable = false;
    Typed::Object* copy = record->tryCopyAs(globalObject, state.layoutForShape(shape), static_cast<Typed::CopyNamesKind>(namesKind), std::span { values + 1, count - 1 }, &neverCopyable);
    bool isKnown = recordShape == firstShape || recordShape == secondShape;
    if (copy && isKnown && !SharedData::contains(cache) && !((static_cast<uint32_t>(words) | copy->orderWord()) & ~Typed::orderAndCountMask) && Typed::orderOf(static_cast<uint32_t>(words)) != Typed::keysAreInLayoutOrder && Typed::orderOf(copy->orderWord()) != Typed::keysAreInLayoutOrder) {
        Slot& slot = cache[recordShape != firstShape];
        slot.offset = copy->orderWord();
        slot.pointer = std::bit_cast<void*>(words);
        instance->cacheCopyKeyOrder(words, cache, copy->orderWord() | (recordShape != firstShape ? Instance::copyKeyOrderFromSecondSource : 0));
    } else if (neverCopyable && !SharedData::contains(cache))
        instance->cacheCopyKeyOrder(words, cache, Instance::copyAlwaysFails);
    OPERATION_RETURN(scope, copy);
}

static bool tryCopyPropertiesOntoRecord(VM& vm, Typed::Object& record, JSObject* source)
{
    Typed::State& state = record.state();
    Structure* structure = source->structure();
    if (state.data.isCompiledForAudit || record.layout().kind != Typed::LayoutKind::Record || record.orderWord() & (Typed::orderWordIsFrozen | Typed::orderWordIsSealed | Typed::orderWordIsNotExtensible))
        return false;
    if (source->type() != FinalObjectType || source->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumeration())
        return false;
    bool isInlinedBlock = true;
    EnsureStillAliveScope structureScope(structure);
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if (entry.attributes() & PropertyAttribute::DontEnum)
            return true;
        const Typed::FieldData* field = entry.key()->isSymbol() ? nullptr : state.field(vm, record.layout(), entry.key());
        isInlinedBlock = field && !record.isDiscriminant(*field);
        if (!isInlinedBlock)
            return false;
        bool wasThere = !!record.read(*field);
        isInlinedBlock = record.write(vm, *field, source->getDirect(entry.offset()));
        if (isInlinedBlock && !wasThere)
            record.didCreateField(state, *field);
        return isInlinedBlock;
    });
    return isInlinedBlock;
}

static void copyDataProperties(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const UncheckedKeyHashSet<RefPtr<UniquedStringImpl>>& excluded)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (source.isUndefinedOrNull())
        return;
    JSObject* from = source.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType && tryCopyDataProperties(instance, vm, target, from, nullptr, "operationAOTTypedLanguageOperation"))
        return;
    if (excluded.isEmpty() && target->type() == FinalObjectType && tryCopyRecordDataProperties(instance, vm, target, from, "operationAOTTypedLanguageOperation"))
        return;
    if (auto* ordinary = dynamicDowncast<JSFinalObject>(target); ordinary && excluded.isEmpty() && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible()) {
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer propertyValues;
        bool isDone = objectAssignFast(globalObject, ordinary, from, properties, propertyValues);
        RETURN_IF_EXCEPTION(scope, void());
        if (isDone)
            return;
    }
    if (auto* record = dynamicDowncast<Typed::Object>(target); record && excluded.isEmpty()) {
        if (auto* other = dynamicDowncast<Typed::Object>(from); other && record->tryCopyFieldsOf(vm, *other))
            return;
        if (tryCopyPropertiesOntoRecord(vm, *record, from))
            return;
    }
    if (auto* ordinary = dynamicDowncast<JSFinalObject>(target); ordinary && from->structure()->compiledLayout() && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible()) {
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer propertyValues;
        EnsureStillAliveScope fromScope(from);
        bool isWalked = Typed::forEachCompiledObjectOwnProperty(from, [&](UniquedStringImpl& name, JSValue value) {
            if (!excluded.contains(&name)) {
                properties.append(&name);
                propertyValues.appendWithCrashOnOverflow(value);
            }
            return true;
        });
        if (isWalked) {
            if (properties.size())
                ordinary->putOwnDataPropertyBatching(vm, properties.mutableSpan().data(), propertyValues.data(), properties.size());
            return;
        }
    }
    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    from->methodTable()->getOwnPropertyNames(from, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& name : names) {
        if (excluded.contains(name.impl()))
            continue;
        PropertySlot slot(from, PropertySlot::InternalMethodType::GetOwnProperty);
        bool has = from->methodTable()->getOwnPropertySlot(from, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (!has || slot.attributes() & PropertyAttribute::DontEnum)
            continue;
        bool needsReread = slot.isTaintedByOpaqueObject() && !from->inherits<Typed::ModuleNamespace>();
        JSValue value = needsReread ? from->get(globalObject, name) : slot.getValue(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        createDataProperty(globalObject, target, name, value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCopyDataProperties, void, (Instance* instance, JSCell* target, EncodedJSValue source))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    countOperationAtSite(instance, callFrame, "CopyDataProperties");
    scope.release();
    copyDataProperties(instance, globalObject, vm, asObject(target), JSValue::decode(source), { });
    OPERATION_RETURN(scope);
}

static Instance::SpreadLiteralPlan planSpreadLiteral(Instance* instance, JSGlobalObject* globalObject, VM& vm, const void* site, const uint32_t* row, unsigned numberOfSources, const Instance::SpreadLiteralPlan::Sources& sources)
{
    Instance::SpreadLiteralPlan plan { site, sources, nullptr, { } };
    unsigned count = row[0] & 0xffff;
    unsigned more = row[0] >> 16;
    Vector<UniquedStringImpl*, 16> names;
    UncheckedKeyHashMap<UniquedStringImpl*, unsigned> places;
    auto add = [&](UniquedStringImpl* name, uint32_t value, PropertyOffset from) {
        size_t place = notFound;
        if (names.size() <= 16)
            place = names.find(name);
        else if (auto found = places.find(name); found != places.end())
            place = found->value;
        if (place != notFound) {
            plan.moves[place] = { value, from };
            return;
        }
        names.append(name);
        plan.moves.append({ value, from });
        if (names.size() == 17) {
            for (unsigned i = 0; i < 17; ++i)
                places.add(names[i], i);
        } else if (names.size() > 17)
            places.add(name, names.size() - 1);
    };
    unsigned nextSource = 0;
    unsigned nextField = numberOfSources;
    bool isEligible = true;
    for (unsigned i = 0; i < count && isEligible; ++i) {
        if (row[1 + i] != Instance::SpreadLiteralPlan::isSpread) {
            add(nameAt(instance, row[1 + i]).impl(), nextField++, invalidOffset);
            continue;
        }
        unsigned which = nextSource++;
        Structure* source = sources[which];
        if (!source)
            continue;
        isEligible = !source->isDictionary() && source->canPerformFastPropertyEnumerationCommon() && !hasIndexedProperties(source->indexingType()) && (!source->typeInfo().hasStaticPropertyTable() || source->staticPropertiesReified());
        if (!isEligible)
            break;
        source->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (PropertyName(entry.key()).isPrivateName() || (entry.attributes() & PropertyAttribute::DontEnum))
                return true;
            isEligible = !entry.key()->isSymbol() && entry.key() != vm.propertyNames->underscoreProto.impl();
            add(entry.key(), which, entry.offset());
            return isEligible;
        });
    }
    if (!isEligible || names.isEmpty() || names.size() + more > JSFinalObject::maxInlineCapacity)
        return plan;
    size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(names.size() + more));
    unsigned inlineCapacity = std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
    plan.structure = instance->literalStructure(emptyLiteralStructureForCopy(globalObject, vm, inlineCapacity), names.span());
    return plan;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewObjectWithSpreads, JSCell*, (Instance* instance, uint32_t membersPlanOffset, uint32_t numberOfSources, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance);
    using Plan = Instance::SpreadLiteralPlan;
    Plan::Sources sources { };
    bool mayBePlanned = globalObject == instance->globalObject;
    for (unsigned i = 0; i < numberOfSources; ++i) {
        if (JSValue source = JSValue::decode(values[i]); source.isCell()) {
            mayBePlanned &= source.asCell()->type() == FinalObjectType;
            sources[i] = source.asCell()->structure();
        }
    }
    auto rowOfSite = [&] {
        return caller(instance, callFrame).info().function()->plans() + membersPlanOffset;
    };
    if (mayBePlanned) {
        DeferGC deferGC(vm);
        const void* site = removeCodePtrTag(callFrame->rawReturnPC());
        const Plan* plan = instance->spreadLiteralPlan(site, sources);
        if (!plan) [[unlikely]]
            plan = instance->addSpreadLiteralPlan(planSpreadLiteral(instance, globalObject, vm, site, rowOfSite(), numberOfSources, sources));
        if (plan->structure) {
            countOperationNamed(instance, __func__, "planned");
            JSFinalObject* object = JSFinalObject::create(vm, plan->structure);
            auto* to = object->inlineStorageUnsafe();
            for (auto& move : plan->moves) {
                JSValue value = JSValue::decode(values[move.value]);
                (to++)->setWithoutWriteBarrier(isValidOffset(move.from) ? asObject(value)->getDirect(move.from) : value);
            }
            OPERATION_RETURN(scope, object);
        }
    }
    countOperationNamed(instance, __func__, "member by member");
    const uint32_t* row = rowOfSite();
    unsigned count = row[0] & 0xffff;
    unsigned nextSource = 0;
    unsigned nextField = numberOfSources;
    JSObject* object = constructEmptyObject(globalObject);
    CallSiteOverride where(*instance, callFrame->callerFrame());
    for (unsigned i = 0; i < count; ++i) {
        where.setItem(i);
        if (row[1 + i] == Plan::isSpread)
            copyDataProperties(instance, globalObject, vm, object, JSValue::decode(values[nextSource++]), { });
        else
            createDataProperty(globalObject, object, nameAt(instance, row[1 + i]), JSValue::decode(values[nextField++]));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, object);
}

static ClonedArguments* createArguments(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSArray* array)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = array->length();
    ClonedArguments* result = tryCreateArgumentsFast(instance, vm, globalObject, array, length);
    countOperationNamed(instance, "CreateArguments", result ? "made at once" : "made step by step");
    if (!result) {
        result = ClonedArguments::createEmpty(vm, globalObject, globalObject->clonedArgumentsStructure(), nullptr, length, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        for (unsigned i = 0; i < length; ++i) {
            result->putDirectIndex(globalObject, i, array->getIndex(globalObject, i));
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        result->putDirectAccessor(globalObject, vm.propertyNames->callee, globalObject->throwTypeErrorArgumentsCalleeGetterSetter(), argumentsCalleeAttributes);
        result->putDirect(vm, vm.propertyNames->iteratorSymbol, globalObject->arrayProtoValuesFunction(), static_cast<unsigned>(PropertyAttribute::DontEnum));
    }
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateArguments, JSCell*, (Instance* instance, JSCell* list))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    countOperationAtSite(instance, callFrame, "CreateArguments");
    OPERATION_RETURN(scope, createArguments(instance, globalObject, vm, uncheckedDowncast<JSArray>(list)));
}

static JSCell* newGenerator(Instance* instance, JSGlobalObject* globalObject, VM& vm, Typed::Closure* resumeClosure, JSValue closure, bool isAsync)
{
    auto& known = closure.isCell() ? instance->typed->generatorStructure(vm, uncheckedDowncast<Typed::Closure>(closure), isAsync) : instance->typed->generatorStructure(vm, resumeClosure->functionIndex(), isAsync);
    if (isAsync)
        return Typed::AsyncGenerator::create(vm, globalObject, known.structure, resumeClosure);
    if (!known.offsetOfState) [[unlikely]] {
        known.offsetOfState = Typed::Generator::offsetOfStateIn(resumeClosure);
        if (!instance->iteratorResultHeader) {
            Structure* ofResults = globalObject->iteratorResultObjectStructure();
            instance->iteratorResultHeader = static_cast<uint64_t>(ofResults->typeInfoBlob()) << 32 | ofResults->id().bits();
        }
    }
    return Typed::Generator::create(vm, known.structure, resumeClosure, known.hasUnmodifiedIteration, known.offsetOfState);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewGenerator, JSCell*, (Instance* instance, JSCell* resumeClosure, EncodedJSValue closure, uint32_t isAsync, uint32_t generatorTemplate, uint32_t numberOfTemplates))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCell* created = newGenerator(instance, globalObject, vm, uncheckedDowncast<Typed::Closure>(resumeClosure), JSValue::decode(closure), isAsync);
    if (generatorTemplate < numberOfTemplates)
        instance->typed->setGeneratorTemplate(generatorTemplate, numberOfTemplates, uncheckedDowncast<Typed::Generator>(created)->asTemplate());
    OPERATION_RETURN(scope, created);
}

static JSObject* tryWrapUnmodifiedGenerator(JSGlobalObject* globalObject, VM& vm, JSValue iterable)
{
    auto* generator = dynamicDowncast<Typed::Generator>(iterable);
    if (!generator || generator->structure()->maxOffset() != invalidOffset)
        return nullptr;
    JSObject* prototype = generator->getPrototypeDirect().getObject();
    JSObject* ofGenerators = globalObject->generatorPrototype();
    JSObject* ofIterators = globalObject->iteratorPrototype();
    JSObject* ofObjects = globalObject->objectPrototype();
    if (prototype != ofGenerators) {
        if (!prototype || prototype->type() != FinalObjectType || prototype->structure()->maxOffset() != invalidOffset || prototype->getPrototypeDirect() != JSValue(ofGenerators))
            return nullptr;
    }
    if (ofGenerators->getPrototypeDirect() != JSValue(ofIterators) || ofIterators->getPrototypeDirect() != JSValue(ofObjects))
        return nullptr;
    for (JSObject* shared : { ofGenerators, ofIterators, ofObjects }) {
        if (shared->structure()->typeInfo().overridesGetOwnPropertySlot() || shared->getDirect(vm, vm.propertyNames->asyncIteratorSymbol))
            return nullptr;
    }
    unsigned attributes;
    if (ofGenerators->getDirect(vm, vm.propertyNames->iteratorSymbol) || ofIterators->getDirect(vm, vm.propertyNames->iteratorSymbol, attributes) != JSValue(globalObject->iteratorProtoSymbolIteratorFunction()) || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return nullptr;
    JSValue next = ofGenerators->getDirect(vm, vm.propertyNames->next, attributes);
    if (!next || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return nullptr;
    return JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), generator, next, IterationMode::Generic);
}

static JSValue getAsyncIterator(JSGlobalObject* globalObject, VM& vm, JSValue iterable)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(iterable); generator && generator->hasUnmodifiedIteration(globalObject))
        return generator;
    if (JSObject* wrapper = tryWrapUnmodifiedGenerator(globalObject, vm, iterable))
        return wrapper;
    if (isJSArray(iterable) && globalObject->hasImmutableIntrinsics() && globalObject->isOriginalArrayStructure(iterable.asCell()->structure()) && globalObject->isArrayPrototypeIteratorProtocolFastAndNonObservable()) {
        auto* iterator = JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), asObject(iterable), IterationKind::Values);
        return JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), iterator, jsUndefined(), IterationMode::FastArrayValues);
    }
    JSValue method = iterable.get(globalObject, vm.propertyNames->asyncIteratorSymbol);
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isUndefinedOrNull())
        RELEASE_AND_RETURN(scope, createAsyncFromSyncIteratorForIterable(globalObject, iterable));
    auto callData = JSC::getCallData(method);
    if (callData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, "The value is not async iterable"_s);
        return { };
    }
    JSValue iterator = call(globalObject, method, callData, iterable, ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    if (!iterator.isObject()) {
        throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        return { };
    }
    return iterator;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetAsyncIterator, EncodedJSValue, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getAsyncIterator(globalObject, vm, JSValue::decode(iterable))));
}

static JSValue constructParent(JSGlobalObject* globalObject, JSValue classObject, JSValue newTarget, const ArgList& arguments, JSValue parentIfKnown)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue parent = parentIfKnown ? parentIfKnown : asObject(classObject)->getPrototype(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto constructData = JSC::getConstructData(parent);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
        return { };
    }
    if (auto* errorConstructor = dynamicDowncast<ErrorConstructor>(parent); errorConstructor && errorConstructor->realm() == globalObject && newTarget.isObject() && newTarget != parent) {
        Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, asObject(newTarget), errorConstructor);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, ErrorInstance::create(globalObject, structure, arguments.at(0), arguments.at(1), nullptr, TypeNothing, ErrorType::Error, true, asObject(newTarget)));
    }
    RELEASE_AND_RETURN(scope, construct(globalObject, parent, constructData, arguments, newTarget));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedLanguageOperation, EncodedJSValue, (Instance* instance, uint32_t which, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        static constexpr char names[][40] = {
#define AOT_TYPED_NAME_LANGUAGE_OPERATION(name) #name,
            FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION(AOT_TYPED_NAME_LANGUAGE_OPERATION)
#undef AOT_TYPED_NAME_LANGUAGE_OPERATION
        };
        countOperationNamed(instance, "language-operation", which < std::size(names) ? names[which] : "unknown");
        countOperationAtSite(instance, callFrame, which < std::size(names) ? names[which] : "unknown");
        if (which == static_cast<uint32_t>(Typed::LanguageOperation::AppendSpread) && count > 1)
            countSpreadForCounters(instance, uncheckedDowncast<JSArray>(JSValue::decode(values[0])), JSValue::decode(values[1]));
    }
    auto argument = [&](unsigned index) {
        return index < count ? JSValue::decode(values[index]) : jsUndefined();
    };
    auto copyDataProperties = [&](JSObject* target, JSValue source, const UncheckedKeyHashSet<RefPtr<UniquedStringImpl>>& excluded) {
        AOT::copyDataProperties(instance, globalObject, vm, target, source, excluded);
    };
    switch (static_cast<Typed::LanguageOperation>(which)) {
    case Typed::LanguageOperation::CopyDataProperties:
        copyDataProperties(asObject(argument(0)), argument(1), { });
        OPERATION_RETURN(scope, values[0]);
    case Typed::LanguageOperation::CopyDataPropertiesExcluding: {
        if (argument(0).isUndefinedOrNull()) {
            throwTypeError(globalObject, scope, "Right side of assignment cannot be destructured"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (Typed::Object* record = instance->typed->compiledObject(argument(0))) {
            if (JSObject* rest = record->tryCopyAsPlainObject(globalObject, std::span { values + 1, count - 1 }))
                OPERATION_RETURN(scope, JSValue::encode(rest));
        }
        if (JSValue source = argument(0); count > 1 && source.isCell() && source.asCell()->type() == FinalObjectType) {
            if (JSObject* rest = tryCopyExcluding(instance, globalObject, vm, asObject(source), std::span { values + 1, count - 1 }))
                OPERATION_RETURN(scope, JSValue::encode(rest));
        }
        UncheckedKeyHashSet<RefPtr<UniquedStringImpl>> excluded;
        for (unsigned i = 1; i < count; ++i) {
            auto key = argument(i).toPropertyKey(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            excluded.add(key.impl());
        }
        JSObject* target = constructEmptyObject(globalObject);
        copyDataProperties(target, argument(0), excluded);
        OPERATION_RETURN(scope, JSValue::encode(target));
    }
    case Typed::LanguageOperation::CloneObject:
        break;
    case Typed::LanguageOperation::CloneObjectWith:
        OPERATION_RETURN(scope, JSValue::encode(cloneObjectForSpread(globalObject, argument(0))));
    case Typed::LanguageOperation::CloneObjectAfter: {
        JSObject* target = constructEmptyObject(globalObject);
        for (unsigned i = 1; i + 1 < count; i += 2) {
            auto key = argument(i).toPropertyKey(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            createDataProperty(globalObject, target, key, argument(i + 1));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        copyDataProperties(target, argument(0), { });
        OPERATION_RETURN(scope, JSValue::encode(target));
    }
    case Typed::LanguageOperation::DefineField: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        createDataProperty(globalObject, asObject(argument(0)), key, argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::UnlistFields:
    case Typed::LanguageOperation::ListFields: {
        bool unlists = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::UnlistFields;
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        uint32_t wordBefore = 0;
        if (auto* object = instance->typed->compiledObject(argument(0))) {
            wordBefore = object->orderWord();
            if (const Typed::FieldData* field = object->fieldNamed(*instance->typed, key))
                object->listFieldsDefinedSoFar(*instance->typed, *field, !unlists && argument(2).isTrue(), unlists || (count == 3 && !argument(2).isTrue()));
        }
        OPERATION_RETURN(scope, JSValue::encode(unlists ? jsNumber(static_cast<int32_t>(wordBefore & INT32_MAX)) : jsUndefined()));
    }
    case Typed::LanguageOperation::DefineGetter:
    case Typed::LanguageOperation::DefineSetter:
        defineAccessor(globalObject, argument(0), argument(1), argument(2), argument(3).isTrue(), static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::DefineGetter, argument(4));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::AppendSpread: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        if (tryAppendArray(globalObject, vm, array, argument(1)))
            OPERATION_RETURN(scope, values[0]);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (Typed::CollectionIteration::tryAppendTo(globalObject, array, argument(1))) {
            countOperationNamed(instance, "AppendSpread", "collection");
            OPERATION_RETURN(scope, values[0]);
        }
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (JSValue iterable = argument(1); iterable.isCell() && isTypedArrayType(iterable.asCell()->type())) {
            auto* view = uncheckedDowncast<JSArrayBufferView>(iterable.asCell());
            if (globalObject->isOriginalTypedArrayStructure(view->structure(), view->isResizableOrGrowableShared())) {
                if (tryAppendTypedArray(globalObject, vm, array, view)) {
                    countOperationNamed(instance, "AppendSpread", "typed array");
                    OPERATION_RETURN(scope, values[0]);
                }
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        }
        if (Options::useAOTOperationCounters()) [[unlikely]] {
            countOperationNamed(instance, "AppendSpread", "iterated");
            if (auto* list = dynamicDowncast<JSArray>(argument(1)))
                countOperationNamed(instance, "AppendSpread:an array is iterated", arrayIterationReasonForCounters(list));
        }
        forEachInSpread(globalObject, argument(1), [&](JSValue value) {
            array->push(globalObject, value);
        });
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::AppendHole: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        array->setLength(globalObject, array->length() + 1, true);
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::Apply:
    case Typed::LanguageOperation::CallWithSpread:
    case Typed::LanguageOperation::ConstructWithArray: {
        bool isCall = static_cast<Typed::LanguageOperation>(which) != Typed::LanguageOperation::ConstructWithArray;
        bool isSpread = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::CallWithSpread;
        CallData callData = isCall ? JSC::getCallData(argument(0)) : CallData();
        if (isCall && !isSpread && callData.type == CallData::Type::None) {
            throwIsNotAFunction(globalObject, scope, argument(0));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSValue list = argument(isCall ? 2 : 1);
        MarkedArgumentBuffer arguments;
        auto fitsOnStack = [&](uint64_t length) {
            return length <= maxArguments && vm.ensureJSStackCapacityFor(static_cast<Register*>(currentStackPointer()) - length);
        };
        if (isSpread) {
            forEachInSpread(globalObject, list, [&](JSValue value) {
                arguments.append(value);
            });
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (callData.type == CallData::Type::None) {
                throwIsNotAFunction(globalObject, scope, argument(0));
                OPERATION_RETURN(scope, encodedJSValue());
            }
        } else if (auto* array = dynamicDowncast<JSArray>(list)) [[likely]] {
            if (!fitsOnStack(array->length())) [[unlikely]] {
                throwStackOverflowError(globalObject, scope);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            for (unsigned i = 0; i < array->length(); ++i) {
                arguments.append(array->getIndex(globalObject, i));
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        } else if (!list.isUndefinedOrNull()) {
            if (!list.isObject()) {
                throwTypeError(globalObject, scope, "second argument to Function.prototype.apply must be an Array-like object"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            uint64_t length = toLength(globalObject, asObject(list));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!fitsOnStack(length)) [[unlikely]] {
                throwStackOverflowError(globalObject, scope);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            for (uint64_t index = 0; index < length; ++index) {
                arguments.append(asObject(list)->getIndex(globalObject, index));
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        }
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (isCall)
            OPERATION_RETURN(scope, JSValue::encode(call(globalObject, argument(0), callData, argument(1), arguments)));
        auto constructData = JSC::getConstructData(argument(0));
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, makeString(State::describe(argument(0)), " is not a constructor"_s));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, argument(0), constructData, arguments)));
    }
    case Typed::LanguageOperation::ConstructWith:
        RELEASE_ASSERT(count >= 2);
        OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, argument(0), argument(1).asCell(), count - 2, values + 2)));
    case Typed::LanguageOperation::ConstructWithSpread: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(2));
        Vector<EncodedJSValue, 8> elements;
        MarkedArgumentBuffer keptAlive;
        for (unsigned i = 0; i < array->length(); ++i) {
            JSValue element = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            keptAlive.append(element);
            elements.append(JSValue::encode(element));
        }
        if (keptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, argument(0), argument(1).asCell(), elements.size(), elements.span().data())));
    }
    case Typed::LanguageOperation::MakeClass: {
        auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(classObject && classObject->classData());
        instance->typed->createClass(globalObject, classObject, argument(1), Typed::State::ClassPart::Everything, count > 2 ? argument(2) : JSValue());
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::NewObjectOfClass: {
        auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(classObject && classObject->classData());
        OPERATION_RETURN(scope, JSValue::encode(instance->typed->createClassInstance(vm, *classObject->classData(), asObject(classObject->prototypeProperty(vm)))));
    }
    case Typed::LanguageOperation::DefineMethod: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        PropertyDescriptor descriptor(argument(2), key.isPrivateName() ? PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly : static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(argument(0))->methodTable()->defineOwnProperty(asObject(argument(0)), globalObject, key, descriptor, true);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SetFunctionName: {
        if (auto* slim = dynamicDowncast<Typed::SlimClosure>(argument(0))) {
            RELEASE_ASSERT(instance->typed->closureIfCreated(slim->functionIndex()) != slim && (argument(1).isString() || argument(1).isSymbol()));
            auto key = argument(1).toPropertyKey(globalObject);
            scope.assertNoException();
            slim->setComputedName(vm, key, static_cast<Typed::Format::MethodKind>(argument(2).asInt32()));
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
        auto* closure = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(closure && instance->typed->closureIfCreated(closure->functionIndex()) != closure && (argument(1).isString() || argument(1).isSymbol()));
        if (JSValue own = closure->classData() ? closure->getDirect(vm, vm.propertyNames->name) : JSValue(); own && !own.isString())
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        auto key = argument(1).toPropertyKey(globalObject);
        scope.assertNoException();
        closure->setComputedName(vm, key, static_cast<Typed::Format::MethodKind>(argument(2).asInt32()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SuperGet:
    case Typed::LanguageOperation::SuperSet: {
        bool isGet = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::SuperGet;
        JSValue home = argument(0);
        if (!argument(isGet ? 3 : 4).isTrue()) {
            home = home.get(globalObject, vm.propertyNames->prototype);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        RELEASE_ASSERT(home.isObject());
        JSValue parent = asObject(home)->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!parent.isObject()) {
            throwTypeError(globalObject, scope, "The prototype of the home object is not an object"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isGet) {
            PropertySlot slot(argument(2), PropertySlot::InternalMethodType::Get);
            bool has = asObject(parent)->getPropertySlot(globalObject, key, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            OPERATION_RETURN(scope, JSValue::encode(has ? slot.getValue(globalObject, key) : jsUndefined()));
        }
        PutPropertySlot slot(argument(3), true);
        asObject(parent)->methodTable()->put(asObject(parent), globalObject, key, argument(2), slot);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SuperConstruct:
    case Typed::LanguageOperation::ConstructOn: {
        bool hasExplicitThis = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::ConstructOn;
        JSArray* array = uncheckedDowncast<JSArray>(argument(hasExplicitThis ? 3 : 2));
        MarkedArgumentBuffer arguments;
        for (unsigned i = 0; i < array->length(); ++i) {
            arguments.append(array->getIndex(globalObject, i));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (hasExplicitThis) {
            auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
            RELEASE_ASSERT(classObject && classObject->classData());
            OPERATION_RETURN(scope, JSValue::encode(classObject->callAsConstructor(globalObject, argument(1), argument(2), arguments)));
        }
        OPERATION_RETURN(scope, JSValue::encode(constructParent(globalObject, argument(0), argument(1), arguments, count > 3 ? argument(3) : JSValue())));
    }
    case Typed::LanguageOperation::ConstructorResult:
        OPERATION_RETURN(scope, values[argument(0).isObject() ? 0 : 1]);
    case Typed::LanguageOperation::DerivedConstructorResult:
        if (argument(0).isObject())
            OPERATION_RETURN(scope, values[0]);
        if (!argument(0).isUndefined()) {
            throwTypeError(globalObject, scope, "Cannot return a non-object type in the constructor of a derived class."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, values[1]);
    case Typed::LanguageOperation::RejectPromise:
        uncheckedDowncast<JSPromise>(argument(0))->reject(vm, argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::NewPromise:
    case Typed::LanguageOperation::ResolvePromise:
    case Typed::LanguageOperation::Await:
    case Typed::LanguageOperation::GetNewTarget:
        break;
    case Typed::LanguageOperation::NewGenerator:
    case Typed::LanguageOperation::NewAsyncGenerator:
        OPERATION_RETURN(scope, JSValue::encode(newGenerator(instance, globalObject, vm, uncheckedDowncast<Typed::Closure>(argument(0)), argument(1), static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::NewAsyncGenerator)));
    case Typed::LanguageOperation::StartAsyncGenerator:
        uncheckedDowncast<Typed::AsyncGenerator>(argument(0))->startEagerly(globalObject);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::CreateArguments:
    case Typed::LanguageOperation::GetFromArguments: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        unsigned length = array->length();
        bool isRead = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::GetFromArguments;
        if (auto index = isRead ? argument(1).tryGetAsUint32Index() : std::nullopt)
            OPERATION_RETURN(scope, JSValue::encode(*index < length ? array->getIndex(globalObject, *index) : jsUndefined()));
        ClonedArguments* result = createArguments(instance, globalObject, vm, array);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!isRead)
            OPERATION_RETURN(scope, JSValue::encode(result));
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(result->get(globalObject, key)));
    }
    case Typed::LanguageOperation::CreateMappedArguments: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        unsigned length = array->length();
        unsigned capacity = std::max(length, static_cast<unsigned>(argument(2).asInt32()));
        DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), length, capacity);
        result->setCallee(vm, uncheckedDowncast<JSFunction>(argument(1)));
        for (unsigned i = 0; i < length; ++i) {
            JSValue value = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            result->argument(DirectArgumentsOffset(i)).set(vm, result, value);
        }
        OPERATION_RETURN(scope, JSValue::encode(result));
    }
    case Typed::LanguageOperation::GetMappedParameter:
        OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<DirectArguments>(argument(0))->argument(DirectArgumentsOffset(argument(1).asInt32())).get()));
    case Typed::LanguageOperation::PutMappedParameter: {
        DirectArguments* object = uncheckedDowncast<DirectArguments>(argument(0));
        object->argument(DirectArgumentsOffset(argument(1).asInt32())).set(vm, object, argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::GetAsyncIterator:
        OPERATION_RETURN(scope, JSValue::encode(getAsyncIterator(globalObject, vm, argument(0))));
    case Typed::LanguageOperation::GetIterator: {
        if (auto* generator = dynamicDowncast<Typed::Generator>(argument(0)); generator && generator->hasUnmodifiedIteration(globalObject))
            OPERATION_RETURN(scope, JSValue::encode(generator));
        JSValue method = argument(0).get(globalObject, vm.propertyNames->iteratorSymbol);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto callData = JSC::getCallData(method);
        if (callData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "Spread syntax requires ...iterable[Symbol.iterator] to be a function"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSValue iterator = call(globalObject, method, callData, argument(0), ArgList());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!iterator.isObject()) {
            throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(iterator));
    }
    case Typed::LanguageOperation::RequireIteratorResultObject:
        if (!argument(0).isObject())
            throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::IteratorCloseAndThrowMissingThrowMethod: {
        JSValue method = argument(0).get(globalObject, vm.propertyNames->returnKeyword);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!method.isUndefinedOrNull()) {
            auto callData = JSC::getCallData(method);
            if (callData.type != CallData::Type::None) {
                call(globalObject, method, callData, argument(0), ArgList());
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        }
        throwTypeError(globalObject, scope, "Delegated generator does not have a 'throw' method."_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    case Typed::LanguageOperation::NewDisposableStack:
        OPERATION_RETURN(scope, JSValue::encode(constructEmptyArray(globalObject, nullptr)));
    case Typed::LanguageOperation::AddDisposableResource: {
        JSArray* stack = uncheckedDowncast<JSArray>(argument(0));
        JSValue resource = argument(1);
        bool isAsync = argument(2).isTrue();
        JSValue kind = jsBoolean(isAsync);
        JSValue method = jsUndefined();
        if (!resource.isUndefinedOrNull()) {
            if (!resource.isObject()) {
                throwTypeError(globalObject, scope, "The value of a using declaration must be an object, null or undefined"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            if (isAsync) {
                method = resource.get(globalObject, vm.propertyNames->asyncDisposeSymbol);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
            if (method.isUndefinedOrNull()) {
                method = resource.get(globalObject, vm.propertyNames->disposeSymbol);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
                if (isAsync)
                    kind = jsNumber(0);
            }
            if (!method.isCallable()) {
                throwTypeError(globalObject, scope, "The value of a using declaration has no callable dispose method"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
        } else if (!isAsync)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        stack->push(globalObject, resource);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        stack->push(globalObject, method);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        stack->push(globalObject, kind);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::RecordDisposeError:
        recordDisposeError(globalObject, uncheckedDowncast<JSArray>(argument(0)), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::DisposeNextResource:
        OPERATION_RETURN(scope, JSValue::encode(disposeNextResource(globalObject, uncheckedDowncast<JSArray>(argument(0)))));
    case Typed::LanguageOperation::DisposeResources: {
        JSArray* stack = uncheckedDowncast<JSArray>(argument(0));
        if (JSValue hasError = stack->getDirect(vm, Identifier::fromString(vm, "hasError"_s)); argument(1).isTrue() && !hasError)
            recordDisposeError(globalObject, stack, argument(2));
        while (stack->length()) {
            stack->pop(globalObject);
            scope.assertNoException();
            JSValue method = stack->pop(globalObject);
            scope.assertNoException();
            JSValue resource = stack->pop(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!method.isUndefined()) {
                auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
                call(globalObject, method, JSC::getCallData(method), resource, ArgList());
                if (Exception* exception = catchScope.exception()) [[unlikely]] {
                    JSValue error = exception->value();
                    if (!catchScope.clearExceptionExceptTermination())
                        OPERATION_RETURN(scope, encodedJSValue());
                    recordDisposeError(globalObject, stack, error);
                }
            }
        }
        JSValue hasError = stack->getDirect(vm, Identifier::fromString(vm, "hasError"_s));
        if (hasError && hasError.isTrue()) {
            JSValue error = stack->getDirect(vm, Identifier::fromString(vm, "error"_s));
            if (!argument(1).isTrue() || error != argument(2))
                throwException(globalObject, scope, error);
        }
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::ToESM:
        OPERATION_RETURN(scope, JSValue::encode(toESM(globalObject, instance, argument(0), argument(1).isTrue())));
    case Typed::LanguageOperation::MakeModuleNamespace:
        OPERATION_RETURN(scope, createModuleNamespace(instance, { }, std::span { values, count }));
    case Typed::LanguageOperation::ParseJSON: {
        auto text = asString(argument(0))->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        JSValue parsed = JSONParseWithException(globalObject, text);
        OPERATION_RETURN(scope, JSValue::encode(parsed));
    }
    case Typed::LanguageOperation::DirectEval: {
        if (!argument(0).isString())
            OPERATION_RETURN(scope, values[0]);
        if (!globalObject->evalEnabled()) [[unlikely]] {
            throwException(globalObject, scope, createEvalError(globalObject, globalObject->evalDisabledErrorMessage()));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        auto text = asString(argument(0))->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        JSObject* names = asObject(argument(1));
        static constexpr Latin1Character thisKey[] = { 0, 'e', 'v', 'a', 'l' };
        JSValue thisValue = names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { thisKey })));
        bool isInFunction = !!names->getDirect(vm, Typed::EvalScope::newTargetKey(vm));
        static constexpr Latin1Character fieldKey[] = { 0, 'f', 'i', 'e', 'l', 'd' };
        bool isInField = !!names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { fieldKey })));
        static constexpr Latin1Character sloppyKey[] = { 0, 's', 'l', 'o', 'p', 'p', 'y' };
        bool isSloppy = !!names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { sloppyKey })));
        SourceOrigin origin;
        if (argument(2).isString()) {
            origin = SourceOrigin { URL::fileURLWithFileSystemPath(String { asString(argument(2))->value(globalObject) }) };
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (argument(2).isTrue() || argument(2).isString()) {
            for (ASCIILiteral name : { "require"_s, "__filename"_s, "__dirname"_s }) {
                if (JSValue accessor = names->getDirect(vm, Identifier::fromString(vm, makeString('\0', name))))
                    names->putDirectAccessor(globalObject, Identifier::fromString(vm, name), uncheckedDowncast<GetterSetter>(accessor.asCell()), static_cast<unsigned>(PropertyAttribute::Accessor));
            }
        }
        TDZEnvironment variablesUnderTDZ;
        PrivateNameEnvironment privateNames;
        auto* executable = DirectEvalExecutable::create(globalObject, makeSource(text.data, origin, SourceTaintedOrigin::Untainted), isSloppy ? NoLexicallyScopedFeatures : StrictModeLexicallyScopedFeature, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, isInFunction, isInField ? EvalContextType::InstanceFieldEvalContext : isInFunction ? EvalContextType::FunctionEvalContext : EvalContextType::None, &variablesUnderTDZ, &privateNames);
        if (Exception* thrown = scope.exception()) [[unlikely]] {
            auto* error = dynamicDowncast<ErrorInstance>(thrown->value());
            if (error && error->errorType() == ErrorType::SyntaxError) {
                DECLARE_TOP_EXCEPTION_SCOPE(vm).clearException();
                String said = error->sanitizedMessageString(globalObject);
                static constexpr Latin1Character superKey[] = { 0, 's', 'u', 'p', 'e', 'r' };
                static constexpr Latin1Character privateNamesKey[] = { 0, 'p', 'r', 'i', 'v', 'a', 't', 'e' };
                bool isSuper = said.startsWith("super is not valid in this context"_s) && names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { superKey })));
                bool isPrivateName = said.startsWith("Cannot reference undeclared private "_s) && names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { privateNamesKey })));
                if (isSuper || isPrivateName)
                    throwSyntaxError(globalObject, scope, withAOTErrorNote(isSuper ? "A direct eval does not see `super` in a program compiled ahead of time"_s : "A direct eval does not see the private names (`#x`) of the class around it in a program compiled ahead of time"_s));
                else
                    throwException(globalObject, scope, thrown);
            }
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (!executable->isInStrictContext() && (!executable->variables().empty() || !executable->topLevelFunctionDecls().empty() || !executable->functionHoistingCandidates().empty())) {
            throwSyntaxError(globalObject, scope, withAOTErrorNote("A direct eval in sloppy-mode code declares a variable or a function in the enclosing function, which a program compiled ahead of time does not support"_s));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSScope* around = Typed::EvalScope::create(vm, instance->typed->evalScopeStructure(vm), instance->loader()->moduleScope(), names);
        OPERATION_RETURN(scope, JSValue::encode(vm.interpreter.executeEval(executable, thisValue, around)));
    }
    case Typed::LanguageOperation::GetRequireFunction:
        OPERATION_RETURN(scope, JSValue::encode(hostRequireFunction(globalObject, instance)));
    case Typed::LanguageOperation::MarkCommonJSEntryPoint:
        if (instance->typed->entryPointImports.isEmpty())
            instance->typed->entryPointIsCommonJS = true;
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::RequireExternalModule:
        OPERATION_RETURN(scope, JSValue::encode(requireFromHost(globalObject, instance, argument(0))));
    case Typed::LanguageOperation::GetTemplateObject: {
        JSObject* cooked = asObject(argument(0));
        JSObject* raw = asObject(argument(1));
        objectConstructorFreeze(globalObject, raw);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        cooked->putDirect(vm, vm.propertyNames->raw, raw, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
        objectConstructorFreeze(globalObject, cooked);
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::GetImportMeta: {
        auto& state = *instance->typed;
        if (count && argument(0).isInt32()) {
            JSObject* meta = state.importMetaForEntryPoint(globalObject, argument(0).asInt32());
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (meta)
                OPERATION_RETURN(scope, JSValue::encode(meta));
        } else if (count)
            OPERATION_RETURN(scope, JSValue::encode(state.importMetaForModule(globalObject, asString(argument(0)))));
        if (!state.importMeta) {
            auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
            JSModuleLoader* ownLoader = instance->loader() ? instance->loader() : globalObject->moduleLoader();
            state.importMeta = ownLoader->createImportMetaProperties(globalObject, identifierToJSValue(vm, state.importerKey(vm)), importer, nullptr);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(state.importMeta));
    }
    case Typed::LanguageOperation::ImportExternalModule: {
        auto rejectWithCaughtException = [&] {
            return JSValue::encode(JSPromise::create(vm, globalObject->promiseStructure())->rejectWithCaughtException(vm, scope));
        };
        JSString* specifier = argument(0).toString(globalObject);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, rejectWithCaughtException());
        auto* importer = uncheckedDowncast<JSModuleRecord>(instance->typed->moduleRecord);
        SourceOrigin referrer = importer ? importer->sourceCode().provider()->sourceOrigin() : SourceOrigin { URL::fileURLWithFileSystemPath(instance->typed->requestedFilePath) };
        if (count > 2)
            referrer = SourceOrigin { URL::fileURLWithFileSystemPath(String { asString(argument(2))->value(globalObject) }) };
        JSPromise* promise = globalObject->moduleLoader()->importModule(globalObject, specifier, argument(1), referrer);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, rejectWithCaughtException());
        didImportFromHost(*instance, promise, count > 3 ? argument(3) : jsUndefined());
        OPERATION_RETURN(scope, JSValue::encode(promise));
    }
    case Typed::LanguageOperation::GetExternalModuleWithType: {
        AbstractModuleRecord* importer = instance->typed->moduleRecord;
        JSModuleLoader* loader = importer ? importer->moduleLoader() : globalObject->moduleLoader();
        Identifier name = asString(argument(0))->toIdentifier(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        String type { asString(argument(1))->value(globalObject) };
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        RefPtr<ScriptFetchParameters> parameters;
        if (auto parsed = ScriptFetchParameters::parseType(type))
            parameters = *parsed == ScriptFetchParameters::Type::HostDefined ? ScriptFetchParameters::create(type) : ScriptFetchParameters::create(*parsed);
        OPERATION_RETURN(scope, externalModuleNamespace(globalObject, instance, loader, name, WTF::move(parameters)));
    }
    case Typed::LanguageOperation::ImportWithOptions: {
        auto attributes = retrieveImportAttributesFromDynamicImportOptions(globalObject, argument(0), { vm.propertyNames->type.impl() });
        if (!scope.exception()) [[likely]]
            retrieveTypeImportAttribute(globalObject, attributes);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, JSValue::encode(JSPromise::create(vm, globalObject->promiseStructure())->rejectWithCaughtException(vm, scope)));
        OPERATION_RETURN(scope, JSValue::encode(call(globalObject, argument(1), JSC::getCallData(argument(1)), jsUndefined(), ArgList())));
    }
    case Typed::LanguageOperation::NewModuleRecord:
        OPERATION_RETURN(scope, JSValue::encode(newModuleRecord(*instance, argument(0).isTrue(), argument(1))));
    case Typed::LanguageOperation::EnterModule:
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(enterModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell())))));
    case Typed::LanguageOperation::DidEvaluateRequiredModule:
        didEvaluateRequiredModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), uncheckedDowncast<JSArray>(argument(1).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::LeaveModule:
        leaveModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::DidStartModuleBody:
        didStartModuleBody(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::WaitsForImportFromHost:
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(waitsForImportFromHost(*instance, argument(0)))));
    case Typed::LanguageOperation::AsyncModuleExecutionFulfilled:
        asyncModuleExecutionFulfilled(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::AsyncModuleExecutionRejected:
        asyncModuleExecutionRejected(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::NewImportTicket:
        OPERATION_RETURN(scope, JSValue::encode(newImportTicket(*instance, argument(0))));
    case Typed::LanguageOperation::SetImportPromise:
        setImportPromise(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::EvaluateModule:
        OPERATION_RETURN(scope, JSValue::encode(evaluateModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1), argument(2))));
    case Typed::LanguageOperation::NewPrivateName: {
        String description = argument(0).toWTFString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get())));
    }
    case Typed::LanguageOperation::PrivateGet:
    case Typed::LanguageOperation::PrivateFieldGet:
        OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, argument(0), PropertyName(asSymbol(argument(1))->privateName()))));
    case Typed::LanguageOperation::PrivateSet:
        privateSet(globalObject, vm, argument(0), PropertyName(asSymbol(argument(1))->privateName()), argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::PrivateHas:
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(privateHas(globalObject, vm, argument(0), PropertyName(asSymbol(argument(1))->privateName())))));
    case Typed::LanguageOperation::PrivateFieldAdd:
        privateAdd(globalObject, vm, asObject(argument(0)), PropertyName(asSymbol(argument(1))->privateName()), argument(2), 0);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::PrivateMethodsAdd:
        privateMethodsAdd(globalObject, vm, asObject(argument(0)), asObject(argument(1)));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::Initialize:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewPromise, JSCell*, (Instance* instance))
{
    return JSPromise::create(*instance->vm, instance->globalObject->promiseStructure());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFinishStepForDriver, EncodedJSValue, (Instance* instance, JSCell* generator, EncodedJSValue result))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<Typed::AsyncGenerator>(generator)->finishStepForDriver(globalObject, JSValue::decode(result))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedResolvePromise, void, (Instance* instance, JSCell* promise, EncodedJSValue resolution))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (auto* frame = std::bit_cast<const uint64_t*>(callFrame->callerFrame()); std::bit_cast<const void*>(frame[1]) == instance->savedFrameReturnAddress && !instance->awaitMustSuspend && instance->microtaskQueue->isEmpty() && Typed::tryScheduleImmediateResume(*instance, frame, uncheckedDowncast<JSPromise>(promise), JSValue::decode(resolution)))
        OPERATION_RETURN(scope);
    uncheckedDowncast<JSPromise>(promise)->resolve(globalObject, vm, JSValue::decode(resolution));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedResolvePromiseOrReturnResult, EncodedJSValue, (Instance* instance, JSCell* promise, EncodedJSValue encodedResolution))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue resolution = JSValue::decode(encodedResolution);
    auto* frame = std::bit_cast<const uint64_t*>(callFrame->callerFrame());
    if (promise) {
        if (std::bit_cast<const void*>(frame[1]) == instance->savedFrameReturnAddress && !instance->awaitMustSuspend && instance->microtaskQueue->isEmpty() && Typed::tryScheduleImmediateResume(*instance, frame, uncheckedDowncast<JSPromise>(promise), resolution))
            OPERATION_RETURN(scope, JSValue::encode(promise));
        uncheckedDowncast<JSPromise>(promise)->resolve(globalObject, vm, resolution);
        OPERATION_RETURN(scope, JSValue::encode(promise));
    }
    bool isNotThenable = !resolution.isObject() || isDefinitelyNonThenable(asObject(resolution), globalObject);
    Typed::SavedFrame* saved = Typed::SavedFrame::forFrame(*instance, frame);
    if (isNotThenable) {
        if (saved)
            saved->fulfill(globalObject, vm, resolution);
        OPERATION_RETURN(scope, encodedResolution);
    }
    JSPromise* created = saved ? saved->ensurePromise(globalObject, vm) : JSPromise::create(vm, globalObject->promiseStructure());
    created->resolve(globalObject, vm, resolution);
    OPERATION_RETURN(scope, JSValue::encode(created));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAwait, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue awaited, uint32_t isAsyncFunction))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    InternalMicrotask task = InternalMicrotask::AOTAsyncFunctionResume;
    if (!isAsyncFunction) {
        uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtAwait();
        task = InternalMicrotask::AOTAsyncGeneratorResume;
    }
    JSValue value = JSValue::decode(awaited);
    if (value.isCell() && value.asCell()->type() == ObjectType && value.asCell()->classInfo() == Typed::AsyncGenerator::info()) {
        if (auto* producer = uncheckedDowncast<Typed::AsyncGenerator>(value); producer->isAwaitingDriver()) [[likely]] {
            producer->setDriver(vm, asObject(resumeClosure), AsyncContextSwapScope::current(vm, globalObject));
            OPERATION_RETURN(scope);
        }
    }
    if (!value.isObject()) {
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), value, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        OPERATION_RETURN(scope);
    }
    if (value.asCell()->structureID() == globalObject->promiseStructure()->id()) {
        uncheckedDowncast<JSPromise>(value)->performPromiseThenWithInternalMicrotask(vm, task, nullptr, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        OPERATION_RETURN(scope);
    }
    if (Typed::driveAsyncIterator(globalObject, vm, value.asCell(), asObject(resumeClosure)))
        OPERATION_RETURN(scope);
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, value, task, resumeClosure);
    if (!isAsyncFunction)
        uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtAwait();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstructSuper, EncodedJSValue, (Instance* instance, EncodedJSValue classObject, EncodedJSValue newTarget, EncodedJSValue list, EncodedJSValue parentIfKnown))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(JSValue::decode(list));
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < array->length(); ++i) {
        arguments.append(array->getIndex(globalObject, i));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(constructParent(globalObject, JSValue::decode(classObject), JSValue::decode(newTarget), arguments, JSValue::decode(parentIfKnown))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue constructor = JSValue::decode(callee);
    auto constructData = JSC::getConstructData(constructor);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, makeString(State::describe(constructor), " is not a constructor"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, constructor, constructData, arguments)));
}

#define AOT_DEFINE_TYPED_BINARY(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue left, EncodedJSValue right)) \
    { \
        AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(left), JSValue::decode(right)))); \
    }
AOT_DEFINE_TYPED_BINARY(operationAOTTypedAdd, jsAdd)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedSub, jsSub)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedMul, jsMul)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedDiv, jsDiv)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedMod, jsRemainder)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedExp, jsPow)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitAnd, jsBitwiseAnd)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitOr, jsBitwiseOr)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitXor, jsBitwiseXor)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedShl, jsLShift)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedShr, jsRShift)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedUshr, jsURShift)
#undef AOT_DEFINE_TYPED_BINARY

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNeg, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue numeric = JSValue::decode(operand).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-numeric.asNumber())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedBitNot, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(operand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToNumber, double, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::decode(operand).toNumber(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToNumeric, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toNumeric(globalObject)));
}

#define AOT_DEFINE_TYPED_COMPARISON(name, expression) \
    JSC_DEFINE_JIT_OPERATION(name, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        OPERATION_RETURN(scope, expression); \
    }
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLess, jsLess<true>(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLessEq, jsLessEq<true>(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedGreater, jsLess<false>(globalObject, right, left))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedGreaterEq, jsLessEq<false>(globalObject, right, left))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedStrictEq, JSValue::strictEqual(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLooseEq, JSValue::equal(globalObject, left, right))
#undef AOT_DEFINE_TYPED_COMPARISON

JSC_DEFINE_JIT_OPERATION(operationAOTTypedInstanceOf, size_t, (Instance* instance, EncodedJSValue value, EncodedJSValue constructor))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue right = JSValue::decode(constructor);
    if (!right.isObject()) {
        throwException(globalObject, scope, createInvalidInstanceofParameterErrorNotFunction(globalObject, right));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, asObject(right)->hasInstance(globalObject, JSValue::decode(value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedInstanceOfClass, size_t, (Instance* instance, EncodedJSValue value, uint32_t ofClass))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSObject* classObject = state.closure(vm, state.classWithIndex(ofClass)->classData().classConstructor);
    instance->typedClassPrototypes[ofClass] = state.classPrototype(vm, ofClass);
    OPERATION_RETURN(scope, classObject->hasInstance(globalObject, JSValue::decode(value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedInstanceOfEngineClass, size_t, (Instance* instance, EncodedJSValue value, uint32_t numberOfClass))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& builtins = Typed::Builtins::singleton();
    auto& engineClass = builtins.classes()[numberOfClass];
    if (globalObject->hasImmutableIntrinsics()) {
        JSCell*& prototype = instance->negativeOffsetFields().typedBuiltins[builtins.prototypeCell(engineClass)];
        if (!prototype)
            prototype = builtins.findCell(globalObject, builtins.prototypeCell(engineClass));
        OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(value), prototype));
    }
    JSValue constructor = globalObject->get(globalObject, Identifier::fromString(vm, engineClass.text));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!constructor.isObject()) {
        throwException(globalObject, scope, createInvalidInstanceofParameterErrorNotFunction(globalObject, constructor));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, asObject(constructor)->hasInstance(globalObject, JSValue::decode(value)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTruthy, size_t, (Instance* instance, EncodedJSValue operand))
{
    return JSValue::decode(operand).toBoolean(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypeOf, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    return jsTypeStringForValue(instance->globalObject, JSValue::decode(operand));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypeOfObject, size_t, (Instance* instance, JSCell* object))
{
    if (object->structure()->masqueradesAsUndefined(instance->globalObject))
        return static_cast<size_t>(TypeofType::Undefined);
    return static_cast<size_t>(object->isCallable() ? TypeofType::Function : TypeofType::Object);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToString, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::decode(operand).toString(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToKey, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toPropertyKeyValue(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFmod, double, (double a, double b))
{
    constexpr double limit = 4611686018427387904.0;
    if (std::abs(a) < limit) [[likely]] {
        int64_t dividend = static_cast<int64_t>(a);
        if (static_cast<double>(dividend) == a && std::abs(b) < limit) [[likely]] {
            int64_t divisor = static_cast<int64_t>(b);
            if (static_cast<double>(divisor) == b && divisor) [[likely]] {
                int64_t remainder = dividend % divisor;
                return remainder ? static_cast<double>(remainder) : a * 0.0;
            }
        }
    }
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIndexInString, int64_t, (double position, uint64_t length, uint32_t isRelativeToEnd))
{
    if (position != position)
        return 0;
    double truncated = std::trunc(position);
    double end = static_cast<double>(length);
    if (truncated >= 0)
        return truncated > end ? static_cast<int64_t>(length) : static_cast<int64_t>(truncated);
    if (!isRelativeToEnd || end + truncated < 0)
        return 0;
    return static_cast<int64_t>(end + truncated);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedToInt32, size_t, (double value))
{
    return static_cast<uint32_t>(toInt32(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedToLiteralString, EncodedJSValue, (Instance* instance, EncodedJSValue encoded))
{
    JSValue value = JSValue::decode(encoded);
    if (Options::useAOTOperationCounters() && value && value.isString()) [[unlikely]] {
        JSString* string = asString(value);
        StringImpl* impl = string->tryGetValueImpl();
        unsigned kind = impl ? (impl->isAtom() ? 0 : 1) : string->isSubstring() ? 2 : 3;
        uint64_t word = instance->typed->literalStringWordFor(string);
        static constexpr const char* details[] = { "atom:numbered", "flat:numbered", "substring:numbered", "rope:numbered", "atom:stranger", "flat:stranger", "substring:stranger", "rope:stranger" };
        countOperationNamed(instance, __func__);
        countOperationNamed(instance, __func__, details[kind + (word == std::bit_cast<uint64_t>(string) ? 4 : 0)]);
        return static_cast<EncodedJSValue>(word);
    }
    return value && value.isString() ? static_cast<EncodedJSValue>(instance->typed->literalStringWordFor(asString(value))) : encoded;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLiteralStringToString, JSCell*, (Instance* instance, uint64_t word))
{
    countOperationNamed(instance, __func__);
    return instance->typed->stringForLiteralStringWord(word);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIntegerEqualTo, int64_t, (EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isInt32())
        return value.asInt32();
    if (!value.isDouble())
        return std::numeric_limits<int64_t>::min();
    double number = value.asDouble();
    if (!(number >= -static_cast<double>(Typed::maxTrackedInteger) && number <= static_cast<double>(Typed::maxTrackedInteger)))
        return std::numeric_limits<int64_t>::min();
    int64_t integer = static_cast<int64_t>(number);
    return static_cast<double>(integer) == number ? integer : std::numeric_limits<int64_t>::min();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAllocSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    return instance->typed->allocate(*instance->vm, allocation);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMaterializeEnvironment, JSCell*, (Instance* instance, uint32_t allocation, const EncodedJSValue* source))
{
    JSCell* cell = instance->typed->allocate(*instance->vm, allocation);
    size_t size = instance->typed->layoutForShape(instance->typed->allocation(allocation).shape).size;
    memcpy(reinterpret_cast<uint8_t*>(cell) + sizeof(JSCell), reinterpret_cast<const uint8_t*>(source) + sizeof(JSCell), size - sizeof(JSCell));
    instance->vm->mutatorFence();
    return cell;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewClosureSlow, JSCell*, (Instance* instance, uint64_t request, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth))
{
    uint64_t captures[] = { first, second, third, fourth };
    RELEASE_ASSERT(ClosureRequest::countOf(request) <= (ClosureRequest::isSlim(request) ? ClosureRequest::maxSlimClosureCaptures : ClosureRequest::maxNumberOfCaptures));
    JSCell* cell;
    auto word = [&](ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + offset);
    };
    auto made = [&] {
        if (ClosureRequest::isCreatedOnce(request)) {
            if (ClosureRequest::variableIsCell32(request))
                *std::bit_cast<uint32_t*>(second) = static_cast<uint32_t>(std::bit_cast<uint64_t>(cell) >> Typed::cell32Shift);
            else
                *std::bit_cast<uint64_t*>(second) = std::bit_cast<uint64_t>(cell);
            instance->vm->writeBarrier(std::bit_cast<JSCell*>(first));
        }
        return cell;
    };
    if (ClosureRequest::isSlim(request)) {
        cell = Typed::SlimClosure::createForCompiledCode(*instance->vm, instance->typed->slimClosureStructure(*instance->vm, ClosureRequest::functionOf(request)), Typed::SlimClosure::allocationSize(ClosureRequest::countOf(request)));
        for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
            word(Typed::SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
        WTF::storeStoreFence();
        word(Typed::SlimClosure::offsetOfWord()) = instance->typedFunctions[ClosureRequest::functionOf(request)];
        WTF::storeStoreFence();
        return made();
    }
    cell = instance->typed->allocate(*instance->vm, ClosureRequest::allocationOf(request));
    word(JSObject::butterflyOffset()) = 0;
    word(JSCallee::offsetOfScopeChain()) = std::bit_cast<uint64_t>(instance->globalObject);
    word(JSFunction::offsetOfExecutableOrRareData()) = std::bit_cast<uint64_t>(instance->typedExecutables[ClosureRequest::executableOf(request)]);
    for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
        word(Typed::Closure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
    WTF::storeStoreFence();
    word(Typed::Closure::offsetOfEntry()) = instance->typedFunctions[ClosureRequest::functionOf(request)];
    WTF::storeStoreFence();
    return made();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCopyClosureOutOfFrame, EncodedJSValue, (Instance* instance, EncodedJSValue encodedClosure))
{
    JSValue value = JSValue::decode(encodedClosure);
    if (!value.isCell() || !Thread::currentSingleton().stack().contains(value.asCell()))
        return encodedClosure;
    VM& vm = *instance->vm;
    JSCell* cell = value.asCell();
    if (isSlimClosureType(cell->type())) {
        auto* closure = uncheckedDowncast<Typed::SlimClosure>(cell);
        return JSValue::encode(Typed::SlimClosure::create(vm, closure->structure(), closure->functionIndex(), closure->captures(), closure->numberOfReferences()));
    }
    auto* closure = uncheckedDowncast<Typed::Closure>(cell);
    return JSValue::encode(Typed::Closure::create(vm, closure->structure(), closure->functionIndex(), closure->captures(), closure->numberOfReferences()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerce, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t typeAndReason))
{
    if ((typeAndReason & 15) == static_cast<uint32_t>(Typed::Format::CastReason::As) || (typeAndReason & 15) == static_cast<uint32_t>(Typed::Format::CastReason::AsIfConvertible)) {
        auto& type = instance->typed->type(typeAndReason >> 4);
        if (instance->typed->tryQuickConversion(*instance->vm, JSValue::decode(value), type))
            return ExceptionOperationResult<EncodedJSValue> { value, nullptr };
        if (instance->typed->staysUnconvertedByPlan(JSValue::decode(value), type)) {
            instance->typed->shareUnconvertibleObjectPlanWithStub(asObject(JSValue::decode(value)), type, typeAndReason);
            return ExceptionOperationResult<EncodedJSValue> { value, nullptr };
        }
        if (instance->typed->passesOpenTypeAsItIs(JSValue::decode(value), type, typeAndReason))
            return ExceptionOperationResult<EncodedJSValue> { value, nullptr };
    }
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    OPERATION_RETURN(scope, JSValue::encode(state.coerce(globalObject, JSValue::decode(value), state.type(typeAndReason >> 4), static_cast<Typed::Format::CastReason>(typeAndReason & 15), { }, State::CoercionResult::ConvertedValue, typeAndReason)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceInPlace, void, (Instance* instance, EncodedJSValue value, uint32_t typeAndReason))
{
    if (instance->typed->tryPreviousTransition(*instance->vm, JSValue::decode(value), typeAndReason))
        return ExceptionOperationResult<void> { nullptr };
    if ((typeAndReason & 15) == static_cast<uint32_t>(Typed::Format::CastReason::As) && instance->typed->tryQuickConversion(*instance->vm, JSValue::decode(value), instance->typed->type(typeAndReason >> 4)))
        return ExceptionOperationResult<void> { nullptr };
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue result = state.coerce(globalObject, JSValue::decode(value), state.type(typeAndReason >> 4), static_cast<Typed::Format::CastReason>(typeAndReason & 15), { }, State::CoercionResult::OriginalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    RELEASE_ASSERT(JSValue::encode(result) == value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceElement, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t index, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue element = array->getIndexQuickly(index);
    if (state.tryConvertPlainObjectInPlace(vm, element, state.type(type), type << 4 | static_cast<uint32_t>(Typed::Format::CastReason::As)))
        OPERATION_RETURN(scope, JSValue::encode(element));
    JSValue converted = state.hold(globalObject, element, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != element) {
        array->putDirectIndex(globalObject, index, converted);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceProperty, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue encodedValue, uint32_t type))
{
    if (instance->typed->staysUnconvertedByPlan(JSValue::decode(encodedValue), instance->typed->type(type)))
        return ExceptionOperationResult<EncodedJSValue> { encodedValue, nullptr };
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue value = JSValue::decode(encodedValue);
    JSValue converted = state.hold(globalObject, value, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != value && cell->type() == FinalObjectType) {
        JSObject* holder = asObject(cell);
        unsigned attributes = 0;
        if (holder->getDirect(vm, nameAt(instance, name), attributes) == value && !attributes)
            holder->putDirect(vm, nameAt(instance, name), converted);
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAuditCoerce, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t lineAndColumn))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    OPERATION_RETURN(scope, JSValue::encode(state.coerce(globalObject, JSValue::decode(value), state.type(type), static_cast<Typed::Format::CastReason>(reason), { function, lineAndColumn })));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCountClosure, void, (Instance* instance, uint32_t function, uint32_t numberOfCaptures))
{
    instance->typed->countClosure(function, numberOfCaptures);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCountPlainAllocation, void, (Instance* instance, JSCell* cell, uint32_t function, uint32_t kindAndValue))
{
    instance->typed->countPlainAllocation(cell, function, kindAndValue);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(state, nameAt(instance, name & ~Typed::nameIsFieldCreatedWithoutStorage));
    RELEASE_ASSERT(field);
    object->didCreateField(state, *field, !(name & Typed::nameIsFieldCreatedWithoutStorage));
}

static bool isMarkedOnlyToAskPrototypeChain(State& state, Typed::Object* object, uint32_t orderWord)
{
    return (orderWord & ~Typed::orderAndCountMask) == Typed::orderWordHasExtraProperties && state.isStructureWithoutExtraProperties(object->structure());
}

static void didCreateFieldAtSite(State& state, Typed::Object* object, const Typed::FieldData& field, uint32_t slotKey, Slot* cache)
{
    uint32_t orderWord = object->orderWord();
    object->didCreateField(state, field);
    if (isMarkedOnlyToAskPrototypeChain(state, object, orderWord)) {
        state.cacheLateFieldCreation(*object, nullptr, orderWord, slotKey);
        return;
    }
    if (!cache || SharedData::contains(cache) || orderWord & ~Typed::orderAndCountMask || Typed::orderOf(object->orderWord()) == Typed::keysAreInLayoutOrder)
        return;
    cache->offset = orderWord ^ object->orderWord();
    cache->pointer = std::bit_cast<void*>(static_cast<uint64_t>(Typed::shapeWordOf(object->layout().shape)) << 32 | orderWord);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset, Slot* cache))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    didCreateFieldAtSite(state, object, state.fieldAt(object->layout(), offset), offset, cache);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateFieldOutOfLineAt, void, (Instance* instance, JSCell* cell, uint32_t offset, Slot* cache))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData& field = state.fieldAt(object->layout(), offset);
    didCreateFieldAtSite(state, object, field, offset, cache);
    object->ensureStorageFor(*instance->vm, state, field);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateFieldsAt, void, (Instance* instance, JSCell* cell, uint64_t offsets, Slot* cache))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint32_t orderWord = object->orderWord();
    uint32_t fieldFlags = 0;
    for (uint64_t rest = offsets; rest; rest >>= 16) {
        const Typed::FieldData& field = state.fieldAt(object->layout(), static_cast<uint16_t>(rest));
        fieldFlags |= field.flags;
        if (!object->read(field))
            object->didCreateField(state, field);
    }
    if (JSObject* extra = object->extraProperties(); (extra || isMarkedOnlyToAskPrototypeChain(state, object, orderWord)) && !(fieldFlags & (Typed::fieldMayExistWithoutStorage | Typed::fieldMayBeUnlisted))) [[unlikely]]
        state.cacheLateFieldCreation(*object, extra, orderWord, offsets);
    if (SharedData::contains(cache) || orderWord & ~Typed::orderAndCountMask || Typed::orderOf(object->orderWord()) == Typed::keysAreInLayoutOrder)
        return;
    cache->offset = orderWord ^ object->orderWord();
    cache->pointer = std::bit_cast<void*>(static_cast<uint64_t>(Typed::shapeWordOf(object->layout().shape)) << 32 | orderWord);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedEnsureStorage, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(state, nameAt(instance, name));
    RELEASE_ASSERT(field);
    object->ensureStorageFor(*instance->vm, state, *field);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedEnsureStorageOfSize, void, (Instance* instance, JSCell* cell, uint32_t size))
{
    uncheckedDowncast<Typed::Object>(cell)->ensureStorageOfSize(*instance->vm, *instance->typed, size);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPutNumberInNumberBox, void, (Instance* instance, JSCell* cell, uint32_t name, double value))
{
    operationAOTTypedPutInNumberBox(instance, cell, name, JSValue::encode(jsNumber(purifyNaN(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPutInNumberBox, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name));
    RELEASE_ASSERT(field);
    object->storeInNumberBox(*instance->vm, *field, JSValue::decode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExtra, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    bool mayHaveOwnField = name & Typed::nameMayBeOwnField;
    uint32_t requestedName = name;
    name &= ~Typed::nameMayBeOwnField;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (instance->typed->isCounting()) [[unlikely]]
        instance->typed->countAccess(State::AccessKind::ReadExtra, removeCodePtrTag(callFrame->rawReturnPC()), nameAt(instance, name), object);
    if (mayHaveOwnField) [[unlikely]] {
        if (const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name))) {
            JSValue held = object->read(*field);
            OPERATION_RETURN(scope, JSValue::encode(held ? held : jsUndefined()));
        }
    }
    JSObject* extra = object->extraProperties();
    unsigned attributes = 0;
    PropertyOffset offset = extra ? extra->structure()->get(vm, nameAt(instance, name), attributes) : invalidOffset;
    JSValue value = isValidOffset(offset) ? extra->getDirect(offset) : JSValue();
    if (extra && &object->state() == instance->typed)
        instance->typed->cacheExtraPropertyRead(object, extra, requestedName, offset, attributes);
    if (!value)
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) {
        bool goesOn = instance->typed->reportOrThrow(globalObject, { .kind = "runtime-accessor"_s, .on = &object->layout(), .member = nameAt(instance, name).impl() }, makeString("Cannot read property '"_s, nameAt(instance, name).string(), "' of "_s, instance->typed->nameOf(object->layout()), ": TypeScript code of the program cannot call a getter that was added at run time"_s), State::InAudit::Ignored);
        OPERATION_RETURN(scope, goesOn ? JSValue::encode(jsUndefined()) : encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFieldEqualsString, size_t, (Instance* instance, JSCell* cell, uint32_t name, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    JSValue held = object->read(*object->fieldNamed(*instance->typed, nameAt(instance, name)));
    if (!held || !held.isString())
        OPERATION_RETURN(scope, false);
    OPERATION_RETURN(scope, asString(held)->equal(globalObject, instance->typed->string(vm, text)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedHasExtra, size_t, (Instance* instance, JSCell* cell, uint32_t name))
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (name & Typed::nameMayBeOwnField) [[unlikely]] {
        name &= ~Typed::nameMayBeOwnField;
        if (const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name)))
            return object->holdsUnmaterializedClosure(*field) || object->read(*field);
    }
    JSObject* extra = object->extraProperties();
    return extra && extra->getDirectOffset(*instance->vm, nameAt(instance, name)) != invalidOffset;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutExtra, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (instance->typed->isCounting()) [[unlikely]]
        instance->typed->countAccess(State::AccessKind::WriteExtra, removeCodePtrTag(callFrame->rawReturnPC()), nameAt(instance, name), cell);
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Structure* oldStructure = object->structure();
    uint64_t wordsBefore = object->orderAndShapeWords();
    State& state = object->state();
    if (state.storeByKnownTransition(vm, object, nameAt(instance, name).impl(), JSValue::decode(value))) {
        if (&state == instance->typed)
            cacheKnownStoreTransition(instance, object, oldStructure, wordsBefore, nameAt(instance, name), name);
        OPERATION_RETURN(scope);
    }
    object->putExtraWithoutRunningCode(globalObject, nameAt(instance, name), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedInheritsFromClass, size_t, (Instance* instance, JSCell* cell, uint32_t classIndex))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* classPrototype = instance->typed->classPrototypeIfExists(classIndex);
    if (!classPrototype || !cell->isObject())
        OPERATION_RETURN(scope, false);
    JSObject* object = asObject(cell);
    while (true) {
        JSValue prototype = object->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        if (!prototype.isObject())
            OPERATION_RETURN(scope, false);
        object = asObject(prototype);
        if (object == classPrototype)
            OPERATION_RETURN(scope, true);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedBigIntConstant, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSCell* kept = instance->typed->bigIntConstantIfCreated(index))
        OPERATION_RETURN(scope, JSValue::encode(JSValue(kept)));
    String text = nameAt(instance, index).string();
    if (text.endsWith('n'))
        text = text.left(text.length() - 1);
    JSValue result = JSBigInt::parseInt(globalObject, text, JSBigInt::ErrorParseMode::ThrowExceptions);
    if (result && result.isCell())
        instance->typed->setBigIntConstant(index, result.asCell());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedConstantSymbol, JSCell*, (Instance* instance, uint32_t index))
{
    return Symbol::create(*instance->vm, static_cast<SymbolImpl&>(*nameAt(instance, index).impl()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    return instance->typed->createAndCacheStringConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringCached, JSCell*, (Instance* instance, uint32_t index))
{
    return instance->typed->createStringConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBarrierSlow, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBarrierSlowPreservingRegisters, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedRememberElement, void, (Instance* instance, JSCell* array, EncodedJSValue value))
{
    instance->typed->rememberElement(*instance->vm, asObject(array), JSValue::decode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
}

static uint8_t* afterStackCheck(uint8_t* returnAddress)
{
    ImageAddressInfo function = classifyAddress(returnAddress - 1);
    RELEASE_ASSERT(function.kind == ImageAddressInfo::Function);
    uint8_t* start = returnAddress - 1 - function.offset;
#if CPU(ARM64)
    auto* call = reinterpret_cast<uint32_t*>(returnAddress) - 1;
    auto* instruction = reinterpret_cast<uint32_t*>(start);
    auto branchTarget = [](uint32_t* branch) { return branch + (static_cast<int32_t>(*branch << 6) >> 6); };
    auto isBranch = [](uint32_t* candidate) { return (*candidate & 0xfc000000) == 0x14000000; };
    if (isBranch(instruction))
        instruction = branchTarget(instruction);
    constexpr unsigned maxInstructionsInStackCheck = 8;
    for (unsigned i = 0; i < maxInstructionsInStackCheck; ++i, ++instruction) {
        bool isConditionalBranch = (*instruction & 0xff000010) == 0x54000000;
        if (isConditionalBranch && instruction + (static_cast<int32_t>(*instruction << 8) >> 13) == call)
            return reinterpret_cast<uint8_t*>(instruction + 1);
        if (isBranch(instruction) && branchTarget(instruction) == call)
            return reinterpret_cast<uint8_t*>(instruction + 1);
    }
#elif CPU(X86_64)
    if (uint8_t* behind = afterStackCheckBranchingTo(start, returnAddress - sizeOfCallAtPrologue)) {
#if OS(LINUX)
        if (trapAtPrologueMakesFrameRecord(returnAddress - sizeOfCallAtPrologue))
            return returnAddress;
#endif
        return behind;
    }
#endif
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPollAtPrologue, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, afterStackCheck(returnAddress));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowToHandlerInFunction, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwException(globalObject, scope, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrow, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (vm.exceptionForInspection()) [[unlikely]] {
        throwException(globalObject, scope, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    instance->thrownValue = value;
    return { std::bit_cast<Exception*>(static_cast<uintptr_t>(1)) };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCatch, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    if (vm.isTerminationException(exception)) [[unlikely]]
        return JSValue::encode(JSValue());
    JSValue value = exception->value();
    if (vm.deferredStackCapture.exception == exception)
        vm.dropDeferredStackCapture();
    scope.clearException();
    return JSValue::encode(value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCatchException, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    if (vm.isTerminationException(exception)) [[unlikely]]
        return JSValue::encode(JSValue());
    scope.clearException();
    return JSValue::encode(exception);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedExceptionValue, EncodedJSValue, (Instance*, EncodedJSValue thrown))
{
    auto* exception = dynamicDowncast<Exception>(JSValue::decode(thrown));
    return exception ? JSValue::encode(exception->value()) : thrown;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedRejectWithException, JSCell*, (Instance* instance, uint64_t frame))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue value;
    if (Exception* exception = scope.exception()) {
        value = exception->value();
        scope.clearException();
    } else
        value = JSValue::decode(std::exchange(instance->thrownValue, 0));
    RELEASE_ASSERT(value);
    if (auto* generator = Typed::AsyncGenerator::forBodyFrame(*instance, std::bit_cast<const uint64_t*>(frame))) {
        generator->didThrow(vm, value);
        return nullptr;
    }
    if (Typed::SavedFrame* saved = Typed::SavedFrame::forFrame(*instance, std::bit_cast<const uint64_t*>(frame))) {
        saved->reject(instance->globalObject, vm, value);
        return saved->promiseOrWaiter();
    }
    JSPromise* promise = JSPromise::create(vm, instance->globalObject->promiseStructure());
    promise->reject(vm, value);
    return promise;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPrepareProxyCall, uintptr_t, (Instance* instance, EncodedJSValue* hostFrame, uint32_t count))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto decline = [&](const char* why) -> uintptr_t {
        countOperationNamed(instance, "call of a proxy", why);
        return 0;
    };
    const EncodedJSValue* thisAndArguments = hostFrame + static_cast<int>(CallFrameSlot::thisArgument);
    EncodedJSValue* results = hostFrame + CallFrame::headerSizeInRegisters + 1 + count;
    auto* proxy = uncheckedDowncast<ProxyObject>(JSValue::decode(hostFrame[static_cast<int>(CallFrameSlot::callee)]).asCell());
    if (!proxy->isCallable())
        return decline("it is not callable");
    JSValue handlerValue = proxy->handler();
    if (!handlerValue.isObject())
        return decline("it is revoked");
    JSObject* handler = asObject(handlerValue);
    if (handler->type() != FinalObjectType)
        return decline("the handler is no plain object");
    unsigned attributes;
    PropertyOffset offset = handler->structure()->get(vm, vm.propertyNames->builtinNames().applyPublicName(), attributes);
    if (!isValidOffset(offset) || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return decline("the handler has no `apply` of its own that is a value");
    JSValue trap = handler->getDirect(offset);
    if (!trap.isCallable())
        return decline("the trap is not callable");
    JSGlobalObject* globalObject = proxy->globalObject();
    if (globalObject->isHavingABadTime())
        return decline("arrays have indexed accessors to look at");
    JSArray* list;
    {
        ObjectInitializationScope initializationScope(vm);
        list = JSArray::tryCreateUninitializedRestricted(initializationScope, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
        if (!list)
            return decline("there is no memory for the list");
        for (unsigned i = 0; i < count; ++i)
            list->initializeIndex(initializationScope, i, JSValue::decode(thisAndArguments[i + 1]));
    }
    countOperationNamed(instance, "call of a proxy", "the trap is called from the stub");
    results[0] = JSValue::encode(trap);
    results[1] = JSValue::encode(handler);
    results[2] = JSValue::encode(proxy->target());
    results[3] = JSValue::encode(JSValue::decode(thisAndArguments[0]).toThis(globalObject, ECMAMode::strict()));
    results[4] = JSValue::encode(list);
    return 1;
}

static ASCIILiteral auditPrefix(State& state)
{
    return state.data.isCompiledForAudit ? "audit stopped (compiled code depends on this check): "_s : ""_s;
}

static void throwTypeMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t type, const String& reason = { })
{
    State& state = *instance->typed;
    state.recordViolation(globalObject->vm(), { .kind = State::defaultViolationKind, .expected = &state.type(type), .got = value, .reason = reason.isEmpty() ? nullptr : &reason });
    throwTypeError(globalObject, scope, withAOTErrorNote(makeString(auditPrefix(state), state.mismatchMessage(globalObject->vm(), value, state.type(type), reason))));
}

#define AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, double, (Instance* instance, EncodedJSValue value, uint32_t typeAndWhetherNumberIsOnRight, double number)) \
    { \
        AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance); \
        constexpr uint32_t numberIsOnRight = 1u << 31; \
        JSValue boxed = jsNumber(purifyNaN(number)); \
        JSValue other = JSValue::decode(value); \
        JSValue result = typeAndWhetherNumberIsOnRight & numberIsOnRight ? function(globalObject, other, boxed) : function(globalObject, boxed, other); \
        OPERATION_RETURN_IF_EXCEPTION(scope, 0); \
        if (!result.isNumber()) [[unlikely]] { \
            throwTypeMismatch(instance, globalObject, scope, result, typeAndWhetherNumberIsOnRight & ~numberIsOnRight); \
            OPERATION_RETURN(scope, 0); \
        } \
        OPERATION_RETURN(scope, result.asNumber()); \
    }
AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE(operationAOTTypedAddNumberAndValueAsNumber, jsAdd)
AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE(operationAOTTypedSubNumberAndValueAsNumber, jsSub)
AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE(operationAOTTypedMulNumberAndValueAsNumber, jsMul)
AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE(operationAOTTypedDivNumberAndValueAsNumber, jsDiv)
#undef AOT_DEFINE_TYPED_BINARY_ON_NUMBER_AND_VALUE

JSC_DEFINE_JIT_OPERATION(operationAOTTypedEnforceType, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue given = JSValue::decode(value);
    if (!given || !instance->typed->accepts(globalObject, instance->typed->type(type), given)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), type);
    OPERATION_RETURN(scope, value);
}

struct InheritedField {
    bool isKnown { false };
    JSObject* holder { nullptr };
    PropertyOffset offset { invalidOffset };
};

static JSValue getFromReceiver(JSGlobalObject* globalObject, JSObject* object, const Identifier& name, InheritedField& inherited)
{
    VM& vm = globalObject->vm();
    Typed::Object* ofLayout = asTypedObject(object);
    if (!ofLayout || parseIndex(name))
        return object->get(globalObject, name);
    bool isAbsentField = false;
    if (const Typed::FieldData* field = ofLayout->fieldNamed(ofLayout->state(), name)) {
        if (JSValue held = ofLayout->read(*field))
            return held;
        isAbsentField = !(field->flags & (Typed::fieldMayExistWithoutStorage | Typed::fieldMayBeUnlisted | Typed::fieldHoldsLazyClosure | Typed::fieldIsPlaceholder));
    } else if (JSObject* extra = ofLayout->extraProperties()) {
        unsigned attributes;
        if (PropertyOffset offset = extra->structure()->get(vm, name, attributes); isValidOffset(offset))
            return attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue ? object->get(globalObject, name) : extra->getDirect(offset);
    }
    JSValue prototype = object->getPrototypeDirect();
    if (!prototype.isObject()) {
        inherited.isKnown = isAbsentField;
        return jsUndefined();
    }
    JSObject* above = asObject(prototype);
    if (auto* found = vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(above->structureID(), name.impl()) : nullptr) {
        inherited.isKnown = isAbsentField;
        if (!found->m_holder)
            return jsUndefined();
        inherited.holder = found->m_holder == JSCell::seenMultipleCalleeObjects() ? above : asObject(found->m_holder);
        inherited.offset = found->m_offset;
        return inherited.holder->getDirect(found->m_offset);
    }
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    return getByIdAndFillMegamorphicCache(globalObject, above, name, slot);
}

static void rememberInheritedField(Instance* instance, JSCell* cell, uint32_t name, uint32_t question, const InheritedField& inherited, JSValue onlyValueChecked = { })
{
    int32_t offset = inherited.holder ? static_cast<int32_t>(offsetRelativeToBase(inherited.offset)) : 0;
    instance->rememberInheritedFieldAnswer({ cell->structureID().bits(), name, question, offset, inherited.holder, static_cast<uint64_t>(JSValue::encode(onlyValueChecked)) });
}

static bool acceptsEveryValue(const Typed::RuntimeType& type)
{
    return (type.primitives & Typed::Primitive::All) == Typed::Primitive::All && type.allowsAnyObject() && type.minInteger() <= -Typed::maxTrackedInteger && type.maxInteger() >= Typed::maxTrackedInteger;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetAbsentField, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    InheritedField inherited;
    JSValue value = getFromReceiver(globalObject, asObject(JSValue::decode(base)), nameAt(instance, name), inherited);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (inherited.isKnown)
        rememberInheritedField(instance, JSValue::decode(base).asCell(), name, Instance::asksForUncheckedValue, inherited);
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByNameChecked, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    InheritedField inherited;
    JSValue value = getFromReceiver(globalObject, asObject(cell), nameAt(instance, name), inherited);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!instance->typed->accepts(globalObject, instance->typed->type(type), value)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, value, type);
    else if (inherited.isKnown) {
        bool isCheckedAtEveryRead = inherited.holder && !acceptsEveryValue(instance->typed->type(type));
        if (!isCheckedAtEveryRead || !value.isObject())
            rememberInheritedField(instance, cell, name, type, inherited, isCheckedAtEveryRead ? value : JSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStoreCreatesField, size_t, (Instance* instance, JSCell* cell, uint32_t name))
{
    static constexpr unsigned maxHolders = 16;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (object->orderWord() & (Typed::orderWordIsFrozen | Typed::orderWordIsSealed | Typed::orderWordIsNotExtensible))
        return 0;
    uint32_t structureID = cell->structureID().bits();
    if (auto* answer = instance->findInheritedFieldAnswer(structureID, name, Instance::asksWhetherStoreCreatesField))
        return answer->offset;
    VM& vm = *instance->vm;
    UniquedStringImpl* uid = nameAt(instance, name).impl();
    bool createsField = !parseIndex(*uid) && !instance->typed->isCounting();
    unsigned numberOfHolders = 0;
    for (JSValue next = object->getPrototypeDirect(); createsField && next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* holder = asObject(next);
        Structure* structure = holder->structure();
        TypeInfo info = structure->typeInfo();
        createsField = ++numberOfHolders <= maxHolders && structure->hasMonoProto() && structure->mayBePrototype() && !info.overridesGetOwnPropertySlot() && !info.overridesGetPrototype() && !holder->hasNonReifiedStaticProperties()
            && (!info.overridesPut() || holder->inherits<Typed::PrototypeObject>());
        if (!createsField)
            break;
        unsigned attributes;
        if (isValidOffset(structure->get(vm, uid, attributes))) {
            createsField = !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue));
            break;
        }
    }
    instance->rememberInheritedFieldAnswer({ structureID, name, Instance::asksWhetherStoreCreatesField, createsField, nullptr, 0 });
    return createsField;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByNameCheckedAsNumber, double, (Instance* instance, JSCell* cell, uint32_t name, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    InheritedField inherited;
    JSValue value = getFromReceiver(globalObject, asObject(cell), nameAt(instance, name), inherited);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!instance->typed->fits(instance->typed->type(type), value) || !(value.isNumber() || value.isUndefinedOrNull())) [[unlikely]] {
        throwTypeMismatch(instance, globalObject, scope, value, type);
        OPERATION_RETURN(scope, 0);
    }
    if (inherited.isKnown && !inherited.holder)
        rememberInheritedField(instance, cell, name, type, inherited);
    if (value.isNumber())
        OPERATION_RETURN(scope, purifyNaN(value.asNumber()));
    OPERATION_RETURN(scope, std::bit_cast<double>(value.isUndefined() ? Typed::undefinedDouble : Typed::nullDouble));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRecordTypeMismatch, void, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t parameter, uint32_t where))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue given = JSValue::decode(value);
    bool isAtCaller = where == 1;
    bool isArgument = where == 1 || where == 2;
    if (where == 3 && !(given && given.isObject() && state.type(type).acceptsUntypedObjects()))
        OPERATION_RETURN(scope);
    String reason;
    if (given && given.isObject() && state.type(type).acceptsUntypedObjects()) {
        if (state.type(type).isRelaxedForLogging())
            OPERATION_RETURN(scope);
        reason = state.explainMismatch(vm, given, state.type(type), true);
        if (reason.isNull())
            OPERATION_RETURN(scope);
    }
    state.recordViolation(vm, { .kind = isArgument ? "enter-argument"_s : "enter-function"_s, .what = isArgument ? "As"_s : "Claim"_s, .expected = &state.type(type), .got = given ? given : jsUndefined(), .parameter = parameter == UINT32_MAX ? std::nullopt : std::optional<unsigned>(parameter), .reason = reason.isNull() ? nullptr : &reason, .isAtCaller = isAtCaller, .action = State::ActionOnViolation::Continued });
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedEnforceTypeOrConvert, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue given = JSValue::decode(value);
    if (given && state.fits(state.type(type), given))
        OPERATION_RETURN(scope, value);
    String failureReason;
    bool didConvertInPlace = given && given.isObject() && state.convertInPlace(globalObject, asObject(given), state.type(type), &failureReason);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!didConvertInPlace)
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), type, failureReason);
    OPERATION_RETURN(scope, value);
}

static void throwKindMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t kinds, uint32_t name)
{
    State& state = *instance->typed;
    state.recordViolation(globalObject->vm(), { .kind = State::defaultViolationKind, .expectedKinds = kinds, .expectedName = name, .got = value });
    throwTypeError(globalObject, scope, withAOTErrorNote(makeString(auditPrefix(state), "expected "_s, state.describeKinds(kinds, name), ", got "_s, State::describe(value))));
}

static void throwNonNullFailed(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t type, uint32_t text)
{
    State& state = *instance->typed;
    state.recordViolation(globalObject->vm(), { .kind = "non-null"_s, .what = "NonNull"_s, .expected = &state.type(type), .got = value });
    throwTypeError(globalObject, scope, withAOTErrorNote(value.isUndefinedOrNull() ? State::nonNullMessage(value, text == UINT32_MAX ? String() : state.text(text)) : state.mismatchMessage(globalObject->vm(), value, state.type(type))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCollectionMethod, EncodedJSValue, (Instance* instance, JSCell* table, uint32_t which))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    MapOrSetMethod method = static_cast<MapOrSetMethod>(which);
    bool isSet = method == MapOrSetMethod::SetHas || method == MapOrSetMethod::SetAdd;
    const Identifier* name = &vm.propertyNames->has;
    Intrinsic own = isSet ? JSSetHasIntrinsic : JSMapHasIntrinsic;
    if (method == MapOrSetMethod::MapGet) {
        name = &vm.propertyNames->get;
        own = JSMapGetIntrinsic;
    } else if (method == MapOrSetMethod::MapSet) {
        name = &vm.propertyNames->set;
        own = JSMapSetIntrinsic;
    } else if (method == MapOrSetMethod::SetAdd) {
        name = &vm.propertyNames->add;
        own = JSSetAddIntrinsic;
    }
    JSValue found;
    if (MegamorphicCache* cache = vm.megamorphicCache()) {
        if (auto* known = cache->findLoad(table->structureID(), name->impl()))
            found = !known->m_holder ? jsUndefined() : asObject(known->m_holder == JSCell::seenMultipleCalleeObjects() ? table : known->m_holder)->getDirect(known->m_offset);
    }
    if (!found) {
        PropertySlot slot(table, PropertySlot::InternalMethodType::Get);
        found = getByIdAndFillMegamorphicCache(globalObject, table, *name, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (!found || !found.isCell()) {
        throwNotAFunction(globalObject, scope, *name, found ? found : jsUndefined());
        OPERATION_RETURN(scope, encodedJSValue());
    }
    auto* function = dynamicDowncast<JSFunction>(found.asCell());
    if (function && function->isHostFunction() && function->intrinsic() == own && table->type() == (isSet ? JSSetType : JSMapType))
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(found));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasByName, EncodedJSValue, (Instance* instance, JSCell* table, EncodedJSValue key, EncodedJSValue method))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue answer = callValue(globalObject, vm, JSValue::decode(method), table, 1, &key);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!answer.isBoolean()) {
        throwKindMismatch(instance, globalObject, scope, answer, Typed::Primitive::Boolean, UINT32_MAX);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(answer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAssertionFailed, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t what, uint32_t function, uint32_t lineAndColumn))
{
    State& state = *instance->typed;
    return JSValue::encode(state.assertionFailed(JSValue::decode(value), state.type(type), what, { function, lineAndColumn }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStoreAssertionFailed, EncodedJSValue, (Instance* instance, EncodedJSValue value, EncodedJSValue storedTo, uint32_t type, uint32_t function, uint32_t lineAndColumn))
{
    State& state = *instance->typed;
    return JSValue::encode(state.assertionFailed(JSValue::decode(value), state.type(type), static_cast<uint32_t>(Typed::Format::CastReason::Store), { function, lineAndColumn }, { }, JSValue::decode(storedTo)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowReachedUnreachable, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwTypeError(globalObject, scope, "audit stopped: reached code the types call unreachable (a function declared to return 'never' returned)"_s);
    OPERATION_RETURN(scope);
}

static void throwNullish(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t name, Typed::NullishAccess access)
{
    if (access == Typed::NullishAccess::Iterate)
        throwTypeError(globalObject, scope, makeString(value.isNull() ? "null"_s : "undefined"_s, " is not iterable"_s));
    else
        throwTypeError(globalObject, scope, State::nullishMessage(value, name == UINT32_MAX ? String() : nameAt(instance, name).string(), access == Typed::NullishAccess::Write));
}

static uint8_t cachePropertyRunOffset(VM& vm, Instance* instance, Slot* header, Structure* structure, unsigned index, bool isStore, uint32_t name)
{
    header->offset |= 1u << index;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, nameAt(instance, name).impl(), attributes);
    bool hasInlineOffset = isValidOffset(offset) && isInlineOffset(offset) && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) && !(isStore && (attributes & PropertyAttribute::ReadOnly || structure->mayBePrototype()));
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("operationAOTTypedCachePropertyLocation", hasInlineOffset ? "learned" : "refused");
    if (!hasInlineOffset)
        return 0;
    if (isStore)
        structure->didCachePropertyReplacement(vm, offset);
    uint8_t place = static_cast<uint8_t>(offset + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
    std::bit_cast<uint8_t*>(&header[index / 8].pointer)[index % 8] = place;
    return place;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCachePropertyRun, size_t, (Instance* instance, Slot* header, EncodedJSValue encodedBase, uint32_t slotsAndStored, uint64_t firstNames, uint64_t nextNames))
{
    unsigned slots = slotsAndStored >> 24;
    if (SharedData::contains(header) || (header->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return 0;
    auto counted = [&](bool cacheIsPopulated) -> size_t {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(*instance->vm).countOperation("operationAOTTypedCachePropertyRun", cacheIsPopulated ? "learned" : "refused");
        return cacheIsPopulated;
    };
    header->offset = (header->offset & ~0xffffffu) + (1u << Slot::attemptsShift);
    if ((header->offset & Slot::attemptsMask) >> Slot::attemptsShift >= 4)
        header->offset |= Slot::attemptsMask;
    header->structureID = StructureID();
    for (unsigned i = 0; i < slots; ++i)
        header[i].pointer = nullptr;
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return counted(false);
    Structure* structure = base.asCell()->structure();
    if (structure->compiledLayout() || structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return counted(false);
    header->structureID = structure->id();
    VM& vm = *instance->vm;
    if (Data* data = instance->dataWithSlot(header, nullptr))
        didFillSlot(vm, data);
    DeferGCForAWhile deferGC(vm);
    for (unsigned index = 0; index < 6; ++index) {
        uint32_t name = static_cast<uint32_t>((index < 3 ? firstNames : nextNames) >> (index % 3 * 21)) & 0x1fffff;
        if (name == 0x1fffff)
            continue;
        cachePropertyRunOffset(vm, instance, header, structure, index, slotsAndStored >> index & 1, name);
    }
    return counted(true);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCachePropertyLocation, size_t, (Instance* instance, Slot* header, EncodedJSValue encodedBase, uint32_t indexAndIsStore, uint32_t name))
{
    VM& vm = *instance->vm;
    unsigned index = indexAndIsStore & 0xff;
    JSValue base = JSValue::decode(encodedBase);
    if (SharedData::contains(header) || !base.isCell() || base.asCell()->structureID() != header->structureID)
        return 0;
    DeferGCForAWhile deferGC(vm);
    return cachePropertyRunOffset(vm, instance, header, base.asCell()->structure(), index, indexAndIsStore >> 8, name);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFactViolated, void, (Instance* instance, EncodedJSValue value, uint32_t function, uint32_t number))
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> reported;
    {
        Locker locker { lock };
        if (!reported->add(static_cast<uint64_t>(function) << 32 | number).isNewEntry)
            return;
    }
    DeferGCForAWhile deferGC(*instance->vm);
    JSValue found = JSValue::decode(value);
    if (!found) {
        dataLogLn("AOT: type fact violated: f", function, " v", number, " is the empty value");
        return;
    }
    bool isDescribedByPrototype = found.isObject() && !dynamicDowncast<Typed::Object>(found) && !asObject(found)->isCallable() && !dynamicDowncast<JSArray>(found);
    dataLogLn("AOT: type fact violated: f", function, " v", number, " is ", isDescribedByPrototype ? makeString("object "_s, asObject(found)->classInfo()->className) : State::describe(found));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCountBranch, void, (Instance* instance, uint32_t module, uint32_t positionAndSide))
{
    if (Options::useAOTOperationCounters())
        runtimeTable(*instance->vm).countOperationAtSite("Branch", module, positionAndSide, module, positionAndSide / 2);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCheckKnownSlot, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t name, uint32_t placeAndFlags, uint32_t function, uint32_t number))
{
    constexpr uint32_t isNeverNullish = 0x100;
    constexpr uint32_t isStore = 0x200;
    constexpr uint32_t isFirstStore = 0x400;
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isUndefinedOrNull() && !(placeAndFlags & isNeverNullish))
        return 0;
    if (base.isCell() && base.asCell()->type() == FinalObjectType) {
        Structure* structure = base.asCell()->structure();
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, nameAt(instance, name).impl(), attributes);
        unsigned refused = static_cast<unsigned>(placeAndFlags & isStore ? PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue : PropertyAttribute::AccessorOrCustomAccessorOrValue);
        if (offset == static_cast<PropertyOffset>(placeAndFlags & 0xff) && static_cast<unsigned>(offset) < structure->inlineCapacity() && !(attributes & refused) && !(placeAndFlags & isFirstStore && attributes) && !(placeAndFlags & isStore && structure->mayBePrototype()))
            return 1;
    }
    operationAOTTypedFactViolated(instance, encodedBase, function, number);
    return 0;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowStackOverflow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

static void throwTDZError(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, uint32_t name)
{
    if (name != UINT32_MAX && instance->typed->data.stringFacts(name) & Typed::Data::stringIsPrivateName) {
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return;
    }
    String variable = name == UINT32_MAX ? String() : nameAt(instance, name).string();
    throwException(globalObject, scope, createReferenceError(globalObject, variable.isEmpty() ? "Cannot access variable before initialization"_str : makeString("Cannot access '"_s, variable, "' before initialization."_s)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t isTrap))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto faultingAccess = isTrap ? std::nullopt : faultingAccessAt(reinterpret_cast<const uint8_t*>(site) - sizeOfNearCall);
    if (faultingAccess)
        site = faultingAccess->throwSite.data();
    uint32_t wordInRegister = static_cast<uint32_t>(registers[static_cast<unsigned>(throwWordGPR)]);
    if (isTrap == ThrowSite::wordIsInRegister)
        site = &wordInRegister;
    else if (isTrap)
        site = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(site) - offsetOfReturnAddressInTrap + 1);
    uint32_t word = *site++;
    uint32_t a = word & ThrowSite::hasWideA ? *site++ : (word & INT32_MAX) >> ThrowSite::aShift;
    if (a == ThrowSite::none)
        a = UINT32_MAX;
    uint32_t b = word & ThrowSite::hasB ? *site++ : UINT32_MAX;
    unsigned number = word >> ThrowSite::registerShift & 31;
    if (static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7) == Typed::Rep::Void && number) {
        bool returnsToSite = word >> 31;
        if (!returnsToSite && !instance->callSiteOverrides && !globalObject->debugger() && !Options::breakOnThrow() && instance->nearestHandlerIgnoresValue(vm, callFrame->callerFrame(), removeCodePtrTag(callFrame->rawReturnPC()))) {
            instance->thrownValue = JSValue::encode(jsUndefined());
            OPERATION_RETURN(scope);
        }
        ErrorType errorType = static_cast<ErrorType>(number - 1);
        constexpr bool useCurrentFrame = true;
        auto* error = ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), instance->typed->string(vm, a), jsUndefined(), nullptr, TypeNothing, errorType, useCurrentFrame);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        bool isAtStackTraceItemSite = static_cast<ThrowKind>(word & 7) == ThrowKind::KindMismatch;
        if (!returnsToSite) {
            instance->thrownValue = JSValue::encode(error);
            instance->thrownValueIsAtStackTraceItemSite = isAtStackTraceItemSite;
            OPERATION_RETURN(scope);
        }
        Exception* exception = throwException(globalObject, scope, error);
        if (isAtStackTraceItemSite)
            exception->setTopFrameIsAtStackTraceItemSite();
        OPERATION_RETURN(scope);
    }
    uint64_t bits = registers[number];
    if (faultingAccess && static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7) == Typed::Rep::Cell && bits >= FaultingAccessInImage::reach)
        bits = (bits - MarkedBlockCage::base()) >> MarkedBlockCage::shift;
    JSValue value;
    switch (static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7)) {
    case Typed::Rep::Void:
        value = jsUndefined();
        break;
    case Typed::Rep::Int32:
        value = jsBoolean(bits & 1);
        break;
    case Typed::Rep::Int64:
        value = jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
        break;
    case Typed::Rep::Double:
        value = jsNumber(purifyNaN(std::bit_cast<double>(registers[ThrowSite::numberOfRegistersPerBank + number])));
        break;
    case Typed::Rep::Cell:
    case Typed::Rep::JSValue:
        value = JSValue::decode(bits);
        if (!value)
            value = jsUndefined();
        break;
    case Typed::Rep::OptionalInt64:
        if (static_cast<int64_t>(bits) == Typed::nullInteger)
            value = jsNull();
        else if (static_cast<int64_t>(bits) == Typed::undefinedInteger || static_cast<int64_t>(bits) == Typed::absentInteger)
            value = jsUndefined();
        else
            value = jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
        break;
    case Typed::Rep::OptionalDouble:
        bits = registers[ThrowSite::numberOfRegistersPerBank + number];
        if (bits == Typed::nullDouble)
            value = jsNull();
        else if (bits == Typed::undefinedDouble || bits == Typed::absentDouble)
            value = jsUndefined();
        else
            value = jsNumber(purifyNaN(std::bit_cast<double>(bits)));
        break;
    }
    switch (static_cast<ThrowKind>(word & 7)) {
    case ThrowKind::TypeMismatch:
        if (faultingAccess && static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7) == Typed::Rep::JSValue) {
            CallSiteOverride castSiteOverride(*instance, callFrame->callerFrame());
            throwTypeMismatch(instance, globalObject, scope, value, a);
            break;
        }
        throwTypeMismatch(instance, globalObject, scope, value, a);
        break;
    case ThrowKind::KindMismatch:
        throwKindMismatch(instance, globalObject, scope, value, a, b);
        break;
    case ThrowKind::NullishRead:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Read);
        break;
    case ThrowKind::NullishWrite:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Write);
        break;
    case ThrowKind::NullishIterate:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Iterate);
        break;
    case ThrowKind::NonNullFailed:
        throwNonNullFailed(instance, globalObject, scope, value, a, b);
        break;
    case ThrowKind::TDZ:
        throwTDZError(instance, globalObject, scope, a);
        break;
    case ThrowKind::NotAFunction:
        if (a == UINT32_MAX)
            throwIsNotAFunction(globalObject, scope, value);
        else
            throwNotAFunction(globalObject, scope, nameAt(instance, a), value);
        break;
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObject, JSCell*, (Instance* instance))
{
    return constructEmptyObject(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObjectForKeys, JSCell*, (Instance* instance, uint32_t hasNullPrototype))
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    Structure*& structure = instance->emptyStructuresForKeyedStores[!!hasNullPrototype];
    if (!structure) [[unlikely]]
        structure = JSFinalObject::createStructure(vm, globalObject, hasNullPrototype ? jsNull() : JSValue(globalObject->objectPrototype()), JSFinalObject::defaultInlineCapacity);
    return constructEmptyObject(vm, structure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObjectWithCapacity, JSCell*, (Instance* instance, uint32_t inlineCapacity))
{
    JSGlobalObject* globalObject = instance->globalObject;
    return constructEmptyObject(*instance->vm, globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), inlineCapacity));
}

Structure* structureAfterPropertyRun(Instance* instance, VM& vm, Structure* structure, std::span<UniquedStringImpl* const> names)
{
    auto& target = instance->propertyRunTarget(structure, instance->canonicalPropertyRun(names), [&](Vector<UniquedStringImpl*, 16>& result) {
        result.append(names);
    });
    Structure* last = target.last;
    if (!last || !Instance::hasPrototypeStructures(structure, target.prototypeStructures.span()))
        return nullptr;
    PropertyOffset offset = structure->maxOffset();
    for (UniquedStringImpl* name : names) {
        offset = Instance::offsetAfter(offset, structure->inlineCapacity());
        if (name->isSymbol() && last->get(vm, name) != offset)
            return nullptr;
    }
    return last;
}

static bool mayCacheForThis(Instance* instance, Data* data, Slot* cache)
{
    if (SharedData::contains(cache) || (cache[0].offset & Slot::attemptsMask) == Slot::attemptsMask)
        return false;
    cache[0].offset += 1u << Slot::attemptsShift;
    cache[0].clear();
    stopWatching(data, cache);
    cache[2].pointer = std::bit_cast<void*>(static_cast<uintptr_t>(instance->negativeOffsetFields().typedPrototypeEpoch));
    return true;
}

static uint32_t slotsForStubToClear(Structure* structure, uint32_t numberInRegisters)
{
    uint32_t places = WTF::roundUpToMultipleOf<2>(structure->inlineCapacity());
    return places > WTF::roundUpToMultipleOf<2>(numberInRegisters) ? places : 0;
}

UniquedStringImpl* nameInStructureDescriptor(VM& vm, unsigned number, unsigned index)
{
    Image* image = Image::withCode();
    auto& data = *image->at<Typed::Data>(image->header().typedDataOffset);
    uint32_t facts = data.stringFacts(data.row(number)[1 + index]);
    RELEASE_ASSERT(facts & Typed::Data::stringHasIdentifierNumber);
    const Identifier* name = VMProgram::of(vm)->identifierIfCreated(facts & Typed::Data::stringHashMask);
    RELEASE_ASSERT(name);
    return name->impl();
}

static JSObject* newObjectWithFields(Instance* instance, JSGlobalObject* globalObject, VM& vm, Data* data, Slot* cache, Structure* empty, JSCell* constructor, uint32_t count, const EncodedJSValue* values, const uint32_t* nameIndicesOf, uint32_t numberInRegisters, uint32_t structureDescriptorPlusOne = 0)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto valueOrUndefinedAt = [&](unsigned i) {
        JSValue value = JSValue::decode(values[i]);
        return value ? value : jsUndefined();
    };
    if (globalObject != instance->globalObject)
        structureDescriptorPlusOne = 0;
    if (count > 1 || (count && structureDescriptorPlusOne)) {
        bool isOrdinaryLiteral = !constructor && empty->storedPrototype() == JSValue(globalObject->objectPrototype());
        Vector<UniquedStringImpl*, 16> names;
        for (unsigned i = 0; i < count; ++i) {
            const Identifier& name = nameAt(instance, nameIndicesOf[i]);
            if (!indexNamedBy(instance, nameIndicesOf[i], name))
                names.append(name.impl());
        }
        bool hasIndex = names.size() != count;
        if (hasIndex)
            structureDescriptorPlusOne = 0;
        if (Structure* structure = hasIndex && names.size() < 2 ? nullptr : isOrdinaryLiteral ? instance->literalStructure(empty, names.span(), structureDescriptorPlusOne) : structureAfterPropertyRun(instance, vm, empty, names.span())) {
            unsigned inlineCapacity = structure->inlineCapacity();
            if (hasIndex) {
                JSObject* object = names.size() > inlineCapacity
                    ? JSFinalObject::createWithButterfly(vm, structure, Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0))
                    : constructEmptyObject(vm, structure);
                unsigned numberOfNamed = 0;
                for (unsigned i = 0; i < count; ++i) {
                    if (indexNamedBy(instance, nameIndicesOf[i], nameAt(instance, nameIndicesOf[i])))
                        continue;
                    object->putDirectOffset(vm, offsetForPropertyNumber(numberOfNamed++, inlineCapacity), JSValue::decode(values[i]));
                }
                for (unsigned i = 0; i < count; ++i) {
                    if (auto index = indexNamedBy(instance, nameIndicesOf[i], nameAt(instance, nameIndicesOf[i]))) {
                        object->putDirectIndex(globalObject, *index, valueOrUndefinedAt(i));
                        RETURN_IF_EXCEPTION(scope, nullptr);
                    }
                }
                return object;
            }
            if (count > inlineCapacity) {
                JSObject* object = JSFinalObject::createWithButterfly(vm, structure, Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0));
                for (unsigned i = 0; i < count; ++i)
                    object->putDirectOffset(vm, i < inlineCapacity ? i : firstOutOfLineOffset + (i - inlineCapacity), JSValue::decode(values[i]));
                return object;
            }
            JSObject* object = constructEmptyObject(vm, structure);
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            if (!constructor || mayCacheForThis(instance, data, cache))
                fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), slotsForStubToClear(structure, numberInRegisters), constructor);
            return object;
        }
    }
    JSObject* object = constructEmptyObject(vm, empty);
    bool areInOrder = true;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, nameIndicesOf[i]);
        JSValue value = valueOrUndefinedAt(i);
        if (auto index = indexNamedBy(instance, nameIndicesOf[i], name)) {
            object->putDirectIndex(globalObject, *index, value);
            RETURN_IF_EXCEPTION(scope, nullptr);
            areInOrder = false;
            continue;
        }
        PutPropertySlot slot(object, true, PutPropertySlot::PutById);
        object->putDirect(vm, name, value, slot);
        areInOrder = areInOrder && slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.cachedOffset() == static_cast<PropertyOffset>(i);
    }
    Structure* structure = object->structure();
    if (areInOrder && !structure->isDictionary() && count <= structure->inlineCapacity() && !object->butterfly()) {
        if (constructor && !mayCacheForThis(instance, data, cache))
            return object;
        fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), slotsForStubToClear(structure, numberInRegisters), constructor);
    }
    return object;
}

static Structure* emptyLiteralStructure(Instance* instance, JSGlobalObject* globalObject, VM& vm, bool hasNullPrototype, unsigned inlineCapacity)
{
    if (hasNullPrototype)
        return instance->emptyStructureWithoutPrototype(inlineCapacity);
    bool isCached = inlineCapacity < std::size(instance->emptyLiteralStructures) && globalObject == instance->globalObject;
    if (isCached && instance->emptyLiteralStructures[inlineCapacity])
        return instance->emptyLiteralStructures[inlineCapacity];
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    if (isCached)
        instance->emptyLiteralStructures[inlineCapacity] = profile.structure();
    return profile.structure();
}

JSObject* newObjectLiteralAtSite(Instance* instance, JSGlobalObject* globalObject, VM& vm, FunctionRef function, Slot* cache, const EncodedJSValue* values, uint32_t numberOfValues)
{
    const uint32_t* row = function.info().function()->plans() + function.siteConstantOf(cache + 1) - 1;
    uint32_t count = *row & 0xffff;
    RELEASE_ASSERT(numberOfValues <= std::min(count, maxLiteralPropertiesInRegisters) && count <= maxNamedPropertiesPerSite);
    std::array<EncodedJSValue, maxNamedPropertiesPerSite> all;
    for (uint32_t i = 0; i < count; ++i)
        all[i] = i < numberOfValues ? values[i] : JSValue::encode(jsUndefined());
    Data* data = function.instance->dataIfExists(function.index);
    JSObject* object = newObjectWithFields(instance, globalObject, vm, data ? data : function.instance->sharedData, cache, emptyLiteralStructure(instance, globalObject, vm, *row >> 31, std::max(count, *row >> 16 & 0x7fff)), nullptr, count, all.data(), row + 1, numberOfValues, row[1 + count]);
    RELEASE_ASSERT(object && !object->butterfly() && count <= object->structure()->inlineCapacity());
    return object;
}

JSObject* newObjectFromDescriptor(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t number, const EncodedJSValue* values, uint32_t numberOfValues)
{
    std::span<const uint32_t> row = reinterpret_cast<const Typed::Data*>(instance->typedData)->row(number);
    uint32_t count = row[0] & 0xff;
    uint32_t inlineCapacityHint = row[0] >> 8 & 0xff;
    bool hasNullPrototype = row[0] >> 16 & 1;
    RELEASE_ASSERT(row.size() == count + 1 && numberOfValues == std::min(count, maxLiteralPropertiesInRegisters) && count <= maxNamedPropertiesPerSite);
    std::array<EncodedJSValue, maxNamedPropertiesPerSite> all;
    for (uint32_t i = 0; i < count; ++i)
        all[i] = i < numberOfValues ? values[i] : JSValue::encode(jsUndefined());
    JSObject* object = newObjectWithFields(instance, globalObject, vm, instance->sharedData, instance->sharedData->slots, emptyLiteralStructure(instance, globalObject, vm, hasNullPrototype, std::max({ count, inlineCapacityHint, instance->learnedInlineCapacityOfDescriptor(number) })), nullptr, count, all.data(), row.data() + 1, numberOfValues, number + 1);
    RELEASE_ASSERT(object && !object->butterfly() && count <= object->structure()->inlineCapacity());
    Structure* structure = object->structure();
    Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator);
    if (allocator && !structure->isDictionary() && structure->typeInfoBlob() == Instance::structureDescriptorTypeInfoBlob && structure->maxOffset() == static_cast<PropertyOffset>(count) - 1) {
        instance->fillStructureDescriptorSlot(number, structure, slotsForStubToClear(structure, numberOfValues), allocator.localAllocator());
        Vector<UniquedStringImpl*, 16> names;
        for (uint32_t i = 0; i < count; ++i)
            names.append(nameAt(instance, row[1 + i]).impl());
        setInlineSlotPropertyNameIDs(vm, structure, names.span());
    }
    return object;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewObjectLiteral, JSCell*, (Instance* instance, Slot* cache, uint32_t count, uint32_t hasNullPrototypeAndInlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    bool hasNullPrototype = hasNullPrototypeAndInlineCapacity & 1;
    AOT_OPERATION_BEGIN_AT(instance, cache);
    if (StructureID structureID = cache[0].structureID) {
        JSObject* object = constructEmptyObject(vm, WTF::opaque(structureID.decode()));
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }
    Structure* empty = emptyLiteralStructure(instance, globalObject, vm, hasNullPrototype, std::max(count, hasNullPrototypeAndInlineCapacity >> 1));
    OPERATION_RETURN(scope, newObjectWithFields(instance, globalObject, vm, callerData(instance, callFrame), cache, empty, nullptr, count, values, caller(instance, callFrame).info().function()->plans() + rowOfNames, std::min(count, maxLiteralPropertiesInRegisters)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisWithFields, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    const uint32_t* nameIndicesOf = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    if (Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity))
        OPERATION_RETURN(scope, newObjectWithFields(instance, globalObject, vm, callerData(instance, callFrame), cache, empty, newTarget, count, values, nameIndicesOf, std::min(count, maxLiteralPropertiesInRegisters)));
    JSValue made = instance->typed->createThis(globalObject, constructor, asObject(newTarget));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    SetForScope onlyCopies(instance->typed->storesConvertPlainData, false);
    for (unsigned i = 0; i < count; ++i) {
        createDataProperty(globalObject, asObject(made), nameAt(instance, nameIndicesOf[i]), JSValue::decode(values[i]));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, made.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisWithCapacity, JSCell*, (Instance* instance, JSCell* constructorCell, JSCell* newTarget, uint32_t inlineCapacity))
{
    AOT_OPERATION_BEGIN(instance);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    if (Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity))
        OPERATION_RETURN(scope, constructEmptyObject(vm, empty));
    OPERATION_RETURN(scope, instance->typed->createThis(globalObject, constructor, asObject(newTarget)).asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisAt, JSCell*, (Instance* instance, Slot* cache, JSCell* constructor, JSCell* newTarget))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!isSlimClosureType(constructor->type()) && !isSlimClosureType(newTarget->type())) {
        auto* closure = uncheckedDowncast<Typed::Closure>(constructor);
        if (const Typed::LayoutData* ofClass = closure->classData(); ofClass && ofClass->hasFixedLayout && !closure->isDeclaredAs(Typed::functionReadsNewTarget))
            OPERATION_RETURN(scope, instance->typed->createThis(globalObject, closure, asObject(newTarget)).asCell());
    }
    callFrame = siteFrame(instance, callFrame);
    countMissAt(instance, callFrame, cache);
    Structure* empty;
    if (isSlimClosureType(constructor->type()) || isSlimClosureType(newTarget->type())) [[unlikely]] {
        empty = instance->typed->structureForThisWithSlimClosure(globalObject, asObject(constructor), asObject(newTarget), JSFinalObject::defaultInlineCapacity);
        auto* closure = dynamicDowncast<Typed::Closure>(constructor);
        bool readsNewTarget = closure && closure->isDeclaredAs(Typed::functionReadsNewTarget);
        if (!empty || readsNewTarget) {
            JSObject* made = empty ? constructEmptyObject(vm, empty) : Typed::ordinaryCreateFromConstructor(globalObject, asObject(newTarget));
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            if (readsNewTarget)
                instance->typed->newTarget = newTarget;
            OPERATION_RETURN(scope, made);
        }
    } else {
        auto* closure = uncheckedDowncast<Typed::Closure>(constructor);
        empty = instance->typed->structureForThis(globalObject, closure, asObject(newTarget), JSFinalObject::defaultInlineCapacity);
        if (closure->isDeclaredAs(Typed::functionReadsNewTarget)) [[unlikely]] {
            if (Allocator allocator = empty ? subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(empty->inlineCapacity()), AllocatorForMode::EnsureAllocator) : Allocator()) {
                if (!SharedData::contains(cache))
                    cache[0].offset |= Slot::attemptsMask;
                Slot* shared = instance->thisAllocationFor(newTarget);
                shared[0].structureID = StructureID();
                shared[0].offset = empty->inlineCapacity() | Slot::pointerIsCell;
                shared[0].pointer = newTarget;
                shared[1].offset = empty->typeInfoBlob();
                shared[1].pointer = allocator.localAllocator();
                shared[2].pointer = std::bit_cast<void*>(static_cast<uintptr_t>(instance->negativeOffsetFields().typedPrototypeEpoch));
                shared[3].offset = 1;
                shared[3].pointer = constructor;
                shared[0].structureID = empty->id();
                instance->hasThisAllocations = true;
            }
            JSValue made = empty ? JSValue(constructEmptyObject(vm, empty)) : instance->typed->createThis(globalObject, closure, asObject(newTarget));
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            instance->typed->newTarget = newTarget;
            OPERATION_RETURN(scope, made.asCell());
        }
        if (!empty)
            OPERATION_RETURN(scope, instance->typed->createThis(globalObject, closure, asObject(newTarget)).asCell());
    }
    Data* data = callerData(instance, callFrame);
    Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(empty->inlineCapacity()), AllocatorForMode::EnsureAllocator);
    if (mayCacheForThis(instance, data, cache))
        fillAllocationCache(vm, data, cache, empty, allocator, empty->inlineCapacity(), newTarget);
    if (allocator) {
        Slot* shared = instance->thisAllocationFor(newTarget);
        shared[0].structureID = StructureID();
        shared[0].offset = empty->inlineCapacity() | Slot::pointerIsCell;
        shared[0].pointer = newTarget;
        shared[1].offset = empty->typeInfoBlob();
        shared[1].pointer = allocator.localAllocator();
        shared[2].pointer = std::bit_cast<void*>(static_cast<uintptr_t>(instance->negativeOffsetFields().typedPrototypeEpoch));
        shared[3].offset = 0;
        shared[3].pointer = constructor;
        shared[0].structureID = empty->id();
        instance->hasThisAllocations = true;
    }
    OPERATION_RETURN(scope, constructEmptyObject(vm, empty));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryCreateThisWithStores, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* valuesThenNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    if (!empty || count > empty->inlineCapacity())
        OPERATION_RETURN(scope, nullptr);
    Vector<UniquedStringImpl*, 16> names;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, static_cast<uint32_t>(valuesThenNames[count + i]));
        if (indexNamedBy(instance, static_cast<uint32_t>(valuesThenNames[count + i]), name))
            OPERATION_RETURN(scope, nullptr);
        names.append(name.impl());
    }
    JSObject* object = constructEmptyObject(vm, empty);
    makePrototypeChainWatchable(vm, object);
    Structure* structure = structureAfterPropertyRun(instance, vm, empty, names.span());
    if (!structure)
        OPERATION_RETURN(scope, nullptr);
    for (unsigned i = 0; i < count; ++i)
        object->putDirectOffset(vm, i, JSValue::decode(valuesThenNames[i]));
    object->setStructure(vm, structure);
    Data* data = callerData(instance, callFrame);
    if (!mayCacheForThis(instance, data, cache))
        OPERATION_RETURN(scope, object);
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        if (valuesThenNames[count + i] >> 32)
            stored.append(names[i]);
    }
    if (watchStores(globalObject, data, cache, object, structure, stored.span()))
        fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisWithStores, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    constexpr uint32_t isStoredTo = 1u << 31;
    Instance::StructureOfThis& known = instance->structureOfThis(newTarget, cache);
    if (known.newTarget == newTarget && known.site == cache && known.prototypeEpoch == instance->negativeOffsetFields().typedPrototypeEpoch) {
        JSValue prototype = known.structure->storedPrototype();
        bool isUnchanged = true;
        for (unsigned i = 0; i < known.numberOfPrototypes && isUnchanged; ++i) {
            isUnchanged = prototype.isObject() && asObject(prototype)->structureID().bits() == known.prototypeStructureIDs[i];
            if (isUnchanged)
                prototype = asObject(prototype)->structure()->storedPrototype();
        }
        if (isUnchanged && !prototype.isObject()) {
            JSObject* object = constructEmptyObject(vm, known.structure);
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            OPERATION_RETURN(scope, object);
        }
    }
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    const uint32_t* nameIndicesOf = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    Data* data = callerData(instance, callFrame);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    bool canAllocateDirectly = empty && count <= empty->inlineCapacity();
    Vector<UniquedStringImpl*, 16> names;
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, nameIndicesOf[i] & ~isStoredTo);
        canAllocateDirectly = canAllocateDirectly && !indexNamedBy(instance, nameIndicesOf[i] & ~isStoredTo, name);
        names.append(name.impl());
        if (nameIndicesOf[i] & isStoredTo)
            stored.append(name.impl());
    }
    auto fill = [&](JSObject* object, Structure* structure) {
        if (watchStores(globalObject, data, cache, object, structure, stored.span()))
            fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    };
    if (canAllocateDirectly) {
        JSObject* object = constructEmptyObject(vm, empty);
        makePrototypeChainWatchable(vm, object);
        if (Structure* structure = structureAfterPropertyRun(instance, vm, empty, names.span())) {
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            object->setStructure(vm, structure);
            if (mayCacheForThis(instance, data, cache))
                fill(object, structure);
            else if (!SharedData::contains(cache)) {
                Instance::StructureOfThis remembered { newTarget, cache, structure, instance->negativeOffsetFields().typedPrototypeEpoch, 0, { } };
                JSValue prototype = structure->storedPrototype();
                while (prototype.isObject() && remembered.numberOfPrototypes < Instance::StructureOfThis::maxPrototypes) {
                    remembered.prototypeStructureIDs[remembered.numberOfPrototypes++] = asObject(prototype)->structureID().bits();
                    prototype = asObject(prototype)->structure()->storedPrototype();
                }
                if (!prototype.isObject())
                    known = remembered;
            }
            OPERATION_RETURN(scope, object);
        }
    }
    JSValue made = empty ? JSValue(constructEmptyObject(vm, empty)) : instance->typed->createThis(globalObject, constructor, asObject(newTarget));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* object = asObject(made);
    {
        CallSiteOverride where(*instance, callFrame->callerFrame());
        for (unsigned i = 0; i < count; ++i) {
            where.setItem(i);
            uint32_t number = nameIndicesOf[i] & ~isStoredTo;
            const Identifier& name = nameAt(instance, number);
            if (isClassPrivateName(instance, name, number)) [[unlikely]]
                privateSet(globalObject, vm, object, name, JSValue::decode(values[i]));
            else if (nameIndicesOf[i] & isStoredTo) {
                PutPropertySlot slot(object, true);
                JSValue(object).put(globalObject, name, JSValue::decode(values[i]), slot);
            } else
                object->createDataProperty(globalObject, name, JSValue::decode(values[i]), true);
            if (scope.exception()) [[unlikely]]
                mayCacheForThis(instance, data, cache);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    if (!mayCacheForThis(instance, data, cache))
        OPERATION_RETURN(scope, object);
    empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    Structure* structure = object->structure();
    if (!empty || structure->isDictionary() || count > structure->inlineCapacity() || object->butterfly())
        OPERATION_RETURN(scope, object);
    Structure* first = structure;
    for (unsigned i = count; i-- && first;) {
        unsigned attributes = 0;
        if (structure->get(vm, names[i], attributes) != static_cast<PropertyOffset>(i) || attributes)
            OPERATION_RETURN(scope, object);
        first = first->previousID();
    }
    if (first != empty)
        OPERATION_RETURN(scope, object);
    makePrototypeChainWatchable(vm, object);
    fill(object, structure);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAssignThisStructure, void, (Instance* instance, Slot* cache, EncodedJSValue self, uint32_t count, uint32_t rowOfNames, uint32_t structureDescriptorPlusOne))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    JSValue value = JSValue::decode(self);
    if (!value.isCell() || value.asCell()->type() != FinalObjectType)
        OPERATION_RETURN(scope);
    JSObject* object = asObject(value);
    Structure* empty = object->structure();
    if (empty->previousID() || isValidOffset(empty->maxOffset()) || empty->isDictionary() || empty->hasPolyProto() || !empty->isStructureExtensible() || empty->mayBePrototype() || empty->compiledLayout() || !empty->propertyAccessesAreCacheable() || object->butterfly() || count > empty->inlineCapacity())
        OPERATION_RETURN(scope);
    const uint32_t* nameIndicesOf = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    Vector<UniquedStringImpl*, 16> names;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, nameIndicesOf[i]);
        if (indexNamedBy(instance, nameIndicesOf[i], name))
            OPERATION_RETURN(scope);
        names.append(name.impl());
    }
    makePrototypeChainWatchable(vm, object);
    Structure* whole = structureAfterPropertyRun(instance, vm, empty, names.span());
    if (!whole || whole->inlineCapacity() != empty->inlineCapacity() || whole->outOfLineCapacity())
        OPERATION_RETURN(scope);
    if (structureDescriptorPlusOne && structureDescriptorPlusOne <= UINT16_MAX && globalObject == instance->globalObject && !whole->structureDescriptorPlusOne())
        whole->setStructureDescriptorPlusOne(structureDescriptorPlusOne);
    object->setStructure(vm, whole);
    if (cache)
        cacheAssignedThisStructure(globalObject, callerData(instance, callFrame), cache, object, empty, whole, names.span());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDidStoreToThis, void, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, JSCell* cell, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* names))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    Data* data = callerData(instance, callFrame);
    if (!mayCacheForThis(instance, data, cache))
        OPERATION_RETURN(scope);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    JSObject* object = asObject(cell);
    Structure* structure = object->structure();
    if (!empty || structure->isDictionary() || count > structure->inlineCapacity() || object->butterfly())
        OPERATION_RETURN(scope);
    Structure* first = structure;
    for (unsigned i = count; i-- && first;) {
        unsigned attributes = 0;
        if (structure->get(vm, PropertyName(nameAt(instance, static_cast<uint32_t>(names[i]))), attributes) != static_cast<PropertyOffset>(i) || attributes)
            OPERATION_RETURN(scope);
        first = first->previousID();
    }
    if (first != empty)
        OPERATION_RETURN(scope);
    makePrototypeChainWatchable(vm, object);
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        if (names[i] >> 32)
            stored.append(nameAt(instance, static_cast<uint32_t>(names[i])).impl());
    }
    if (!watchStores(globalObject, data, cache, object, structure, stored.span()))
        OPERATION_RETURN(scope);
    fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE bool tryPutPropertiesFromCache(Instance* instance, const Slot* cache, EncodedJSValue encodedBase, uint32_t count, uint64_t keyed, const EncodedJSValue* values)
{
    JSValue base = JSValue::decode(encodedBase);
    if (!instance->propertyRuns || keyed || !base.isCell())
        return false;
    JSCell* cell = base.asCell();
    uint32_t structureID = cell->structureID().bits();
    const auto& run = instance->propertyRuns[Instance::indexOfPropertyRun(structureID, cache)];
    VM& vm = *instance->vm;
    if (run.structureID != structureID || run.site != cache || run.count != count || !run.newStructureID || run.epoch != vm.megamorphicCache()->epoch())
        return false;
    JSObject* object = asObject(cell);
    for (unsigned i = 0; i < count; ++i)
        object->putDirectOffset(vm, run.first + i, JSValue::decode(values[i]));
    object->setStructure(vm, std::bit_cast<StructureID>(run.newStructureID).decode());
    countOperationNamed(instance, "operationAOTTypedTryPutProperties", "as-remembered");
    return true;
}

static ALWAYS_INLINE bool isCachedAsRejected(Instance* instance, const Slot* cache, EncodedJSValue encodedBase)
{
    JSValue base = JSValue::decode(encodedBase);
    if (!instance->propertyRuns || !base.isCell())
        return false;
    uint32_t structureID = base.asCell()->structureID().bits();
    const auto& run = instance->propertyRuns[Instance::indexOfPropertyRun(structureID, cache)];
    if (run.structureID != structureID || run.site != cache || run.newStructureID || run.epoch != instance->vm->megamorphicCache()->epoch())
        return false;
    countOperationNamed(instance, "operationAOTTypedTryPutProperties", "refused-as-remembered");
    return true;
}

static void cacheRejectedPropertyRun(Instance* instance, VM& vm, const Slot* cache, Structure* structure, std::span<UniquedStringImpl* const> names)
{
    if (SharedData::contains(cache))
        return;
    if (!instance->propertyRuns)
        instance->propertyRuns = static_cast<Instance::PropertyRun*>(fastZeroedMalloc(Instance::numberOfPropertyRuns * sizeof(Instance::PropertyRun)));
    MegamorphicCache& facts = vm.ensureMegamorphicCache();
    uint16_t epoch = facts.epoch();
    for (UniquedStringImpl* name : names)
        epoch = facts.epochForName(*name, structure->id());
    instance->propertyRuns[Instance::indexOfPropertyRun(structure->id().bits(), cache)] = { structure->id().bits(), 0, cache, static_cast<uint32_t>(names.size()), epoch, 0 };
}

static void cachePropertyRunInInstance(Instance* instance, VM& vm, const Slot* cache, Structure* structure, Structure* last, PropertyOffset first, std::span<UniquedStringImpl* const> names)
{
    if (SharedData::contains(cache) || !isInlineOffset(first))
        return;
    if (!instance->propertyRuns)
        instance->propertyRuns = static_cast<Instance::PropertyRun*>(fastZeroedMalloc(Instance::numberOfPropertyRuns * sizeof(Instance::PropertyRun)));
    MegamorphicCache& facts = vm.ensureMegamorphicCache();
    uint16_t epoch = facts.epoch();
    for (UniquedStringImpl* name : names)
        epoch = facts.epochForName(*name, structure->id());
    instance->propertyRuns[Instance::indexOfPropertyRun(structure->id().bits(), cache)] = { structure->id().bits(), last->id().bits(), cache, static_cast<uint32_t>(names.size()), epoch, static_cast<uint16_t>(first) };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryPutProperties, size_t, (Instance* instance, Slot* cache, EncodedJSValue encodedBase, uint64_t countAndNamesPlanOffset, uint64_t keyed, const EncodedJSValue* valuesThenKeys))
{
    uint32_t count = static_cast<uint32_t>(countAndNamesPlanOffset);
    constexpr uint32_t mayAlreadyHaveSomeBit = 1u << 27;
    if (tryPutPropertiesFromCache(instance, cache, encodedBase, count & ~mayAlreadyHaveSomeBit, keyed, valuesThenKeys))
        return OperationReturnType<size_t> { 1 };
    if (isCachedAsRejected(instance, cache, encodedBase))
        return OperationReturnType<size_t> { 0 };
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto refuse = [&]() -> size_t {
        if (SharedData::contains(cache))
            return 0;
        if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
            cache->offset += 1u << Slot::attemptsShift;
        else if (!cache->structureID)
            cache->offset |= Slot::isGetter;
        return 0;
    };
    bool hasElementStores = count >> 31;
    bool mayBePrototype = count >> 30 & 1;
    bool mayReplace = count >> 29 & 1;
    bool keysAreFixed = count >> 28 & 1;
    bool isPlainRun = !keyed && !(count >> 28);
    bool mayFindExistingProperties = count & mayAlreadyHaveSomeBit;
    count &= INT32_MAX >> 4;
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        OPERATION_RETURN(scope, refuse());
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (hasElementStores && (hasAnyArrayStorage(structure->indexingType()) || structure->holesMustForwardToPrototype(object)))
        OPERATION_RETURN(scope, refuse());
    if (structure->compiledLayout() || structure->isDictionary() || !structure->isStructureExtensible() || structure->hasImmutableShape() || structure->hasPolyProto() || (structure->mayBePrototype() && !mayBePrototype)
        || structure->transitionCountEstimate() > Structure::s_maxTransitionLength)
        OPERATION_RETURN(scope, refuse());
    if (hasElementStores) {
        object->tryMakeWritableContiguous(vm);
        structure = object->structure();
    }
    Vector<UniquedStringImpl*, 16> names;
    Vector<JSCell*, 8> keys;
    FunctionRef function = caller(instance, callFrame);
    const uint32_t* nameIndicesOf = function.info().function()->plans() + (countAndNamesPlanOffset >> 32);
    for (unsigned i = 0; i < count; ++i) {
        if (!(keyed >> i & 1)) {
            names.append(nameAt(instance, nameIndicesOf[i]).impl());
            continue;
        }
        JSValue key = JSValue::decode(valuesThenKeys[count + i]);
        if (!key.isSymbol())
            OPERATION_RETURN(scope, refuse());
        names.append(&asSymbol(key)->uid());
        keys.append(key.asCell());
    }
    Data* owner = SharedData::contains(cache) ? function.instance->sharedData : function.instance->dataIfExists(function.index);
    if (mayReplace && !structure->mayBePrototype() && structure->propertyAccessesAreCacheable()) {
        Vector<PropertyOffset, 16> offsets;
        for (UniquedStringImpl* name : names) {
            unsigned attributes;
            PropertyOffset offset = structure->get(vm, name, attributes);
            if (!isValidOffset(offset) || attributes || !isInlineOffset(offset))
                break;
            offsets.append(offset);
        }
        if (offsets.size() == count) {
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, offsets[i], JSValue::decode(valuesThenKeys[i]));
            if (!cacheReplacementPropertyRun(vm, owner, cache + 1, structure, offsets.span()))
                refuse();
            OPERATION_RETURN(scope, 1);
        }
    }
    auto siteIsPolymorphic = [&] {
        bool wasFilledOften = (cache->offset & Slot::attemptsMask) >= 4u << Slot::attemptsShift;
        return isPlainRun && wasFilledOften && cache->structureID && cache->structureID != structure->id();
    };
    if (!mayReplace && !hasElementStores && !structure->mayBePrototype() && !structure->hasImmutableProperties() && structure->propertyAccessesAreCacheable()) {
        unsigned attributes = 0;
        PropertyOffset first = structure->get(vm, names[0], attributes);
        bool areContiguous = isValidOffset(first) && isInlineOffset(first + static_cast<PropertyOffset>(count) - 1);
        for (unsigned i = 0; areContiguous && i < count; ++i) {
            bool isPrivate = names[i]->isSymbol() && static_cast<SymbolImpl*>(names[i])->isPrivate();
            areContiguous = !isPrivate && structure->get(vm, names[i], attributes) == first + static_cast<PropertyOffset>(i) && !attributes;
        }
        if (areContiguous) {
            countOperationNamed(instance, "operationAOTTypedTryPutProperties", "names-that-lie-in-a-row");
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, first + static_cast<PropertyOffset>(i), JSValue::decode(valuesThenKeys[i]));
            if (siteIsPolymorphic())
                cachePropertyRunInInstance(instance, vm, cache, structure, structure, first, names.span());
            else
                cacheContiguousReplacementPropertyRun(vm, owner, cache, structure, first, count, keysAreFixed ? std::span<JSCell* const> { } : keys.span());
            OPERATION_RETURN(scope, 1);
        }
    }
    if (mayFindExistingProperties && !structure->mayBePrototype() && !structure->hasImmutableProperties() && structure->propertyAccessesAreCacheable()) {
        Vector<PropertyOffset, 16> offsets;
        Vector<UniquedStringImpl*, 16> added;
        bool isHandled = true;
        bool addsUnderSymbol = false;
        for (UniquedStringImpl* name : names) {
            unsigned attributes = 0;
            PropertyOffset offset = structure->get(vm, name, attributes);
            offsets.append(offset);
            if (!isValidOffset(offset)) {
                added.append(name);
                addsUnderSymbol |= name->isSymbol();
                continue;
            }
            bool isPrivate = name->isSymbol() && static_cast<SymbolImpl*>(name)->isPrivate();
            isHandled &= !isPrivate && !attributes && isInlineOffset(offset);
        }
        isHandled &= added.size() < count;
        Structure* last = structure;
        if (isHandled && !added.isEmpty()) {
            makePrototypeChainWatchable(vm, object);
            if (addsUnderSymbol && !instance->hasPropertyRunTarget(structure, added.span()))
                isHandled = instance->reservePropertyRunTarget(Instance::PropertyRunBudget::SymbolKeyed);
            last = isHandled ? structureAfterPropertyRun(instance, vm, structure, added.span()) : nullptr;
            isHandled = last && last->outOfLineCapacity() == structure->outOfLineCapacity() && isInlineOffset(last->maxOffset());
        }
        if (isHandled) {
            countOperationNamed(instance, "operationAOTTypedTryPutProperties", "some-names-are-there");
            PropertyOffset next = structure->maxOffset();
            for (unsigned i = 0; i < count; ++i) {
                if (!isValidOffset(offsets[i])) {
                    next = Instance::offsetAfter(next, structure->inlineCapacity());
                    offsets[i] = next;
                }
                object->putDirectOffset(vm, offsets[i], JSValue::decode(valuesThenKeys[i]));
            }
            object->setStructure(vm, last);
            if (!cachePropertyRunByOffsets(globalObject, owner, cache + 2 + (mayReplace ? (count - 1) / sizeof(void*) : 0), object, structure, offsets.span(), added.span()))
                refuse();
            OPERATION_RETURN(scope, 1);
        }
    }
    makePrototypeChainWatchable(vm, object);
    if (!keys.isEmpty() && !instance->hasPropertyRunTarget(structure, names.span()) && !instance->reservePropertyRunTarget(Instance::PropertyRunBudget::SymbolKeyed))
        OPERATION_RETURN(scope, refuse());
    Structure* last = structureAfterPropertyRun(instance, vm, structure, names.span());
    if (!last) {
        if (isPlainRun && cache->structureID && cache->structureID != structure->id())
            cacheRejectedPropertyRun(instance, vm, cache, structure, names.span());
        OPERATION_RETURN(scope, refuse());
    }
    PropertyOffset offset = structure->maxOffset();
    bool grows = structure->outOfLineCapacity() != last->outOfLineCapacity();
    bool canCacheWithStorage = !keyed && (!grows || hasOnlyNamedOutOfLineStorage(structure, last));
    if (grows)
        object->nukeStructureAndSetButterfly(vm, structure->id(), object->allocateMoreOutOfLineStorage(vm, structure->outOfLineCapacity(), last->outOfLineCapacity()));
    for (unsigned i = 0; i < count; ++i) {
        offset = Instance::offsetAfter(offset, structure->inlineCapacity());
        object->putDirectOffset(vm, offset, JSValue::decode(valuesThenKeys[i]));
    }
    object->setStructure(vm, last);
    if (structure->mayBePrototype()) {
        for (UniquedStringImpl* name : names)
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add, object, name);
        OPERATION_RETURN(scope, 1);
    }
    bool siteCanCache = isPlainRun || SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) != Slot::attemptsMask;
    if (!grows && isInlineOffset(offset) && siteCanCache) {
        if (siteIsPolymorphic())
            cachePropertyRunInInstance(instance, vm, cache, structure, last, Instance::offsetAfter(structure->maxOffset(), structure->inlineCapacity()), names.span());
        else
            cachePropertyRun(globalObject, owner, cache, object, structure, names.span(), keysAreFixed ? std::span<JSCell* const> { } : keys.span());
    } else if (canCacheWithStorage && cachePropertyRunWithStorage(globalObject, owner, cache + 1, object, structure, names.span(), grows)) {
    } else if (!SharedData::contains(cache)) {
        if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
            cache->offset += 1u << Slot::attemptsShift;
        else {
            if (cache->structureID) {
                cache->clear();
                stopWatching(owner, cache);
            }
            cache->offset |= Slot::isGetter;
        }
    }
    OPERATION_RETURN(scope, 1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    unsigned emptyCapacity = count >> capacityShiftInArrayLiteralCount;
    count &= (1u << capacityShiftInArrayLiteralCount) - 1;
    unsigned vectorLength = emptyCapacity ? emptyCapacity : arrayLiteralVectorLength(count);
    if (hasContiguous(structure->indexingType()) && !structure->outOfLineCapacity() && vectorLength <= maxNumberArrayCapacity) {
        if (void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull)) {
            Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
            butterfly->setVectorLength(vectorLength);
            butterfly->setPublicLength(count);
            for (uint32_t i = 0; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(i < count ? JSValue::decode(values[i]) : JSValue());
            if (Options::useAOTOperationCounters()) [[unlikely]]
                runtimeTable(vm).countOperation("operationAOTTypedNewArray", "in-one-go");
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, structure, butterfly));
        }
    }
    JSArray* array = JSArray::tryCreate(vm, structure, count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < count; ++i)
        array->putDirectIndex(globalObject, i, JSValue::decode(values[i]));
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewNumberArray, JSCell*, (Instance* instance, uint32_t lengthAndCapacity, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    bool isUnderConstruction = indexingType & numberArrayIsUnderConstruction;
    indexingType &= ~numberArrayIsUnderConstruction;
    unsigned length = isUnderConstruction ? lengthAndCapacity : lengthAndCapacity & ((1u << numberArrayCapacityShift) - 1);
    unsigned capacity = isUnderConstruction ? length | 1 : lengthAndCapacity >> numberArrayCapacityShift;
    RELEASE_ASSERT(length <= capacity && (isUnderConstruction || !globalObject->isHavingABadTime()) && (indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous));
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(capacity);
    butterfly->setPublicLength(length);
    for (unsigned i = 0; i < capacity; ++i) {
        if (indexingType == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        else
            butterfly->contiguous().atUnsafe(i).clear();
    }
    OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, isUnderConstruction ? instance->arrayUnderConstructionStructure(indexingType) : globalObject->originalArrayStructureForIndexingType(indexingType), butterfly));
}

static OperationReturnType<EncodedJSValue> pushToArray(Instance* instance, CallFrame* callFrame, JSCell* cell, EncodedJSValue value, uint64_t expectedAndSite)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, callFrame, "operationAOTTypedArrayPush");
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    uint32_t expected = expectedAndSite >> 32;
    if (uint32_t site = static_cast<uint32_t>(expectedAndSite) >> 1)
        operationAOTTypedUpdateVectorLengthHint(instance, cell, site - 1);
    if (array->type() != ArrayType) [[unlikely]] {
        bool hasReadMethod = expectedAndSite & 1;
        JSValue callee = (hasReadMethod ? globalObject->arrayPrototype() : array)->get(globalObject, vm.propertyNames->builtinNames().pushPublicName());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (auto* function = dynamicDowncast<JSFunction>(callee); function && function->isHostFunction() && function->nativeFunction() == TaggedNativeFunction(arrayProtoFuncPush) && function->realm() == globalObject) {
            array->push(globalObject, JSValue::decode(value));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
        }
        OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, callee, array, 1, &value)));
    }
    if (unsigned newLength = pushToFullVector(vm, array, JSValue::decode(value), expected))
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(newLength)));
    array->push(globalObject, JSValue::decode(value));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t expectedAndSite))
{
    return pushToArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, value, expectedAndSite);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPushNumber, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t numberBits, uint64_t expectedAndSite))
{
    CallFrame* callFrame = DECLARE_CALL_FRAME(*instance->vm);
    double number = std::bit_cast<double>(numberBits);
    if (number == number)
        asObject(cell)->tryMakeWritableDouble(*instance->vm);
    return pushToArray(instance, callFrame, cell, JSValue::encode(jsNumber(purifyNaN(number))), expectedAndSite);
}

static unsigned arraySliceCell()
{
    auto& builtins = Typed::Builtins::singleton();
    static const unsigned function = [&] {
        for (unsigned i = 0; i < builtins.size(); ++i) {
            auto& implementation = builtins.at(Typed::BuiltinID(i)).implementation;
            if (implementation.kind == Typed::BuiltinImplementation::Inline && implementation.inlined == Typed::InlineBuiltin::ArraySlice)
                return builtins.cellOf(Typed::BuiltinID(i));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    return function;
}

static JSArray* sliceLikeStub(VM& vm, JSGlobalObject* globalObject, JSCell* cell, int64_t from, int64_t to)
{
    if (cell->type() != ArrayType || globalObject->isHavingABadTime())
        return nullptr;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return nullptr;
    int64_t length = array->butterfly()->publicLength();
    from = from < 0 ? std::max<int64_t>(from + length, 0) : std::min(from, length);
    to = to < 0 ? std::max<int64_t>(to + length, 0) : std::min(to, length);
    int64_t count = std::max<int64_t>(to - from, 0);
    if (count > 64)
        return nullptr;
    unsigned vectorLength = static_cast<unsigned>(count) | 1;
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage)
        return nullptr;
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(static_cast<unsigned>(count));
    const uint64_t* source = array->butterfly()->indexingPayload<uint64_t>() + from;
    uint64_t* target = butterfly->indexingPayload<uint64_t>();
    for (unsigned i = 0; i < vectorLength; ++i)
        target[i] = i < count ? source[i] : shape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    JSArray* result = JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape)), butterfly);
    ensureStillAliveHere(array);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayCopy, EncodedJSValue, (Instance* instance, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSArray* copy = sliceLikeStub(vm, globalObject, array, 0, static_cast<int64_t>(1) << 32))
        OPERATION_RETURN(scope, JSValue::encode(copy));
    MarkedArgumentBuffer arguments;
    OPERATION_RETURN(scope, JSValue::encode(callEngineBuiltin(instance, arraySliceCell(), array, arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySlice, EncodedJSValue, (Instance* instance, JSCell* array, int64_t from, int64_t to))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSArray* part = sliceLikeStub(vm, globalObject, array, from, to))
        OPERATION_RETURN(scope, JSValue::encode(part));
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<double>(from)));
    arguments.append(jsNumber(static_cast<double>(to)));
    OPERATION_RETURN(scope, JSValue::encode(callEngineBuiltin(instance, arraySliceCell(), array, arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySearchByCall, EncodedJSValue, (Instance* instance, JSCell* array, EncodedJSValue searched, EncodedJSValue methodGetById, uint32_t isIndexOf))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Identifier name = isIndexOf ? Identifier::fromString(vm, "indexOf"_s) : vm.propertyNames->includes;
    JSValue method = JSValue::decode(methodGetById);
    bool hasReadMethod = !!method;
    if (!hasReadMethod) {
        method = JSValue(array).get(globalObject, name);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto callData = JSC::getCallData(method);
    if (callData.type == CallData::Type::None) {
        if (hasReadMethod || method.isCell())
            throwIsNotAFunction(globalObject, scope, method);
        else
            throwNotAFunction(globalObject, scope, name, method);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    MarkedArgumentBuffer arguments;
    arguments.append(JSValue::decode(searched));
    EncodedJSValue found = JSValue::encode(call(globalObject, method, callData, array, arguments));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!isIndexOf || static_cast<uint64_t>(found) >> 32)
        OPERATION_RETURN(scope, found);
    vm.encodedHostCallReturnValue = found;
    OPERATION_RETURN(scope, static_cast<EncodedJSValue>(Typed::arraySearchDeclined));
}

static bool reportOrThrowWouldRunCode(JSGlobalObject* globalObject, State& state)
{
    return state.reportOrThrow(globalObject, { .kind = "array-not-plain"_s }, "Cannot read array element: the array or its prototype chain has accessors or a Proxy, which are not run for an array of a declared element type"_s, State::InAudit::Ignored);
}

static bool reportOrThrowHasOwnIterator(JSGlobalObject* globalObject, State& state)
{
    return state.reportOrThrow(globalObject, { .kind = "array-not-plain"_s }, "Cannot iterate over the array: it has its own Symbol.iterator method, which is not called where the declared type is an array, a tuple or a typed array"_s, State::InAudit::Ignored);
}

static std::optional<JSValue> getWithoutRunningCode(JSGlobalObject* globalObject, State& state, JSArray* array, PropertyName name, std::optional<uint32_t> index)
{
    if (index && array->canGetIndexQuickly(*index))
        return array->getIndexQuickly(*index);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = index ? array->getPropertySlot(globalObject, *index, slot) : array->getPropertySlot(globalObject, name, slot);
    scope.assertNoException();
    if (slot.isTaintedByOpaqueObject() || (isFound && !slot.isValue())) {
        bool goesOn = reportOrThrowWouldRunCode(globalObject, state);
        EXCEPTION_ASSERT(!!scope.exception() == !goesOn);
        if (!goesOn)
            return std::nullopt;
        return jsUndefined();
    }
    if (!isFound)
        return jsUndefined();
    return slot.getValue(globalObject, name);
}

static OperationReturnType<EncodedJSValue> getFromArray(Instance* instance, CallFrame* callFrame, JSCell* cell, EncodedJSValue key)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, callFrame, "operationAOTTypedArrayGet");
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    instance->typed->countArrayGet(array, JSValue::decode(key));
    std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index();
    Identifier name;
    if (!index)
        name = JSValue::decode(key).isNumber() ? Identifier::from(vm, JSValue::decode(key).asNumber()) : JSValue::decode(key).isNull() ? vm.propertyNames->nullKeyword : vm.propertyNames->undefinedKeyword;
    auto value = getWithoutRunningCode(globalObject, *instance->typed, array, name, index);
    if (value && value->isDouble())
        value = jsNumber(value->asDouble());
    OPERATION_RETURN(scope, value ? JSValue::encode(*value) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGet, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue key))
{
    return getFromArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, key);
}

static OperationReturnType<EncodedJSValue> getFromArrayAs(Instance* instance, CallFrame* callFrame, JSCell* cell, double key, uint32_t type)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, callFrame, "operationAOTTypedArrayElementAtNumberAs");
    State& state = *instance->typed;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue boxedKey = jsNumber(purifyNaN(key));
    state.countArrayGet(array, boxedKey);
    std::optional<uint32_t> index = boxedKey.tryGetAsUint32Index();
    if (index && array->canGetIndexQuickly(*index)) {
        JSValue own = array->getIndexQuickly(*index);
        if (!own.isDouble() && state.fits(state.type(type), own))
            OPERATION_RETURN(scope, JSValue::encode(own));
        if (state.tryConvertPlainObjectInPlace(vm, own, state.type(type), type << 4 | static_cast<uint32_t>(Typed::Format::CastReason::As)))
            OPERATION_RETURN(scope, JSValue::encode(own));
    }
    Identifier name;
    if (!index)
        name = Identifier::from(vm, boxedKey.asNumber());
    auto value = getWithoutRunningCode(globalObject, state, array, name, index);
    EXCEPTION_ASSERT(!!scope.exception() == !value);
    if (!value)
        OPERATION_RETURN(scope, encodedJSValue());
    if (!value->isDouble() && state.fits(state.type(type), *value)) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).countOperation("an element that fits is not coerced");
        OPERATION_RETURN(scope, JSValue::encode(*value));
    }
    JSValue element = value->isDouble() ? jsNumber(value->asDouble()) : *value;
    JSValue converted = state.hold(globalObject, element, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != element && index && array->canGetIndexQuickly(*index)) {
        array->putDirectIndex(globalObject, *index, converted);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayElementAtNumberAs, EncodedJSValue, (Instance* instance, JSCell* cell, double key, uint32_t type))
{
    return getFromArrayAs(instance, DECLARE_CALL_FRAME(*instance->vm), cell, key, type);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayElementAs, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t key, uint32_t type))
{
    return getFromArrayAs(instance, DECLARE_CALL_FRAME(*instance->vm), cell, static_cast<double>(static_cast<int64_t>(key)), type);
}

static bool mayGrowVector(unsigned vectorLength, uint32_t index)
{
    return index >= vectorLength && (index < MIN_BEYOND_LENGTH_SPARSE_INDEX || index / 2 <= vectorLength);
}

static SparseArrayValueMap* sparseMapOf(JSArray* array)
{
    return array->indexingType() == ArrayWithArrayStorage ? array->butterfly()->arrayStorage()->m_sparseMap.get() : nullptr;
}

static void convertArrayStorageToContiguous(VM& vm, JSArray* array)
{
    if (vm.heap.mutatorShouldBeFenced())
        return;
    Structure* structure = array->structure();
    JSGlobalObject* globalObject = structure->globalObject();
    if (globalObject->isHavingABadTime() || structure != globalObject->originalArrayStructureForIndexingType(ArrayWithArrayStorage))
        return;
    ArrayStorage* storage = array->butterfly()->arrayStorage();
    SparseArrayValueMap* map = storage->m_sparseMap.get();
    unsigned length = storage->length();
    if (length > MAX_STORAGE_VECTOR_LENGTH || (map && (map->sparseMode() || map->lengthIsReadOnly())))
        return;
    if (!isDenseEnoughForVector(length, storage->m_numValuesInVector + (map ? map->size() : 0)))
        return;
    DeferGC deferGC(vm);
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), length);
    Butterfly* butterfly = Butterfly::tryCreateUninitialized(vm, array, 0, 0, true, vectorLength * sizeof(EncodedJSValue));
    if (!butterfly)
        return;
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(length);
    WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
    unsigned numberInVector = std::min(storage->vectorLength(), length);
    for (unsigned i = 0; i < numberInVector; ++i)
        elements[i].setWithoutWriteBarrier(storage->m_vector[i].get());
    for (unsigned i = numberInVector; i < vectorLength; ++i)
        elements[i].clear();
    if (map) {
        for (auto& entry : *map)
            elements[entry.index()].setWithoutWriteBarrier(entry.getNonSparseMode());
    }
    {
        Locker locker { array->cellLock() };
        array->nukeStructureAndSetButterfly(vm, structure->id(), butterfly);
        array->setStructure(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous));
    }
    vm.writeBarrier(array);
}

static ALWAYS_INLINE void putArrayIndexInline(JSGlobalObject* globalObject, VM& vm, JSArray* array, uint32_t index, JSValue value, bool isStrict, unsigned expected = 0)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned vectorLength = array->indexingType() & IndexingShapeMask ? array->butterfly()->vectorLength() : 0;
    if (index == vectorLength && pushToFullVector(vm, array, value, expected))
        return;
    if (array->type() == ArrayType && mayGrowVector(vectorLength, index) && array->tryPutIndexInPlace(vm, index, value))
        return;
    bool hadSparseMap = sparseMapOf(array);
    array->methodTable()->putByIndex(array, globalObject, index, value, isStrict);
    if (!scope.exception() && array->type() == ArrayType && array->indexingType() == ArrayWithArrayStorage && (hadSparseMap || sparseMapOf(array)))
        convertArrayStorageToContiguous(vm, array);
}

void putArrayIndex(JSGlobalObject* globalObject, JSArray* array, uint32_t index, JSValue value, bool isStrict)
{
    putArrayIndexInline(globalObject, globalObject->vm(), array, index, value, isStrict);
}

static OperationReturnType<void> putToArray(Instance* instance, CallFrame* callFrame, JSCell* cell, EncodedJSValue key, EncodedJSValue value, unsigned expected = 0)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, callFrame, "operationAOTTypedArrayPut");
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    IndexingType shapeBefore = array->indexingType() & IndexingShapeMask;
    unsigned vectorLengthBefore = shapeBefore ? array->butterfly()->vectorLength() : 0;
    auto count = makeScopeExit([&] {
        instance->typed->countArrayPut(array, JSValue::decode(key), shapeBefore, vectorLengthBefore);
    });
    if (std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index()) {
        scope.release();
        putArrayIndexInline(globalObject, vm, array, *index, JSValue::decode(value), true, expected);
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(array, true);
    array->methodTable()->put(array, globalObject, Identifier::from(vm, JSValue::decode(key).asNumber()), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, EncodedJSValue key, EncodedJSValue value))
{
    return putToArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, key, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStoreElement, void, (Instance* instance, JSCell* cell, int64_t index, EncodedJSValue value))
{
    return putToArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, JSValue::encode(jsNumber(static_cast<double>(index))), value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStoreNumberElement, void, (Instance* instance, JSCell* cell, int64_t index, double value))
{
    CallFrame* callFrame = DECLARE_CALL_FRAME(*instance->vm);
    if (value == value)
        asObject(cell)->tryMakeWritableDouble(*instance->vm);
    return putToArray(instance, callFrame, cell, JSValue::encode(jsNumber(static_cast<double>(index))), JSValue::encode(jsNumber(purifyNaN(value))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGetAtNumber, EncodedJSValue, (Instance* instance, JSCell* cell, double key))
{
    return getFromArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, JSValue::encode(jsNumber(purifyNaN(key))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGetAtIndex, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t key))
{
    return getFromArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGetAtIndexAsNumber, double, (Instance* instance, JSCell* cell, uint64_t key, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue boxedKey = jsNumber(static_cast<double>(static_cast<int64_t>(key)));
    state.countArrayGet(array, boxedKey);
    std::optional<uint32_t> index = boxedKey.tryGetAsUint32Index();
    Identifier name;
    if (!index)
        name = Identifier::from(vm, boxedKey.asNumber());
    auto value = getWithoutRunningCode(globalObject, state, array, name, index);
    EXCEPTION_ASSERT(!!scope.exception() == !value);
    if (!value) [[unlikely]]
        OPERATION_RETURN(scope, 0);
    if (!state.fits(state.type(type), *value) || !(value->isNumber() || value->isUndefinedOrNull())) [[unlikely]] {
        throwTypeMismatch(instance, globalObject, scope, *value, type);
        OPERATION_RETURN(scope, 0);
    }
    if (value->isNumber())
        OPERATION_RETURN(scope, purifyNaN(value->asNumber()));
    OPERATION_RETURN(scope, std::bit_cast<double>(value->isUndefined() ? Typed::undefinedDouble : Typed::nullDouble));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutAtIndex, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t key))
{
    return putToArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))), value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutNumberAtIndex, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t key))
{
    CallFrame* callFrame = DECLARE_CALL_FRAME(*instance->vm);
    if (JSValue number = JSValue::decode(value); number.isNumber() && !std::isnan(number.asNumber()) && hasInt32(cell->indexingType()))
        asObject(cell)->tryMakeWritableDouble(*instance->vm);
    return putToArray(instance, callFrame, cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))), value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutAtIndexExpecting, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t key, uint64_t expectedAndSite))
{
    CallFrame* callFrame = DECLARE_CALL_FRAME(*instance->vm);
    if (JSValue number = JSValue::decode(value); expectedAndSite & 1 && number.isNumber() && !std::isnan(number.asNumber()) && hasInt32(cell->indexingType()))
        asObject(cell)->tryMakeWritableDouble(*instance->vm);
    if (uint32_t site = static_cast<uint32_t>(expectedAndSite) >> 1)
        operationAOTTypedUpdateVectorLengthHint(instance, cell, site - 1);
    return putToArray(instance, callFrame, cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))), value, expectedAndSite >> 32);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutAtNumber, void, (Instance* instance, JSCell* cell, EncodedJSValue value, double key))
{
    return putToArray(instance, DECLARE_CALL_FRAME(*instance->vm), cell, JSValue::encode(jsNumber(purifyNaN(key))), value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayTryMakeWritableDouble, size_t, (Instance* instance, JSCell* cell))
{
    countOperationNamed(instance, __func__);
    IndexingType shape = cell->indexingType() & IndexingShapeMask;
    if (shape != UndecidedShape && shape != Int32Shape && shape != DoubleShape)
        return 0;
    return !!asObject(cell)->tryMakeWritableDouble(*instance->vm).data();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryAddNamedProperties, size_t, (Instance* instance, JSCell* cell, uint32_t count, const EncodedJSValue* valuesThenNames))
{
    VM& vm = *instance->vm;
    countOperationNamed(instance, __func__);
    JSObject* array = asObject(cell);
    Structure* from = array->structure();
    auto* transition = instance->typed->namedPropertiesTransition(vm, from, { valuesThenNames + count, count });
    if (!transition || !transition->to)
        return 0;
    if (!transition->oldOutOfLineCapacity && transition->elementsDependOnVectorLength) {
        unsigned capacity = transition->newOutOfLineCapacity;
        Butterfly* butterfly = array->butterfly();
        size_t sizeOfElements = sizeof(IndexingHeader) + butterfly->vectorLength() * sizeof(EncodedJSValue);
        auto* base = static_cast<EncodedJSValue*>(vm.auxiliarySpace().allocate(vm, capacity * sizeof(EncodedJSValue) + sizeOfElements, nullptr, AllocationFailureMode::Assert));
        for (unsigned i = 0; i < capacity; ++i)
            base[i] = 0;
        memcpy(base + capacity, butterfly->indexingHeader(), sizeOfElements);
        array->nukeStructureAndSetButterfly(vm, from->id(), Butterfly::fromBase(base, 0, capacity));
    } else if (transition->oldOutOfLineCapacity != transition->newOutOfLineCapacity)
        array->nukeStructureAndSetButterfly(vm, from->id(), array->allocateMoreOutOfLineStorage(vm, transition->oldOutOfLineCapacity, transition->newOutOfLineCapacity));
    PropertyStorage storage = array->butterfly()->propertyStorage();
    const PropertyOffset* offsets = transition->offsets.span().data();
    for (uint32_t i = 0; i < count; ++i)
        storage[offsetInOutOfLineStorage(offsets[i])].setWithoutWriteBarrier(JSValue::decode(valuesThenNames[i]));
    array->setStructure(vm, transition->to);
    vm.writeBarrier(array);
    return 1;
}

static bool isUnmodifiedArrayValuesOfAnotherRealm(JSValue found)
{
    auto* function = dynamicDowncast<JSFunction>(found);
    if (!function)
        return false;
    JSGlobalObject* realm = function->realm();
    return function == realm->arrayProtoValuesFunction() && realm->arrayIteratorProtocolWatchpointSet().isStillValid();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedOpenModifiedArray, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto found = getWithoutRunningCode(globalObject, *instance->typed, uncheckedDowncast<JSArray>(cell), vm.propertyNames->iteratorSymbol, std::nullopt);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (*found != JSValue(globalObject->linkTimeConstant(LinkTimeConstant::arrayProtoValues)) && !isUnmodifiedArrayValuesOfAnotherRealm(*found))
        reportOrThrowHasOwnIterator(globalObject, *instance->typed);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTypedArrayIteratorOpen, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
    if (!view->isIteratorProtocolFastAndNonObservable()) {
        JSObject* common = asObject(globalObject->typedArrayPrototype(typedArrayType(view->type()))->getPrototypeDirect());
        PropertySlot slot(view, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = view->getPropertySlot(globalObject, vm.propertyNames->iteratorSymbol, slot);
        scope.assertNoException();
        if (slot.isTaintedByOpaqueObject() || !isFound || !slot.isValue()) {
            reportOrThrowWouldRunCode(globalObject, *instance->typed);
            OPERATION_RETURN(scope);
        }
        if (slot.getValue(globalObject, vm.propertyNames->iteratorSymbol) != common->getDirect(vm, vm.propertyNames->iteratorSymbol)) {
            reportOrThrowHasOwnIterator(globalObject, *instance->typed);
            OPERATION_RETURN(scope);
        }
    }
    validateTypedArray(globalObject, view);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTypedArrayIteratorHasNext, size_t, (Instance* instance, JSCell* cell, uint64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
    validateTypedArray(globalObject, view);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, index < view->length());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceToType, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint64_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue given = JSValue::decode(value);
    if (!given || !instance->typed->accepts(globalObject, instance->typed->type(static_cast<uint32_t>(type)), given)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), static_cast<uint32_t>(type));
    OPERATION_RETURN(scope, value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    JSCell* cell = Typed::Builtins::singleton().findCell(instance->globalObject, index);
    instance->negativeOffsetFields().typedBuiltins[index] = cell;
    return cell;
}

JSCell* typedBuiltinCell(Instance* instance, unsigned index)
{
    if (JSCell* cell = instance->negativeOffsetFields().typedBuiltins[index]) [[likely]]
        return cell;
    return operationAOTTypedBuiltinCell(instance, index);
}

JSValue callEngineBuiltin(Instance* instance, unsigned function, JSValue thisValue, const ArgList& arguments)
{
    JSCell* cell = typedBuiltinCell(instance, function);
    return call(instance->globalObject, cell, JSC::getCallData(cell), thisValue, arguments);
}

template<typename View>
static JSCell* newTypedArrayView(Instance* instance, JSGlobalObject* globalObject, size_t length)
{
    static_assert(View::TypedArrayStorageType < Instance::numberOfTypedArrayAllocations && sizeof(View) == sizeof(JSArrayBufferView));
    Structure* structure = globalObject->typedArrayStructure(View::TypedArrayStorageType, false);
    if (auto& row = instance->typedArrayAllocations[View::TypedArrayStorageType]; !row.allocator) {
        VM& vm = *instance->vm;
        row.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
        row.elementSize = View::elementSize;
        instance->primitiveAuxiliarySpace = &vm.primitiveGigacageAuxiliarySpace();
        row.allocator = subspaceFor<View>(vm)->allocatorFor(sizeof(View), AllocatorForMode::EnsureAllocator).localAllocator();
    }
    return View::create(globalObject, structure, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewTypedArray, JSCell*, (Instance* instance, uint32_t type, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    double index = std::isnan(number) ? 0 : std::trunc(number);
    if (index < 0 || index > MAX_ARRAY_BUFFER_SIZE) {
        throwRangeError(globalObject, scope, "Length out of range of buffer"_s);
        OPERATION_RETURN(scope, nullptr);
    }
    size_t length = static_cast<size_t>(index);
    switch (static_cast<TypedArrayType>(type)) {
#define AOT_TYPED_NEW_TYPED_ARRAY_VIEW(name) \
    case Type ## name: \
        OPERATION_RETURN(scope, newTypedArrayView<JS ## name ## Array>(instance, globalObject, length));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_NEW_TYPED_ARRAY_VIEW)
#undef AOT_TYPED_NEW_TYPED_ARRAY_VIEW
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypedArrayPut, void, (Instance* instance, JSCell* view, uint64_t index, double number))
{
    DeferTerminationForAWhile deferTermination(*instance->vm);
    JSValue value(JSValue::EncodeAsDouble, purifyNaN(number));
    switch (view->type()) {
#define AOT_TYPED_PUT_ELEMENT(name) \
    case name ## ArrayType: \
        uncheckedDowncast<JS ## name ## Array>(view)->setIndex(instance->globalObject, index, value); \
        return;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_PUT_ELEMENT)
#undef AOT_TYPED_PUT_ELEMENT
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedClosureConstant, JSCell*, (Instance* instance, uint32_t function))
{
    countOperationNamed(instance, __func__);
    if (instance->typed->closuresHaveCaptures(function))
        return instance->typed->lazyClosureCell(*instance->vm, function);
    return instance->typed->closureObjectFor(*instance->vm, function);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    DeferTerminationForAWhile deferTermination(*instance->vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*instance->vm);
    JSValue made = isSlimClosureType(constructor->type()) || isSlimClosureType(newTarget->type()) ? JSValue(Typed::ordinaryCreateFromConstructor(instance->globalObject, asObject(newTarget))) : instance->typed->createThis(instance->globalObject, uncheckedDowncast<Typed::Closure>(constructor), asObject(newTarget));
    scope.assertNoException();
    if (auto* closure = dynamicDowncast<Typed::Closure>(constructor); closure && closure->isDeclaredAs(Typed::functionReadsNewTarget)) [[unlikely]]
        instance->typed->newTarget = newTarget;
    return made.asCell();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(globalObject, uncheckedDowncast<JSString>(left), uncheckedDowncast<JSString>(right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcatThree, JSCell*, (Instance* instance, JSCell* first, JSCell* second, JSCell* third))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(globalObject, uncheckedDowncast<JSString>(first), uncheckedDowncast<JSString>(second), uncheckedDowncast<JSString>(third)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRopeCodeUnit, size_t, (Instance* instance, JSCell* string, int64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t outside = static_cast<size_t>(-1);
    auto value = uncheckedDowncast<JSString>(string)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, outside);
    if (static_cast<uint64_t>(index) >= value->length())
        OPERATION_RETURN(scope, outside);
    OPERATION_RETURN(scope, static_cast<size_t>(value.data[static_cast<unsigned>(index)]));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* part = jsSubstring(globalObject, uncheckedDowncast<JSString>(string), from, to - from);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (to - from == 2 && !part->isRope())
        jsString(globalObject, part, part);
    OPERATION_RETURN(scope, part);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFlatten, void*, (Instance* instance, JSCell* string))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto value = uncheckedDowncast<JSString>(string)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, value->impl());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringEq, size_t, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, uncheckedDowncast<JSString>(left)->equal(globalObject, uncheckedDowncast<JSString>(right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringHasAt, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    unsigned length = uncheckedDowncast<JSString>(string)->length();
    unsigned otherLength = uncheckedDowncast<JSString>(other)->length();
    if (place < 0 || otherLength > length || static_cast<unsigned>(place) > length - otherLength)
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto otherCharacters = uncheckedDowncast<JSString>(other)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->substring(place, otherLength) == otherCharacters.data);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringIndexOf, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = uncheckedDowncast<JSString>(string);
    JSString* argument = uncheckedDowncast<JSString>(other);
    unsigned position = place;
    if (base->length() < static_cast<uint64_t>(argument->length()) + position)
        OPERATION_RETURN(scope, none);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (argument->length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, otherCharacters[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), otherCharacters, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIntToString, JSCell*, (Instance* instance, uint64_t bits))
{
    VM& vm = *instance->vm;
    int64_t value = static_cast<int64_t>(bits);
    if (value == static_cast<int32_t>(value)) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(value), 10);
        string->tryGetValueImpl()->hash();
        return string;
    }
    return jsString(vm, String::number(static_cast<double>(value)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDoubleToString, JSCell*, (Instance* instance, double value))
{
    VM& vm = *instance->vm;
    int32_t integer = truncateDoubleToInt32(value);
    if (integer == value)
        return int32ToString(vm, integer, 10);
    return vm.numericStrings.addJSString(vm, value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    return jsSingleCharacterString(*instance->vm, static_cast<char16_t>(character));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMap, JSCell*, (Instance* instance))
{
    prepareInlineAllocation<JSMap>(instance, Instance::InlineAllocation::Map, instance->globalObject->mapStructure());
    return JSMap::create(*instance->vm, instance->globalObject->mapStructure());
}

static ALWAYS_INLINE bool canIterateArrayDirectly(JSValue iterable)
{
    auto* array = dynamicDowncast<JSArray>(iterable);
    if (!array || array->classInfo() != JSArray::info())
        return false;
    if (array->isIteratorProtocolFastAndNonObservable()) [[likely]]
        return true;
    return hasDefaultArrayIteration(array);
}

static ALWAYS_INLINE bool isBuiltinIterator(JSObject* iterator)
{
    switch (iterator->type()) {
    case JSArrayIteratorType:
    case JSMapIteratorType:
    case JSSetIteratorType:
    case JSRegExpStringIteratorType:
        return true;
    case ObjectType:
        return !!dynamicDowncast<Typed::Generator>(iterator);
    default:
        return false;
    }
}

template<typename Functor, typename OpenFunctor>
static ALWAYS_INLINE void openGenericIteration(JSGlobalObject* globalObject, JSValue iterable, const Functor& makeRecord, const OpenFunctor& tryOpenCollectionIteration, ASCIILiteral notIterableMessage = { }, JSValue knownMethod = { }, JSValue knownIterator = { })
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = knownMethod || knownIterator ? knownMethod : iterable.get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto validation = knownIterator ? IterableValidationResult::Valid : validateIterable(vm, iterable, method); validation != IterableValidationResult::Valid) [[unlikely]] {
        if (notIterableMessage.isNull())
            throwTypeError(globalObject, scope, getIteratorErrorMessage(validation, iterable));
        else
            throwTypeError(globalObject, scope, notIterableMessage);
        return;
    }
    JSValue iterator;
    if (knownIterator)
        iterator = knownIterator;
    else if (iterable.isObject() && method == JSValue(globalObject->arrayProtoValuesFunction()))
        iterator = JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), asObject(iterable), IterationKind::Values);
    else {
        iterator = call(globalObject, method, JSC::getCallData(method), iterable, ArgList());
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!iterator.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        return;
    }
    if (isBuiltinIterator(asObject(iterator)) && tryOpenCollectionIteration(iterator))
        return;
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, void());
    auto* state = makeRecord();
    state->setIndex(vm, 0, iterator);
    state->setIndex(vm, 1, next.isInt32() ? jsUndefined() : next);
    state->setIndex(vm, 2, jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpen, EncodedJSValue, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (canIterateArrayDirectly(JSValue::decode(iterable)))
        OPERATION_RETURN(scope, iterable);
    if (JSCellButterfly* state = Typed::CollectionIteration::tryOpen(globalObject, JSValue::decode(iterable)))
        OPERATION_RETURN(scope, JSValue::encode(state));
    JSCellButterfly* state = nullptr;
    openGenericIteration(globalObject, JSValue::decode(iterable), [&] {
        state = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, Typed::CollectionIteration::numberOfWords);
        state->setIndex(vm, Typed::CollectionIteration::structureOfResultsWord, jsNull());
        state->setIndex(vm, Typed::CollectionIteration::offsetsInResultsWord, jsNumber(0));
        return state;
    }, [&](JSValue iterator) {
        return !!(state = Typed::CollectionIteration::tryOpen(globalObject, iterator));
    });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(state));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpenInFrame, EncodedJSValue, (Instance* instance, EncodedJSValue iterable, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (canIterateArrayDirectly(JSValue::decode(iterable)))
        OPERATION_RETURN(scope, iterable);
    auto* state = Typed::CollectionIteration::InFrame::at(words);
    if (Typed::CollectionIteration::tryOpen(globalObject, JSValue::decode(iterable), state))
        OPERATION_RETURN(scope, JSValue::encode(globalObject));
    state->setIndex(vm, Typed::CollectionIteration::structureOfResultsWord, jsNull());
    state->setIndex(vm, Typed::CollectionIteration::offsetsInResultsWord, jsNumber(0));
    if (!instance->iteratorSymbol) [[unlikely]] {
        instance->iteratorSymbol = Symbol::create(vm, static_cast<SymbolImpl&>(*vm.propertyNames->iteratorSymbol.impl()));
        gcProtect(instance->iteratorSymbol);
    }
    openGenericIteration(globalObject, JSValue::decode(iterable), [&] {
        return state;
    }, [&](JSValue iterator) {
        return Typed::CollectionIteration::tryOpen(globalObject, iterator, state);
    });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(globalObject));
}

static ALWAYS_INLINE void continueOpeningInFrame(JSGlobalObject* globalObject, JSValue iterable, EncodedJSValue* words, JSValue method, JSValue iterator)
{
    VM& vm = globalObject->vm();
    auto* state = Typed::CollectionIteration::InFrame::at(words);
    state->setIndex(vm, Typed::CollectionIteration::structureOfResultsWord, jsNull());
    state->setIndex(vm, Typed::CollectionIteration::offsetsInResultsWord, jsNumber(0));
    openGenericIteration(globalObject, iterable, [&] {
        return state;
    }, [&](JSValue opened) {
        return Typed::CollectionIteration::tryOpen(globalObject, opened, state);
    }, { }, method, iterator);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFinishIterOpenInFrame, EncodedJSValue, (Instance* instance, EncodedJSValue iterator, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    continueOpeningInFrame(globalObject, jsUndefined(), words, { }, JSValue::decode(iterator));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpenInFrameWithMethod, EncodedJSValue, (Instance* instance, EncodedJSValue iterable, EncodedJSValue* words, EncodedJSValue method))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    continueOpeningInFrame(globalObject, JSValue::decode(iterable), words, JSValue::decode(method), { });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(globalObject));
}

template<typename Record>
static ALWAYS_INLINE bool stepIteration(JSGlobalObject* globalObject, Record* state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (Typed::CollectionIteration::isOne(state))
        RELEASE_AND_RETURN(scope, Typed::CollectionIteration::step(globalObject, state));
    IterationRecord record { state->get(0), state->get(1) };
    if (!record.nextMethod.isCallable()) [[unlikely]] {
        throwException(globalObject, scope, createNotAFunctionError(globalObject, record.nextMethod));
        return false;
    }
    JSValue result = iteratorNextExported(globalObject, record);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSObject* object = asObject(result); object->structure() == globalObject->iteratorResultObjectStructure()) {
        if (object->getDirect(iteratorResultObjectDonePropertyOffset).toBoolean(globalObject))
            return false;
        state->setIndex(vm, 2, object->getDirect(iteratorResultObjectValuePropertyOffset));
        return true;
    }
    bool isDone = iteratorCompleteExported(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    if (isDone)
        return false;
    JSValue value = iteratorValue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    state->setIndex(vm, 2, value);
    return true;
}

static void forEachInSpread(JSGlobalObject* globalObject, JSValue iterable, const ScopedLambda<void(JSValue)>& functor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (getIterationMode(iterable) == IterationMode::FastArray) {
        scope.release();
        forEachInIterable(globalObject, iterable, [&](VM&, JSGlobalObject*, JSValue value) {
            functor(value);
        });
        return;
    }
    Typed::CollectionIteration::InFrame record { };
    openGenericIteration(globalObject, iterable, [&] {
        return &record;
    }, [&](JSValue iterator) {
        return Typed::CollectionIteration::tryOpen(globalObject, iterator, &record);
    }, "Spread syntax requires ...iterable[Symbol.iterator] to be a function"_s);
    RETURN_IF_EXCEPTION(scope, void());
    for (;;) {
        bool hasValue = stepIteration(globalObject, &record);
        RETURN_IF_EXCEPTION(scope, void());
        if (!hasValue)
            return;
        functor(record.get(2));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterHas, size_t, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, stepIteration(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell())));
}

static void rememberIteratorResultShape(Instance* instance, Structure* structure, int32_t offsetOfDone, int32_t offsetOfValue)
{
    for (auto& shape : instance->iteratorResultShapes) {
        if (shape.structureID == structure->id().bits())
            return;
    }
    auto& shape = instance->iteratorResultShapes[instance->nextIteratorResultShape++ % Instance::numberOfIteratorResultShapes];
    if (shape.structureID)
        gcUnprotect(std::bit_cast<StructureID>(shape.structureID).decode());
    gcProtect(structure);
    shape = { structure->id().bits(), static_cast<uint16_t>(offsetOfDone), static_cast<uint16_t>(offsetOfValue) };
}

template<typename Record>
static ALWAYS_INLINE bool finishIterationStep(Instance* instance, Record* state, JSValue result)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!result.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        return false;
    }
    Structure* structure = asObject(result)->structure();
    if (!structure->isDictionary() && !structure->typeInfo().overridesGetOwnPropertySlot()) {
        unsigned attributesOfDone;
        unsigned attributesOfValue;
        PropertyOffset done = structure->get(vm, vm.propertyNames->done, attributesOfDone);
        PropertyOffset value = structure->get(vm, vm.propertyNames->value, attributesOfValue);
        if (isValidOffset(done) && isValidOffset(value) && isInlineOffset(done) && isInlineOffset(value) && !((attributesOfDone | attributesOfValue) & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
            auto offsetInCell = [](PropertyOffset offset) {
                return static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue));
            };
            state->setIndex(vm, Typed::CollectionIteration::structureOfResultsWord, structure);
            state->setIndex(vm, Typed::CollectionIteration::offsetsInResultsWord, jsNumber(offsetInCell(done) | offsetInCell(value) << 16));
            rememberIteratorResultShape(instance, structure, offsetInCell(done), offsetInCell(value));
        }
    }
    bool isDone = iteratorCompleteExported(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    if (isDone)
        return false;
    JSValue value = iteratorValue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    state->setIndex(vm, 2, value);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFinishIterStepInFrame, size_t, (Instance* instance, EncodedJSValue* words, EncodedJSValue result))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, finishIterationStep(instance, Typed::CollectionIteration::InFrame::at(words), JSValue::decode(result)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFinishIterStep, size_t, (Instance* instance, EncodedJSValue state, EncodedJSValue result))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, finishIterationStep(instance, uncheckedDowncast<JSCellButterfly>(JSValue::decode(state).asCell()), JSValue::decode(result)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterHasInFrame, size_t, (Instance* instance, EncodedJSValue* words))
{
    if (Typed::CollectionIteration::mayStepQuickly(words) && !Options::useAOTOperationCounters())
        [[clang::musttail]] return Typed::CollectionIteration::stepQuickly(instance, words);
    [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
}

NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stepIterationInFrameGeneric(Instance* instance, EncodedJSValue* words)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, "operationAOTTypedIterHasInFrame");
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, stepIteration(globalObject, Typed::CollectionIteration::InFrame::at(words)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIterGet, EncodedJSValue, (Instance*, EncodedJSValue encodedState))
{
    return JSValue::encode(uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell())->get(2));
}

template<typename Record>
static ALWAYS_INLINE void closeIteration(JSGlobalObject* globalObject, Record* state)
{
    if (Typed::CollectionIteration::isOne(state))
        Typed::CollectionIteration::close(globalObject, state);
    else
        iteratorClose(globalObject, state->get(0));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterClose, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIteration(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell()));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseInFrame, void, (Instance* instance, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIteration(globalObject, Typed::CollectionIteration::InFrame::at(words));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedReadBatchedElements, void, (Instance* instance, EncodedJSValue encodedIterable, uint32_t count, EncodedJSValue* taken))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterable = JSValue::decode(encodedIterable);
    for (uint32_t i = 0; i < count; ++i)
        taken[i] = JSValue::encode(jsUndefined());
    if (canIterateArrayDirectly(iterable)) {
        JSArray* array = asArray(iterable);
        for (uint32_t i = 0; i < count && i < array->length(); ++i) {
            JSValue element = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            taken[i] = JSValue::encode(element);
        }
        OPERATION_RETURN(scope);
    }
    Typed::CollectionIteration::InFrame record;
    if (!Typed::CollectionIteration::tryOpen(globalObject, iterable, &record)) {
        openGenericIteration(globalObject, iterable, [&] {
            return &record;
        }, [&](JSValue iterator) {
            return Typed::CollectionIteration::tryOpen(globalObject, iterator, &record);
        });
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    for (uint32_t i = 0; i < count; ++i) {
        bool has = stepIteration(globalObject, &record);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (!has)
            OPERATION_RETURN(scope);
        taken[i] = JSValue::encode(record.get(2));
    }
    closeIteration(globalObject, &record);
    OPERATION_RETURN(scope);
}

template<typename Record>
static ALWAYS_INLINE void closeIterationForThrow(JSGlobalObject* globalObject, Record* state)
{
    VM& vm = globalObject->vm();
    if (Typed::CollectionIteration::isOne(state)) {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Typed::CollectionIteration::close(globalObject, state);
        catchScope.clearExceptionExceptTermination();
    } else {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSValue iterator = state->get(0);
        JSValue method = iterator.get(globalObject, vm.propertyNames->returnKeyword);
        if (!catchScope.exception() && !method.isUndefinedOrNull()) {
            if (auto callData = JSC::getCallData(method); callData.type != CallData::Type::None)
                call(globalObject, method, callData, iterator, ArgList());
        }
        catchScope.clearExceptionExceptTermination();
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseForThrow, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIterationForThrow(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell()));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseForThrowInFrame, void, (Instance* instance, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIterationForThrow(globalObject, Typed::CollectionIteration::InFrame::at(words));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
