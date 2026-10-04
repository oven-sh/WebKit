/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "CodeBlock.h"
#include "CommonSlowPaths.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GetPutInfo.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "MathCommon.h"
#include "MegamorphicCache.h"
#include "NumberPrototype.h"
#include "PutByIdFlags.h"
#include "StructureChain.h"
#include "VMTrapsInlines.h"

namespace JSC { namespace AOT {

#define AOT_BINARY_OPERATION(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)))); \
    }

JSC_DEFINE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (left.isString() && right.isInt32())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, asString(left), int32ToString(vm, right.asInt32(), 10))));
    if (left.isInt32() && right.isString())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, int32ToString(vm, left.asInt32(), 10), asString(right))));
    OPERATION_RETURN(scope, JSValue::encode(jsAdd(globalObject, left, right)));
}

AOT_BINARY_OPERATION(operationAOTValueSub, jsSub)
AOT_BINARY_OPERATION(operationAOTValueMul, jsMul)
AOT_BINARY_OPERATION(operationAOTValueDiv, jsDiv)
AOT_BINARY_OPERATION(operationAOTValueMod, jsRemainder)
AOT_BINARY_OPERATION(operationAOTValuePow, jsPow)
AOT_BINARY_OPERATION(operationAOTValueBitAnd, jsBitwiseAnd)
AOT_BINARY_OPERATION(operationAOTValueBitOr, jsBitwiseOr)
AOT_BINARY_OPERATION(operationAOTValueBitXor, jsBitwiseXor)
AOT_BINARY_OPERATION(operationAOTValueLShift, jsLShift)
AOT_BINARY_OPERATION(operationAOTValueRShift, jsRShift)
AOT_BINARY_OPERATION(operationAOTValueURShift, jsURShift)

JSC_DEFINE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue primitive = JSValue::decode(encodedOperand).toPrimitive(globalObject, PreferNumber);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (primitive.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, primitive.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-primitive.toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueInc, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsInc(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueDec, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsDec(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumber, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(JSValue::decode(encodedOperand).toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toNumeric(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToString, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toString(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTProgramConstant, EncodedJSValue, (Instance* instance, uint32_t number))
{
    countOperationNamed(instance, __func__);
    DeferGCForAWhile deferGC(*instance->vm);
    return JSValue::encode(instance->program->constant(number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTemplateObject, EncodedJSValue, (Instance* instance, uint32_t number))
{
    countOperationNamed(instance, __func__);
    DeferGCForAWhile deferGC(*instance->vm);
    return JSValue::encode(instance->templateObjectFor(number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCreateTransientConstant, EncodedJSValue, (Instance* instance, uint32_t number))
{
    countOperationNamed(instance, __func__);
    DeferGCForAWhile deferGC(*instance->vm);
    return JSValue::encode(instance->program->createTransientConstant(number));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLess, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, jsLess<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, jsLessEq<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreater, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, jsLess<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, jsLessEq<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::strictEqual(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    noteNamedAccess(instance, __func__, base, { }, ident.impl());
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        countOperationNamed(instance, __func__, !base.isObject() ? "base-is-not-object" : vm.megamorphicCache() && vm.megamorphicCache()->findLoad(base.asCell()->structureID(), ident.impl()) ? "value-is-in-megamorphic-cache" : "value-is-not-in-megamorphic-cache");
        if (vm.megamorphicCache())
            runtimeTable(vm).countChangeOfMegamorphicCacheEpoch(vm.megamorphicCache()->epoch());
    }
    if (base.isCell()) {
        auto& known = instance->customGetterFor(base.asCell()->structureID().bits(), ident.impl());
        if (known.uid == ident.impl() && known.structureID == base.asCell()->structureID().bits() && vm.megamorphicCache() && known.epoch == vm.megamorphicCache()->epoch()) {
            countOperationNamed(instance, __func__, "known-custom-getter");
            auto getter = GetValueFunc(std::bit_cast<GetValueFunc::Ptr>(known.getter));
            OPERATION_RETURN(scope, getter(known.holder->globalObject(), known.passesHolder ? JSValue::encode(known.holder) : encodedBase, ident));
        }
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
        noteCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    if (slot.isUnset() && structureBefore && structureBefore->knownShape())
        caller(instance, callFrame).instance->inspectObjectPrototype();
    cacheGetById(globalObject, callerData(instance, callFrame), base, structureBefore, ident, slot, cache, true);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t flagBits))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;

    PutPropertySlot slot(base, isStrict, putByIdContextOf(instance, callFrame));
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (Options::useAOTOperationCounters() && cache && oldStructure && (cache->offset & Slot::attemptsMask) == Slot::attemptsMask) [[unlikely]] {
        auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl()) : nullptr;
        countOperationNamed(instance, __func__, !entry ? "abandoned-and-not-in-table" : entry->m_reallocating ? "abandoned-and-entry-reallocates" : "abandoned-and-in-table");
    }
    if (MegamorphicCache* table = base.isObject() ? vm.megamorphicCache() : nullptr) {
        if (auto* entry = table->findStore(oldStructure->id(), ident.impl()); entry && entry->m_reallocating == MegamorphicCache::StoreEntry::reallocates) {
            countOperationNamed(instance, __func__, "grows-storage-by-entry");
            JSObject* object = asObject(base);
            Structure* newStructure = entry->m_newStructureID.decode();
            PropertyOffset offset = entry->m_offset;
            Butterfly* butterfly = object->allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), newStructure->outOfLineCapacity());
            object->nukeStructureAndSetButterfly(vm, oldStructure->id(), butterfly);
            object->putDirectOffset(vm, offset, value);
            object->setStructure(vm, newStructure);
            ensureStillAliveHere(oldStructure);
            ensureStillAliveHere(newStructure);
            OPERATION_RETURN(scope);
        }
    }
    if (isDirect && oldStructure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) [[unlikely]] {
        if (!asObject(base)->putDirect(vm, ident, value, slot) && !value.isUndefined()) [[unlikely]]
            throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, oldStructure, ident.impl(), value));
    } else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if (!isDirect && slot.isCacheableSetter() && base.isObject() && base.asCell()->structure() == oldStructure)
        noteInheritedSetter(globalObject, *instance, asObject(base), ident, slot);
    cachePutById(instance, callerData(instance, callFrame), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetElementOrEmpty, EncodedJSValue, (Instance* instance, EncodedJSValue encodedArray, EncodedJSValue encodedIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSObject* array = JSValue::decode(encodedArray).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(array->getIfPropertyExists(globalObject, static_cast<uint64_t>(JSValue::decode(encodedIndex).asNumber()))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    noteNamedAccess(instance, __func__, base, property);

    if (base.isObject() && property.isString()) [[likely]] {
        auto existingAtomString = asString(property)->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (existingAtomString) {
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            OPERATION_RETURN(scope, JSValue::encode(getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, existingAtomString.data), slot)));
        }
    }

    if (base.isObject() && property.isSymbol()) {
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        OPERATION_RETURN(scope, JSValue::encode(getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(asSymbol(property)->privateName()), slot)));
    }

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
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

    base.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSValue value = JSValue::decode(encodedValue);
    noteNamedAccess(instance, __func__, base, property);

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
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
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    base.put(globalObject, key, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, key, slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveScope, JSObject*, (Instance* instance, JSScope* startScope, uint32_t identifierIndex, Slot* cache, uint32_t localScopeDepth))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    UNUSED_VARIABLE(uid);
    if (localScopeDepth == Site::resolvesInGlobalScopes)
        startScope = callerBytecodeOwner(instance, callFrame).isBuiltinFunction() ? globalObject->globalLexicalEnvironment() : instance->loader()->moduleScope();
    JSObject* resolved = JSScope::resolve(globalObject, startScope, ident);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));

    bool cacheable = false;
    if (resolved->isGlobalObject()) {
        bool hasProperty = resolved->hasProperty(globalObject, ident);
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        cacheable = hasProperty && resolved == globalObject;
    } else if (resolved->isGlobalLexicalEnvironment())
        cacheable = resolved == globalObject->globalLexicalEnvironment();
    else if (resolved->type() == ModuleEnvironmentType)
        cacheable = true;
    else if (resolved->type() == LexicalEnvironmentType)
        cacheable = !uncheckedDowncast<JSLexicalEnvironment>(resolved)->symbolTable()->get(uid).isNull();

    unsigned depth = 0;
    if (cacheable) {
        for (JSScope* current = startScope; current && current != resolved; current = current->next()) {
            if (current->isWithScope() || (current->isJSLexicalEnvironment() && uncheckedDowncast<JSLexicalEnvironment>(current)->symbolTable()->usesSloppyEval())) {
                cacheable = false;
                break;
            }
            depth++;
        }
    }
    if (cacheable && resolved->type() == LexicalEnvironmentType && localScopeDepth != Site::resolvesInGlobalScopes) {
        cache->pointer = nullptr;
        cache->offset = Slot::resolvesByDepth | depth;
    } else if (uint32_t epochPlusOne = globalObject->globalLexicalBindingEpoch() + 1; cacheable && !(epochPlusOne & Slot::resolvesByDepth)) {
        cache->offset = 0;
        cache->pointer = resolved;
        cache->offset = epochPlusOne;
    }
    OPERATION_RETURN(scope, resolved);
}

static void cacheEnvironmentVariable(VM& vm, Data* codeBlock, Slot* cache, JSLexicalEnvironment* environment, ScopeOffset offset)
{
    if (offset.offset() > Slot::offsetMask)
        return;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->offset = Slot::pointerIsCell | offset.offset();
    cache->pointer = environment->symbolTable();
    WTF::storeStoreFence();
    cache->structureID = environment->structureID();
    didFillSlot(vm, codeBlock);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFromScope, EncodedJSValue, (Instance* instance, JSObject* scopeObject, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    bool throwIfNotFound = how & Site::throwsIfNotFound;
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();

    auto cacheAddress = [&](void* address) {
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::pointerIsNotCell;
        cache->pointer = address;
        WTF::storeStoreFence();
        cache->structureID = scopeObject->structureID();
        didFillSlot(vm, callerData(instance, callFrame));
    };

    if (scopeObject->type() == ModuleEnvironmentType) {
        auto* environment = uncheckedDowncast<JSModuleEnvironment>(scopeObject);
        SymbolTable* symbolTable = environment->symbolTable();
        ScopeOffset offset;
        if (!(how & Site::isImport)) {
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto iter = symbolTable->find(locker, uid);
            if (iter != symbolTable->end(locker))
                offset = iter->value.scopeOffset();
        }
        if (!!offset) {
            JSValue result = JSModuleEnvironment::readLazyClosureVar(vm, environment, offset);
            if (result)
                cacheAddress(environment->variableAt(offset).slot());
            OPERATION_RETURN(scope, JSValue::encode(result));
        }

        auto resolution = environment->moduleRecord()->resolveImport(globalObject, ident);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (resolution.type == AbstractModuleRecord::Resolution::Type::Resolved) {
            JSModuleEnvironment* exporter = resolution.moduleRecord->moduleEnvironment();
            auto entry = exporter->symbolTable()->get(resolution.localName.impl());
            if (!entry.isNull()) {
                JSValue result = JSModuleEnvironment::readLazyClosureVar(vm, exporter, entry.scopeOffset());
                if (result)
                    cacheAddress(exporter->variableAt(entry.scopeOffset()).slot());
                OPERATION_RETURN(scope, JSValue::encode(result));
            }
        }
    }

    if (scopeObject->type() == LexicalEnvironmentType) {
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scopeObject);
        auto entry = environment->symbolTable()->get(uid);
        if (!entry.isNull()) {
            cacheEnvironmentVariable(vm, callerData(instance, callFrame), cache, environment, entry.scopeOffset());
            OPERATION_RETURN(scope, JSValue::encode(environment->variableAt(entry.scopeOffset()).get()));
        }
    }

    OPERATION_RETURN(scope, JSValue::encode(scopeObject->getPropertySlot(globalObject, ident, [&](bool found, PropertySlot& slot) -> JSValue {
        if (!found) {
            if (throwIfNotFound)
                throwException(globalObject, scope, createUndefinedVariableError(globalObject, ident));
            return jsUndefined();
        }

        if (scopeObject->isGlobalLexicalEnvironment()) {
            JSValue result = slot.getValue(globalObject, ident);
            if (result == jsTDZValue()) {
                throwException(globalObject, scope, createTDZError(globalObject, ident.string()));
                return jsUndefined();
            }
            auto* environment = uncheckedDowncast<JSGlobalLexicalEnvironment>(scopeObject);
            auto entry = environment->symbolTable()->get(uid);
            if (!entry.isNull())
                cacheAddress(environment->variableAt(entry.scopeOffset()).slot());
            return result;
        }

        if (scopeObject->isGlobalObject()) {
            auto* variables = uncheckedDowncast<JSGlobalObject>(scopeObject);
            auto entry = variables->symbolTable()->get(uid);
            if (!entry.isNull())
                cacheAddress(variables->variableAt(entry.scopeOffset()).slot());
            else if (slot.isCacheableValue() && slot.slotBase() == scopeObject && scopeObject->structure()->propertyAccessesAreCacheable()) {
                cache->structureID = StructureID();
                WTF::storeStoreFence();
                cache->offset = slot.cachedOffset();
                cache->pointer = nullptr;
                WTF::storeStoreFence();
                cache->structureID = scopeObject->structureID();
                didFillSlot(vm, callerData(instance, callFrame));
            }
        }
        return slot.getValue(globalObject, ident);
    })));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFillImportSlot, JSObject*, (Instance* instance, JSObject* importer, uint32_t slot))
{
    AOT_OPERATION_BEGIN(instance);
    auto* environment = uncheckedDowncast<JSModuleEnvironment>(importer);
    OPERATION_RETURN(scope, uncheckedDowncast<JSModuleRecord>(environment->moduleRecord())->fillImportSlot(globalObject, slot));
}

JSC_DEFINE_JIT_OPERATION(operationAOTReadLazyClosureVar, EncodedJSValue, (Instance* instance, JSObject* scopeObject, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSModuleEnvironment::readLazyClosureVar(vm, scopeObject, ScopeOffset(offset))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutToScope, void, (Instance* instance, JSObject* scopeObject, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    GetPutInfo getPutInfo(static_cast<ResolveMode>(how & 1), GlobalProperty, static_cast<InitializationMode>((how >> 1) & 3), how & 8 ? ECMAMode::strict() : ECMAMode::sloppy());
    JSValue value = JSValue::decode(encodedValue);

    if (scopeObject->isJSLexicalEnvironment()) {
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scopeObject);
        SymbolTable* symbolTable = environment->symbolTable();
        ScopeOffset offset;
        InlineWatchpointSet* set = nullptr;
        {
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto iter = symbolTable->find(locker, uid);
            if (iter != symbolTable->end(locker) && (!iter->value.isReadOnly() || isInitialization(getPutInfo.initializationMode()))) {
                offset = iter->value.scopeOffset();
                set = iter->value.watchpointSet();
            }
        }
        if (!!offset) {
            environment->variableAt(offset).set(vm, environment, value);
            if (set)
                set->invalidate(vm, StringFireDetail("Executed op_put_to_scope in AOT code"));
            cacheEnvironmentVariable(vm, callerData(instance, callFrame), cache, environment, offset);
            OPERATION_RETURN(scope);
        }
    }

    bool hasProperty = scopeObject->hasProperty(globalObject, ident);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (hasProperty && scopeObject->isGlobalLexicalEnvironment() && !isInitialization(getPutInfo.initializationMode())) {
        PropertySlot slot(scopeObject, PropertySlot::InternalMethodType::Get);
        JSGlobalLexicalEnvironment::getOwnPropertySlot(scopeObject, globalObject, ident, slot);
        if (slot.getValue(globalObject, ident) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, ident.string()));
            OPERATION_RETURN(scope);
        }
    }
    if (getPutInfo.resolveMode() == ThrowIfNotFound && !hasProperty) {
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, ident));
        OPERATION_RETURN(scope);
    }

    PutPropertySlot slot(scopeObject, getPutInfo.ecmaMode().isStrict(), PutPropertySlot::UnknownContext, isInitialization(getPutInfo.initializationMode()));
    scope.release();
    scopeObject->methodTable()->put(scopeObject, globalObject, ident, value, slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrow, void, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance);
    throwException(globalObject, scope, JSValue::decode(encodedValue));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckType, void, (Instance* instance, EncodedJSValue encodedValue, uint32_t mask))
{
    AOT_OPERATION_BEGIN(instance);
    unsigned tag = soundTypeTag(JSValue::decode(encodedValue));
    if (soundTypeMaskAccepts(mask, JSValue::decode(encodedValue)))
        OPERATION_RETURN(scope);
    throwTypeError(globalObject, scope, makeString("Type check failed: expected "_s, toString(SoundTypeMaskDump(mask)), ", got "_s, toString(SoundTypeMaskDump(tag))));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckTypeAheadOfCreateThis, void, (Instance* instance, EncodedJSValue encodedValue, uint32_t mask, JSObject* newTarget))
{
    AOT_OPERATION_BEGIN(instance);
    unsigned tag = soundTypeTag(JSValue::decode(encodedValue));
    if (soundTypeMaskAccepts(mask, JSValue::decode(encodedValue)))
        OPERATION_RETURN(scope);
    if (auto* constructor = dynamicDowncast<JSFunction>(newTarget); !constructor || !constructor->canUseAllocationProfiles()) {
        JSValue prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (!prototype.isObject()) {
            getFunctionRealm(globalObject, newTarget);
            OPERATION_RETURN_IF_EXCEPTION(scope);
        }
    }
    throwTypeError(globalObject, scope, makeString("Type check failed: expected "_s, toString(SoundTypeMaskDump(mask)), ", got "_s, toString(SoundTypeMaskDump(tag))));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetLengthSlow, size_t, (Instance* instance, JSObject* array))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue length = array->get(globalObject, vm.propertyNames->length);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, length.toLength(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckTypedLayout, void, (Instance* instance, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) {
        if (!TypedLayoutTable::isAuditing())
            value.toObject(globalObject);
        OPERATION_RETURN(scope);
    }
    if (value.isObject() && (asObject(value)->structure()->typedLayoutID() == layoutID || Instance::convertToTypedLayout(vm, asObject(value), safeCast<uint16_t>(layoutID))))
        OPERATION_RETURN(scope);
    if (!value.isObject())
        TypedLayoutTable::s_lastConversionFailure = "it is no object"_s;
    if (TypedLayoutTable::isAuditing()) {
        TypedLayoutTable::reportViolation(TypedLayoutTable::s_lastConversionFailure, safeCast<uint16_t>(layoutID), value);
        OPERATION_RETURN(scope);
    }
    String className = instance->nameOfClassWithLayout(safeCast<uint16_t>(layoutID));
    throwTypeError(globalObject, scope, makeString("Type check failed: the object does not have the layout of its declared type"_s, className.isEmpty() ? ""_s : ", the class "_s, className, ", and cannot be converted to it: "_s, TypedLayoutTable::s_lastConversionFailure));
    OPERATION_RETURN(scope);
}

alignas(16) static const EncodedJSValue s_emptyTypedObject[2 + 256] = { };

JSC_DEFINE_JIT_OPERATION(operationAOTCoerceToTypedLayout, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isObject() && (asObject(value)->structure()->typedLayoutID() == layoutID || Instance::convertToTypedLayout(vm, asObject(value), safeCast<uint16_t>(layoutID))))
        OPERATION_RETURN(scope, encodedValue);
    if (TypedLayoutTable::isAuditing() && !value.isUndefinedOrNull()) [[unlikely]]
        TypedLayoutTable::reportViolation(value.isObject() ? TypedLayoutTable::s_lastConversionFailure : "it is no object"_s, safeCast<uint16_t>(layoutID), value);
    OPERATION_RETURN(scope, static_cast<EncodedJSValue>(std::bit_cast<uintptr_t>(&s_emptyTypedObject[0])));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFieldSlow, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint64_t which))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationAtSite(instance, callFrame, __func__);
    uint16_t layoutID = static_cast<uint16_t>(which >> 32);
    unsigned slot = which >> 48 & 0xff;
    bool allowsUndefined = which >> 56 & 1;
    JSValue base = JSValue::decode(encodedBase);
    UniquedStringImpl* uid = instance->program->identifier(static_cast<uint32_t>(which));
    JSValue value;
    MegamorphicCache* cache = vm.megamorphicCache();
    if (auto* known = cache && base.isObject() ? cache->findLoad(asObject(base)->structureID(), uid) : nullptr) {
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        value = getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, uid), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (value.isUndefined() && allowsUndefined)
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (TypedLayoutTable::checkStore(layoutID, slot, value) == TypedLayoutTable::StoreCheck::Rejected) {
        if (auto* fieldType = TypedLayoutTable::fieldTypeInSlot(layoutID, slot))
            throwTypeError(globalObject, scope, TypedLayoutTable::describeMismatch(uid, *fieldType, value));
        else
            throwTypeError(globalObject, scope, "Type check failed: the value of a property does not match its declared type"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(TypedLayoutTable::toFieldRepresentation(layoutID, slot, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t which))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationAtSite(instance, callFrame, __func__);
    unsigned slot = which >> 16 & 0xff;
    bool allowsUndefined = which >> 24 & 1;
    const TypedLayoutTable::Field& field = TypedLayoutTable::fieldWithID(slot, static_cast<uint16_t>(which));
    uint16_t layoutID = TypedLayoutTable::layoutIDOf(field);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isObject()) {
        JSObject* object = asObject(base);
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).noteShape("ReadField", makeString("wants layout "_s, layoutID, " slot "_s, slot, " id "_s, field.id, " name "_s, StringView(instance->program->identifier(field.identifier)), " there "_s, object->structure()->typedLayoutID() ? object->structure()->fieldIDInSlot(slot) : 0, " of"_s), vm, object->structure());
        if (!object->structure()->cannotConvertToTypedLayout() && !object->structure()->typedLayoutID())
            countOperationNamed(instance, __func__, Instance::convertToTypedLayout(vm, object, layoutID) ? "converted" : "conversion-refused");
        else
            countOperationNamed(instance, __func__, object->structure()->typedLayoutID() ? "has-a-layout" : "cannot-be-converted");
        Structure* structure = object->structure();
        if (structure->typedLayoutID() == layoutID) {
            uint16_t there = structure->fieldIDInSlot(slot);
            if (there == field.id) {
                if (JSValue value = object->getDirect(static_cast<PropertyOffset>(slot)))
                    OPERATION_RETURN(scope, JSValue::encode(value));
            } else if (there != Structure::ambiguousFieldID && allowsUndefined)
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    UniquedStringImpl* uid = instance->program->identifier(field.identifier);
    JSValue value;
    MegamorphicCache* cache = vm.megamorphicCache();
    if (auto* known = cache && base.isObject() ? cache->findLoad(asObject(base)->structureID(), uid) : nullptr) {
        countOperationNamed(instance, __func__, "value-is-in-megamorphic-cache");
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
        countOperationNamed(instance, __func__, "value-is-not-in-megamorphic-cache");
        PropertySlot propertySlot(base, PropertySlot::InternalMethodType::Get);
        value = getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, uid), propertySlot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!base.isObject() || (propertySlot.isUnset() ? propertySlot.isTaintedByOpaqueObject() : !propertySlot.isCacheableValue()))
            instance->noteObservableRead(slot, field.id);
    }
    if (value.isUndefined() && allowsUndefined)
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (TypedLayoutTable::checkStore(field, value) == TypedLayoutTable::StoreCheck::Rejected) {
        if (auto* fieldType = TypedLayoutTable::fieldTypeOf(field))
            throwTypeError(globalObject, scope, TypedLayoutTable::describeMismatch(uid, *fieldType, value));
        else
            throwTypeError(globalObject, scope, "Type check failed: the value of a property does not match its declared type"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(TypedLayoutTable::toFieldRepresentation(field, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTVerifyGuardedRead, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t slot, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    RELEASE_ASSERT(base.isObject());
    unsigned attributes = 0;
    PropertyOffset offset = base.asCell()->structure()->get(vm, identifierAt(instance, callFrame, identifierIndex), attributes);
    RELEASE_ASSERT(static_cast<unsigned>(offset) == slot && !attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTToFieldValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue, uint64_t packedFieldType, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue value = JSValue::decode(encodedValue);
    TypedLayoutTable::FieldType fieldType { static_cast<uint16_t>(packedFieldType), static_cast<uint16_t>(packedFieldType >> 16), static_cast<uint16_t>(packedFieldType >> 32), 0 };
    if (!TypedLayoutTable::accepts(fieldType, value)) {
        throwTypeError(globalObject, scope, TypedLayoutTable::describeMismatch(identifierAt(instance, callFrame, identifierIndex).impl(), fieldType, value));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(TypedLayoutTable::toFieldRepresentation(&fieldType, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCountFamilyGuard, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t which))
{
    AOT_OPERATION_BEGIN(instance);
    uint16_t family = static_cast<uint16_t>(which);
    JSValue base = JSValue::decode(encodedBase);
    auto outcome = [&]() -> const char* {
        if (which >> 16)
            return instance->departedFamilies[family] ? "exits-departed" : "passes";
        if (!base.isCell())
            return "exits-not-a-cell";
        uint16_t there = base.asCell()->structure()->family();
        return there == family ? "passes" : there ? "exits-with-another-number" : "exits-without-number";
    }();
    runtimeTable(vm).countForFamily("Family::guard", "Family::guard-of", family, outcome);
    if (outcome[0] == 'e') {
        countOperationNamed(instance, "exit-into-generic-copy", outcome);
        countOperationAtSite(instance, callFrame, "exit-into-generic-copy");
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTVerifyGuardedStore, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t slot, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    RELEASE_ASSERT(base.isObject());
    Structure* structure = base.asCell()->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, identifierAt(instance, callFrame, identifierIndex), attributes);
    RELEASE_ASSERT(static_cast<unsigned>(offset) == slot && !attributes);
    RELEASE_ASSERT(!structure->isDictionary() && structure->propertyAccessesAreCacheable() && !structure->mayBePrototype());
    RELEASE_ASSERT(!structure->typedLayoutID() || !TypedLayoutTable::hasTypedFields());
    WatchpointSet* replacements = structure->propertyReplacementWatchpointSet(offset);
    RELEASE_ASSERT(!replacements || !replacements->isStillValid());
    OPERATION_RETURN(scope);
}

static const char* howReadByNameWillBeServed(VM& vm, JSValue base, UniquedStringImpl* name, const Slot* cache)
{
    StructureID structureID = base.isCell() ? base.asCell()->structureID() : StructureID();
    bool isInTable = structureID && vm.megamorphicCache() && vm.megamorphicCache()->findLoad(structureID, name);
    auto hasNameInSlot = [&](uint16_t id, unsigned slot) {
        return id && id < Structure::firstReservedPropertyNameID && slot < Structure::numberOfSlotsWithPropertyNameIDs && base.isCell() && base.asCell()->structure()->fieldIDInSlot(slot) == id;
    };
    if (SharedData::contains(cache))
        return isInTable ? "site-has-nobodys-slot-and-table-hits" : "site-has-nobodys-slot-and-table-misses";
    if (cache->isPolymorphic()) {
        auto* several = static_cast<const PolymorphicSlots*>(cache->pointer);
        unsigned used = 0;
        bool hits = false;
        for (const Slot& slot : several->slots) {
            used += !!slot.structureID;
            hits |= structureID && slot.structureID == structureID;
        }
        bool hitsByName = false;
        for (unsigned i = 0; i < several->numberOfUsedInlineNameSlots; ++i)
            hitsByName |= hasNameInSlot(static_cast<uint16_t>(several->byName), several->byName >> (PolymorphicSlots::inlineNameSlotsShift + i * 8) & 0xff);
        static_assert(PolymorphicSlots::numberOfSlots == 4);
        static constexpr const char* services[PolymorphicSlots::numberOfSlots + 1][4] = {
            { "polymorphic-site-of-0-hits-by-name", "polymorphic-site-of-0-hits", "polymorphic-site-of-0-misses-and-table-hits", "polymorphic-site-of-0-misses-and-table-misses" },
            { "polymorphic-site-of-1-hits-by-name", "polymorphic-site-of-1-hits", "polymorphic-site-of-1-misses-and-table-hits", "polymorphic-site-of-1-misses-and-table-misses" },
            { "polymorphic-site-of-2-hits-by-name", "polymorphic-site-of-2-hits", "polymorphic-site-of-2-misses-and-table-hits", "polymorphic-site-of-2-misses-and-table-misses" },
            { "polymorphic-site-of-3-hits-by-name", "polymorphic-site-of-3-hits", "polymorphic-site-of-3-misses-and-table-hits", "polymorphic-site-of-3-misses-and-table-misses" },
            { "polymorphic-site-of-4-hits-by-name", "polymorphic-site-of-4-hits", "polymorphic-site-of-4-misses-and-table-hits", "polymorphic-site-of-4-misses-and-table-misses" },
        };
        return services[used][hitsByName ? 0 : hits ? 1 : isInTable ? 2 : 3];
    }
    if (structureID && cache->structureID == structureID)
        return cache->offset & Slot::flagsMask ? "site-hits-indirectly" : "site-hits";
    constexpr unsigned wordsBeforeInlineStorage = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue);
    if (!(cache->offset & (Slot::flagsMask | Slot::attemptsMask)) && hasNameInSlot(static_cast<uint16_t>(cache->offset >> Slot::nameIDShift), (cache->offset & Slot::directLocationMask) - wordsBeforeInlineStorage))
        return "site-hits-by-name";
    if (cache->structureID)
        return isInTable ? "site-holds-another-structure-and-table-hits" : "site-holds-another-structure-and-table-misses";
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return isInTable ? "site-is-abandoned-and-table-hits" : "site-is-abandoned-and-table-misses";
    return isInTable ? "site-is-empty-and-table-hits" : "site-is-empty-and-table-misses";
}

JSC_DEFINE_JIT_OPERATION(operationAOTCountReadByName, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    UniquedStringImpl* name = identifierAt(instance, callFrame, identifierIndex).impl();
    NamedAccess access = classifyNamedAccess(vm, base, { }, name);
    const char* outcome = base.isObject() && asObject(base)->type() == FinalObjectType ? access.place : access.receiver;
    const char* service = howReadByNameWillBeServed(vm, base, name, cache);
    auto& table = runtimeTable(vm);
    for (const char* detail : { access.receiver, access.place, service })
        table.countOperation(__func__, detail);
    table.countOperation(outcome, service);
    OPERATION_RETURN(scope);
}

#define AOT_SERVICES_OF_STORE_WITH_TABLE(state, site) \
    "store-site-" state "-and-table-misses" site, "store-site-" state "-and-table-hits" site, "store-site-" state "-and-table-hits-and-reallocates" site, "store-site-" state "-and-table-hits-and-allocates-storage" site
#define AOT_SERVICES_OF_STORE(site) { \
    "store-site-hits" site, "store-site-hits-with-transition" site, "store-site-hits-typed-field" site, \
    AOT_SERVICES_OF_STORE_WITH_TABLE("has-nobodys-slot", site), AOT_SERVICES_OF_STORE_WITH_TABLE("holds-another-structure", site), \
    AOT_SERVICES_OF_STORE_WITH_TABLE("is-abandoned", site), AOT_SERVICES_OF_STORE_WITH_TABLE("is-empty", site) }

JSC_DEFINE_JIT_OPERATION(operationAOTCountStoreByName, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache, uint32_t kindOfSite))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    UniquedStringImpl* name = identifierAt(instance, callFrame, identifierIndex).impl();
    bool isDirect = kindOfSite & 1;
    bool isStrict = kindOfSite & 2;
    unsigned site = isDirect ? 0 : kindOfSite & 4 ? 1 : kindOfSite & 8 ? 2 : kindOfSite & 16 ? 3 : 4;
    static constexpr const char* sites[] = { "direct-store", "store-behind-another-path", "store-to-object-born-here", "keyed-store-with-constant-key", "store-without-guess" };
    static constexpr const char* services[][19] = { AOT_SERVICES_OF_STORE("-at-direct-store"), AOT_SERVICES_OF_STORE("-behind-another-path"), AOT_SERVICES_OF_STORE("-to-object-born-here"), AOT_SERVICES_OF_STORE("-with-constant-key"), AOT_SERVICES_OF_STORE("-without-guess") };
    auto outcomeOfStore = [&]() -> const char* {
        if (!base.isObject())
            return "store-to-no-object";
        if (asObject(base)->type() != FinalObjectType)
            return "store-to-object-that-is-not-final";
        Structure* structure = asObject(base)->structure();
        if (structure->isDictionary())
            return "store-to-dictionary";
        constexpr unsigned accessor = PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue;
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (offset != invalidOffset) {
            if (isDirect && attributes)
                return "store-redefines-property-with-attributes";
            if (attributes & accessor)
                return "store-calls-own-setter";
            if (attributes & PropertyAttribute::ReadOnly)
                return isStrict ? "store-is-refused-by-own-read-only-property-and-throws" : "store-is-refused-by-own-read-only-property";
            if (!isInlineOffset(offset))
                return "store-replaces-out-of-line";
            if (static_cast<unsigned>(offset) >= Structure::numberOfSlotsWithPropertyNameIDs)
                return "store-replaces-beyond-named-slots";
            uint16_t id = structure->fieldIDInSlot(offset);
            return id && id < Structure::firstReservedPropertyNameID && structure->recordsPropertyNames() ? "store-replaces-in-slot-with-id" : "store-replaces-in-slot-without-id";
        }
        for (JSValue next = isDirect ? JSValue() : asObject(base)->getPrototypeDirect(); next && next.isObject(); next = asObject(next)->getPrototypeDirect()) {
            Structure* holderStructure = asObject(next)->structure();
            if (asObject(next)->type() == ProxyObjectType || holderStructure->typeInfo().overridesGetOwnPropertySlot() || (holderStructure->typeInfo().hasStaticPropertyTable() && !holderStructure->staticPropertiesReified()))
                return "store-may-meet-prototype-that-cannot-be-asked";
            if (holderStructure->get(vm, name, attributes) == invalidOffset)
                continue;
            if (attributes & accessor)
                return "store-calls-inherited-setter";
            if (attributes & PropertyAttribute::ReadOnly)
                return isStrict ? "store-is-refused-by-inherited-read-only-property-and-throws" : "store-is-refused-by-inherited-read-only-property";
            break;
        }
        if (!structure->isStructureExtensible())
            return isStrict ? "store-to-object-that-is-not-extensible-and-throws" : "store-to-object-that-is-not-extensible";
        if (structure->inlineSize() < structure->inlineCapacity() && !structure->outOfLineSize())
            return "store-adds-inline";
        return structure->outOfLineSize() < structure->outOfLineCapacity() ? "store-adds-out-of-line" : "store-adds-out-of-line-and-grows-storage";
    };
    auto serviceOfStore = [&]() -> unsigned {
        StructureID structureID = base.isCell() ? base.asCell()->structureID() : StructureID();
        if (!SharedData::contains(cache) && structureID && cache->structureID == structureID)
            return cache->offset & Slot::hasFieldType ? 2 : cache->offset & Slot::isIndirect ? 1 : 0;
        auto* entry = structureID && vm.megamorphicCache() ? vm.megamorphicCache()->findStore(structureID, name) : nullptr;
        unsigned table = !entry ? 0 : entry->m_reallocating == MegamorphicCache::StoreEntry::reallocates ? 2 : entry->m_reallocating == MegamorphicCache::StoreEntry::allocatesInitialOutOfLineStorage ? 3 : 1;
        unsigned state = SharedData::contains(cache) ? 0 : cache->structureID ? 1 : (cache->offset & Slot::attemptsMask) == Slot::attemptsMask ? 2 : 3;
        return 3 + state * 4 + table;
    };
    const char* outcome = outcomeOfStore();
    const char* service = services[site][serviceOfStore()];
    auto& table = runtimeTable(vm);
    for (const char* detail : { sites[site], outcome, service })
        table.countOperation(__func__, detail);
    table.countOperation(outcome, service);
    if (base.isCell()) {
        Structure* structure = base.asCell()->structure();
        if (structure->typedLayoutID())
            table.countOperation(__func__, "structure-has-typed-layout");
        if (structure->mayBePrototype())
            table.countOperation(__func__, "structure-may-be-prototype");
        if (structure->isWatchingReplacement())
            table.countOperation(__func__, "structure-watches-replacements");
    }
    OPERATION_RETURN(scope);
}

#undef AOT_SERVICES_OF_STORE
#undef AOT_SERVICES_OF_STORE_WITH_TABLE

JSC_DEFINE_JIT_OPERATION(operationAOTValidateTypedObject, void, (Instance* instance, JSObject* object))
{
    AOT_OPERATION_BEGIN(instance);
    uint16_t layoutID = object->structure()->typedLayoutID();
    auto reject = [&](const String& message) {
        for (unsigned slot = 0; slot < std::min<unsigned>(object->structure()->inlineCapacity(), TypedLayoutTable::inlineSlots(layoutID)); ++slot)
            object->locationForOffset(static_cast<PropertyOffset>(slot))->clear();
        throwTypeError(globalObject, scope, message);
    };
    if (TypedLayoutTable::usesFieldIDs(layoutID)) {
        UniquedStringImpl* rejected = nullptr;
        JSValue rejectedValue;
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (auto* field = TypedLayoutTable::findField(vm, layoutID, entry.key()); field && TypedLayoutTable::checkStore(*field, object->getDirect(entry.offset())) == TypedLayoutTable::StoreCheck::Rejected) {
                rejected = entry.key();
                rejectedValue = object->getDirect(entry.offset());
            }
            return !rejected;
        });
        if (rejected)
            reject(TypedLayoutTable::describeRejectedStore(vm, object->structure(), rejected, rejectedValue));
        OPERATION_RETURN(scope);
    }
    for (unsigned slot = 0; slot < TypedLayoutTable::numberOfSlots(layoutID); ++slot) {
        JSValue value = object->getDirect(TypedLayoutTable::offsetInLayout(layoutID, slot));
        if (value && TypedLayoutTable::checkStore(layoutID, slot, value) == TypedLayoutTable::StoreCheck::Rejected) {
            UniquedStringImpl* name = nullptr;
            object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
                if (entry.offset() == TypedLayoutTable::offsetInLayout(layoutID, slot))
                    name = entry.key();
                return !name;
            });
            reject(name ? TypedLayoutTable::describeRejectedStore(vm, object->structure(), name, value) : String { TypedFieldError });
            OPERATION_RETURN(scope);
        }
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTVerifyInferredType, size_t, (Instance* instance, EncodedJSValue encodedValue, uint64_t typeLowHalf, uint64_t typeHighHalf, uint32_t which, uint32_t identifierIndexPlusOne, uint64_t scopeWhenCompiled, uint32_t scopeOffset))
{
    JSGlobalObject* globalObject = instance->globalObject;
    Type type = static_cast<Type>(typeHighHalf) << 64 | typeLowHalf;
    Type actual = valueType(JSValue::decode(encodedValue));
    if (actual & TFinalObjectTag)
        actual = (actual & ~TFinalObject) | objectTypeForLayout(JSValue::decode(encodedValue).asCell()->structure()->typedLayoutID());
    uint32_t intrinsic = 0;
    if ((actual & TFunctionTag) && (type & TAnyFunctionNumber) != TAnyFunctionNumber && ImmutableIntrinsics::shared()) {
        for (unsigned number = 1; number < ImmutableIntrinsics::shared()->count() && !intrinsic; ++number) {
            if (instance->intrinsics[number] == encodedValue)
                intrinsic = ImmutableIntrinsics::shared()->at(number).canonical;
        }
    }
    if (intrinsic && isSubtype(intrinsicFunctionType(intrinsic), type))
        actual = (actual & ~TFunction) | intrinsicFunctionType(intrinsic);
    else if (actual & TFunctionTag) {
        if (auto* function = dynamicDowncast<JSFunction>(JSValue::decode(encodedValue).asCell()); function && !function->isHostFunction()) {
            Image* image = Image::withCode();
            bool hasWord = function->hasAOTFunctionWord();
            uint32_t index = hasWord ? function->aotFunctionIndex() : function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall);
            bool hasEntry = hasWord || function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall);
            if (image && hasEntry && index < image->header().numberOfFunctions) {
                if (uint32_t number = image->at<uint32_t>(image->header().functionNumbersOffset)[index])
                    actual = (actual & ~TFunction) | functionType(number);
            } else if (!hasEntry) {
                actual = (actual & ~TAnyFunctionNumber) | (type & TAnyFunctionNumber);
            }
        }
    }
    if (isSubtype(actual, type)) [[likely]]
        return 0;
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    NativeCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    scope.clearException();
    dataLog("AOT: inferred type violation: ");
    if (which >= 1000000)
        dataLog(opcodeNames[which / 1000000], " at bc#", which % 1000000);
    else if (which % 100 == static_cast<unsigned>(NodeKind::Argument))
        dataLog("argument ", which / 100);
    else
        dataLog("a node of kind ", which);
    if (identifierIndexPlusOne)
        dataLog(" of `", identifierAt(instance, callFrame, identifierIndexPlusOne - 1).impl(), "`");
    if (scopeWhenCompiled)
        dataLog(" (scope ", RawPointer(std::bit_cast<void*>(static_cast<uintptr_t>(scopeWhenCompiled))), " offset ", scopeOffset, ")");
    dataLog(" was inferred to be ");
    if (!type)
        dataLog("unreachable");
    else
        dumpType(WTF::dataFile(), type);
    dataLog(" but is ");
    dumpType(WTF::dataFile(), actual);
    if (auto* function = (actual & TFunctionTag) ? dynamicDowncast<JSFunction>(JSValue::decode(encodedValue).asCell()) : nullptr; function && !function->isHostOrBuiltinFunction())
        dataLog(" (index ", function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall), ", entry ", RawHex(function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall)), ")");
    dataLog(" (bits ", RawHex(static_cast<uint64_t>(encodedValue)));
    if (static_cast<uint64_t>(encodedValue) == std::bit_cast<uintptr_t>(&s_emptyTypedObject[0]))
        dataLog(": the empty typed object placeholder");
    else if (JSValue value = JSValue::decode(encodedValue); value && value.isCell())
        dataLog(", a cell of type ", static_cast<unsigned>(value.asCell()->type()), value.isObject() ? " " : "", value.isObject() ? asObject(value)->classInfo()->className : ""_s);
    dataLog(")");
    dataLogLn();
    if (JSValue value = JSValue::decode(encodedValue); value && value.isCell() && !value.asCell()->type() && static_cast<uint64_t>(encodedValue) != std::bit_cast<uintptr_t>(&s_emptyTypedObject[0])) {
        JSCell* cell = value.asCell();
        auto* words = std::bit_cast<const uint64_t*>(cell);
        dataLog("    words:");
        for (unsigned i = 0; i < 12; ++i)
            dataLog(" ", RawHex(words[i]));
        dataLogLn();
        dataLog("    before it:");
        for (int i = -8; i < 0; ++i)
            dataLog(" ", RawHex(words[i]));
        dataLogLn();
        if (StructureID id = cell->structureID()) {
            Structure* structure = id.decode();
            auto* structureWords = std::bit_cast<const uint64_t*>(structure);
            dataLog("    its structure ", RawPointer(structure), ":");
            for (unsigned i = 0; i < 6; ++i)
                dataLog(" ", RawHex(structureWords[i]));
            dataLogLn("; blob ", RawHex(structure->typeInfoBlob()), ", type ", static_cast<unsigned>(structure->typeInfo().type()), ", born as ", structure->typedLayoutID(), ", inline capacity ", structure->inlineCapacity(),
                structure->markedBlock().handle().isLive(structure) ? ", live" : ", NOT LIVE");
        }
        if (!cell->isPreciseAllocation()) {
            MarkedBlock& block = cell->markedBlock();
            dataLogLn("    in a block of cells of ", block.handle().cellSize(), " bytes; marked: ", block.isMarked(cell), ", newly allocated: ", block.isNewlyAllocated(cell), ", live: ", block.handle().isLive(cell), ", free-listed: ", block.handle().isFreeListed());
        }
    }
    JSObject* error = createError(globalObject, "the stack:"_s);
    JSValue stack = error->get(globalObject, vm.propertyNames->stack);
    if (!scope.exception() && stack.isString())
        dataLogLn(asString(stack)->value(globalObject).data);
    _exit(70);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportBranchFoldedWrongly, void, (Instance* instance, uint32_t branch, uint32_t tested, uint32_t isTaken))
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    NativeCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    dataLog("AOT: branch folded wrongly: ", opcodeNames[branch / 1000000], " at bc#", branch % 1000000, " on ");
    if (tested >= 1000000)
        dataLog(opcodeNames[tested / 1000000], " at bc#", tested % 1000000);
    else
        dataLog("a node of kind ", tested);
    dataLogLn(" was inferred to be ", isTaken ? "always" : "never", " taken but is ", isTaken ? "not taken" : "taken");
    JSObject* error = createError(globalObject, "the stack:"_s);
    JSValue stack = error->get(globalObject, vm.propertyNames->stack);
    if (!scope.exception() && stack.isString())
        dataLogLn(asString(stack)->value(globalObject).data);
    _exit(70);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHandleTraps, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance);
    ASSERT(vm.traps().needHandling(VMTraps::AsyncEvents));
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM* vmPointer, JSCell* cell))
{
    countOperationNamed(*vmPointer, __func__);
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, Exception*, (VM* vmPointer))
{
    countOperationNamed(*vmPointer, __func__);
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    RELEASE_ASSERT(!!scope.exception());
    Exception* exception = scope.exception();
    if (!scope.tryClearException())
        return nullptr;
    return exception;
}

JSC_DEFINE_JIT_OPERATION(operationAOTLatin1StringEqualTo, StringImpl*, (Instance* instance, JSString* string))
{
    AOT_OPERATION_BEGIN(instance);
    auto value = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    StringImpl* impl = value.data.impl();
    if (impl->is8Bit()) [[likely]]
        OPERATION_RETURN(scope, impl);
    if (!WTF::charactersAreAllLatin1(impl->span16()))
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, instance->retainUntilNextCall(String::make8Bit(impl->span16())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSwitchString, int32_t, (Instance* instance, EncodedJSValue encodedValue, uint32_t tableIndex, uint32_t whose))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        OPERATION_RETURN(scope, 0);
    auto string = asString(value)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    const UnlinkedStringJumpTable& table = callerBytecodeOwner(instance, callFrame, whose).stringSwitchJumpTable(tableIndex);
    OPERATION_RETURN(scope, table.offsetForValue(string.data.impl()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSwitchChar, int32_t, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        OPERATION_RETURN(scope, -1);
    JSString* string = asString(value);
    if (string->length() != 1)
        OPERATION_RETURN(scope, -1);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, -1);
    OPERATION_RETURN(scope, view[0]);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFMod, double, (double a, double b))
{
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToInt32, int32_t, (double value))
{
    return JSC::toInt32(value);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
