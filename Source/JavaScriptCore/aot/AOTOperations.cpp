/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTLockedOperation.h"
#include "AOTOperationHelpers.h"
#include "AOTTypedObject.h"
#include "AOTTypedOperations.h"
#include "ArrayPrototype.h"
#include "CodeBlock.h"
#include "CommonSlowPaths.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GetPutInfo.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "MathCommon.h"
#include "MegamorphicCache.h"
#include "NumberPrototype.h"
#include "ObjectPrototype.h"
#include "ProxyObject.h"
#include "PutByIdFlags.h"
#include "StructureChain.h"
#include "VMTrapsInlines.h"

namespace JSC { namespace AOT {

enum class NodeKind : uint8_t {
    Bytecode,
    Constant,
    ConstantCell,
    Intrinsic,
    LinkTimeConstant,
    Argument,
    Phi,
    Proj,
    GetStack,
    SetStack,
    Guard,
    Narrow,
};

#define AOT_BINARY_OPERATION(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)))); \
    }

#define AOT_BINARY_OPERATION_ON_NUMERICS(name, function, functionOnBigInts) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::functionOnBigInts(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

JSC_DEFINE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (left.isHeapBigInt() && right.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::add(globalObject, left.asHeapBigInt(), right.asHeapBigInt())));
    if (left.isString() && right.isInt32())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, asString(left), int32ToString(vm, right.asInt32(), 10))));
    if (left.isInt32() && right.isString())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, int32ToString(vm, left.asInt32(), 10), asString(right))));
    OPERATION_RETURN(scope, JSValue::encode(jsAdd(globalObject, left, right)));
}

AOT_BINARY_OPERATION_ON_NUMERICS(operationAOTValueSub, jsSub, sub)
AOT_BINARY_OPERATION_ON_NUMERICS(operationAOTValueMul, jsMul, multiply)
AOT_BINARY_OPERATION_ON_NUMERICS(operationAOTValueDiv, jsDiv, divide)
AOT_BINARY_OPERATION_ON_NUMERICS(operationAOTValueMod, jsRemainder, remainder)
AOT_BINARY_OPERATION(operationAOTValuePow, jsPow)
AOT_BINARY_OPERATION(operationAOTValueBitAnd, jsBitwiseAnd)
AOT_BINARY_OPERATION(operationAOTValueBitOr, jsBitwiseOr)
AOT_BINARY_OPERATION(operationAOTValueBitXor, jsBitwiseXor)
AOT_BINARY_OPERATION(operationAOTValueLShift, jsLShift)
AOT_BINARY_OPERATION(operationAOTValueRShift, jsRShift)
AOT_BINARY_OPERATION(operationAOTValueURShift, jsURShift)

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

static ALWAYS_INLINE std::optional<JSBigInt::ComparisonResult> orderOfBigInts(EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (!left.isHeapBigInt() || !right.isHeapBigInt())
        return std::nullopt;
    return JSBigInt::compare(left.asHeapBigInt(), right.asHeapBigInt());
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLess, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    if (auto order = orderOfBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *order == JSBigInt::ComparisonResult::LessThan);
    OPERATION_RETURN(scope, jsLess<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    if (auto order = orderOfBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *order != JSBigInt::ComparisonResult::GreaterThan);
    OPERATION_RETURN(scope, jsLessEq<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreater, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    if (auto order = orderOfBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *order == JSBigInt::ComparisonResult::GreaterThan);
    OPERATION_RETURN(scope, jsLess<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    if (auto order = orderOfBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *order != JSBigInt::ComparisonResult::LessThan);
    OPERATION_RETURN(scope, jsLessEq<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEqToString, size_t, (Instance* instance, EncodedJSValue encodedLeft, uint64_t text))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), instance->typed->createStringConstant(vm, static_cast<uint32_t>(text))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::strictEqual(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

static ALWAYS_INLINE const Instance::CustomGetter* findKnownCustomGetter(VM& vm, Instance* instance, JSCell* base, UniquedStringImpl* uid)
{
    auto* known = instance->customGetterFor(base->structureID().bits(), uid);
    if (!known || known->uid != uid || known->structureID != base->structureID().bits() || !vm.megamorphicCache() || known->epoch != vm.megamorphicCache()->epoch())
        return nullptr;
    return known;
}

static ALWAYS_INLINE EncodedJSValue callCustomGetter(const Instance::CustomGetter& known, EncodedJSValue base, const Identifier& ident)
{
    auto getter = GetValueFunc(std::bit_cast<GetValueFunc::Ptr>(known.getter));
    return getter(known.globalObject, known.passedForReceiver ? JSValue::encode(known.passedForReceiver) : base, ident);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    if (Typed::State* typed = instance->typed) {
        if (typed->isCounting()) [[unlikely]]
            typed->countAccess(Typed::State::AccessKind::Read, removeCodePtrTag(callFrame->rawReturnPC()), ident, base);
        if (base.isCell() && base.asCell()->type() == ProxyObjectType) {
            countOperationNamed(instance, __func__, "proxy");
            OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident)));
        }
        if (JSValue value = base.isCell() ? Typed::Object::tryGetOwnData(vm, base.asCell(), ident) : JSValue()) {
            constexpr uint64_t isNotField = static_cast<uint64_t>(Slot::FieldRead::NoField) << Slot::fieldReadShift;
            if (cache->structureID == base.asCell()->structureID() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags && std::bit_cast<uint64_t>(cache->pointer) == isNotField)
                OPERATION_RETURN(scope, JSValue::encode(value));
            cacheNamedFieldRead(instance, base.asCell(), ident, identifierIndex);
            if (!SharedData::contains(cache))
                cacheCompiledField(vm, dataWithSlot(instance, callFrame, cache), cache, base.asCell(), ident, true);
            OPERATION_RETURN(scope, JSValue::encode(value));
        }
        if (base.isUndefinedOrNull()) [[unlikely]] {
            throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), false));
            OPERATION_RETURN(scope, encodedJSValue());
        }
    }
    recordNamedAccess(instance, __func__, base, { }, ident.impl());
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        countOperationNamed(instance, __func__, !base.isObject() ? "base-is-not-object" : vm.megamorphicCache() && vm.megamorphicCache()->findLoad(base.asCell()->structureID(), ident.impl()) ? "value-is-in-megamorphic-cache" : "value-is-not-in-megamorphic-cache");
        if (vm.megamorphicCache())
            runtimeTable(vm).countMegamorphicCacheEpochChange(vm.megamorphicCache()->epoch());
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result;
    if (!tryGetFromOrdinaryObject(globalObject, base, ident.impl(), slot, result)) {
        if (base.isCell()) {
            if (auto* known = findKnownCustomGetter(vm, instance, base.asCell(), ident.impl())) {
                countOperationNamed(instance, __func__, "known-custom-getter");
                if (!SharedData::contains(cache))
                    cacheKnownCustomGetter(vm, *instance, dataWithSlot(instance, callFrame, cache), cache, base.asCell()->structure(), ident, *known);
                OPERATION_RETURN(scope, callCustomGetter(*known, encodedBase, ident));
            }
        }
        if (instance->typed)
            instance->typed->didGetDeclaredMember = false;
        result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (instance->typed && std::exchange(instance->typed->didGetDeclaredMember, false))
            OPERATION_RETURN(scope, JSValue::encode(result));
        if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
            cacheCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!SharedData::contains(cache))
        cacheGetById(globalObject, dataWithSlot(instance, callFrame, cache), base, structureBefore, ident, slot, cache, true);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static ALWAYS_INLINE JSValue valueInHolder(JSObject* holder, PropertyOffset offset)
{
    return isInlineOffset(offset) ? holder->inlineStorageUnsafe()[offsetInInlineStorage(offset)].get() : holder->getDirect(offset);
}

static ALWAYS_INLINE void countMissAvoidedByLookupTable(Instance* instance, CallFrame* callFrame)
{
    countOperationFor(instance, callFrame);
}

static const Identifier& proxyGetIdentifier(Instance* instance, CallFrame* callFrame, uint32_t name)
{
    return identifierAt(instance, callFrame, name & Instance::nameIsIdentifierIndex ? name & ~Instance::nameIsIdentifierIndex : PropertyLookupTable::identifierIndexForName(name));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareProxyGet, uintptr_t, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue* results))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto decline = [&](const char* why) -> uintptr_t {
        countOperationNamed(instance, "read of a proxy", why);
        return 0;
    };
    auto* proxy = uncheckedDowncast<ProxyObject>(cell);
    JSValue handlerValue = proxy->handler();
    if (!handlerValue.isObject())
        return decline("it is revoked");
    JSObject* handler = asObject(handlerValue);
    if (handler->type() != FinalObjectType)
        return decline("the handler is no plain object");
    unsigned attributes;
    PropertyOffset offset = handler->structure()->get(vm, vm.propertyNames->get, attributes);
    if (isValidOffset(offset) && attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return decline("the handler's `get` is no value");
    JSValue trap;
    if (isValidOffset(offset))
        trap = handler->getDirect(offset);
    else {
        auto scope = DECLARE_THROW_SCOPE(vm);
        PropertySlot slot(handler, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = handler->getPropertySlot(instance->globalObject, vm.propertyNames->get, slot);
        scope.assertNoException();
        if (!isFound || slot.isTaintedByOpaqueObject() || !slot.isValue())
            return decline("the handler inherits no `get` that is a value");
        trap = slot.getValue(instance->globalObject, vm.propertyNames->get);
    }
    if (!trap.isCallable())
        return decline("the trap is not callable");
    const Identifier& ident = proxyGetIdentifier(instance, callFrame, name);
    if (ident.isSymbol())
        return decline("the name is no text");
    countOperationNamed(instance, "read of a proxy", "the trap is called from the stub");
    results[0] = JSValue::encode(trap);
    results[1] = JSValue::encode(handler);
    results[2] = JSValue::encode(proxy->target());
    results[3] = JSValue::encode(instance->proxyTrapKey(vm, ident.impl()));
    return 1;
}

JSC_DEFINE_JIT_OPERATION(operationAOTValidateProxyGet, EncodedJSValue, (Instance* instance, JSCell* target, uint32_t name, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& ident = proxyGetIdentifier(instance, callFrame, name);
    Structure* structure = target->structure();
    bool hasNoLazyPropertyNamed = !structure->typeInfo().overridesGetOwnPropertySlot() || Typed::hasNoLazyPropertyNamed(vm, target, ident.impl(), true);
    if (hasNoLazyPropertyNamed && !structure->hasNonReifiedStaticProperties() && !parseIndex(ident)) {
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, ident, attributes);
        if (!isValidOffset(offset)) {
            countOperationNamed(instance, "read of a proxy", "the target lacks the property");
            OPERATION_RETURN(scope, answer);
        }
        bool isFixedValue = attributes & PropertyAttribute::DontDelete && attributes & PropertyAttribute::ReadOnly;
        if (!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && (!isFixedValue || asObject(target)->getDirect(offset) == JSValue::decode(answer))) {
            countOperationNamed(instance, "read of a proxy", "the target's Structure allows the answer");
            OPERATION_RETURN(scope, answer);
        }
    }
    ProxyObject::validateGetTrapResult(globalObject, JSValue::decode(answer), asObject(target), ident);
    OPERATION_RETURN(scope, answer);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByNameAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance);
    using Access = PropertyLookupTable::Access;
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, PropertyLookupTable::identifierIndexForName(name));
    recordLookup(instance, __func__, base, { }, ident.impl());
    if (Structure* structure = base.isCell() && base.asCell()->type() == FinalObjectType ? base.asCell()->structure() : nullptr; structure && structure->isUncacheableDictionary() && structure->hasBeenFlattenedBefore() && !structure->compiledLayout()) {
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, ident.impl(), attributes);
        if (isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
            countOperationNamed(instance, __func__, "permanent-dictionary-own-hit");
            OPERATION_RETURN(scope, JSValue::encode(asObject(base)->getDirect(offset)));
        }
    }
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    Data* data = table.dataFor(instance);
    if ((base.isString() || base.isSymbol() ? table.isSharedByInstances() : base.isNumber() || base.isBoolean()) && ident != vm.propertyNames->length) {
        JSObject*& prototype = base.isString() ? instance->stringPrototype : base.isSymbol() ? instance->symbolPrototype : base.isNumber() ? instance->numberPrototype : instance->booleanPrototype;
        if (!prototype)
            prototype = base.isString() ? globalObject->stringPrototype() : base.isSymbol() ? globalObject->symbolPrototype() : base.isNumber() ? globalObject->numberPrototype() : globalObject->booleanPrototype();
        PropertySlot inquiry(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = prototype->getPropertySlot(globalObject, ident, inquiry);
        inquiry.disallowVMEntry.reset();
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!inquiry.isTaintedByOpaqueObject() && (!isFound || inquiry.isValue())) {
            base = prototype;
            encodedBase = JSValue::encode(base);
        }
    }
    if (instance->typed && base.isObject() && base.asCell()->type() == FinalObjectType) {
        Structure* heirStructure = base.asCell()->structure();
        bool mayDelegate = !heirStructure->compiledLayout() && !heirStructure->isDictionary() && heirStructure->hasMonoProto() && heirStructure->propertyAccessesAreCacheable() && !heirStructure->needImpurePropertyWatchpoint();
        JSObject* prototype = mayDelegate ? heirStructure->storedPrototypeObject() : nullptr;
        const Typed::LayoutData* layout = prototype ? prototype->structure()->compiledLayout() : nullptr;
        if (layout && Typed::isObjectLayout(layout->kind) && !isValidOffset(heirStructure->get(vm, ident.impl()))) {
            PropertySlot inquiry(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
            bool isFound = prototype->getPropertySlot(globalObject, ident, inquiry);
            inquiry.disallowVMEntry.reset();
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!inquiry.isTaintedByOpaqueObject() && (!isFound || inquiry.isValue()) && table.setDelegatesToPrototype(heirStructure, name, prototype)) {
                base = prototype;
                encodedBase = JSValue::encode(base);
            }
        }
    }
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    if (structureBefore && structureBefore->typeInfo().type() == ProxyObjectType) {
        countOperationNamed(instance, __func__, "proxy");
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident)));
    }
    bool remainsDictionary = structureBefore && base.isObject() && structureBefore->isUncacheableDictionary() && structureBefore->hasBeenFlattenedBefore() && !structureBefore->compiledLayout();
    bool receiverIsMissingProperty = false;
    if (uint32_t descriptorPlusOne = base.isObject() ? structureBefore->structureDescriptorPlusOne() : 0) {
        uint32_t identifier = PropertyLookupTable::identifierIndexForName(name);
        uint32_t displacement = instance->nameDisplacements[identifier];
        uint32_t nameAndOffset = instance->offsetsByNameAndStructure[(displacement & ~Typed::Data::nameIsOnObjectPrototype) + descriptorPlusOne];
        if (nameAndOffset >> 8 == identifier) {
            PropertyOffset offset = nameAndOffset & 0xff;
            if (JSValue value = asObject(base)->getDirect(offset); value && table.setOwnOffset(structureBefore, name, offset, false)) {
                countMissAvoidedByLookupTable(instance, callFrame);
                OPERATION_RETURN(scope, JSValue::encode(value));
            }
        } else if (descriptorPlusOne < instance->firstThisStructureDescriptorPlusOne && !(displacement & Typed::Data::nameIsOnObjectPrototype) && table.setAbsent(structureBefore, name, nullptr)) {
            countMissAvoidedByLookupTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        } else
            receiverIsMissingProperty = true;
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    JSValue result;
    bool isOrdinaryResult = !receiverIsMissingProperty && tryGetFromOrdinaryObject(globalObject, base, ident.impl(), slot, result, false, &receiverIsMissingProperty);
    if (isOrdinaryResult && (slot.isCacheableValue() ? table.setOwnOffset(structureBefore, name, slot.cachedOffset(), false) : table.setAbsent(structureBefore, name, nullptr))) {
        countOperationNamed(instance, __func__, "ordinary-object-fast-path");
        countMissAvoidedByLookupTable(instance, callFrame);
        OPERATION_RETURN(scope, JSValue::encode(result));
    }
    if (auto* entry = structureBefore && vm.megamorphicCache() && (base.isObject() || !table.isSharedByInstances()) ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr) {
        UniquedStringImpl* prototypeDependentUID = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : ident.impl();
        PropertyOffset offset = entry->m_offset;
        if (!entry->m_holder) {
            if (table.setAbsent(structureBefore, name, prototypeDependentUID))
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        } else if (entry->m_holder == JSCell::seenMultipleCalleeObjects()) {
            if (table.setOwnOffset(structureBefore, name, offset, false))
                OPERATION_RETURN(scope, JSValue::encode(asObject(base)->getDirect(offset)));
        } else if (JSObject* holder = asObject(entry->m_holder); table.setInherited(structureBefore, name, holder, offset, prototypeDependentUID))
            OPERATION_RETURN(scope, JSValue::encode(valueInHolder(holder, offset)));
    }
    if (instance->typed) {
        if (structureBefore && structureBefore->compiledLayout() && structureBefore->markingStampAndFlags() & Structure::isStructureWithExtraProperties && !structureBefore->extraPropertiesStructureID()) {
            uncheckedDowncast<Typed::Object>(base.asCell())->specializeStructureForExtraProperties(vm);
            structureBefore = base.asCell()->structure();
        }
        if (unsigned attributes = 0; auto offset = structureBefore && structureBefore->compiledLayout() && structureBefore->extraPropertiesStructureID() ? Typed::Object::offsetOfExtraProperty(vm, base.asCell(), ident, attributes) : std::nullopt) {
            if (!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && table.setExtraProperty(structureBefore, name, *offset, false, false))
                OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()->getDirect(*offset)));
        }
        if (JSValue value = base.isCell() ? Typed::Object::tryGetOwnData(vm, base.asCell(), ident) : JSValue()) {
            constexpr uint64_t isNotField = static_cast<uint64_t>(Slot::FieldRead::NoField) << Slot::fieldReadShift;
            Slot* cache = table.ensureSlot(structureBefore, name, Access::Read);
            if (cache && !(cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags && std::bit_cast<uint64_t>(cache->pointer) == isNotField))
                cacheCompiledField(vm, data, cache, base.asCell(), ident, false);
            if (cache && cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags)
                table.setField(structureBefore, name, std::bit_cast<uint64_t>(cache->pointer));
            OPERATION_RETURN(scope, JSValue::encode(value));
        }
        if (base.isUndefinedOrNull()) [[unlikely]] {
            throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), false));
            OPERATION_RETURN(scope, encodedJSValue());
        }
    }
    if (instance->typed && base.isObject() && base.asCell()->classInfo() == Typed::ModuleNamespace::info()) {
        auto* moduleNamespace = uncheckedDowncast<Typed::ModuleNamespace>(base.asCell());
        unsigned number = moduleNamespace->numberOf(ident.impl());
        if (number != Typed::ModuleNamespace::noExport && moduleNamespace->canReadByIndexDirectly() && !table.isSharedByInstances()) {
            table.setNamespaceExport(structureBefore, name, number);
            OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident)));
        }
        if (number == Typed::ModuleNamespace::noExport && moduleNamespace->canReadByIndexDirectly() && !ident.isSymbol() && ident != vm.propertyNames->__esModule && !moduleNamespace->isStarExport(ident.impl())) {
            JSValue prototype = structureBefore->storedPrototype();
            bool chainIsMissingProperty = prototype.isNull();
            if (!chainIsMissingProperty) {
                PropertySlot inquiry(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
                chainIsMissingProperty = !asObject(prototype)->getPropertySlot(globalObject, ident, inquiry) && !inquiry.isTaintedByOpaqueObject();
                inquiry.disallowVMEntry.reset();
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
            if (chainIsMissingProperty && table.setAbsent(structureBefore, name, prototype.isNull() ? nullptr : ident.impl()))
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    if (instance->typed && base.isObject() && base.asCell()->type() == FinalObjectType) {
        PropertyOffset offset = invalidOffset;
        bool isCreatedByStubs = false;
        if (JSObject* closure = instance->typed->reifyLazyClosureIfPresent(vm, asObject(base), ident.impl(), offset, isCreatedByStubs)) {
            Structure* after = base.asCell()->structure();
            if (isCreatedByStubs && !table.isSharedByInstances() && storageAfterAddition(structureBefore, after, offset) == StorageAfterAddition::Stays)
                table.setLazyClosure(structureBefore, name, offset, after);
            OPERATION_RETURN(scope, JSValue::encode(closure));
        }
    }
    if (remainsDictionary) {
        countOperationNamed(instance, __func__, "permanent-dictionary");
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident, slot)));
    }
    if (!isOrdinaryResult) {
        bool isLookedUp = false;
        auto findThroughLazyPrototypes = [&] {
            return instance->typed && base.isObject() ? instance->typed->findThroughLazyPrototypes(vm, asObject(base), ident.impl(), PropertyLookupTable::identifierIndexForName(name), receiverIsMissingProperty) : std::nullopt;
        };
        if (auto found = findThroughLazyPrototypes(); found && found->member) {
            auto& place = *found->member;
            JSValue member = place.holder->getDirect(place.offset);
            if (!place.isAccessor) {
                table.setInherited(structureBefore, name, place.holder, place.offset, ident.impl());
                OPERATION_RETURN(scope, JSValue::encode(member));
            }
            auto* accessor = uncheckedDowncast<GetterSetter>(member.asCell());
            cacheGetterShortcut(accessor);
            table.setAccessor(structureBefore, name, place.holder, place.offset, ident.impl());
            OPERATION_RETURN(scope, JSValue::encode(accessor->callGetter(globalObject, base)));
        } else if (found && !found->firstObjectAbove) {
            table.setAbsent(structureBefore, name, ident.impl());
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        } else if (found) {
            JSObject* above = found->firstObjectAbove;
            if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(above->structureID(), ident.impl()) : nullptr) {
                JSObject* holder = entry->m_holder == JSCell::seenMultipleCalleeObjects() ? above : entry->m_holder ? asObject(entry->m_holder) : nullptr;
                if (holder ? table.setInherited(structureBefore, name, holder, entry->m_offset, ident.impl()) : table.setAbsent(structureBefore, name, ident.impl())) {
                    countMissAvoidedByLookupTable(instance, callFrame);
                    OPERATION_RETURN(scope, JSValue::encode(holder ? valueInHolder(holder, entry->m_offset) : jsUndefined()));
                }
            }
            instance->typed->didGetDeclaredMember = false;
            result = getByIdAndFillMegamorphicCache(globalObject, above, ident, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            Structure* structureNow = base.asCell()->structure();
            bool getterChangedReceiver = structureNow != structureBefore && slot.isCacheableGetter() && structureNow->hasMonoProto() && structureNow->storedPrototype() == structureBefore->storedPrototype();
            auto foundNow = getterChangedReceiver ? instance->typed->findThroughLazyPrototypes(vm, asObject(base), ident.impl(), PropertyLookupTable::identifierIndexForName(name), true) : findThroughLazyPrototypes();
            if (std::exchange(instance->typed->didGetDeclaredMember, false) || (structureNow != structureBefore && !getterChangedReceiver) || !foundNow || foundNow->member || foundNow->firstObjectAbove != above || !vm.megamorphicCache())
                OPERATION_RETURN(scope, JSValue::encode(result));
            bool isInTable = false;
            if (auto* entry = getterChangedReceiver ? nullptr : vm.megamorphicCache()->findLoad(above->structureID(), ident.impl())) {
                if (!entry->m_holder)
                    isInTable = table.setAbsent(structureBefore, name, ident.impl());
                else
                    isInTable = table.setInherited(structureBefore, name, entry->m_holder == JSCell::seenMultipleCalleeObjects() ? above : entry->m_holder, entry->m_offset, ident.impl());
            } else if (auto* entry = vm.megamorphicCache()->findGetter(above->structureID(), ident.impl())) {
                if (slot.isCacheableGetter())
                    cacheGetterShortcut(slot.getterSetter());
                isInTable = table.setAccessor(structureBefore, name, entry->m_holder == JSCell::seenMultipleCalleeObjects() ? above : entry->m_holder, entry->m_offset, ident.impl());
            }
            ensureStillAliveHere(structureBefore);
            if (isInTable) {
                countMissAvoidedByLookupTable(instance, callFrame);
                OPERATION_RETURN(scope, JSValue::encode(result));
            }
            isLookedUp = true;
        }
        if (base.isCell() && !isLookedUp) {
            if (auto* known = findKnownCustomGetter(vm, instance, base.asCell(), ident.impl())) {
                if (Slot* cache = table.ensureSlot(structureBefore, name, Access::Read)) {
                    cacheKnownCustomGetter(vm, *instance, data, cache, structureBefore, ident, *known);
                    table.setKnownCustomGetter(structureBefore, name, known->offsetOfCachedValue, ident.impl());
                }
                OPERATION_RETURN(scope, callCustomGetter(*known, encodedBase, ident));
            }
        }
        if (!isLookedUp) {
            if (instance->typed)
                instance->typed->didGetDeclaredMember = false;
            result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (instance->typed && std::exchange(instance->typed->didGetDeclaredMember, false))
            OPERATION_RETURN(scope, JSValue::encode(result));
        if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
            cacheCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!structureBefore || (!base.isObject() && table.isSharedByInstances()))
        OPERATION_RETURN(scope, JSValue::encode(result));
    if (base.asCell()->structure() != structureBefore) {
        if (auto* entry = slot.isCacheableGetter() && vm.megamorphicCache() ? vm.megamorphicCache()->findGetter(structureBefore->id(), ident.impl()) : nullptr) {
            bool isOwn = entry->m_holder == JSCell::seenMultipleCalleeObjects();
            cacheGetterShortcut(slot.getterSetter());
            table.setAccessor(structureBefore, name, isOwn ? nullptr : entry->m_holder, entry->m_offset, isOwn ? nullptr : ident.impl());
        }
        ensureStillAliveHere(structureBefore);
        OPERATION_RETURN(scope, JSValue::encode(result));
    }
    if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr) {
        UniquedStringImpl* prototypeDependentUID = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : ident.impl();
        bool isInTable;
        if (!entry->m_holder)
            isInTable = table.setAbsent(structureBefore, name, prototypeDependentUID);
        else if (entry->m_holder == JSCell::seenMultipleCalleeObjects())
            isInTable = table.setOwnOffset(structureBefore, name, entry->m_offset, false);
        else
            isInTable = table.setInherited(structureBefore, name, entry->m_holder, entry->m_offset, prototypeDependentUID);
        if (isInTable) {
            countMissAvoidedByLookupTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    } else if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findGetter(structureBefore->id(), ident.impl()) : nullptr) {
        bool isOwn = entry->m_holder == JSCell::seenMultipleCalleeObjects();
        if (slot.isCacheableGetter())
            cacheGetterShortcut(slot.getterSetter());
        if (table.setAccessor(structureBefore, name, isOwn ? nullptr : entry->m_holder, entry->m_offset, isOwn ? nullptr : ident.impl())) {
            countMissAvoidedByLookupTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    }
    if (Slot* cache = table.ensureSlot(structureBefore, name, Access::Read)) {
        cacheGetById(globalObject, data, base, structureBefore, ident, slot, cache);
        bool isPermanentlyInherited = cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == (Slot::isIndirect | Slot::pointerIsCell) && slot.isCacheableValue() && cache->pointer == slot.slotBase() && slot.slotBase() != base.asCell() && !table.isWatched(cache);
        bool isOwn = cache->structureID == structureBefore->id() && !(cache->offset & (Slot::isGetter | Slot::pointerIsNotCell | Slot::pointerIsCell)) && slot.isCacheableValue() && slot.slotBase() == base.asCell() && !structureBefore->compiledLayout() && !table.isWatched(cache);
        auto* namespaceExport = slot.moduleNamespaceSlot() && vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr;
        if (namespaceExport && namespaceExport->m_epoch == MegamorphicCache::permanentEpoch && namespaceExport->m_holder && namespaceExport->m_holder != JSCell::seenMultipleCalleeObjects() && table.replaceSlotWithInherited(structureBefore, name, namespaceExport->m_holder, namespaceExport->m_offset))
            table.releaseSlot(structureBefore, name, Access::Read);
        else if ((isPermanentlyInherited && table.replaceSlotWithInherited(structureBefore, name, slot.slotBase(), slot.cachedOffset())) || (isOwn && table.setOwnOffset(structureBefore, name, slot.cachedOffset(), false)))
            table.releaseSlot(structureBefore, name, Access::Read);
        else if (uint32_t offsetAndKind = cache->structureID == structureBefore->id() && cache->readsExtraProperty() ? static_cast<uint32_t>(std::bit_cast<uint64_t>(cache->pointer) >> Slot::extraPropertyShift) : 0; offsetAndKind && !(offsetAndKind & ~Slot::extraPropertyOffsetMask))
            table.setExtraProperty(structureBefore, name, (offsetAndKind - JSObject::offsetOfInlineStorage()) / sizeof(EncodedJSValue), true, false);
    }
    ensureStillAliveHere(structureBefore);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static void growStorageAndPut(VM& vm, JSObject* object, Structure* oldStructure, const MegamorphicCache::StoreEntry& entry, JSValue value)
{
    Structure* newStructure = WTF::opaque(entry.m_newStructureID.decode());
    PropertyOffset offset = entry.m_offset;
    Butterfly* butterfly = object->allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), newStructure->outOfLineCapacity());
    object->nukeStructureAndSetButterfly(vm, oldStructure->id(), butterfly);
    object->putDirectOffset(vm, offset, value);
    object->setStructure(vm, newStructure);
    ensureStillAliveHere(oldStructure);
    ensureStillAliveHere(newStructure);
}

static NEVER_INLINE void assignDescriptorToTransitionTarget(Instance* instance, Structure* from, Structure* to, uint32_t identifier, PropertyOffset offset)
{
    auto& data = *reinterpret_cast<const Typed::Data*>(instance->typedData);
    uint32_t number = from->structureDescriptorPlusOne() - 1;
    if (to->previousID() != from || to->isDictionary() || to->structureDescriptorPlusOne() || number >= data.firstThisStructureDescriptor || offset != static_cast<PropertyOffset>(data.row(number)[0] & 0xff) || to->globalObject() != instance->globalObject)
        return;
    std::span all { data.at<std::array<uint32_t, 3>>(data.structureTransitionsOffset), data.numberOfStructureTransitions };
    std::array<uint32_t, 3> key { number, identifier, 0 };
    auto found = std::ranges::lower_bound(all, key);
    if (found != all.end() && (*found)[0] == number && (*found)[1] == identifier && (*found)[2] < UINT16_MAX)
        to->setStructureDescriptorPlusOne((*found)[2] + 1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t flagBits))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    countOperationAtSite(instance, callFrame, __func__);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    if (flagBits & Site::storesLazyClosure) {
        using StoreEntry = MegamorphicCache::StoreEntry;
        Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
        if (MegamorphicCache* table = base.isObject() ? vm.megamorphicCache() : nullptr) {
            if (auto* entry = table->findStore(structureBefore->id(), ident.impl()); entry && entry->m_reallocating == (StoreEntry::addsLazyClosure | StoreEntry::reallocates)) {
                growStorageAndPut(vm, asObject(base), structureBefore, *entry, value);
                OPERATION_RETURN(scope);
            }
        }
        instance->typed->putLazyClosure(globalObject, base, ident, instance->typed->lazyClosureFunction(value.asCell()));
        OPERATION_RETURN_IF_EXCEPTION(scope);
        unsigned attributes;
        PropertyOffset offset = base.isObject() ? base.asCell()->structure()->get(vm, ident.impl(), attributes) : invalidOffset;
        if (isValidOffset(offset) && attributes & PropertyAttribute::CustomValue && asObject(base)->getDirect(offset) == value) {
            Structure* structureAfter = base.asCell()->structure();
            bool growsStorage = structureAfter->outOfLineCapacity() != structureBefore->outOfLineCapacity();
            if (!growsStorage) {
                PutPropertySlot slot(base, true);
                slot.setNewProperty(asObject(base), offset);
                cachePutById(instance, SharedData::contains(cache) ? nullptr : callerData(instance, callFrame), base, structureBefore, ident, slot, false, cache);
            }
            if (offset <= MegamorphicCache::maxOffset && structureAfter->previousID() == structureBefore && !structureBefore->mayBePrototype() && structureBefore->propertyAccessesAreCacheable() && structureAfter->propertyAccessesAreCacheable())
                vm.ensureMegamorphicCache().initAsTransition(structureBefore->id(), structureAfter->id(), ident.impl(), offset, StoreEntry::addsLazyClosure | (growsStorage ? StoreEntry::reallocates : 0));
        }
        OPERATION_RETURN(scope);
    }
    if (flagBits & Site::hasThisStructure && tryStoreIntoThisStructure(globalObject, SharedData::contains(cache) ? nullptr : callerData(instance, callFrame), cache, base, ident, value))
        OPERATION_RETURN(scope);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;
    if (!isDirect && base.isCell()) {
        uint32_t structureID = base.asCell()->structureID().bits();
        auto& known = instance->inheritedSetterFor(structureID, ident.impl());
        if (known.nativeSetter && known.structureID == structureID && known.uid == ident.impl() && vm.megamorphicCache() && known.epoch == vm.megamorphicCache()->epoch()) {
            CustomAccessorValueFunc(std::bit_cast<CustomAccessorValueFunc::Ptr>(known.nativeSetter))(known.holder->realm(), encodedBase, encodedValue, ident);
            OPERATION_RETURN(scope);
        }
    }
    if (instance->typed && instance->typed->isCounting()) [[unlikely]]
        instance->typed->countAccess(Typed::State::AccessKind::Write, removeCodePtrTag(callFrame->rawReturnPC()), ident, base);
    if (base.isUndefinedOrNull() && instance->typed) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), true));
        OPERATION_RETURN(scope);
    }

    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (constexpr uint64_t isNotField = static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNotField) << 32; cache && oldStructure && cache->structureID == oldStructure->id() && cache->offset & Slot::pointerIsNotCell && std::bit_cast<uint64_t>(cache->pointer) == isNotField) {
        if (JSObject* extra = uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()) {
            unsigned attributes = 0;
            PropertyOffset offset = extra->structure()->get(vm, ident.impl(), attributes);
            if (isValidOffset(offset) && !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                extra->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        }
    }
    Typed::Object* flaggedObject = nullptr;
    const Typed::FieldData* flaggedObjectField = nullptr;
    Typed::Object* objectMissingField = nullptr;
    uint64_t wordsBeforeCreation = 0;
    if (!isDirect && oldStructure && oldStructure->compiledLayout() && base.asCell()->inherits<Typed::Object>()) {
        auto* object = uncheckedDowncast<Typed::Object>(base.asCell());
        uint64_t wordsBefore = object->orderAndShapeWords();
        if (instance->typed->storeByKnownTransition(vm, object, ident.impl(), value)) {
            if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
                cacheStoreTransition(instance, dataWithSlot(instance, callFrame, cache), object, oldStructure, wordsBefore, ident, cache);
            OPERATION_RETURN(scope);
        }
        if (object->hasExtraProperties()) {
            flaggedObject = object;
            flaggedObjectField = object->fieldNamed(object->state(), ident);
        }
        if (flaggedObjectField && !object->read(*flaggedObjectField)) {
            object->specializeStructureForExtraProperties(vm);
            oldStructure = object->structure();
            if (instance->typed->tryCreateFieldWithCachedTransition(vm, object, *flaggedObjectField, value))
                OPERATION_RETURN(scope);
            objectMissingField = object;
            wordsBeforeCreation = wordsBefore;
        }
    }
    PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
    if (Options::useAOTOperationCounters() && cache && oldStructure && (cache->offset & Slot::attemptsMask) == Slot::attemptsMask) [[unlikely]] {
        auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl()) : nullptr;
        countOperationNamed(instance, __func__, !entry ? "abandoned-and-not-in-table" : entry->m_reallocating ? "abandoned-and-entry-reallocates" : "abandoned-and-in-table");
    }
    bool hasGivenUpCaching = cache && !cache->structureID && (cache->offset & Slot::attemptsMask) == Slot::attemptsMask && !isDirect && oldStructure && oldStructure->typeInfo().overridesPut();
    if (MegamorphicCache* table = base.isObject() && !hasGivenUpCaching ? vm.megamorphicCache() : nullptr) {
        if (auto* entry = table->findStore(oldStructure->id(), ident.impl()); entry && entry->m_reallocating == MegamorphicCache::StoreEntry::reallocates) {
            countOperationNamed(instance, __func__, "grows-storage-by-entry");
            growStorageAndPut(vm, asObject(base), oldStructure, *entry, value);
            OPERATION_RETURN(scope);
        }
    }
    if (unsigned attributes = 0; isDirect && instance->typed && (base.asCell()->type() != FinalObjectType || oldStructure->compiledLayout() || !oldStructure->isStructureExtensible() || (oldStructure->get(vm, ident, attributes) != invalidOffset && attributes))) [[unlikely]]
        asObject(base)->createDataProperty(globalObject, ident, value, true);
    else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else if (flaggedObject)
        Typed::Object::putWithKnownField(flaggedObject, globalObject, ident, value, slot, flaggedObjectField);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (objectMissingField && objectMissingField->structure() == oldStructure && Typed::Object::didCreateOneField(static_cast<uint32_t>(wordsBeforeCreation), objectMissingField->orderWord()) && Typed::Object::tryGetOwnData(vm, objectMissingField, ident)) {
        if (cacheFieldCreation(instance, !cache || SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), objectMissingField, wordsBeforeCreation, ident, cache, true))
            OPERATION_RETURN(scope);
    }
    if (flaggedObjectField && flaggedObjectField->flags & Typed::fieldIsOutOfLine) {
        cachePutById(instance, SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), base, oldStructure, ident, slot, isDirect, cache);
        OPERATION_RETURN(scope);
    }
    if (oldStructure && slot.type() == PutPropertySlot::NewProperty && oldStructure->structureDescriptorPlusOne() && slot.base() == base.asCell()) [[unlikely]]
        assignDescriptorToTransitionTarget(instance, oldStructure, base.asCell()->structure(), identifierIndex, slot.cachedOffset());
    if (!isDirect && base.isObject() && oldStructure->compiledLayout() && base.asCell()->structure() == oldStructure)
        cacheNamedFieldWrite(instance, base.asCell(), ident, identifierIndex);
    if (hasGivenUpCaching)
        OPERATION_RETURN(scope);
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if (!isDirect && slot.isCacheableSetter() && base.isObject())
        cacheInheritedSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot);
    if (!isDirect && slot.isCustomAccessor() && slot.isCacheableCustom() && base.isObject())
        cacheNativeSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot);
    cachePutById(instance, SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

static bool isFunctionWithOrdinaryPut(VM& vm, JSValue base, Structure* structure, const Identifier& ident)
{
    if (!base.isCell() || base.asCell()->type() != JSFunctionType)
        return false;
    if (base.asCell()->classInfo() == Typed::Closure::info()) {
        auto* closure = uncheckedDowncast<Typed::Closure>(base.asCell());
        if (closure->classData() || closure->kind() == Typed::Format::FunctionKind::Bound || closure->kind() == Typed::Format::FunctionKind::Constructor)
            return false;
    } else if (base.asCell()->classInfo() != JSBoundFunction::info())
        return false;
    if (ident == vm.propertyNames->prototype || ident == vm.propertyNames->name || ident == vm.propertyNames->length)
        return false;
    JSValue prototype = structure->storedPrototype();
    return !structure->hasPolyProto() && !structure->typeInfo().overridesGetPrototype() && prototype.isObject() && storesAreOrdinaryAlongPrototypeChain(asObject(prototype)->structure());
}

static bool arrayHasOrdinaryDefineForName(VM& vm, JSValue base, Structure* structure, const Identifier& ident)
{
    return base.isCell() && isExactlyJSArray(base.asCell()) && ident != vm.propertyNames->length && !parseIndex(ident) && structure->isStructureExtensible();
}

static bool hostObjectHasOrdinaryDefineForName(VM& vm, JSValue base, Structure* structure, const Identifier& ident)
{
    if (!base.isObject() || base.asCell()->type() == FinalObjectType || structure->compiledLayout() || !structure->isStructureExtensible() || parseIndex(ident))
        return false;
    if (structure->classInfoForCells() == ErrorInstance::info())
        return !ErrorInstance::isLazilyMaterialized(vm, ident);
    TypeInfo typeInfo = structure->typeInfo();
    return structure->classInfoForCells()->methodTable.defineOwnProperty == JSObject::defineOwnProperty && !typeInfo.overridesGetOwnPropertySlot() && !typeInfo.hasStaticPropertyTable();
}

static bool defaultArrayPrototypeChainLacksName(JSGlobalObject* globalObject, Structure* structure, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    JSObject* arrayPrototype = globalObject->arrayPrototype();
    JSObject* objectPrototype = globalObject->objectPrototype();
    Structure* arrayPrototypeStructure = arrayPrototype->structure();
    Structure* objectPrototypeStructure = objectPrototype->structure();
    if (!structure->hasMonoProto() || structure->storedPrototype() != JSValue(arrayPrototype) || !arrayPrototypeStructure->hasMonoProto() || arrayPrototypeStructure->storedPrototype() != JSValue(objectPrototype) || !objectPrototypeStructure->hasMonoProto() || !objectPrototypeStructure->storedPrototype().isNull())
        return false;
    if (arrayPrototypeStructure->hasNonReifiedStaticProperties() || objectPrototypeStructure->hasNonReifiedStaticProperties() || objectPrototypeStructure->typeInfo().overridesGetOwnPropertySlot())
        return false;
    unsigned attributes = 0;
    return !isValidOffset(arrayPrototypeStructure->get(vm, uid, attributes)) && !isValidOffset(objectPrototypeStructure->get(vm, uid, attributes));
}

static std::optional<uint32_t> newStructureFlagsAfterAdd(Structure* before, Structure* after, PropertyOffset offset, bool isFunction)
{
    StorageAfterAddition storage = storageAfterAddition(before, after, offset);
    if (storage == StorageAfterAddition::Stays)
        return 0;
    if (storage == StorageAfterAddition::Unsupported || (before->typeInfo().type() == JSFunctionType && !isFunction) || before->compiledLayout())
        return std::nullopt;
    if (storage == StorageAfterAddition::IsFirst)
        return PropertyLookupTable::needsFirstStorage;
    constexpr unsigned maxCapacityGrownByStub = 32;
    return before->outOfLineCapacity() <= maxCapacityGrownByStub ? std::optional(PropertyLookupTable::growsStorage) : std::nullopt;
}

static bool storeAddsBelowTypedPrototype(JSGlobalObject* globalObject, JSObject* object, const Identifier& ident)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Structure* structure = object->structure();
    TypeInfo info = structure->typeInfo();
    if (ident == vm.propertyNames->underscoreProto || structure->hasPolyProto() || info.overridesGetPrototype() || info.overridesPut() || info.wouldHaveFixedProperties() || structure->hasReadOnlyOrGetterSetterPropertiesExcludingProtoOrImmutableProperties())
        return false;
    bool hasTypedPrototype = false;
    for (JSValue next = structure->storedPrototype(); next.isObject();) {
        JSObject* holder = asObject(next);
        Structure* holderStructure = holder->structure();
        if (holderStructure->hasPolyProto() || holderStructure->typeInfo().overridesGetPrototype())
            return false;
        const Typed::LayoutData* layout = holderStructure->compiledLayout();
        if (layout && Typed::isObjectLayout(layout->kind)) {
            PropertySlot own(holder, PropertySlot::InternalMethodType::VMInquiry, &vm);
            bool isFound = holder->methodTable()->getOwnPropertySlot(holder, globalObject, ident, own);
            own.disallowVMEntry.reset();
            if (scope.exception()) [[unlikely]] {
                scope.clearExceptionExceptTermination();
                return false;
            }
            if (own.isTaintedByOpaqueObject() || (isFound && (!own.isValue() || own.attributes() & PropertyAttribute::ReadOnly)))
                return false;
            hasTypedPrototype = true;
        } else if (holderStructure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || holderStructure->typeInfo().overridesPut())
            return false;
        next = holderStructure->storedPrototype();
    }
    return hasTypedPrototype;
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByNameAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t name, uint32_t flagBits))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, PropertyLookupTable::identifierIndexForName(name));
    recordLookup(instance, __func__, base, { }, ident.impl());
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;
    if (base.isUndefinedOrNull() && instance->typed) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), true));
        OPERATION_RETURN(scope);
    }
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (!isDirect && base.isObject() && oldStructure->isUncacheableDictionary() && !oldStructure->compiledLayout()) {
        if (!flattenOnce(vm, asObject(base))) {
            unsigned attributes = 0;
            PropertyOffset offset = base.asCell()->type() == FinalObjectType ? oldStructure->get(vm, ident.impl(), attributes) : invalidOffset;
            if (isValidOffset(offset) && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)) {
                countOperationNamed(instance, __func__, "permanent-dictionary-own-hit");
                oldStructure->didReplaceProperty(offset);
                asObject(base)->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
            countOperationNamed(instance, __func__, "permanent-dictionary");
            PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
            base.putInline(globalObject, ident, value, slot);
            OPERATION_RETURN(scope);
        }
        oldStructure = base.asCell()->structure();
    }
    if (uint32_t descriptorPlusOne = base.isObject() ? oldStructure->structureDescriptorPlusOne() : 0; descriptorPlusOne && !oldStructure->mayBePrototype()) {
        uint32_t identifier = PropertyLookupTable::identifierIndexForName(name);
        uint32_t nameAndOffset = instance->offsetsByNameAndStructure[(instance->nameDisplacements[identifier] & ~Typed::Data::nameIsOnObjectPrototype) + descriptorPlusOne];
        PropertyOffset offset = nameAndOffset & 0xff;
        if (nameAndOffset >> 8 == identifier && asObject(base)->getDirect(offset) && PropertyLookupTable::of(vm).setOwnOffset(oldStructure, name, offset, true)) {
            oldStructure->didCachePropertyReplacement(vm, offset);
            asObject(base)->putDirectOffset(vm, offset, value);
            countMissAvoidedByLookupTable(instance, callFrame);
            OPERATION_RETURN(scope);
        }
    }
    if (!isDirect && oldStructure) {
        uint32_t structureID = oldStructure->id().bits();
        auto& known = instance->inheritedSetterFor(structureID, ident.impl());
        if (known.nativeSetter && known.structureID == structureID && known.uid == ident.impl() && vm.megamorphicCache() && known.epoch == vm.megamorphicCache()->epoch()) {
            countOperationNamed(instance, __func__, "calls-the-native-setter-it-knows");
            CustomAccessorValueFunc(std::bit_cast<CustomAccessorValueFunc::Ptr>(known.nativeSetter))(known.holder->realm(), encodedBase, encodedValue, ident);
            OPERATION_RETURN(scope);
        }
    }
    if (!isDirect && base.isObject() && isJSArray(base.asCell()) && ident == vm.propertyNames->length && !oldStructure->mayBePrototype()) {
        IndexingType mode = oldStructure->indexingMode();
        if ((hasUndecided(mode) || hasInt32(mode) || hasDouble(mode) || hasContiguous(mode)) && !isCopyOnWrite(mode))
            PropertyLookupTable::of(vm).setArrayLength(oldStructure, name, hasDouble(mode));
    }
    bool storeAddsToArray = !isDirect && arrayHasOrdinaryDefineForName(vm, base, oldStructure, ident) && defaultArrayPrototypeChainLacksName(globalObject, oldStructure, ident.impl());
    if (Slot* abandonedSlot = !isDirect && !storeAddsToArray && base.isObject() ? PropertyLookupTable::of(vm).slotIfExists(oldStructure, name, PropertyLookupTable::Access::Store) : nullptr; abandonedSlot && !abandonedSlot->structureID && (abandonedSlot->offset & Slot::attemptsMask) == Slot::attemptsMask) {
        PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
        base.putInline(globalObject, ident, value, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (slot.type() == PutPropertySlot::NewProperty && oldStructure->structureDescriptorPlusOne() && slot.base() == base.asCell()) [[unlikely]]
            assignDescriptorToTransitionTarget(instance, oldStructure, base.asCell()->structure(), PropertyLookupTable::identifierIndexForName(name), slot.cachedOffset());
        OPERATION_RETURN(scope);
    }
    if (!isDirect && base.isObject() && base.asCell()->type() == FinalObjectType && !oldStructure->compiledLayout() && !oldStructure->isDictionary() && !oldStructure->hasImmutableProperties() && storeMayBeCachedByStructureAndName(vm, oldStructure, ident.impl())) {
        JSObject* object = asObject(base);
        PropertyLookupTable& table = PropertyLookupTable::of(vm);
        unsigned attributes = 0;
        PropertyOffset offset = oldStructure->get(vm, ident.impl(), attributes);
        if (isValidOffset(offset)) {
            if (!attributes) {
                oldStructure->didCachePropertyReplacement(vm, offset);
                if (table.setOwnOffset(oldStructure, name, offset, true)) {
                    object->putDirectOffset(vm, offset, value);
                    countMissAvoidedByLookupTable(instance, callFrame);
                    OPERATION_RETURN(scope);
                }
            }
        } else if (!oldStructure->mayBePrototype() && ((object->canPerformFastPutInline(vm, ident) && storesAreOrdinaryAlongPrototypeChain(oldStructure)) || storeAddsBelowTypedPrototype(globalObject, object, ident))) {
            PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
            object->putInlineFast(globalObject, ident, value, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            if (slot.type() == PutPropertySlot::NewProperty && slot.isCacheablePut() && slot.base() == object) {
                Structure* newStructure = object->structure();
                offset = slot.cachedOffset();
                bool isTransition = !newStructure->isDictionary() && newStructure->previousID() == oldStructure;
                if (oldStructure->structureDescriptorPlusOne()) [[unlikely]]
                    assignDescriptorToTransitionTarget(instance, oldStructure, newStructure, PropertyLookupTable::identifierIndexForName(name), offset);
                if (isTransition && !isInlineOffset(offset))
                    instance->didAddOutOfLineProperty(newStructure);
                auto flags = isTransition && newStructure->propertyAccessesAreCacheable() ? newStructureFlagsAfterAdd(oldStructure, newStructure, offset, false) : std::nullopt;
                if (flags && table.setAdded(oldStructure, name, PropertyLookupTable::Access::Store, newStructure, offset, *flags, ident.impl())) {
                    countMissAvoidedByLookupTable(instance, callFrame);
                    OPERATION_RETURN(scope);
                }
            }
            fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
            if (Slot* cache = table.ensureSlot(oldStructure, name, PropertyLookupTable::Access::Store); cache && (cache->structureID || (cache->offset & Slot::attemptsMask) != Slot::attemptsMask))
                cachePutById(instance, table.dataFor(instance), base, oldStructure, ident, slot, false, cache);
            ensureStillAliveHere(oldStructure);
            OPERATION_RETURN(scope);
        }
    }
    bool definesOnArray = isDirect && arrayHasOrdinaryDefineForName(vm, base, oldStructure, ident);
    if ((definesOnArray || storeAddsToArray) && !oldStructure->isDictionary() && !oldStructure->mayBePrototype() && !isCopyOnWrite(oldStructure->indexingMode())) {
        PropertyOffset offset = invalidOffset;
        Structure* newStructure = Structure::addPropertyTransitionToExistingStructure(oldStructure, ident, 0, offset);
        if (newStructure && newStructure->outOfLineCapacity() != oldStructure->outOfLineCapacity()) {
            countOperationNamed(instance, __func__, "grows-the-storage-of-an-array");
            JSObject* object = asObject(base);
            Butterfly* butterfly = object->allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), newStructure->outOfLineCapacity());
            object->nukeStructureAndSetButterfly(vm, oldStructure->id(), butterfly);
            object->putDirectOffset(vm, offset, value);
            object->setStructure(vm, newStructure);
            ensureStillAliveHere(oldStructure);
            ensureStillAliveHere(newStructure);
            OPERATION_RETURN(scope);
        }
    }
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    PropertyLookupTable::Access access = isDirect ? PropertyLookupTable::Access::Define : PropertyLookupTable::Access::Store;
    bool hasCompiledLayout = oldStructure && oldStructure->compiledLayout();
    auto enterReplacement = [&](PropertyOffset offset) {
        unsigned attributes = 0;
        if (isHostObjectOverridingPut(oldStructure) || oldStructure->get(vm, ident, attributes) != offset || attributes)
            return false;
        oldStructure->didCachePropertyReplacement(vm, offset);
        return table.setOwnOffset(oldStructure, name, offset, true);
    };
    auto flagsIfAddIsCacheable = [&](Structure* newStructure, PropertyOffset offset) -> std::optional<uint32_t> {
        if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || isHostObjectOverridingPut(oldStructure) || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
            return std::nullopt;
        bool isFunction = !isDirect && isFunctionWithOrdinaryPut(vm, base, oldStructure, ident);
        if (!isDirect && !isFunction && !storesAreOrdinaryAlongPrototypeChain(oldStructure))
            return std::nullopt;
        return newStructureFlagsAfterAdd(oldStructure, newStructure, offset, isFunction);
    };
    bool canUseMegamorphicCacheEntry = base.isObject() && !hasCompiledLayout && vm.megamorphicCache() && (!isDirect || base.asCell()->type() == FinalObjectType) && storeMayBeCachedByStructureAndName(vm, oldStructure, ident.impl());
    if (auto* entry = canUseMegamorphicCacheEntry ? vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl()) : nullptr; entry && !(entry->m_reallocating & MegamorphicCache::StoreEntry::addsLazyClosure)) {
        JSObject* object = asObject(base);
        PropertyOffset offset = entry->m_offset;
        if (entry->m_newStructureID == oldStructure->id()) {
            if (enterReplacement(offset)) {
                object->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        } else if (Structure* newStructure = WTF::opaque(entry->m_newStructureID.decode()); auto flags = flagsIfAddIsCacheable(newStructure, offset)) {
            if (table.setAdded(oldStructure, name, access, newStructure, offset, *flags, isDirect ? nullptr : ident.impl())) {
                if (*flags)
                    growStorageAndPut(vm, object, oldStructure, *entry, value);
                else {
                    object->putDirectOffset(vm, offset, value);
                    object->setStructure(vm, newStructure);
                }
                if (!isInlineOffset(offset))
                    instance->didAddOutOfLineProperty(newStructure);
                if (oldStructure->structureDescriptorPlusOne()) [[unlikely]]
                    assignDescriptorToTransitionTarget(instance, oldStructure, newStructure, PropertyLookupTable::identifierIndexForName(name), offset);
                OPERATION_RETURN(scope);
            }
        }
    }
    auto* asTypedObject = hasCompiledLayout && Typed::isObjectLayout(oldStructure->compiledLayout()->kind) ? uncheckedDowncast<Typed::Object>(base.asCell()) : nullptr;
    const Typed::FieldData* storedFieldOf = asTypedObject ? asTypedObject->fieldNamed(asTypedObject->state(), ident) : nullptr;
    if (storedFieldOf && !isDirect && asTypedObject->hasExtraProperties() && !asTypedObject->read(*storedFieldOf)) {
        asTypedObject->specializeStructureForExtraProperties(vm);
        oldStructure = asTypedObject->structure();
        if (instance->typed->tryCreateFieldWithCachedTransition(vm, asTypedObject, *storedFieldOf, value))
            OPERATION_RETURN(scope);
    }
    if (asTypedObject && !storedFieldOf) {
        auto* object = asTypedObject;
        object->specializeStructureForExtraProperties(vm);
        oldStructure = object->structure();
        PropertyOffset offset = invalidOffset;
        bool hasDefaultAttributes = false;
        switch (object->tryStoreExtraProperty(vm, ident, value, offset, true, isDirect, &hasDefaultAttributes, true)) {
        case Typed::Object::ExtraPropertyStore::Replaced:
            table.setExtraProperty(oldStructure, name, offset, false, true, hasDefaultAttributes);
            OPERATION_RETURN(scope);
        case Typed::Object::ExtraPropertyStore::Added:
            table.setExtraPropertyAdded(oldStructure, name, object->structure(), offset, instance->typed->isStructureWithoutExtraProperties(oldStructure), ident.impl(), isDirect);
            OPERATION_RETURN(scope);
        case Typed::Object::ExtraPropertyStore::NotHandled:
            break;
        }
    }
    Structure* slotStructure = oldStructure;
    Slot* cache = hasCompiledLayout ? table.ensureSlot(oldStructure, name, access) : nullptr;
    Data* data = table.dataFor(instance);
    if (constexpr uint64_t isNotField = static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNotField) << 32; cache && cache->structureID == oldStructure->id() && cache->offset & Slot::pointerIsNotCell && std::bit_cast<uint64_t>(cache->pointer) == isNotField) {
        if (JSObject* extra = uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()) {
            unsigned attributes = 0;
            PropertyOffset offset = extra->structure()->get(vm, ident.impl(), attributes);
            if (isValidOffset(offset) && !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                extra->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        }
    }
    if (!isDirect && hasCompiledLayout && base.asCell()->inherits<Typed::Object>()) {
        auto* object = uncheckedDowncast<Typed::Object>(base.asCell());
        uint64_t wordsBefore = object->orderAndShapeWords();
        if (instance->typed->storeByKnownTransition(vm, object, ident.impl(), value)) {
            if (cache && (cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
                cacheStoreTransition(instance, data, object, oldStructure, wordsBefore, ident, cache);
            OPERATION_RETURN(scope);
        }
    }
    auto* objectPossiblyMissingField = cache && storedFieldOf ? asTypedObject : nullptr;
    uint64_t wordsBeforeStore = objectPossiblyMissingField ? objectPossiblyMissingField->orderAndShapeWords() : 0;
    PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
    bool definesOnHostObject = isDirect && hostObjectHasOrdinaryDefineForName(vm, base, oldStructure, ident);
    if (unsigned attributesOnArray = 0; (definesOnArray || definesOnHostObject) && (oldStructure->get(vm, ident, attributesOnArray) == invalidOffset || !attributesOnArray))
        asObject(base)->putDirect(vm, ident, value, 0, slot);
    else if (unsigned attributes = 0; isDirect && instance->typed && (base.asCell()->type() != FinalObjectType || hasCompiledLayout || !oldStructure->isStructureExtensible() || (oldStructure->get(vm, ident, attributes) != invalidOffset && attributes))) [[unlikely]]
        asObject(base)->createDataProperty(globalObject, ident, value, true);
    else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else if (asTypedObject)
        Typed::Object::putWithKnownField(asTypedObject, globalObject, ident, value, slot, storedFieldOf);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDirect && objectPossiblyMissingField && storedFieldOf->flags & Typed::fieldMayExistWithoutStorage && objectPossiblyMissingField->structure() == oldStructure && objectPossiblyMissingField->orderAndShapeWords() == wordsBeforeStore) {
        if (Slot* storeSlot = table.ensureSlot(oldStructure, name, access); storeSlot && cacheStorageCreation(instance, data, objectPossiblyMissingField, ident, storeSlot))
            OPERATION_RETURN(scope);
    }
    if (objectPossiblyMissingField && objectPossiblyMissingField->structure() == oldStructure && Typed::Object::didCreateOneField(static_cast<uint32_t>(wordsBeforeStore), objectPossiblyMissingField->orderWord()) && Typed::Object::tryGetOwnData(vm, objectPossiblyMissingField, ident)) {
        if (Slot* storeSlot = table.ensureSlot(oldStructure, name, access); storeSlot && cacheFieldCreation(instance, data, objectPossiblyMissingField, wordsBeforeStore, ident, storeSlot, !isDirect))
            OPERATION_RETURN(scope);
    }
    if (oldStructure && slot.type() == PutPropertySlot::NewProperty && oldStructure->structureDescriptorPlusOne() && slot.base() == base.asCell()) [[unlikely]]
        assignDescriptorToTransitionTarget(instance, oldStructure, base.asCell()->structure(), PropertyLookupTable::identifierIndexForName(name), slot.cachedOffset());
    if (!oldStructure || oldStructure != slotStructure)
        OPERATION_RETURN(scope);
    if (base.asCell()->type() == JSFunctionType && slot.type() == PutPropertySlot::NewProperty && base.asCell()->structure()->previousID() != oldStructure)
        OPERATION_RETURN(scope);
    if (!isDirect && slot.isCacheableSetter() && base.isObject()) {
        if (auto offset = inheritedSetterOffset(globalObject, asObject(base), oldStructure, ident, slot)) {
            if (hasCompiledLayout)
                table.releaseSlot(oldStructure, name, access);
            if (table.setAccessor(oldStructure, name, slot.base(), *offset, ident.impl(), true))
                OPERATION_RETURN(scope);
        }
        if (unsigned attributes = 0; slot.base() == base.asCell() && !hasCompiledLayout && oldStructure->propertyAccessesAreCacheable() && !isHostObjectOverridingPut(oldStructure) && oldStructure->get(vm, ident, attributes) == slot.cachedOffset() && attributes & PropertyAttribute::Accessor && table.setAccessor(oldStructure, name, nullptr, slot.cachedOffset(), nullptr, true))
            OPERATION_RETURN(scope);
    }
    if (!isDirect && slot.isCustomAccessor() && slot.isCacheableCustom() && base.isObject() && cacheNativeSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot))
        OPERATION_RETURN(scope);
    if (base.isObject() && !hasCompiledLayout && slot.isCacheablePut() && slot.base() == base.asCell() && storeMayBeCachedByStructureAndName(vm, oldStructure, ident.impl())) {
        Structure* newStructure = base.asCell()->structure();
        PropertyOffset offset = slot.cachedOffset();
        bool isInTable = false;
        if (slot.type() == PutPropertySlot::ExistingProperty)
            isInTable = newStructure == oldStructure && enterReplacement(offset);
        else if (slot.type() == PutPropertySlot::NewProperty) {
            auto flags = flagsIfAddIsCacheable(newStructure, offset);
            if (!isInlineOffset(offset) && !oldStructure->isDictionary() && !newStructure->isDictionary() && !oldStructure->mayBePrototype() && newStructure->previousID() == oldStructure)
                instance->didAddOutOfLineProperty(newStructure);
            if (flags)
                isInTable = table.setAdded(oldStructure, name, access, newStructure, offset, *flags, isDirect ? nullptr : ident.impl());
        }
        if (isInTable) {
            countMissAvoidedByLookupTable(instance, callFrame);
            OPERATION_RETURN(scope);
        }
    }
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if ((cache = table.ensureSlot(oldStructure, name, access)); cache && (cache->structureID || (cache->offset & Slot::attemptsMask) != Slot::attemptsMask))
        cachePutById(instance, data, base, oldStructure, ident, slot, isDirect, cache);
    if (storedFieldOf && !isDirect && base.asCell()->structure() == oldStructure && Typed::Object::tryGetOwnData(vm, base.asCell(), ident)) {
        if (Slot* readSlot = table.ensureSlot(oldStructure, name, PropertyLookupTable::Access::Read)) {
            cacheCompiledField(vm, data, readSlot, base.asCell(), ident, false);
            if (readSlot->structureID == oldStructure->id() && (readSlot->offset & Slot::flagsMask) == Slot::compiledFieldFlags)
                table.setField(oldStructure, name, std::bit_cast<uint64_t>(readSlot->pointer));
        }
    }
    ensureStillAliveHere(slotStructure);
    OPERATION_RETURN(scope);
}

static JSValue findAndCacheImmutableElement(Instance* instance, JSObject* object, uint32_t index)
{
    if (!hasAnyArrayStorage(object->indexingType()) || !isExactlyJSArray(object) || index >> Instance::immutableElementIndexBits)
        return { };
    SparseArrayValueMap* map = object->butterfly()->arrayStorage()->m_sparseMap.get();
    if (!map)
        return { };
    auto entry = map->find(index);
    constexpr unsigned immutableAttributes = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    if (entry == map->notFound() || (entry->attributes() & (immutableAttributes | PropertyAttribute::Accessor)) != immutableAttributes)
        return { };
    JSValue value = entry->get();
    instance->cacheImmutableElement(object, index, value);
    return value;
}

static OperationReturnType<EncodedJSValue> getByVal(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue property)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    recordNamedAccess(instance, "operationAOTGetByVal", base, property);
    if (base.isUndefinedOrNull() && instance->typed) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(globalObject, base, property, false));
        OPERATION_RETURN(scope, encodedJSValue());
    }

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
            if (JSValue result = findAndCacheImmutableElement(instance, object, i))
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    scope.release();
    return getByVal(instance, globalObject, vm, JSValue::decode(encodedBase), JSValue::decode(encodedProperty));
}

static OperationReturnType<void> putByVal(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue property, JSValue value, bool isStrict)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    recordNamedAccess(instance, "operationAOTPutByVal", base, property);
    if (base.isUndefinedOrNull() && instance->typed) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(globalObject, base, property, true));
        OPERATION_RETURN(scope);
    }

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        uint32_t i = *index;
        if (base.isObject()) {
            JSObject* object = asObject(base);
            if (object->trySetIndexQuickly(vm, i, value))
                OPERATION_RETURN(scope);
            scope.release();
            if (instance->typed && object->type() == ArrayType)
                putArrayIndex(globalObject, uncheckedDowncast<JSArray>(object), i, value, isStrict);
            else
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

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    scope.release();
    return putByVal(instance, globalObject, vm, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), isStrict);
}

static std::optional<Identifier> nameForKeyedCache(JSGlobalObject* globalObject, JSValue property)
{
    VM& vm = globalObject->vm();
    if (property.isSymbol())
        return Identifier::fromUid(asSymbol(property)->privateName());
    if (!property.isString())
        return std::nullopt;
    auto existingAtomString = asString(property)->toExistingAtomString(globalObject);
    if (!existingAtomString || parseIndex(*existingAtomString.data))
        return std::nullopt;
    return Identifier::fromUid(vm, existingAtomString.data);
}

static UniquedStringImpl* keyAtom(JSValue key)
{
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text && text->isAtom() ? static_cast<UniquedStringImpl*>(text) : nullptr;
}

static uint16_t epochForKey(VM& vm, JSCell* key, Structure* structure)
{
    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    const StringImpl* name = key->isSymbol() ? &asSymbol(key)->uid() : asString(key)->tryGetValueImpl();
    return name ? cache.epochForName(*name, structure->id()) : cache.epochForAnyName(structure->id());
}

static void cacheKeyedHas(Instance* instance, VM& vm, Structure* structure, JSCell* key, bool dependsOnlyOnStructure, bool result, bool isPermanent = false)
{
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = dependsOnlyOnStructure ? (isPermanent ? MegamorphicCache::permanentEpoch : epochForKey(vm, key, structure)) | static_cast<uint32_t>(result) << Instance::keyedHasResultShift : Slot::isGetter;
    entry.pointer = key;
    entry.structureID = structure->id();
}

static void cacheKeyedHasOfField(Instance* instance, VM& vm, Structure* structure, JSCell* key, const Typed::FieldData& field, bool isPermanent)
{
    auto test = presenceTestFor(field);
    if (!test)
        return;
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = Instance::keyedHasReadsField | test->index << Instance::keyedHasFieldIndexShift | static_cast<uint32_t>(test->kind) << Instance::keyedHasFieldKindShift | (isPermanent ? MegamorphicCache::permanentEpoch : epochForKey(vm, key, structure));
    entry.pointer = key;
    entry.structureID = structure->id();
}

static JSObject* prototypeToAskBelowTypedObject(VM& vm, JSObject* object, UniquedStringImpl* name)
{
    auto missesByStructure = [&](JSObject* holder) {
        Structure* structure = holder->structure();
        return holder->type() == FinalObjectType && !structure->compiledLayout() && !structure->isDictionary() && structure->hasMonoProto() && structure->propertyAccessesAreCacheable()
            && structure->propertyAccessesAreCacheableForAbsence() && !isValidOffset(structure->get(vm, name));
    };
    if (parseIndex(*name))
        return nullptr;
    for (JSObject* holder = object; missesByStructure(holder);) {
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            return nullptr;
        holder = asObject(prototype);
        if (const Typed::LayoutData* layout = holder->structure()->compiledLayout(); layout && Typed::isObjectLayout(layout->kind))
            return asObject(object->getPrototypeDirect());
    }
    return nullptr;
}

static bool lookupAboveRunsNoCode(JSObject* object)
{
    for (JSValue next = object->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        if (asObject(next)->type() == ProxyObjectType || asObject(next)->structure()->typeInfo().overridesGetPrototype())
            return false;
    }
    return true;
}

static void cacheKeyedAddition(Instance* instance, VM& vm, Structure* before, Structure* after, JSCell* key, PropertyOffset offset)
{
    if (before->isDictionary() || after->isDictionary())
        return;
    auto* place = instance->allocateKeyedAddition(before->id(), key);
    if (!place)
        return;
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->hasPolyProto();
    };
    StorageAfterAddition storage = storageAfterAddition(before, after, offset);
    if (!isPlain(before) || !isPlain(after) || storage == StorageAfterAddition::Unsupported)
        return;
    auto& entry = *place;
    cacheKeyedAccess(entry.slot, before, key, offset);
    entry.newStructureID = after->id();
    entry.epoch = epochForKey(vm, key, before);
    entry.newStorageCapacity = storage == StorageAfterAddition::Stays ? 0 : after->outOfLineCapacity();
}

template<typename Found>
static ALWAYS_INLINE auto lookUpGeneric(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
{
    if (object->type() == GlobalProxyType && uid->isSymbol() && object->classInfo() == JSGlobalProxy::info())
        object = uncheckedDowncast<JSGlobalProxy>(object)->target();
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        bool hasSymbolsOnlyInStructure = uid->isSymbol() && object->type() == GlobalObjectType && object->methodTable()->getOwnPropertySlot == JSGlobalObject::getOwnPropertySlot;
        if (!hasSymbolsOnlyInStructure && (info.overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags())))
            return found(nullptr, invalidOffset, 0u);
        if (info.overridesGetPrototype() || structure->compiledLayout())
            return found(nullptr, invalidOffset, 0u);
        if (PropertyTable* table = structure->materializedPropertyTable()) {
            auto [offset, attributes] = table->get(uid);
            if (isValidOffset(offset))
                return found(object, offset, attributes);
        } else if (isValidOffset(structure->maxOffset()))
            return found(nullptr, invalidOffset, 0u);
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return found(object, invalidOffset, 0u);
        object = asObject(prototype);
    }
}

static NEVER_INLINE void validateLookupResult(JSObject* object, UniquedStringImpl* uid, bool isThere, JSValue value)
{
    JSGlobalObject* globalObject = object->globalObject();
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &globalObject->vm());
    bool has = object->getPropertySlot(globalObject, uid, slot);
    if (has == isThere && (!has || !value || (slot.isValue() && slot.getValue(globalObject, uid) == value)))
        return;
    dataLogLn("AOT: a keyed cache says ", isThere ? "there" : "nowhere", ", ", value, ", and the object says ", has ? "there" : "nowhere", ": ", JSValue(object), ", name ", uid);
    CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGeneric, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid))
{
    return lookUpGeneric(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) {
        if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            return encodedJSValue();
        JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateLookupResult(asObject(base), uid, isValidOffset(offset), result);
        return JSValue::encode(result);
    });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasGeneric, size_t, (JSCell* base, UniquedStringImpl* uid))
{
    return lookUpGeneric(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned) {
        if (holder && Options::validateAOTTypedFacts()) [[unlikely]]
            validateLookupResult(asObject(base), uid, isValidOffset(offset), { });
        return holder ? static_cast<size_t>(isValidOffset(offset)) : Instance::cannotLookUp;
    });
}

template<bool isCounting>
static ALWAYS_INLINE bool findGeneric(Structure* structure, UniquedStringImpl* uid, PropertyOffset& offset, unsigned& attributes, unsigned& stepsBack, bool& isExcluded, unsigned& hintHits, unsigned& hintMisses)
{
    offset = invalidOffset;
    attributes = 0;
    if (structure->seenProperties().ruleOut(CompactPtr<UniquedStringImpl>::encode(uid))) {
        if constexpr (isCounting)
            isExcluded = true;
        return true;
    }
    for (Structure* current = structure;;) {
        if (PropertyTable* table = current->materializedPropertyTable()) {
            std::tie(offset, attributes) = table->get(uid);
            return true;
        }
        if (current->transitionPropertyName() == uid) {
            switch (current->transitionKind()) {
            case TransitionKind::PropertyAddition:
            case TransitionKind::PropertyAttributeChange:
                offset = current->transitionOffset();
                attributes = current->transitionPropertyAttributes();
                return true;
            case TransitionKind::PropertyDeletion:
                return true;
            default:
                return false;
            }
        }
        Structure* previous = current->previousID();
        if (!previous)
            return !isValidOffset(current->maxOffset());
        constexpr uintptr_t fourCells = 4 * WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(sizeof(Structure));
        uintptr_t fourLower = std::bit_cast<uintptr_t>(previous) - fourCells;
        __builtin_prefetch(std::bit_cast<const void*>(fourLower));
        __builtin_prefetch(std::bit_cast<const void*>(fourLower + Structure::propertyTableUnsafeOffset()));
        if constexpr (isCounting) {
            Structure* fourBack = previous;
            for (unsigned i = 0; i < 4 && fourBack; ++i)
                fourBack = fourBack->previousID();
            if (fourBack)
                (std::bit_cast<uintptr_t>(fourBack) == fourLower ? hintHits : hintMisses)++;
        }
        if constexpr (isCounting)
            stepsBack++;
        current = previous;
    }
}

static NEVER_INLINE void validateLookupResultOrNamespace(JSObject* asked, JSObject* base, UniquedStringImpl* uid, bool isThere, JSValue value)
{
    validateLookupResult(base, uid, isThere, value);
    if (asked == base)
        return;
    JSGlobalObject* globalObject = asked->globalObject();
    PropertySlot slot(asked, PropertySlot::InternalMethodType::Get);
    JSValue general = asked->getPropertySlot(globalObject, uid, slot) ? slot.getValue(globalObject, uid) : jsUndefined();
    if (general == value)
        return;
    dataLogLn("AOT: a look through a view says ", value, ", and the view says ", general, ": ", JSValue(asked), ", name ", uid);
    CRASH();
}

static JSObject* liveBindingSource(Instance* instance, JSObject* view, PropertyOffset offset, unsigned attributes)
{
    if (!(attributes & PropertyAttribute::CustomValue) || !offset)
        return nullptr;
    CustomGetterSetter* accessor = instance->typed->liveBindingAccessorIfCreated();
    if (!accessor || view->getDirect(offset) != JSValue(accessor))
        return nullptr;
    return view->getDirect(0).getObject();
}

static NEVER_INLINE JSValue getGenericSlow(Instance* instance, JSCell* asked, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    MegamorphicCache* cache = vm.megamorphicCache();
    JSObject* base = asObject(asked);
    bool isThere = false;
    bool isExportIndex = false;
    const char* why = nullptr;
    auto unknownResult = [&](const char* reason) -> JSValue {
        why = reason;
        return { };
    };
    auto answer = [&]() -> JSValue {
        if (asked->type() == ObjectType) {
            if (asked->classInfo() != Typed::ModuleNamespace::info())
                return unknownResult("cannot-say:not-ordinary");
            unsigned number = uncheckedDowncast<Typed::ModuleNamespace>(asked)->numberOf(uid);
            if (number == Typed::ModuleNamespace::noExport)
                return unknownResult("cannot-say:no-export");
            isExportIndex = true;
            return JSValue::decode(static_cast<EncodedJSValue>(1ull << 48 | number));
        }
        StructureID structureID = base->structureID();
        auto redirectLookupTo = [&](JSObject* source) {
            base = source;
            structureID = source->structureID();
            return !structureID.decode()->isDictionary();
        };
        if (structureID.decode()->isDictionary()) {
            PropertyTable* table = structureID.decode()->materializedPropertyTable();
            if (!table)
                return unknownResult("cannot-say:dictionary");
            auto [offset, attributes] = table->get(uid);
            JSObject* source = isValidOffset(offset) ? liveBindingSource(instance, base, offset, attributes) : nullptr;
            if (!source)
                return unknownResult("cannot-say:dictionary");
            if (!redirectLookupTo(source))
                return unknownResult("cannot-say:a-view-of-a-dictionary");
        }
        bool isLazyFunctionPropertyName = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype;
        bool dependsOnlyOnStructure = true;
        bool isPermanent = true;
        for (JSObject* object = base;;) {
            Structure* structure = object->structure();
            TypeInfo info = structure->typeInfo();
            bool isClassObject = object->type() == JSFunctionType && info.getOwnPropertySlotIsImpureForPropertyAbsence() && object->classInfo() == Typed::Closure::info() && uncheckedDowncast<Typed::Closure>(object)->kind() == Typed::Format::FunctionKind::Constructor;
            bool dependsOnlyOnOwnStructure = object == base && !isClassObject && info.getOwnPropertySlotIsImpureForPropertyAbsence() && Typed::hasNoLazyPropertyNamed(vm, object, uid);
            if (isClassObject) {
                const Typed::State& state = *structure->aotInstance()->typed;
                const Typed::LayoutData* ofClass = uncheckedDowncast<Typed::Closure>(object)->classData();
                if (ofClass && state.hasStaticMethodNamed(*ofClass, uid))
                    return unknownResult("cannot-say:a-static-of-the-class");
                dependsOnlyOnStructure = false;
            }
            bool hasCustomLookup = info.overridesGetOwnPropertySlot() && (isLazyFunctionPropertyName || (!isExactlyJSArray(object) && (object->type() != JSFunctionType || (info.getOwnPropertySlotIsImpureForPropertyAbsence() && !isClassObject && !dependsOnlyOnOwnStructure)) && object != instance->globalObject->arrayPrototype()));
            if (hasCustomLookup || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || !structure->propertyAccessesAreCacheable())
                return unknownResult("cannot-say:not-ordinary");
            if (object != base) {
                if (structure->isDictionary())
                    return unknownResult("cannot-say:a-dictionary-for-a-prototype");
                isPermanent &= structure->hasImmutableProperties();
            }
            PropertyOffset offset;
            unsigned attributes;
            unsigned stepsBack = 0;
            unsigned hintHits = 0;
            unsigned hintMisses = 0;
            bool isExcluded = false;
            if (!findGeneric<false>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses))
                return unknownResult("cannot-say:the-chain-cannot-say");
            if (isValidOffset(offset)) {
                if (attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || offset > MegamorphicCache::maxOffset) {
                    JSObject* source = object == asked ? liveBindingSource(instance, object, offset, attributes) : nullptr;
                    if (!source)
                        return unknownResult("cannot-say:accessor-or-custom");
                    if (!redirectLookupTo(source))
                        return unknownResult("cannot-say:a-view-of-a-dictionary");
                    object = source;
                    continue;
                }
                JSValue result = object->getDirect(offset);
                if (!result)
                    return unknownResult("cannot-say:empty");
                isThere = true;
                if (!dependsOnlyOnStructure && object != base)
                    return result;
                if (isPermanent && object != base)
                    cache->initAsPermanentHit(structureID, uid, object, offset);
                else
                    cache->initAsHit(structureID, uid, object, offset, object == base);
                return result;
            }
            if ((!structure->propertyAccessesAreCacheableForAbsence() && !isClassObject && !dependsOnlyOnOwnStructure) || !structure->hasMonoProto())
                return unknownResult("cannot-say:not-cacheable-for-absence");
            JSValue prototype = structure->storedPrototype();
            if (!prototype.isObject())
                break;
            object = asObject(prototype);
        }
        if (!dependsOnlyOnStructure)
            return jsUndefined();
        if (isPermanent)
            cache->initAsPermanentMiss(structureID, uid);
        else
            cache->initAsMiss(structureID, uid);
        return jsUndefined();
    };
    JSValue result = answer();
    if (Options::validateAOTTypedFacts() && result && !isExportIndex) [[unlikely]]
        validateLookupResultOrNamespace(asObject(asked), base, uid, isThere, result);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        countOperationNamed(instance, "plainly", isExportIndex ? "an-export" : result ? "answered" : why);
        if (result && !isExportIndex)
            countOperationNamed(instance, "plainly", asked != base ? "answered-through-a-view" : "answered-the-long-way");
    }
    return result;
}

template<bool isCounting>
static ALWAYS_INLINE JSValue getGenericAndCacheIf(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreeNameSlot)
{
    bool needsSlowLookup = false;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    StructureID structureID = base->structureID();
    bool isThere = false;
    unsigned stepsBack = 0;
    unsigned hintHits = 0;
    unsigned hintMisses = 0;
    bool isExcluded = false;
    const char* why = nullptr;
    auto unknownResult = [&](const char* reason) ALWAYS_INLINE_LAMBDA -> JSValue {
        if constexpr (isCounting)
            why = reason;
        return { };
    };
    auto answer = [&]() ALWAYS_INLINE_LAMBDA -> JSValue {
        if (!cache)
            return unknownResult("cannot-say:no-megamorphic-cache");
        if (structureID.decode()->isDictionary()) {
            needsSlowLookup = true;
            return { };
        }
        bool isPermanent = true;
        for (JSObject* object = asObject(base);;) {
            Structure* structure = object->structure();
            TypeInfo info = structure->typeInfo();
            if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || !structure->propertyAccessesAreCacheable()) {
                needsSlowLookup = true;
                return { };
            }
            if (object != base) {
                if (structure->isDictionary())
                    return unknownResult("cannot-say:a-dictionary-for-a-prototype");
                isPermanent &= structure->hasImmutableProperties();
            }
            PropertyOffset offset;
            unsigned attributes;
            if (!findGeneric<isCounting>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses))
                return unknownResult("cannot-say:the-chain-cannot-say");
            if (isValidOffset(offset)) {
                if (attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || offset > MegamorphicCache::maxOffset) {
                    needsSlowLookup = object == base && attributes & PropertyAttribute::CustomValue;
                    return unknownResult("cannot-say:accessor-or-custom");
                }
                if (hasFreeNameSlot && object == base && !attributes && static_cast<unsigned>(offset) < Structure::numberOfSlotsWithPropertyNameIDs && isInlineOffset(offset) && structure->fieldIDInSlot(offset) < Structure::firstReservedPropertyNameID)
                    return unknownResult("cannot-say:a-place-by-name-is-free");
                JSValue result = object->getDirect(offset);
                if (!result)
                    return unknownResult("cannot-say:empty");
                isThere = true;
                if (isPermanent && object != base)
                    cache->initAsPermanentHit(structureID, uid, object, offset);
                else
                    cache->initAsHit(structureID, uid, object, offset, object == base);
                return result;
            }
            if (!structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto())
                return unknownResult("cannot-say:not-cacheable-for-absence");
            JSValue prototype = structure->storedPrototype();
            if (!prototype.isObject())
                break;
            object = asObject(prototype);
        }
        if (isPermanent)
            cache->initAsPermanentMiss(structureID, uid);
        else
            cache->initAsMiss(structureID, uid);
        return jsUndefined();
    };
    JSValue result = answer();
    if (needsSlowLookup) [[unlikely]]
        return getGenericSlow(instance, base, uid);
    if (Options::validateAOTTypedFacts() && result) [[unlikely]]
        validateLookupResult(asObject(base), uid, isThere, result);
    if constexpr (isCounting) {
        countOperationNamed(instance, "plainly", result ? "answered" : why);
        if (stepsBack && result)
            countOperationNamed(instance, "plainly", stepsBack <= 4 ? "walked-back:1-to-4" : stepsBack <= 8 ? "walked-back:5-to-8" : stepsBack <= 16 ? "walked-back:9-to-16" : stepsBack <= 32 ? "walked-back:17-to-32" : "walked-back:over-32");
        if (isExcluded)
            countOperationNamed(instance, "plainly", "ruled-out");
        while (hintHits--)
            countOperationNamed(instance, "plainly", "hint-right");
        while (hintMisses--)
            countOperationNamed(instance, "plainly", "hint-wrong");
    }
    return result;
}

static NEVER_INLINE JSValue getGenericAndCacheWithCounters(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreeNameSlot)
{
    return getGenericAndCacheIf<true>(instance, base, uid, hasFreeNameSlot);
}

static ALWAYS_INLINE JSValue getGenericAndCache(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreeNameSlot)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        return getGenericAndCacheWithCounters(instance, base, uid, hasFreeNameSlot);
    return getGenericAndCacheIf<false>(instance, base, uid, hasFreeNameSlot);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGenericAtSite, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid, PolymorphicSlots* several))
{
    JSValue result = getGenericAndCache(several->owner->instance, base, uid, several->numberOfUsedInlineNameSlots < PolymorphicSlots::numberOfInlineSlotsByName);
    if (result)
        several->didMissWithoutRefill();
    return JSValue::encode(result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGenericAndCache, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid, Instance* instance))
{
    if (uid == instance->vm->propertyNames->underscoreProto.impl() || (uid->length() && isASCIIDigit((*uid)[0])))
        return encodedJSValue();
    return JSValue::encode(getGenericAndCache(instance, base, uid, false));
}

static void cacheKeyedRead(Instance* instance, Structure* structure, JSCell* key, const MegamorphicCache::LoadEntry& found)
{
    auto& read = instance->keyedReadFor(structure->id(), key);
    if (found.m_holder == JSCell::seenMultipleCalleeObjects()) {
        cacheKeyedAccess(read.slot, structure, key, found.m_offset);
        return;
    }
    auto location = propertyLocation(found.m_offset);
    if (!location)
        return;
    read.slot.structureID = StructureID();
    read.holder = found.m_holder;
    read.epoch = found.m_epoch;
    read.slot.offset = !found.m_holder && found.m_epoch == MegamorphicCache::permanentEpoch ? Instance::keyIsPermanentlyAbsent : *location | Slot::isGetter | Slot::pointerIsCell;
    read.slot.pointer = key;
    read.slot.structureID = structure->id();
}

template<typename Found>
static ALWAYS_INLINE auto lookUpInPrototypeChainGeneric(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
{
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout())
            return found(nullptr, invalidOffset, 0u);
        if (uid) {
            PropertyOffset offset;
            unsigned attributes;
            unsigned stepsBack = 0;
            unsigned hintHits = 0;
            unsigned hintMisses = 0;
            bool isExcluded = false;
            if (!findGeneric<false>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses))
                return found(nullptr, invalidOffset, 0u);
            if (isValidOffset(offset))
                return found(object, offset, attributes);
        }
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return found(object, invalidOffset, 0u);
        object = asObject(prototype);
    }
}

static JSValue getFromPrototypeChainGeneric(JSObject* base, UniquedStringImpl* uid)
{
    return lookUpInPrototypeChainGeneric(base, uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) -> JSValue {
        if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            return { };
        JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
        if (Options::validateAOTTypedFacts() && uid && result) [[unlikely]]
            validateLookupResult(base, uid, isValidOffset(offset), result);
        return result;
    });
}

static ALWAYS_INLINE bool propertyNameForKeyGeneric(Instance* instance, JSCell* key, UniquedStringImpl*& uid, bool& shouldCacheByKeyCell, bool atomizes = false, const StringImpl** previouslyHashedString = nullptr)
{
    shouldCacheByKeyCell = true;
    if (key->isSymbol()) {
        uid = &asSymbol(key)->uid();
        return true;
    }
    if (!key->isString())
        return false;
    constexpr unsigned maxIndexLength = 10;
    JSString* string = asString(key);
    StringImpl* text = string->tryGetValueImpl();
    if (text && (!text->length() || isASCIIDigit((*text)[0])))
        return false;
    if (!text || !text->isAtom()) {
        if (string->length() > JSString::maxLengthForOnStackResolve)
            return false;
        instance->lastAtomizedKey = key;
        shouldCacheByKeyCell = false;
        if (previouslyHashedString && text && text->hasHash())
            *previouslyHashedString = text;
        bool mayBeIndex = !text && string->length() <= maxIndexLength;
        text = atomizes ? string->toAtomString(instance->globalObject).data : string->toExistingAtomString(instance->globalObject).data;
        if (mayBeIndex && (!text || isASCIIDigit((*text)[0])))
            return false;
    } else if (key == instance->lastAtomizedKey) {
        shouldCacheByKeyCell = false;
        instance->lastAtomizedKey = nullptr;
    }
    uid = static_cast<UniquedStringImpl*>(text);
    return uid != instance->vm->propertyNames->underscoreProto.impl();
}

static bool cacheAbsentStringKey(Instance* instance, JSObject* base, JSCell* key, const StringImpl& text)
{
    bool isPermanent = !base->structure()->isDictionary();
    for (JSObject* holder = base;;) {
        Structure* structure = holder->structure();
        if (!structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto())
            return false;
        if (holder != base)
            isPermanent &= !structure->isDictionary() && structure->hasImmutableProperties();
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            break;
        holder = asObject(prototype);
    }
    Structure* structure = base->structure();
    bool isDictionary = structure->isDictionary();
    auto& read = instance->keyedReadFor(structure->id(), key);
    read.slot.structureID = StructureID();
    read.slot.pointer = key;
    read.holder = isDictionary ? std::bit_cast<JSCell*>(static_cast<uintptr_t>(structure->dictionaryPropertyCount())) : nullptr;
    read.epoch = isPermanent ? MegamorphicCache::permanentEpoch : instance->vm->ensureMegamorphicCache().epochForName(text, structure->id());
    if (isDictionary)
        read.epoch |= Instance::epochIsDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
    read.slot.offset = isPermanent ? Instance::keyIsPermanentlyAbsent : Slot::isGetter | Slot::pointerIsCell;
    read.slot.structureID = structure->id();
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetByKeyGeneric, EncodedJSValue, (JSCell* base, JSCell* key, Instance* instance))
{
    recordLookup(instance, "operationAOTGetByKeyGeneric", base, key);
    if (instance->keyedReadsByNameUntilCacheAttempt < 0) {
        instance->keyedTableRandomState = instance->keyedTableRandomState * 1664525u + 1013904223u;
        instance->keyedReadsByNameUntilCacheAttempt = 7 + (instance->keyedTableRandomState >> 29);
    }
    auto answer = [&](const char* how, JSValue result) {
        countOperationNamed(instance, "operationAOTGetByKeyGeneric", result ? how : "cannot-say");
        return JSValue::encode(result);
    };
    JSType type = base->type();
    if (type != FinalObjectType && !isExactlyJSArray(base) && type != JSFunctionType && type != ObjectType)
        return answer(nullptr, { });
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    const StringImpl* previouslyHashedString = nullptr;
    if (!cache || !propertyNameForKeyGeneric(instance, key, uid, shouldCacheByKeyCell, false, &previouslyHashedString) || (type == JSFunctionType && key->isSymbol()))
        return answer(nullptr, { });
    if (!uid) {
        JSValue result = getFromPrototypeChainGeneric(asObject(base), nullptr);
        if (!result || !previouslyHashedString || !cacheAbsentStringKey(instance, asObject(base), key, *previouslyHashedString))
            return answer("no-such-name", result);
        return answer("no-such-name-and-cached", result);
    }
    Structure* structure = base->structure();
    if (type == ObjectType) {
        if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind)) {
            auto* typedObject = uncheckedDowncast<Typed::Object>(base);
            const Typed::FieldData* field = typedObject->fieldNamed(typedObject->state(), uid);
            if (JSObject* extra = field ? nullptr : typedObject->extraProperties()) {
                unsigned attributes;
                PropertyOffset offset = extra->structure()->get(*instance->vm, uid, attributes);
                if (isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
                    if (auto* read = shouldCacheByKeyCell && !extra->structure()->isDictionary() ? instance->allocateKeyedRead(extra->structureID(), key) : nullptr; read && read->slot.structureID != extra->structureID())
                        cacheKeyedAccess(read->slot, extra->structure(), key, offset);
                    return answer("beyond-the-layout", extra->getDirect(offset));
                }
            }
            if (!shouldCacheByKeyCell)
                return answer("a-field", field ? typedObject->readInline(*field) : JSValue());
        }
        return lookUpInPrototypeChainGeneric(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) {
            if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
                return answer(nullptr, { });
            JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
            if (!result)
                return answer(nullptr, { });
            if (Options::validateAOTTypedFacts()) [[unlikely]]
                validateLookupResult(asObject(base), uid, isValidOffset(offset), result);
            if (!shouldCacheByKeyCell)
                return answer("of-an-object-that-is-not-final", result);
            if (!isValidOffset(offset))
                cacheAbsentStringKey(instance, asObject(base), key, *uid);
            else if (auto* read = holder == base && structure->propertyAccessesAreCacheable() ? instance->allocateKeyedRead(structure->id(), key) : nullptr)
                cacheKeyedAccess(read->slot, structure, key, offset);
            return answer("of-an-object-that-is-not-final-and-offered-to-the-table", result);
        });
    }
    JSValue result;
    if (type == FinalObjectType && !structure->compiledLayout()) {
        PropertyOffset offset;
        unsigned attributes;
        unsigned stepsBack = 0;
        unsigned hintHits = 0;
        unsigned hintMisses = 0;
        bool isExcluded = false;
        if (findGeneric<false>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses) && isValidOffset(offset) && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)))
            result = asObject(base)->getDirect(offset);
        if (result) {
            if (Options::validateAOTTypedFacts()) [[unlikely]]
                validateLookupResult(asObject(base), uid, true, result);
            bool isCacheable = structure->propertyAccessesAreCacheable();
            if (instance->lastAtomizedKey != key && isCacheable && !structure->isDictionary() && offset <= MegamorphicCache::maxOffset && !cache->findLoad(structure->id(), uid))
                cache->initAsHit(structure->id(), uid, base, offset, true);
            auto* read = shouldCacheByKeyCell && isCacheable ? instance->allocateKeyedRead(structure->id(), key) : nullptr;
            if (!read)
                return answer("own", result);
            cacheKeyedAccess(read->slot, structure, key, offset);
            return answer("own-and-cached", result);
        }
    }
    const MegamorphicCache::LoadEntry* found = cache->findLoad(structure->id(), uid);
    if (found) {
        JSCell* holder = found->m_holder;
        result = holder ? valueInHolder(asObject(holder == JSCell::seenMultipleCalleeObjects() ? base : holder), found->m_offset) : jsUndefined();
    } else if (structure->isDictionary())
        return answer("along-the-chain-of-a-dictionary", shouldCacheByKeyCell ? JSValue() : getFromPrototypeChainGeneric(asObject(base), uid));
    else {
        result = getGenericAndCache(instance, base, uid, false);
        if (result)
            found = cache->findLoad(structure->id(), uid);
    }
    if (!found || !shouldCacheByKeyCell)
        return answer("inherited-or-absent", result);
    cacheKeyedRead(instance, structure, key, *found);
    return answer("inherited-or-absent-and-cached", result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasByKeyGeneric, size_t, (JSCell* base, JSCell* key, Instance* instance))
{
    auto answer = [&](const char* how, size_t result) {
        countOperationNamed(instance, "operationAOTHasByKeyGeneric", result != Instance::cannotLookUp ? how : "cannot-say");
        return result;
    };
    JSType type = base->type();
    if (type != FinalObjectType && !isExactlyJSArray(base))
        return answer(nullptr, Instance::cannotLookUp);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    const StringImpl* previouslyHashedString = nullptr;
    if (!cache || !propertyNameForKeyGeneric(instance, key, uid, shouldCacheByKeyCell, false, &previouslyHashedString))
        return answer(nullptr, Instance::cannotLookUp);
    auto searchPrototypeChain = [&] {
        return lookUpInPrototypeChainGeneric(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned) -> size_t {
            if (Options::validateAOTTypedFacts() && holder && uid) [[unlikely]]
                validateLookupResult(asObject(base), uid, isValidOffset(offset), { });
            return holder ? static_cast<size_t>(isValidOffset(offset)) : Instance::cannotLookUp;
        });
    };
    if (!uid)
        return answer("no-such-name", previouslyHashedString ? Instance::cannotLookUp : searchPrototypeChain());
    Structure* structure = base->structure();
    const MegamorphicCache::LoadEntry* found = cache->findLoad(structure->id(), uid);
    if (!found) {
        if (structure->isDictionary() || !getGenericAndCache(instance, base, uid, false))
            return answer("along-the-chain", shouldCacheByKeyCell ? Instance::cannotLookUp : searchPrototypeChain());
        found = cache->findLoad(structure->id(), uid);
        if (!found)
            return answer(nullptr, Instance::cannotLookUp);
    }
    bool result = found->m_holder;
    if (!shouldCacheByKeyCell)
        return answer("by-the-structure", result);
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = found->m_epoch | static_cast<uint32_t>(result) << Instance::keyedHasResultShift;
    entry.pointer = key;
    entry.structureID = structure->id();
    return answer("by-the-structure-and-cached", result);
}

bool findOwnPropertyGeneric(JSValue base, UniquedStringImpl* uid, PropertyOffset& offset, unsigned& attributes)
{
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return false;
    Structure* structure = base.asCell()->structure();
    if (structure->isDictionary() || structure->compiledLayout() || structure->typeInfo().prohibitsPropertyCaching() || !structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence())
        return false;
    unsigned stepsBack = 0;
    unsigned hintHits = 0;
    unsigned hintMisses = 0;
    bool isExcluded = false;
    if (!findGeneric<false>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses))
        return false;
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        unsigned attributesInTable;
        RELEASE_ASSERT(structure->getConcurrently(uid, attributesInTable) == offset && (!isValidOffset(offset) || attributesInTable == attributes));
    }
    return true;
}

bool findOwnPropertyByKeyGeneric(Instance* instance, JSValue base, JSValue key, UniquedStringImpl*& uid, PropertyOffset& offset, unsigned& attributes)
{
    bool shouldCacheByKeyCell;
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !key.isCell() || !propertyNameForKeyGeneric(instance, key.asCell(), uid, shouldCacheByKeyCell, true))
        return false;
    if (uid)
        return findOwnPropertyGeneric(base, uid, offset, attributes);
    offset = invalidOffset;
    attributes = 0;
    return !base.asCell()->structure()->compiledLayout();
}

static void cacheKeyedReplacementOfExtraProperty(Instance* instance, Structure* structure, JSCell* key, PropertyOffset offset)
{
    if (!structure->propertyAccessesAreCacheable() || !propertyLocation(offset))
        return;
    auto* read = instance->allocateKeyedRead(structure->id(), key);
    if (!read)
        return;
    cacheKeyedAccess(read->slot, structure, key, offset);
    read->slot.offset &= ~Instance::keyMayNotBeEvicted;
}

static void cacheKeyedReplacement(Instance* instance, Structure* structure, JSCell* key, PropertyOffset offset)
{
    const Typed::LayoutData* layout = structure->compiledLayout();
    if (!structure->propertyAccessesAreCacheable() || !propertyLocation(offset) || (layout && Typed::isObjectLayout(layout->kind)))
        return;
    auto* entry = instance->allocateKeyedAddition(structure->id(), key);
    if (!entry)
        return;
    entry->slot.structureID = StructureID();
    entry->slot.offset = *propertyLocation(offset);
    entry->slot.pointer = key;
    entry->newStructureID = structure->id();
    entry->epoch = Instance::keyedExtraPropertyAdditionSeenOnce;
    entry->newStorageCapacity = 1;
    entry->holderValueCount = 1;
    entry->slot.structureID = structure->id();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPutByKeyGeneric, size_t, (JSCell* base, JSCell* key, EncodedJSValue encodedValue, Instance* instance))
{
    auto answer = [&](const char* how) -> size_t {
        countOperationNamed(instance, "operationAOTPutByKeyGeneric", how ? how : "left-to-the-operation");
        return !!how;
    };
    if (base->type() != FinalObjectType) {
        JSValue stored = JSValue::decode(encodedValue);
        const Typed::LayoutData* layout = base->type() == ObjectType ? base->structure()->compiledLayout() : nullptr;
        if (!layout || !Typed::isObjectLayout(layout->kind))
            return answer(nullptr);
        auto* typedObject = uncheckedDowncast<Typed::Object>(base);
        Typed::State& state = typedObject->state();
        UniquedStringImpl* name;
        bool shouldCacheByKeyCell;
        if (state.data.isCompiledForAudit || !propertyNameForKeyGeneric(instance, key, name, shouldCacheByKeyCell, true))
            return answer(nullptr);
        const Typed::FieldData* field = typedObject->fieldNamed(state, name);
        if (PropertyOffset offset; !field) {
            Structure* before = typedObject->structure();
            bool hadNoExtraProperties = !typedObject->extraProperties();
            auto* place = shouldCacheByKeyCell && (hadNoExtraProperties || before->extraPropertiesStructureID()) ? instance->freeKeyedAdditionEntry(before->id(), key) : nullptr;
            bool wasSeenBefore = place && place->slot.structureID == before->id() && place->slot.pointer == key && place->epoch >= Instance::keyedExtraPropertyAdditionSeenOnce;
            auto how = typedObject->tryStoreExtraProperty(*instance->vm, name, stored, offset, true, false, nullptr, wasSeenBefore);
            if (how == Typed::Object::ExtraPropertyStore::Added && wasSeenBefore)
                cacheKeyedExtraPropertyAddition(instance, before, typedObject->structure(), key, offset, hadNoExtraProperties);
            else if (how == Typed::Object::ExtraPropertyStore::Added && place)
                recordKeyedExtraPropertyAddition(*place, before, key);
            if (Structure* ofExtraProperties = how == Typed::Object::ExtraPropertyStore::Replaced && shouldCacheByKeyCell ? typedObject->extraProperties()->structure() : nullptr; ofExtraProperties && !ofExtraProperties->isDictionary())
                cacheKeyedReplacementOfExtraProperty(instance, ofExtraProperties, key, offset);
            return answer(how != Typed::Object::ExtraPropertyStore::NotHandled ? "stored-beyond-the-layout" : nullptr);
        }
        if (stored.isObject() || typedObject->hasFlagsInOrderWord() || typedObject->isDiscriminant(*field) || !typedObject->readInline(*field) || !typedObject->write(*instance->vm, *field, stored))
            return answer(nullptr);
        if (!shouldCacheByKeyCell)
            return answer("stored-into-a-field");
        cacheKeyedCompiledFieldStore(instance, base, key, Identifier::fromUid(*instance->vm, name));
        return answer("stored-into-a-field-and-offered-to-the-table");
    }
    VM& vm = *instance->vm;
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (structure->compiledLayout() || structure->hasImmutableShape() || structure->inheritorsMayOverrideReadOnlyProperties() || !object->canPerformFastPutInlineExcludingProto())
        return answer(nullptr);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    if (!propertyNameForKeyGeneric(instance, key, uid, shouldCacheByKeyCell, true))
        return answer(nullptr);
    bool canOnlyReplace = structure->mayBePrototype();
    if (Structure* next = canOnlyReplace ? nullptr : structure->trySingleTransition(); next && next->transitionPropertyName() == uid && next->transitionKind() == TransitionKind::PropertyAddition && !next->transitionPropertyAttributes() && next->outOfLineCapacity() == structure->outOfLineCapacity()) {
        object->putDirectOffset(vm, next->transitionOffset(), JSValue::decode(encodedValue));
        object->setStructure(vm, next);
        if (!shouldCacheByKeyCell)
            return answer("added-by-the-only-transition");
        cacheKeyedAddition(instance, vm, structure, next, key, next->transitionOffset());
        return answer("added-by-the-only-transition-and-offered-to-the-table");
    }
    PropertyOffset offset;
    unsigned attributes;
    unsigned stepsBack = 0;
    unsigned hintHits = 0;
    unsigned hintMisses = 0;
    bool isExcluded = false;
    if (!findGeneric<false>(structure, uid, offset, attributes, stepsBack, isExcluded, hintHits, hintMisses))
        return answer(nullptr);
    JSValue value = JSValue::decode(encodedValue);
    if (isValidOffset(offset)) {
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || structure->isWatchingReplacement())
            return answer(nullptr);
        object->putDirectOffset(vm, offset, value);
        if (!shouldCacheByKeyCell || !structure->propertyAccessesAreCacheable())
            return answer("replaced");
        structure->setIsReplacedSilently(true);
        cacheKeyedReplacement(instance, structure, key, offset);
        return answer("replaced-and-cached");
    }
    if (canOnlyReplace || !structure->isStructureExtensible())
        return answer(nullptr);
    if (!structure->isDictionary() && (instance->emptyStructuresForKeyedStores[0] || instance->emptyStructuresForKeyedStores[1])) {
        Structure* first = structure;
        while (Structure* previous = first->previousID())
            first = previous;
        PropertyOffset place;
        if ((first == instance->emptyStructuresForKeyedStores[0] || first == instance->emptyStructuresForKeyedStores[1]) && !Structure::addPropertyTransitionToExistingStructure(structure, uid, 0, place) && !instance->wasTransitionRequestedBefore(structure->id(), uid, vm.random().getUint32()))
            object->convertToDictionary(vm);
    }
    PutPropertySlot slot(object, true);
    object->putDirect(vm, uid, value, slot);
    if (!shouldCacheByKeyCell || !slot.isCacheablePut() || slot.type() != PutPropertySlot::NewProperty)
        return answer("added");
    cacheKeyedAddition(instance, vm, structure, object->structure(), key, slot.cachedOffset());
    return answer("added-and-offered-to-the-table");
}

static bool canLookUpGeneric(JSCell* key, PropertyName name)
{
    return key->isSymbol() || (keyAtom(key) && !parseIndex(name));
}

static void cacheKeyedLookUp(Slot& entry, Structure* structure, JSCell* key, uint32_t flags)
{
    entry.structureID = StructureID();
    entry.offset = flags;
    entry.pointer = key;
    entry.structureID = structure->id();
}

static bool isCachedAsAbsentInDictionary(const Instance::KeyedRead& read, VM& vm, Structure* structure, JSCell* key)
{
    if (read.slot.structureID != structure->id() || read.slot.pointer != key || read.slot.offset != (Slot::isGetter | Slot::pointerIsCell) || (read.epoch & ~Instance::epochIsValidated) != (vm.ensureMegamorphicCache().epoch() | Instance::epochIsDictionary))
        return false;
    return std::bit_cast<uintptr_t>(read.holder) == structure->dictionaryPropertyCount();
}

static NEVER_INLINE void validateAbsence(JSObject* object, JSCell* key)
{
    if (UniquedStringImpl* uid = key->isSymbol() ? &asSymbol(key)->uid() : keyAtom(key))
        validateLookupResult(object, uid, false, { });
}

static void cacheAbsentInDictionary(Instance::KeyedRead& read, VM& vm, JSObject* base, Structure* structure, JSCell* key, PropertyName name)
{
    read.slot.structureID = StructureID();
    read.slot.offset = Slot::isGetter;
    read.slot.pointer = key;
    read.slot.structureID = structure->id();
    if (parseIndex(name))
        return;
    for (JSObject* holder = base;;) {
        Structure* holderStructure = holder->structure();
        if (TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) || holderStructure->compiledLayout())
            return;
        if (!holderStructure->propertyAccessesAreCacheable() || !holderStructure->propertyAccessesAreCacheableForAbsence() || !holderStructure->hasMonoProto())
            return;
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            break;
        holder = asObject(prototype);
    }
    read.holder = std::bit_cast<JSCell*>(static_cast<uintptr_t>(structure->dictionaryPropertyCount()));
    read.epoch = vm.ensureMegamorphicCache().epochForName(*name.uid(), structure->id()) | Instance::epochIsDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
    read.slot.offset = Slot::isGetter | Slot::pointerIsCell;
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> getByValFromDictionaryAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* object, JSValue property)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = object->structure();
    JSCell* key = property.asCell();
    auto& read = instance->keyedReadFor(structure->id(), key);
    Slot& entry = read.slot;
    if (isCachedAsAbsentInDictionary(read, vm, structure, key)) {
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateAbsence(object, key);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    auto name = nameForKeyedCache(globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!name) {
        cacheKeyedAccess(entry, structure, key, std::nullopt);
        scope.release();
        return getByVal(instance, globalObject, vm, object, property);
    }
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool isThere = object->getPropertySlot(globalObject, *name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = isThere ? slot.getValue(globalObject, *name) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (object->structure() != structure)
        OPERATION_RETURN(scope, JSValue::encode(result));
    if (isThere && slot.isCacheableValue() && slot.slotBase() == object && structure->propertyAccessesAreCacheable())
        cacheKeyedAccess(entry, structure, key, slot.cachedOffset());
    else if (isThere && !structure->hasBeenFlattenedBefore() && flattenOnce(vm, object))
        OPERATION_RETURN(scope, JSValue::encode(result));
    else if (!isThere && !structure->isUncacheableDictionary())
        cacheAbsentInDictionary(read, vm, object, structure, key, *name);
    else if (canLookUpGeneric(key, *name))
        cacheKeyedLookUp(entry, structure, key, Instance::keyNeedsLookup);
    else
        cacheKeyedAccess(entry, structure, key, std::nullopt);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static NEVER_INLINE OperationReturnType<size_t> hasPropertyInDictionaryAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* object, JSCell* key, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = object->structure();
    auto& read = instance->keyedReadFor(structure->id(), key);
    if (isCachedAsAbsentInDictionary(read, vm, structure, key)) {
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateAbsence(object, key);
        OPERATION_RETURN(scope, 0);
    }
    bool result = object->hasProperty(globalObject, name);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (object->structure() != structure)
        OPERATION_RETURN(scope, result);
    if (result && structure->propertyAccessesAreCacheable() && isValidOffset(structure->get(vm, name)))
        cacheKeyedHas(instance, vm, structure, key, true, true);
    else if (result && !structure->hasBeenFlattenedBefore() && flattenOnce(vm, object))
        OPERATION_RETURN(scope, result);
    else if (canLookUpGeneric(key, name))
        cacheKeyedLookUp(instance->keyedHasFor(structure->id(), key), structure, key, Instance::keyedHasNeedsLookup);
    else if (!result && !structure->isUncacheableDictionary())
        cacheAbsentInDictionary(read, vm, object, structure, key, name);
    OPERATION_RETURN(scope, result);
}

static std::optional<JSValue> tryGetByLongRopeKey(VM& vm, JSObject* base, JSString* key)
{
    unsigned length = key->length();
    unsigned remainingNames = 4096;
    Vector<StringView, 32> leaves;
    for (JSObject* holder = base;;) {
        Structure* structure = holder->structure();
        if (TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) || structure->compiledLayout() || TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags()))
            return std::nullopt;
        bool givesUp = false;
        std::optional<JSValue> found;
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (!remainingNames--) {
                givesUp = true;
                return false;
            }
            if (entry.key()->length() != length || entry.key()->isSymbol())
                return true;
            if (leaves.isEmpty() && !key->tryCollectLeaves(leaves)) {
                givesUp = true;
                return false;
            }
            StringView name { entry.key() };
            unsigned position = 0;
            for (auto& leaf : leaves) {
                if (name.substring(position, leaf.length()) != leaf)
                    return true;
                position += leaf.length();
            }
            if (entry.attributes() & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
                givesUp = true;
            else
                found = holder->getDirect(entry.offset());
            return false;
        });
        if (givesUp)
            return std::nullopt;
        if (found)
            return found;
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            return jsUndefined();
        holder = asObject(prototype);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    recordLookup(instance, __func__, base, property);
    if (auto name = findNameEqualToLongString(base, property)) [[unlikely]] {
        if (!*name)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        OPERATION_RETURN(scope, JSValue::encode(asObject(base)->get(globalObject, Identifier::fromUid(vm, *name))));
    }
    Structure* structure = base.asCell()->structure();
    auto isOrdinary = [](JSObject* object) {
        Structure* structure = object->structure();
        return !TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && !structure->compiledLayout();
    };
    if (property.isString() && base.isObject() && isOrdinary(asObject(base))) {
        JSString* key = asString(property);
        if (StringImpl* text = key->tryGetValueImpl(); !text || !text->isAtom()) {
            if (!text && key->length() > 2048) {
                if (auto result = tryGetByLongRopeKey(vm, asObject(base), key))
                    OPERATION_RETURN(scope, JSValue::encode(*result));
            }
            auto name = key->toExistingAtomString(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (name) {
                unsigned attributes;
                if (PropertyOffset offset = parseIndex(*name.data) ? invalidOffset : structure->get(vm, name.data, attributes); offset != invalidOffset && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))) {
                    cacheKeyedAccess(instance->keyedReadFor(structure->id(), key).slot, structure, key, offset);
                    OPERATION_RETURN(scope, JSValue::encode(asObject(base)->getDirect(offset)));
                }
            } else {
                auto flat = key->value(globalObject);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
                for (JSObject* holder = parseIndex(*flat.data.impl()) ? nullptr : asObject(base); holder && isOrdinary(holder) && !TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags());) {
                    JSValue prototype = holder->getPrototypeDirect();
                    if (!prototype.isObject())
                        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
                    holder = asObject(prototype);
                }
            }
        }
    }
    if (structure->isDictionary() && base.isObject()) [[unlikely]] {
        scope.release();
        return getByValFromDictionaryAndCache(instance, globalObject, vm, asObject(base), property);
    }
    auto& read = instance->keyedReadFor(structure->id(), property.asCell());
    Slot& entry = read.slot;
    if (base.isObject()) {
        auto name = nameForKeyedCache(globalObject, property);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!name && structure->compiledLayout() && property.isString()) {
            Identifier stringIdentifier = asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*stringIdentifier.impl()))
                name = WTF::move(stringIdentifier);
        }
        if (name) {
            JSObject* object = asObject(base);
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, *name, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            bool isInlineValue = slot.isCacheableValue() && slot.slotBase() == object && object->structure() == structure;
            cacheKeyedAccess(entry, structure, property.asCell(), isInlineValue ? std::optional(slot.cachedOffset()) : std::nullopt);
            if (object->type() == GlobalProxyType && property.isSymbol())
                cacheKeyedLookUp(entry, structure, property.asCell(), Instance::keyNeedsLookup);
            if (slot.isCacheableGetter() && slot.slotBase() == object && object->structure() == structure && structure->propertyAccessesAreCacheable() && !structure->compiledLayout()) {
                if (auto location = propertyLocation(slot.cachedOffset()))
                    entry.offset = (*location & Slot::offsetMask) | Instance::keyIsOwnAccessor;
            }
            if (object->structure() == structure && structure->compiledLayout())
                cacheKeyedCompiledFieldRead(globalObject, read, object, *name);
            if (auto* found = isInlineValue || object->structure() != structure ? nullptr : vm.ensureMegamorphicCache().findLoad(structure->id(), name->impl()); found && found->m_holder != JSCell::seenMultipleCalleeObjects()) {
                if (auto location = propertyLocation(found->m_offset)) {
                    read.holder = found->m_holder;
                    read.epoch = found->m_epoch;
                    entry.offset = !found->m_holder && found->m_epoch == MegamorphicCache::permanentEpoch ? Instance::keyIsPermanentlyAbsent : *location | Slot::isGetter | Slot::pointerIsCell;
                }
            }
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    }
    cacheKeyedAccess(entry, structure, property.asCell(), std::nullopt);
    scope.release();
    return getByVal(instance, globalObject, vm, base, property);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        instance->validateKeyedTables();
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    JSObject* object = asObject(base);
    if (auto name = findNameEqualToLongString(base, property)) [[unlikely]]
        OPERATION_RETURN(scope, !!*name);
    UniquedStringImpl* atom = keyAtom(property);
    if (!atom && property.isString()) {
        auto name = asString(property)->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        if (!name) {
            auto flat = asString(property)->value(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, 0);
            bool dependsOnlyOnStructure = !object->structure()->isDictionary();
            for (JSObject* holder = parseIndex(*flat.data.impl()) ? nullptr : object; holder && !TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) && !TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags());) {
                Structure* holderStructure = holder->structure();
                if (holderStructure->compiledLayout())
                    break;
                dependsOnlyOnStructure &= holderStructure->propertyAccessesAreCacheable() && holderStructure->propertyAccessesAreCacheableForAbsence() && holderStructure->hasMonoProto();
                JSValue prototype = holder->getPrototypeDirect();
                if (!prototype.isObject()) {
                    cacheKeyedHas(instance, vm, object->structure(), property.asCell(), dependsOnlyOnStructure, false);
                    OPERATION_RETURN(scope, 0);
                }
                holder = asObject(prototype);
            }
        }
    }
    Identifier key;
    if (!atom) {
        key = property.toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        if (!property.isString() && !property.isSymbol())
            OPERATION_RETURN(scope, object->hasProperty(globalObject, key));
        atom = key.impl();
    }
    while (JSObject* prototype = prototypeToAskBelowTypedObject(vm, object, atom)) {
        cacheKeyedLookUp(instance->keyedHasFor(object->structureID(), property.asCell()), object->structure(), property.asCell(), Instance::keyedHasAsksPrototype);
        object = prototype;
    }
    Structure* structure = object->structure();
    if (structure->isDictionary()) [[unlikely]] {
        scope.release();
        return hasPropertyInDictionaryAndCache(instance, globalObject, vm, object, property.asCell(), atom);
    }
    if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind) && structure->hasMonoProto()) {
        auto* ofLayout = uncheckedDowncast<Typed::Object>(object);
        const Typed::FieldData* field = ofLayout->fieldNamed(ofLayout->state(), atom);
        if (field && ofLayout->state().mayBeAbsent(*field) && !(field->flags & (Typed::fieldMayExistWithoutStorage | Typed::fieldMayBeUnlisted | Typed::fieldIsOutOfLine)) && lookupAboveRunsNoCode(object)) {
            bool isInherited = false;
            bool dependsOnlyOnStructures = true;
            bool isPermanent = true;
            if (JSValue prototype = object->getPrototypeDirect(); prototype.isObject()) {
                isInherited = hasPropertyAndFillMegamorphicCache(globalObject, asObject(prototype), atom, dependsOnlyOnStructures, &isPermanent);
                OPERATION_RETURN_IF_EXCEPTION(scope, 0);
                Structure* above = asObject(prototype)->structure();
                isPermanent &= above->hasImmutableProperties() && !above->isDictionary() && !above->isStructureExtensible();
            }
            if (isInherited && dependsOnlyOnStructures && object->structure() == structure)
                cacheKeyedHas(instance, vm, structure, property.asCell(), true, true, isPermanent);
            else if (dependsOnlyOnStructures && object->structure() == structure)
                cacheKeyedHasOfField(instance, vm, structure, property.asCell(), *field, isPermanent);
            OPERATION_RETURN(scope, isInherited || !!ofLayout->read(*field));
        }
    }
    if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind) && structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties) {
        auto* ofLayout = uncheckedDowncast<Typed::Object>(object);
        if (!structure->extraPropertiesStructureID()) {
            ofLayout->specializeStructureForExtraProperties(vm);
            structure = object->structure();
        }
        const Typed::FieldData* field = ofLayout->fieldNamed(ofLayout->state(), atom);
        if (field && !(field->flags & (Typed::fieldMayExistWithoutStorage | Typed::fieldMayBeUnlisted))) {
            if (ofLayout->read(*field)) {
                if (!ofLayout->state().mayBeAbsent(*field))
                    cacheKeyedHas(instance, vm, structure, property.asCell(), true, true, true);
                OPERATION_RETURN(scope, 1);
            }
            if (structure->markingStampAndFlags() & Structure::hasPrototypeOfLayout)
                OPERATION_RETURN(scope, 0);
        }
        if (unsigned attributes = 0; !field && Typed::Object::offsetOfExtraProperty(vm, object, atom, attributes)) {
            cacheKeyedHas(instance, vm, structure, property.asCell(), true, true, true);
            OPERATION_RETURN(scope, 1);
        }
    }
    bool dependsOnlyOnStructure;
    bool isPermanent;
    bool result = hasPropertyAndFillMegamorphicCache(globalObject, object, atom, dependsOnlyOnStructure, &isPermanent);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    cacheKeyedHas(instance, vm, structure, property.asCell(), dependsOnlyOnStructure && object->structure() == structure, result, isPermanent);
    OPERATION_RETURN(scope, result);
}

template<uint32_t isStrict>
static ALWAYS_INLINE OperationReturnType<void> putByValAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (auto name = findNameEqualToLongString(base, property); name && *name) [[unlikely]] {
        PutPropertySlot slot(base, isStrict);
        base.put(globalObject, Identifier::fromUid(vm, *name), JSValue::decode(encodedValue), slot);
        OPERATION_RETURN(scope);
    }
    if (base.isObject()) {
        UniquedStringImpl* atom = keyAtom(property);
        bool isNew = !atom && property.isString();
        std::optional<Identifier> name;
        if (atom || isNew) {
            Identifier stringIdentifier = atom ? Identifier::fromUid(vm, atom) : asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            if (!parseIndex(*stringIdentifier.impl()))
                name = WTF::move(stringIdentifier);
        } else
            name = nameForKeyedCache(globalObject, property);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (name) {
            JSObject* object = asObject(base);
            Structure* oldStructure = object->structure();
            PutPropertySlot slot(base, isStrict);
            base.put(globalObject, *name, JSValue::decode(encodedValue), slot);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            if (!isNew)
                fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, *name, slot);
            if (const Typed::LayoutData* layout = atom && object->structure() == oldStructure ? oldStructure->compiledLayout() : nullptr; layout && Typed::isObjectLayout(layout->kind)) {
                cacheKeyedCompiledFieldStore(instance, object, property.asCell(), *name);
                OPERATION_RETURN(scope);
            }
            bool isInlineReplace = slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == object && object->structure() == oldStructure && !isHostObjectOverridingPut(oldStructure);
            if (isInlineReplace)
                oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            if (slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.base() == object && !isHostObjectOverridingPut(oldStructure)) {
                cacheKeyedAddition(instance, vm, oldStructure, object->structure(), property.asCell(), slot.cachedOffset());
                OPERATION_RETURN(scope);
            }
            if (isInlineReplace)
                cacheKeyedReplacement(instance, oldStructure, property.asCell(), slot.cachedOffset());
            OPERATION_RETURN(scope);
        }
    }
    scope.release();
    return putByVal(instance, globalObject, vm, base, property, JSValue::decode(encodedValue), isStrict);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance);
    scope.release();
    return putByValAndCache<1>(instance, globalObject, vm, encodedBase, encodedProperty, encodedValue);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCacheSloppy, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance);
    scope.release();
    return putByValAndCache<0>(instance, globalObject, vm, encodedBase, encodedProperty, encodedValue);
}

enum class LongRopeOwnNameResult : uint8_t { Found, Absent, Unknown };

static LongRopeOwnNameResult findOwnNameForLongRope(VM& vm, JSObject* object, JSValue property, Identifier& name)
{
    if (!property.isString() || !asString(property)->isRope() || asString(property)->length() <= 2048)
        return LongRopeOwnNameResult::Unknown;
    Structure* structure = object->structure();
    if (object->methodTable()->deleteProperty != JSFinalObject::info()->methodTable.deleteProperty || structure->compiledLayout() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || object->isLockedBuiltin())
        return LongRopeOwnNameResult::Unknown;
    JSString* key = asString(property);
    unsigned length = key->length();
    unsigned remainingNames = 4096;
    Vector<StringView, 32> leaves;
    auto result = LongRopeOwnNameResult::Absent;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (!remainingNames--) {
            result = LongRopeOwnNameResult::Unknown;
            return false;
        }
        if (entry.key()->length() != length || entry.key()->isSymbol())
            return true;
        if (leaves.isEmpty() && !key->tryCollectLeaves(leaves)) {
            result = LongRopeOwnNameResult::Unknown;
            return false;
        }
        StringView text { entry.key() };
        unsigned position = 0;
        for (auto& leaf : leaves) {
            if (text.substring(position, leaf.length()) != leaf)
                return true;
            position += leaf.length();
        }
        name = Identifier::fromUid(vm, entry.key());
        result = LongRopeOwnNameResult::Found;
        return false;
    });
    return result;
}

static ALWAYS_INLINE void prepareDictionaryForKeyedDelete(VM& vm, JSObject* object, PropertyName name)
{
    Structure* structure = object->structure();
    if (!structure->isDictionary() || structure->isUncacheableDictionary() || structure->typeInfo().type() != FinalObjectType || structure->compiledLayout() || structure->mayBePrototype())
        return;
    unsigned attributes;
    if (isValidOffset(structure->get(vm, name, attributes)) && !(attributes & PropertyAttribute::DontDelete))
        object->convertToUncacheableDictionary(vm);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValAndCacheSloppy, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Identifier key;
    auto ownName = findOwnNameForLongRope(vm, object, property, key);
    if (ownName == LongRopeOwnNameResult::Absent)
        OPERATION_RETURN(scope, true);
    if (ownName == LongRopeOwnNameResult::Unknown)
        key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Typed::Object::specializeStructureForExtraPropertiesIfNeeded(vm, object);
    prepareDictionaryForKeyedDelete(vm, object, key);
    Structure* oldStructure = object->structure();
    uint32_t orderWordBefore = orderWordBeforeFieldDeletion(object);
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isDeleted && object->recordIfOnlyLockRejects(vm, key.impl(), false, true)) {
        throwTypeError(globalObject, scope, AOT::lockedOperationErrorMessage(vm, object, AOT::LockedOperation::Delete, key.impl()));
        OPERATION_RETURN(scope, 0);
    }
    if (isDeleted && base.isObject() && (property.isString() || property.isSymbol())) {
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
        cacheKeyedDeletion(instance, oldStructure, object->structure(), property, key.impl(), slot);
        cacheKeyedFieldDeletion(instance, object, oldStructure, orderWordBefore, property, key.impl());
    }
    OPERATION_RETURN(scope, isDeleted);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Identifier key;
    auto ownName = findOwnNameForLongRope(vm, object, property, key);
    if (ownName == LongRopeOwnNameResult::Absent)
        OPERATION_RETURN(scope);
    if (ownName == LongRopeOwnNameResult::Unknown)
        key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Typed::Object::specializeStructureForExtraPropertiesIfNeeded(vm, object);
    prepareDictionaryForKeyedDelete(vm, object, key);
    Structure* oldStructure = object->structure();
    uint32_t orderWordBefore = orderWordBeforeFieldDeletion(object);
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDeleted) {
        throwTypeError(globalObject, scope, object->recordIfOnlyLockRejects(vm, key.impl(), false, true) ? AOT::lockedOperationErrorMessage(vm, object, AOT::LockedOperation::Delete, key.impl()) : String(UnableToDeletePropertyError));
        OPERATION_RETURN(scope);
    }
    if (base.isObject() && (property.isString() || property.isSymbol())) {
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
        cacheKeyedDeletion(instance, oldStructure, object->structure(), property, key.impl(), slot);
        cacheKeyedFieldDeletion(instance, object, oldStructure, orderWordBefore, property, key.impl());
    }
    OPERATION_RETURN(scope);
}

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (void* address, uint32_t sizeAndIsStore, Instance* instance))
{
    void* returnAddress = __builtin_return_address(0);
    void* frame = __builtin_frame_address(1);
    dataLogLn("AOT: compiled code touches poisoned memory: function ", FunctionRef::at(instance, returnAddress).index, ", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
    __asan_report_error(returnAddress, frame, frame, address, sizeAndIsStore >> 8, sizeAndIsStore & 0xff);
    CRASH();
}
#endif

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM* vmPointer, JSCell* cell))
{
    countOperationNamed(*vmPointer, __func__);
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.writeBarrierSlowPath(cell);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
