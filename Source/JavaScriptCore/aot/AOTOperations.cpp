/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#include "AOTImage.h"

#if ENABLE(FTL_JIT)

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
#include "JSModuleRecord.h"
#include "MathCommon.h"
#include "MegamorphicCache.h"
#include "PutByIdFlags.h"
#include "StructureChain.h"
#include "VMTrapsInlines.h"

namespace JSC { namespace AOT {

#define AOT_OPERATION_PROLOGUE(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    countOperationFor(globalObject, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

#define AOT_BINARY_OPERATION(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_PROLOGUE(globalObject); \
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

JSC_DEFINE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue primitive = JSValue::decode(encodedOperand).toPrimitive(globalObject, PreferNumber);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (primitive.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, primitive.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-primitive.toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueInc, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsInc(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueDec, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsDec(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumber, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(JSValue::decode(encodedOperand).toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toNumeric(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToString, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toString(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLess, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLess<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLessEq<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreater, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLess<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLessEq<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::strictEqual(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    Instance& instance = *globalObject->aotInstance();
    if (base.isCell()) {
        auto& known = instance.customGetterFor(base.asCell()->structureID().bits(), ident.impl());
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
        noteCustomGetter(globalObject, instance, asObject(base), ident, slot);
    if (slot.isUnset() && structureBefore && structureBefore->knownShape())
        caller(globalObject, callFrame).instance->lookAtObjectPrototype();
    cacheGetById(globalObject, callerData(globalObject, callFrame), base, structureBefore, ident, slot, cache, true);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t flagBits))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;

    PutPropertySlot slot(base, isStrict, putByIdContextOf(globalObject, callFrame));
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (isDirect && oldStructure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) [[unlikely]] {
        // (A class field that is declared without an initializer is undefined until the constructor assigns it. Its slot stays
        // empty until then.)
        if (!asObject(base)->putDirect(vm, ident, value, slot) && !value.isUndefined()) [[unlikely]]
            throwTypeError(globalObject, scope, TypedFieldError);
    } else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    // (A direct put that adds a property has the same effect as an ordinary put, provided that nothing on the prototype chain
    // intercepts the store.)
    if (!isDirect || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    cachePutById(globalObject, callerData(globalObject, callFrame), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

// For Graph::readsElementsOrEmpty. index: a non-negative integer.
JSC_DEFINE_JIT_OPERATION(operationAOTGetElementOrEmpty, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedArray, EncodedJSValue encodedIndex))
{
    AOT_OPERATION_PROLOGUE(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);

    if (base.isObject() && property.isString()) [[likely]] {
        // A string that has not been atomized cannot be the name of an ordinary property. One that has been atomized is added to
        // the megamorphic cache, so that the next lookup is faster (Lowering::lowerGetByVal()).
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
            // An index past the end of an ordinary array, or at a hole, when the prototype chain has no indexed properties.
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

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_PROLOGUE(globalObject);
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

// Resolves a name that no enclosing function or module declares. The result is a scope object. It is cached when it cannot change:
// for an import, or for a global as long as no global lexical binding shadows it.
// cache->pointer: the scope. cache->offset: the global lexical binding epoch at the time of the lookup, plus one.
JSC_DEFINE_JIT_OPERATION(operationAOTResolveScope, JSObject*, (JSGlobalObject* globalObject, JSScope* startScope, uint32_t identifierIndex, Slot* cache, uint32_t localScopeDepth))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    UNUSED_VARIABLE(uid);
    // The compiler has seen the declarations of the enclosing code, and this name is not among them. Skipping those scopes also
    // means that they do not have to keep the names of their variables.
    if (localScopeDepth == Site::resolvesInGlobalScopes)
        startScope = globalObject->globalLexicalEnvironment();
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
    else if (resolved->type() == LexicalEnvironmentType) {
        // Only if the name is declared there. A variable that sloppy eval adds to a scope is not in the symbol table, and only
        // exists once eval has run.
        cacheable = !uncheckedDowncast<JSLexicalEnvironment>(resolved)->symbolTable()->get(uid).isNull();
    }

    // A `with` scope, or a scope that sloppy eval can add variables to, between the start and the result makes the result valid for
    // this lookup only.
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
    if (cacheable && resolved->type() == LexicalEnvironmentType) {
        // The scope of an enclosing function. It is a different object on every call, but its depth is fixed by how the code nests.
        cache->pointer = nullptr;
        cache->offset = Slot::resolvesByDepth | depth;
    } else if (uint32_t epochPlusOne = globalObject->globalLexicalBindingEpoch() + 1; cacheable && !(epochPlusOne & Slot::resolvesByDepth)) {
        cache->offset = 0;
        cache->pointer = resolved;
        cache->offset = epochPlusOne;
    }
    OPERATION_RETURN(scope, resolved);
}

// For op_get_from_scope and op_put_to_scope: the variable is at `offset` in every environment with this environment's symbol table.
static void cacheVariableOfEnvironment(VM& vm, Data* codeBlock, Slot* cache, JSLexicalEnvironment* environment, ScopeOffset offset)
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

// cache->structureID: the structure that the scope must have for the cache to apply.
// cache->pointer: the address of the variable, if that address is stable. Otherwise see cacheVariableOfEnvironment().
// cache->offset: without a pointer, the offset of the property in the global object.
JSC_DEFINE_JIT_OPERATION(operationAOTGetFromScope, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* scopeObject, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    bool throwIfNotFound = how & Site::throwsIfNotFound;
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();

    // For a scope that is unique at this site: the global object, its lexical environment, or the module's environment. The
    // structure is still checked, because the name may resolve to another kind of scope next time.
    auto cacheAddress = [&](void* address) {
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::pointerIsNotCell;
        cache->pointer = address;
        WTF::storeStoreFence();
        cache->structureID = scopeObject->structureID();
        didFillSlot(vm, callerData(globalObject, callFrame));
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
            // An empty value is either a binding in its temporal dead zone, which the op_check_tdz that follows reports, or a
            // function declaration that has not been instantiated yet.
            JSValue result = JSModuleEnvironment::readLazyClosureVar(vm, environment, offset);
            if (result)
                cacheAddress(environment->variableAt(offset).slot());
            OPERATION_RETURN(scope, JSValue::encode(result));
        }

        // An import: the variable belongs to the exporting module.
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
        // Every scope created at the same place in the code has the same symbol table.
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scopeObject);
        auto entry = environment->symbolTable()->get(uid);
        if (!entry.isNull()) {
            cacheVariableOfEnvironment(vm, callerData(globalObject, callFrame), cache, environment, entry.scopeOffset());
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
            // A `var` or function declared by a program is a variable of the global object, not a property in its storage.
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
                didFillSlot(vm, callerData(globalObject, callFrame));
            }
        }
        return slot.getValue(globalObject, ident);
    })));
}

// Returns the environment that holds an import whose location the compiler knows.
JSC_DEFINE_JIT_OPERATION(operationAOTFillImportSlot, JSObject*, (JSGlobalObject* globalObject, JSObject* importer, uint32_t slot))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    auto* environment = uncheckedDowncast<JSModuleEnvironment>(importer);
    OPERATION_RETURN(scope, uncheckedDowncast<JSModuleRecord>(environment->moduleRecord())->fillImportSlot(globalObject, slot));
}

JSC_DEFINE_JIT_OPERATION(operationAOTReadLazyClosureVar, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* scopeObject, uint32_t offset))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSModuleEnvironment::readLazyClosureVar(vm, scopeObject, ScopeOffset(offset))));
}

// closureOffsetPlusOne: nonzero if the compiler knows that the variable is at that offset, minus one, in the environment.
// cache->offset: 1 once the variable's watchpoint set has been looked up. cache->pointer: the set, if there is one.
// how: the resolve mode, then the initialization mode (two bits), then whether the code is strict.
JSC_DEFINE_JIT_OPERATION(operationAOTPutToScope, void, (JSGlobalObject* globalObject, JSObject* scopeObject, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    Slot unusedSlot { };
    if (SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
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
            // From now on compiled code stores to the variable without firing the watchpoint, so invalidate it now.
            if (set)
                set->invalidate(vm, StringFireDetail("Executed op_put_to_scope in AOT code"));
            cacheVariableOfEnvironment(vm, callerData(globalObject, callFrame), cache, environment, offset);
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

JSC_DEFINE_JIT_OPERATION(operationAOTThrow, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    throwException(globalObject, scope, JSValue::decode(encodedValue));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckType, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t mask))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    unsigned tag = soundTypeTag(JSValue::decode(encodedValue));
    if (soundTypeMaskAccepts(mask, JSValue::decode(encodedValue)))
        OPERATION_RETURN(scope);
    throwTypeError(globalObject, scope, makeString("Type check failed: expected "_s, toString(SoundTypeMaskDump(mask)), ", got "_s, toString(SoundTypeMaskDump(tag))));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetLengthSlow, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedBase).get(globalObject, vm.propertyNames->length)));
}

// Returns normally only if the value has the typed layout, after conversion if necessary.
JSC_DEFINE_JIT_OPERATION(operationAOTCheckTypedLayout, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) {
        // Throw the error that accessing a property of it would throw.
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

// The stand-in for a value whose type has a typed layout but which does not have that layout itself. All of its slots are empty,
// and it is never written to.
alignas(16) static const EncodedJSValue s_emptyTypedObject[2 + 256] = { };

// Returns the object that typed code should read the value's fields from: the value itself if it has the typed layout, after
// conversion if necessary.
JSC_DEFINE_JIT_OPERATION(operationAOTCoerceToTypedLayout, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t layoutID))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isObject() && (asObject(value)->structure()->typedLayoutID() == layoutID || Instance::convertToTypedLayout(vm, asObject(value), safeCast<uint16_t>(layoutID))))
        OPERATION_RETURN(scope, encodedValue);
    if (TypedLayoutTable::isAuditing() && !value.isUndefinedOrNull()) [[unlikely]]
        TypedLayoutTable::reportViolation(value.isObject() ? TypedLayoutTable::s_lastConversionFailure : "it is no object"_s, safeCast<uint16_t>(layoutID), value);
    OPERATION_RETURN(scope, static_cast<EncodedJSValue>(std::bit_cast<uintptr_t>(&s_emptyTypedObject[0])));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFieldSlow, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint64_t which))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    uint16_t layoutID = static_cast<uint16_t>(which >> 32);
    unsigned slot = which >> 48 & 0xff;
    bool allowsUndefined = which >> 56 & 1;
    JSValue base = JSValue::decode(encodedBase);
    UniquedStringImpl* uid = StaticHeap::identifiersOfProgram()[static_cast<uint32_t>(which)];
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
        throwTypeError(globalObject, scope, "Type check failed: the value of a property does not match its declared type"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(TypedLayoutTable::toFieldRepresentation(layoutID, slot, value)));
}

// Called when the Structure of the base does not say that the field is in its slot.
JSC_DEFINE_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t which))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    unsigned slot = which >> 16 & 0xff;
    bool allowsUndefined = which >> 24 & 1;
    const TypedLayoutTable::Field& field = TypedLayoutTable::fieldWithID(slot, static_cast<uint16_t>(which));
    uint16_t layoutID = TypedLayoutTable::layoutIDOf(field);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isObject()) {
        JSObject* object = asObject(base);
        // An object created by untyped code is converted to the typed layout, if possible.
        if (!object->structure()->cannotConvertToTypedLayout() && !object->structure()->typedLayoutID())
            Instance::convertToTypedLayout(vm, object, layoutID);
        Structure* structure = object->structure();
        if (structure->typedLayoutID() == layoutID) {
            uint16_t there = structure->fieldIDInSlot(slot);
            if (there == field.id) {
                if (JSValue value = object->getDirect(static_cast<PropertyOffset>(slot)))
                    OPERATION_RETURN(scope, JSValue::encode(value));
            } else if (there != Structure::ambiguousFieldID && allowsUndefined) // (As in the stub: see generateReadSlot().)
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    UniquedStringImpl* uid = StaticHeap::identifiersOfProgram()[field.identifier];
    JSValue value;
    MegamorphicCache* cache = vm.megamorphicCache();
    if (auto* known = cache && base.isObject() ? cache->findLoad(asObject(base)->structureID(), uid) : nullptr) {
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
        PropertySlot propertySlot(base, PropertySlot::InternalMethodType::Get);
        value = getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, uid), propertySlot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        // (An entry in the megamorphic cache is always a plain property or the absence of one.)
        if (!base.isObject() || (propertySlot.isUnset() ? propertySlot.isTaintedByOpaqueObject() : !propertySlot.isCacheableValue()))
            globalObject->aotInstance()->noteObservableRead(slot, field.id);
    }
    if (value.isUndefined() && allowsUndefined)
        OPERATION_RETURN(scope, JSValue::encode(value));
    // (Later code assumes that the result has the field's type, so this either returns such a value or throws.)
    if (TypedLayoutTable::checkStore(field, value) == TypedLayoutTable::StoreCheck::Rejected) {
        throwTypeError(globalObject, scope, "Type check failed: the value of a property does not match its declared type"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(TypedLayoutTable::toFieldRepresentation(field, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValidateTypedObject, void, (JSGlobalObject* globalObject, JSObject* object))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    uint16_t layoutID = object->structure()->typedLayoutID();
    if (TypedLayoutTable::usesFieldIDs(layoutID)) {
        bool isRejected = false;
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (auto* field = TypedLayoutTable::findField(layoutID, entry.key()))
                isRejected = TypedLayoutTable::checkStore(*field, object->getDirect(entry.offset())) == TypedLayoutTable::StoreCheck::Rejected;
            return !isRejected;
        });
        if (isRejected)
            throwTypeError(globalObject, scope, TypedFieldError);
        OPERATION_RETURN(scope);
    }
    for (unsigned slot = 0; slot < TypedLayoutTable::numberOfSlots(layoutID); ++slot) {
        JSValue value = object->getDirect(TypedLayoutTable::offsetInLayout(layoutID, slot));
        if (value && TypedLayoutTable::checkStore(layoutID, slot, value) == TypedLayoutTable::StoreCheck::Rejected) {
            throwTypeError(globalObject, scope, TypedFieldError);
            OPERATION_RETURN(scope);
        }
    }
    OPERATION_RETURN(scope);
}

// For Options::validateAOTInferredTypes().
// Validation must not change what the program does: this may be called while an exception is propagating.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTVerifyInferredType, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint64_t lowHalfOfType, uint64_t highHalfOfType, uint32_t which, uint32_t identifierIndexPlusOne, uint64_t scopeWhenCompiled, uint32_t scopeOffset))
{
    Type type = static_cast<Type>(highHalfOfType) << 64 | lowHalfOfType;
    Type actual = typeOfValue(JSValue::decode(encodedValue));
    // Refine the type with the object's typed layout.
    if (actual & TFinalObjectTag)
        actual = (actual & ~TFinalObject) | typeOfObjectWithLayout(JSValue::decode(encodedValue).asCell()->structure()->typedLayoutID());
    // Refine the type with the function's number, if it is one of the program's functions.
    if (actual & TFunctionTag) {
        if (auto* function = dynamicDowncast<JSFunction>(JSValue::decode(encodedValue).asCell()); function && !function->isHostFunction()) {
            Image* image = Image::withCode();
            uint32_t index = function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall);
            if (image && function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall) && index < image->header().numberOfFunctions) {
                if (uint32_t number = image->at<uint32_t>(image->header().numbersOfFunctionsOffset)[index])
                    actual = (actual & ~TFunction) | typeOfFunction(number);
            } else if (!function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall)) {
                // A function without code: all of its calls were inlined. It cannot be identified, so assume that it matches.
                actual = (actual & ~TAnyFunctionNumber) | (type & TAnyFunctionNumber);
            }
        }
    }
    // (Zero is what a stub interprets as "no exception".)
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
        dataLog(" of `", identifierAt(globalObject, callFrame, identifierIndexPlusOne - 1).impl(), "`");
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
        // A cell that has been freed or is not initialized yet.
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
            auto* ofStructure = std::bit_cast<const uint64_t*>(structure);
            dataLog("    its structure ", RawPointer(structure), ":");
            for (unsigned i = 0; i < 6; ++i)
                dataLog(" ", RawHex(ofStructure[i]));
            dataLogLn("; blob ", RawHex(structure->typeInfoBlob()), ", type ", static_cast<unsigned>(structure->typeInfo().type()), ", born as ", structure->typedLayoutID(), ", inline capacity ", structure->inlineCapacity(), StaticHeap::contains(structure) ? " (static)" : "",
                !StaticHeap::contains(structure) ? (structure->markedBlock().handle().isLive(structure) ? ", live" : ", NOT LIVE") : "");
        }
        if (!cell->isPreciseAllocation() && !StaticHeap::contains(cell)) {
            MarkedBlock& block = cell->markedBlock();
            dataLogLn("    in a block of cells of ", block.handle().cellSize(), " bytes; marked: ", block.isMarked(cell), ", newly allocated: ", block.isNewlyAllocated(cell), ", live: ", block.handle().isLive(cell), ", free-listed: ", block.handle().isFreeListed());
        }
    }
    // Print the stack trace, formatted as for an Error.
    JSObject* error = createError(globalObject, "the stack:"_s);
    JSValue stack = error->get(globalObject, vm.propertyNames->stack);
    if (!scope.exception() && stack.isString())
        dataLogLn(asString(stack)->value(globalObject).data);
    // (Exit instead of crashing: a crash reporter may take a long time, and the caller is a test.)
    _exit(70);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHandleTraps, void, (JSGlobalObject* globalObject))
{
    AOT_OPERATION_PROLOGUE(globalObject);
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

// Returns null if the exception cannot be caught by JavaScript code.
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

JSC_DEFINE_JIT_OPERATION(operationAOTFindEqualAtom, StringImpl*, (JSGlobalObject* globalObject, JSString* string))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    auto atom = string->toExistingAtomString(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr); // Out of memory resolving a rope.
    StringImpl* impl = atom.data;
    OPERATION_RETURN(scope, impl && impl->is8Bit() ? impl : nullptr);
}

// The jump offset, relative to the switch; 0 for the default.
JSC_DEFINE_JIT_OPERATION(operationAOTSwitchString, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t tableIndex, uint32_t whose))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        OPERATION_RETURN(scope, 0);
    auto string = asString(value)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0); // Out of memory resolving a rope.
    const UnlinkedStringJumpTable& table = bytecodeOwnerOfCaller(globalObject, callFrame, whose).stringSwitchJumpTable(tableIndex);
    OPERATION_RETURN(scope, table.offsetForValue(string.data.impl()));
}

// The character of a one character string, or -1.
JSC_DEFINE_JIT_OPERATION(operationAOTSwitchChar, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue))
{
    AOT_OPERATION_PROLOGUE(globalObject);
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

#endif // ENABLE(FTL_JIT)
