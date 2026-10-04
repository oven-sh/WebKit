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

AOT_BINARY_OPERATION(operationAOTValueAdd, jsAdd)
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
    JSGlobalObject* globalObject = instance->globalObject;
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTProgramConstant, EncodedJSValue, (Instance* instance, uint32_t number))
{
    DeferGCForAWhile deferGC(*instance->vm);
    return JSValue::encode(instance->program->constant(number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTemplateObject, EncodedJSValue, (Instance* instance, uint32_t number))
{
    DeferGCForAWhile deferGC(*instance->vm);
    return JSValue::encode(instance->templateObjectFor(number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCreateTransientConstant, EncodedJSValue, (Instance* instance, uint32_t number))
{
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
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    if (base.isCell()) {
        auto& known = instance->customGetterFor(base.asCell()->structureID().bits(), ident.impl());
        if (known.uid == ident.impl() && known.structureID == base.asCell()->structureID().bits() && vm.megamorphicCache() && known.epoch == vm.megamorphicCache()->epoch()) {
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
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;

    PutPropertySlot slot(base, isStrict, putByIdContextOf(instance, callFrame));
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (isDirect && oldStructure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) [[unlikely]] {
        if (!asObject(base)->putDirect(vm, ident, value, slot) && !value.isUndefined()) [[unlikely]]
            throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, oldStructure, ident.impl(), value));
    } else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDirect || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
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
    PropertySlot slot(array, PropertySlot::InternalMethodType::Get);
    double index = JSValue::decode(encodedIndex).asNumber();
    bool has = index <= MAX_ARRAY_INDEX ? array->getPropertySlot(globalObject, static_cast<unsigned>(index), slot) : array->getPropertySlot(globalObject, Identifier::from(vm, index), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!has)
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(index <= MAX_ARRAY_INDEX ? slot.getValue(globalObject, static_cast<unsigned>(index)) : slot.getValue(globalObject, Identifier::from(vm, index))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);

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

JSC_DEFINE_JIT_OPERATION(operationAOTGetLengthSlow, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedBase).get(globalObject, vm.propertyNames->length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckTypedLayout, void, (Instance* instance, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
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
    throwTypeError(globalObject, scope, makeString("Type check failed: the object does not have the layout of its declared type, and cannot be converted to it: "_s, TypedLayoutTable::s_lastConversionFailure));
    OPERATION_RETURN(scope);
}

alignas(16) static const EncodedJSValue s_emptyTypedObject[2 + 256] = { };

JSC_DEFINE_JIT_OPERATION(operationAOTCoerceToTypedLayout, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
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
    unsigned slot = which >> 16 & 0xff;
    bool allowsUndefined = which >> 24 & 1;
    const TypedLayoutTable::Field& field = TypedLayoutTable::fieldWithID(slot, static_cast<uint16_t>(which));
    uint16_t layoutID = TypedLayoutTable::layoutIDOf(field);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isObject()) {
        JSObject* object = asObject(base);
        if (!object->structure()->cannotConvertToTypedLayout() && !object->structure()->typedLayoutID())
            Instance::convertToTypedLayout(vm, object, layoutID);
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
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
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

JSC_DEFINE_JIT_OPERATION(operationAOTValidateTypedObject, void, (Instance* instance, JSObject* object))
{
    AOT_OPERATION_BEGIN(instance);
    uint16_t layoutID = object->structure()->typedLayoutID();
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
            throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, object->structure(), rejected, rejectedValue));
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
            if (name)
                throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, object->structure(), name, value));
            else
                throwTypeError(globalObject, scope, TypedFieldError);
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
    if (actual & TFunctionTag) {
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

JSC_DEFINE_JIT_OPERATION(operationAOTHandleTraps, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance);
    ASSERT(vm.traps().needHandling(VMTraps::AsyncEvents));
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM* vmPointer, JSCell* cell))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, Exception*, (VM* vmPointer))
{
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
