/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTLockWords.h"
#include "AOTOperationHelpers.h"
#include "AOTTypedObject.h"
#include "AOTTypedOperations.h"
#include "CodeBlock.h"
#include "CommonSlowPaths.h"
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEqToText, size_t, (Instance* instance, EncodedJSValue encodedLeft, uint64_t text))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), instance->typed->makeStringConstant(vm, static_cast<uint32_t>(text))));
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
            // See Slot::FieldRead::NoField.
            constexpr uint64_t isNoField = static_cast<uint64_t>(Slot::FieldRead::NoField) << Slot::fieldReadShift;
            if (cache->structureID == base.asCell()->structureID() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags && std::bit_cast<uint64_t>(cache->pointer) == isNoField)
                OPERATION_RETURN(scope, JSValue::encode(value));
            noteNamedFieldRead(instance, base.asCell(), ident, identifierIndex);
            if (!SharedData::contains(cache))
                cacheCompiledField(vm, dataWithSlot(instance, callFrame, cache), cache, base.asCell(), ident, true);
            OPERATION_RETURN(scope, JSValue::encode(value));
        }
        if (base.isUndefinedOrNull()) [[unlikely]] {
            throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), false));
            OPERATION_RETURN(scope, encodedJSValue());
        }
    }
    noteNamedAccess(instance, __func__, base, { }, ident.impl());
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        countOperationNamed(instance, __func__, !base.isObject() ? "base-is-not-object" : vm.megamorphicCache() && vm.megamorphicCache()->findLoad(base.asCell()->structureID(), ident.impl()) ? "value-is-in-megamorphic-cache" : "value-is-not-in-megamorphic-cache");
        if (vm.megamorphicCache())
            runtimeTable(vm).countChangeOfMegamorphicCacheEpoch(vm.megamorphicCache()->epoch());
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
            noteCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!SharedData::contains(cache))
        cacheGetById(globalObject, dataWithSlot(instance, callFrame, cache), base, structureBefore, ident, slot, cache, true);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static ALWAYS_INLINE JSValue valueInHolder(JSObject* holder, PropertyOffset offset)
{
    return isInlineOffset(offset) ? holder->inlineStorageUnsafe()[offsetInInlineStorage(offset)].get() : holder->getDirect(offset);
}

static ALWAYS_INLINE void countMissSparedByEntryInTable(Instance* instance, CallFrame* callFrame)
{
    countOperationFor(instance, callFrame);
}

static const Identifier& identifierReadOfProxy(Instance* instance, CallFrame* callFrame, uint32_t name)
{
    return identifierAt(instance, callFrame, name & Instance::nameIsNumberOfIdentifier ? name & ~Instance::nameIsNumberOfIdentifier : PlaceTable::numberOfIdentifierNamed(name));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareGetOfProxy, uintptr_t, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue* results))
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
    const Identifier& ident = identifierReadOfProxy(instance, callFrame, name);
    if (ident.isSymbol())
        return decline("the name is no text");
    countOperationNamed(instance, "read of a proxy", "the trap is called from the stub");
    results[0] = JSValue::encode(trap);
    results[1] = JSValue::encode(handler);
    results[2] = JSValue::encode(proxy->target());
    results[3] = JSValue::encode(instance->keyHandedToTraps(vm, ident.impl()));
    return 1;
}

JSC_DEFINE_JIT_OPERATION(operationAOTValidateGetOfProxy, EncodedJSValue, (Instance* instance, JSCell* target, uint32_t name, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& ident = identifierReadOfProxy(instance, callFrame, name);
    Structure* structure = target->structure();
    bool lacksWhatItsStructureLacks = !structure->typeInfo().overridesGetOwnPropertySlot() || Typed::lacksWhatItsStructureLacks(vm, target, ident.impl(), true);
    if (lacksWhatItsStructureLacks && !structure->hasNonReifiedStaticProperties() && !parseIndex(ident)) {
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetByNameAndFillTable, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance);
    using Access = PlaceTable::Access;
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, PlaceTable::numberOfIdentifierNamed(name));
    noteLookup(instance, __func__, base, { }, ident.impl());
    PlaceTable& table = PlaceTable::of(vm);
    Data* data = table.dataFor(instance);
    if ((base.isString() || base.isSymbol() ? table.servesSeveralInstances() : base.isNumber() || base.isBoolean()) && ident != vm.propertyNames->length) {
        JSObject*& prototype = base.isString() ? instance->prototypeOfStrings : base.isSymbol() ? instance->prototypeOfSymbols : base.isNumber() ? instance->prototypeOfNumbers : instance->prototypeOfBooleans;
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
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    if (structureBefore && structureBefore->typeInfo().type() == ProxyObjectType) {
        countOperationNamed(instance, __func__, "proxy");
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident)));
    }
    bool staysDictionary = structureBefore && base.isObject() && structureBefore->isUncacheableDictionary() && structureBefore->hasBeenFlattenedBefore() && !structureBefore->compiledLayout();
    if (staysDictionary && base.asCell()->type() == FinalObjectType) {
        unsigned attributes = 0;
        PropertyOffset offset = structureBefore->get(vm, ident.impl(), attributes);
        if (isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
            countOperationNamed(instance, __func__, "a-dictionary-that-stays-one-has-it");
            OPERATION_RETURN(scope, JSValue::encode(asObject(base)->getDirect(offset)));
        }
    }
    bool receiverLacksIt = false;
    if (uint32_t rowPlusOne = base.isObject() ? structureBefore->numberOfRowPlusOne() : 0) {
        uint32_t identifier = PlaceTable::numberOfIdentifierNamed(name);
        uint32_t displacement = instance->displacementsOfNames[identifier];
        uint32_t nameAndPlace = instance->placesByNameAndRow[(displacement & ~Typed::Data::nameIsOnObjectPrototype) + rowPlusOne];
        if (nameAndPlace >> 8 == identifier) {
            PropertyOffset offset = nameAndPlace & 0xff;
            if (JSValue value = asObject(base)->getDirect(offset); value && table.setOwnPlace(structureBefore, name, offset, false)) {
                countMissSparedByEntryInTable(instance, callFrame);
                OPERATION_RETURN(scope, JSValue::encode(value));
            }
        } else if (rowPlusOne < instance->firstRowOfThisPlusOne && !(displacement & Typed::Data::nameIsOnObjectPrototype) && table.setAbsent(structureBefore, name, nullptr)) {
            countMissSparedByEntryInTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        } else
            receiverLacksIt = true;
    }
    if (auto* entry = structureBefore && vm.megamorphicCache() && (base.isObject() || !table.servesSeveralInstances()) ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr) {
        UniquedStringImpl* uidIfItDependsOnPrototypes = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : ident.impl();
        PropertyOffset offset = entry->m_offset;
        if (!entry->m_holder) {
            if (table.setAbsent(structureBefore, name, uidIfItDependsOnPrototypes))
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        } else if (entry->m_holder == JSCell::seenMultipleCalleeObjects()) {
            if (table.setOwnPlace(structureBefore, name, offset, false))
                OPERATION_RETURN(scope, JSValue::encode(asObject(base)->getDirect(offset)));
        } else if (JSObject* holder = asObject(entry->m_holder); table.setInherited(structureBefore, name, holder, offset, uidIfItDependsOnPrototypes))
            OPERATION_RETURN(scope, JSValue::encode(valueInHolder(holder, offset)));
    }
    if (instance->typed) {
        if (structureBefore && structureBefore->compiledLayout() && structureBefore->markingStampAndFlags() & Structure::isOfObjectsWithExtraProperties && !structureBefore->structureIDOfExtraProperties()) {
            uncheckedDowncast<Typed::Object>(base.asCell())->knowExtraPropertiesByStructure(vm);
            structureBefore = base.asCell()->structure();
        }
        if (unsigned attributes = 0; auto offset = structureBefore && structureBefore->compiledLayout() && structureBefore->structureIDOfExtraProperties() ? Typed::Object::offsetOfExtraProperty(vm, base.asCell(), ident, attributes) : std::nullopt) {
            if (!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && table.setExtraProperty(structureBefore, name, *offset, false, false))
                OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()->getDirect(*offset)));
        }
        if (JSValue value = base.isCell() ? Typed::Object::tryGetOwnData(vm, base.asCell(), ident) : JSValue()) {
            constexpr uint64_t isNoField = static_cast<uint64_t>(Slot::FieldRead::NoField) << Slot::fieldReadShift;
            Slot* cache = table.ensureSlot(structureBefore, name, Access::Read);
            if (cache && !(cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags && std::bit_cast<uint64_t>(cache->pointer) == isNoField))
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
        auto* nameSpace = uncheckedDowncast<Typed::ModuleNamespace>(base.asCell());
        unsigned number = nameSpace->numberOf(ident.impl());
        if (number != Typed::ModuleNamespace::noExport && nameSpace->isReadByNumberAtOnce() && !table.servesSeveralInstances()) {
            table.setExportOfNamespace(structureBefore, name, number);
            OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident)));
        }
        if (number == Typed::ModuleNamespace::noExport && nameSpace->isReadByNumberAtOnce() && !ident.isSymbol() && ident != vm.propertyNames->__esModule && !nameSpace->isBroughtByStar(ident.impl())) {
            JSValue prototype = structureBefore->storedPrototype();
            bool chainLacksIt = prototype.isNull();
            if (!chainLacksIt) {
                PropertySlot inquiry(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
                chainLacksIt = !asObject(prototype)->getPropertySlot(globalObject, ident, inquiry) && !inquiry.isTaintedByOpaqueObject();
                inquiry.disallowVMEntry.reset();
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
            if (chainLacksIt && table.setAbsent(structureBefore, name, prototype.isNull() ? nullptr : ident.impl()))
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    if (instance->typed && base.isObject() && base.asCell()->type() == FinalObjectType) {
        PropertyOffset offset = invalidOffset;
        bool isMadeByStubs = false;
        if (JSObject* closure = instance->typed->reifyLazyClosureIfAny(vm, asObject(base), ident.impl(), offset, isMadeByStubs)) {
            Structure* after = base.asCell()->structure();
            if (isMadeByStubs && !table.servesSeveralInstances() && storageAfterAddition(structureBefore, after, offset) == StorageAfterAddition::Stays)
                table.setLazyClosure(structureBefore, name, offset, after);
            OPERATION_RETURN(scope, JSValue::encode(closure));
        }
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    if (staysDictionary) {
        countOperationNamed(instance, __func__, "a-dictionary-that-stays-one");
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, ident, slot)));
    }
    JSValue result;
    if (!receiverLacksIt && tryGetFromOrdinaryObject(globalObject, base, ident.impl(), slot, result, false, &receiverLacksIt)) {
        if (slot.isCacheableValue() ? table.setOwnPlace(structureBefore, name, slot.cachedOffset(), false) : table.setAbsent(structureBefore, name, nullptr)) {
            countMissSparedByEntryInTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    } else {
        bool isLookedUp = false;
        auto findThroughUnmadePrototypes = [&] {
            return instance->typed && base.isObject() ? instance->typed->findThroughUnmadePrototypes(vm, asObject(base), ident.impl(), PlaceTable::numberOfIdentifierNamed(name), receiverLacksIt) : std::nullopt;
        };
        if (auto found = findThroughUnmadePrototypes(); found && found->member) {
            auto& place = *found->member;
            JSValue member = place.holder->getDirect(place.offset);
            if (!place.isAccessor) {
                table.setInherited(structureBefore, name, place.holder, place.offset, ident.impl());
                OPERATION_RETURN(scope, JSValue::encode(member));
            }
            auto* accessor = uncheckedDowncast<GetterSetter>(member.asCell());
            cacheShortcutToGetter(accessor);
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
                    countMissSparedByEntryInTable(instance, callFrame);
                    OPERATION_RETURN(scope, JSValue::encode(holder ? valueInHolder(holder, entry->m_offset) : jsUndefined()));
                }
            }
            instance->typed->didGetDeclaredMember = false;
            result = getByIdAndFillMegamorphicCache(globalObject, above, ident, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            auto foundNow = findThroughUnmadePrototypes();
            if (std::exchange(instance->typed->didGetDeclaredMember, false) || base.asCell()->structure() != structureBefore || !foundNow || foundNow->member || foundNow->firstObjectAbove != above || !vm.megamorphicCache())
                OPERATION_RETURN(scope, JSValue::encode(result));
            bool isInTable = false;
            if (auto* entry = vm.megamorphicCache()->findLoad(above->structureID(), ident.impl())) {
                if (!entry->m_holder)
                    isInTable = table.setAbsent(structureBefore, name, ident.impl());
                else
                    isInTable = table.setInherited(structureBefore, name, entry->m_holder == JSCell::seenMultipleCalleeObjects() ? above : entry->m_holder, entry->m_offset, ident.impl());
            } else if (auto* entry = vm.megamorphicCache()->findGetter(above->structureID(), ident.impl())) {
                if (slot.isCacheableGetter())
                    cacheShortcutToGetter(slot.getterSetter());
                isInTable = table.setAccessor(structureBefore, name, entry->m_holder == JSCell::seenMultipleCalleeObjects() ? above : entry->m_holder, entry->m_offset, ident.impl());
            }
            if (isInTable) {
                countMissSparedByEntryInTable(instance, callFrame);
                OPERATION_RETURN(scope, JSValue::encode(result));
            }
            isLookedUp = true;
        }
        if (base.isCell() && !isLookedUp) {
            if (auto* known = findKnownCustomGetter(vm, instance, base.asCell(), ident.impl())) {
                if (Slot* cache = table.ensureSlot(structureBefore, name, Access::Read)) {
                    cacheKnownCustomGetter(vm, *instance, data, cache, structureBefore, ident, *known);
                    table.setKnownCustomGetterIsInSlot(structureBefore, name, known->offsetOfCachedValue, ident.impl());
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
            noteCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!structureBefore || base.asCell()->structure() != structureBefore || (!base.isObject() && table.servesSeveralInstances()))
        OPERATION_RETURN(scope, JSValue::encode(result));
    if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr) {
        UniquedStringImpl* uidIfItDependsOnPrototypes = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : ident.impl();
        bool isInTable;
        if (!entry->m_holder)
            isInTable = table.setAbsent(structureBefore, name, uidIfItDependsOnPrototypes);
        else if (entry->m_holder == JSCell::seenMultipleCalleeObjects())
            isInTable = table.setOwnPlace(structureBefore, name, entry->m_offset, false);
        else
            isInTable = table.setInherited(structureBefore, name, entry->m_holder, entry->m_offset, uidIfItDependsOnPrototypes);
        if (isInTable) {
            countMissSparedByEntryInTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    } else if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findGetter(structureBefore->id(), ident.impl()) : nullptr) {
        bool isOwn = entry->m_holder == JSCell::seenMultipleCalleeObjects();
        if (slot.isCacheableGetter())
            cacheShortcutToGetter(slot.getterSetter());
        if (table.setAccessor(structureBefore, name, isOwn ? nullptr : entry->m_holder, entry->m_offset, isOwn ? nullptr : ident.impl())) {
            countMissSparedByEntryInTable(instance, callFrame);
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    }
    if (Slot* cache = table.ensureSlot(structureBefore, name, Access::Read)) {
        cacheGetById(globalObject, data, base, structureBefore, ident, slot, cache);
        bool isInheritedForGood = cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == (Slot::isIndirect | Slot::pointerIsCell) && slot.isCacheableValue() && cache->pointer == slot.slotBase() && slot.slotBase() != base.asCell() && !table.isWatched(cache);
        bool isOwn = cache->structureID == structureBefore->id() && !(cache->offset & (Slot::isGetter | Slot::pointerIsNotCell | Slot::pointerIsCell)) && slot.isCacheableValue() && slot.slotBase() == base.asCell() && !structureBefore->compiledLayout() && !table.isWatched(cache);
        auto* exportOfNamespace = slot.moduleNamespaceSlot() && vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr;
        if (exportOfNamespace && exportOfNamespace->m_epoch == MegamorphicCache::permanentEpoch && exportOfNamespace->m_holder && exportOfNamespace->m_holder != JSCell::seenMultipleCalleeObjects() && table.setInheritedInPlaceOfSlot(structureBefore, name, exportOfNamespace->m_holder, exportOfNamespace->m_offset))
            table.releaseSlot(structureBefore, name, Access::Read);
        else if ((isInheritedForGood && table.setInheritedInPlaceOfSlot(structureBefore, name, slot.slotBase(), slot.cachedOffset())) || (isOwn && table.setOwnPlace(structureBefore, name, slot.cachedOffset(), false)))
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

static NEVER_INLINE void numberStructureGrownInto(Instance* instance, Structure* from, Structure* to, uint32_t identifier, PropertyOffset offset)
{
    auto& data = *reinterpret_cast<const Typed::Data*>(instance->typedData);
    uint32_t number = from->numberOfRowPlusOne() - 1;
    if (to->previousID() != from || to->isDictionary() || to->numberOfRowPlusOne() || number >= data.firstNumberOfRowsOfThis || offset != static_cast<PropertyOffset>(data.row(number)[0] & 0xff) || to->globalObject() != instance->globalObject)
        return;
    std::span all { data.at<std::array<uint32_t, 3>>(data.rowsGrownIntoOffset), data.numberOfRowsGrownInto };
    std::array<uint32_t, 3> key { number, identifier, 0 };
    auto found = std::ranges::lower_bound(all, key);
    if (found != all.end() && (*found)[0] == number && (*found)[1] == identifier && (*found)[2] < UINT16_MAX)
        to->setNumberOfRowPlusOne((*found)[2] + 1);
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
        instance->typed->putLazyClosure(globalObject, base, ident, instance->typed->functionOfLazyClosure(value.asCell()));
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
    if (flagBits & Site::isOfRowOfThis && tryStoreIntoRowOfThis(globalObject, SharedData::contains(cache) ? nullptr : callerData(instance, callFrame), cache, base, ident, value))
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
    // See Slot::FieldWrite::isNoField.
    if (constexpr uint64_t isNoField = static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField) << 32; cache && oldStructure && cache->structureID == oldStructure->id() && cache->offset & Slot::pointerIsNotCell && std::bit_cast<uint64_t>(cache->pointer) == isNoField) {
        if (JSObject* extra = uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()) {
            unsigned attributes = 0;
            PropertyOffset offset = extra->structure()->get(vm, ident.impl(), attributes);
            if (isValidOffset(offset) && !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                extra->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        }
    }
    Typed::Object* markedObject = nullptr;
    const Typed::FieldData* fieldOfMarkedObject = nullptr;
    Typed::Object* objectThatLacksField = nullptr;
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
            markedObject = object;
            fieldOfMarkedObject = object->fieldNamed(object->state(), ident);
        }
        if (fieldOfMarkedObject && !object->read(*fieldOfMarkedObject)) {
            object->knowExtraPropertiesByStructure(vm);
            oldStructure = object->structure();
            if (instance->typed->tryCreateFieldAsBefore(vm, object, *fieldOfMarkedObject, value))
                OPERATION_RETURN(scope);
            objectThatLacksField = object;
            wordsBeforeCreation = wordsBefore;
        }
    }
    PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
    if (Options::useAOTOperationCounters() && cache && oldStructure && (cache->offset & Slot::attemptsMask) == Slot::attemptsMask) [[unlikely]] {
        auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl()) : nullptr;
        countOperationNamed(instance, __func__, !entry ? "abandoned-and-not-in-table" : entry->m_reallocating ? "abandoned-and-entry-reallocates" : "abandoned-and-in-table");
    }
    // No slot and no table will ever serve it: nothing is looked up and nothing is noted.
    bool isGivenUp = cache && !cache->structureID && (cache->offset & Slot::attemptsMask) == Slot::attemptsMask && !isDirect && oldStructure && oldStructure->typeInfo().overridesPut();
    if (MegamorphicCache* table = base.isObject() && !isGivenUp ? vm.megamorphicCache() : nullptr) {
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
    else if (markedObject)
        Typed::Object::putKnowingField(markedObject, globalObject, ident, value, slot, fieldOfMarkedObject);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (objectThatLacksField && objectThatLacksField->structure() == oldStructure && Typed::Object::didCreateOneField(static_cast<uint32_t>(wordsBeforeCreation), objectThatLacksField->orderWord()) && Typed::Object::tryGetOwnData(vm, objectThatLacksField, ident)) {
        if (cacheCreationOfField(instance, !cache || SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), objectThatLacksField, wordsBeforeCreation, ident, cache, true))
            OPERATION_RETURN(scope);
    }
    if (fieldOfMarkedObject && fieldOfMarkedObject->flags & Typed::fieldIsOutOfLine) {
        cachePutById(instance, SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), base, oldStructure, ident, slot, isDirect, cache);
        OPERATION_RETURN(scope);
    }
    if (oldStructure && slot.type() == PutPropertySlot::NewProperty && oldStructure->numberOfRowPlusOne() && slot.base() == base.asCell()) [[unlikely]]
        numberStructureGrownInto(instance, oldStructure, base.asCell()->structure(), identifierIndex, slot.cachedOffset());
    // Also where the slot is nobody's: a shared one reads as given up.
    if (!isDirect && base.isObject() && oldStructure->compiledLayout() && base.asCell()->structure() == oldStructure)
        noteNamedFieldWrite(instance, base.asCell(), ident, identifierIndex);
    if (isGivenUp)
        OPERATION_RETURN(scope);
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if (!isDirect && slot.isCacheableSetter() && base.isObject())
        noteInheritedSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot);
    if (!isDirect && slot.isCustomAccessor() && slot.isCacheableCustom() && base.isObject())
        noteNativeSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot);
    cachePutById(instance, SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

static bool isFunctionThatTakesStoresAsAnOrdinaryObject(VM& vm, JSValue base, Structure* structure, const Identifier& ident)
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

static bool arrayDefinesNameLikeOrdinaryObject(VM& vm, JSValue base, Structure* structure, const Identifier& ident)
{
    return base.isCell() && isInstanceOfJSArrayItself(base.asCell()) && ident != vm.propertyNames->length && !parseIndex(ident) && structure->isStructureExtensible();
}

static std::optional<uint32_t> flagsOfStructureAfterAddition(Structure* before, Structure* after, PropertyOffset offset, bool isFunction)
{
    StorageAfterAddition storage = storageAfterAddition(before, after, offset);
    if (storage == StorageAfterAddition::Stays)
        return 0;
    if (storage == StorageAfterAddition::IsNotForStubs || (before->typeInfo().type() == JSFunctionType && !isFunction) || before->compiledLayout())
        return std::nullopt;
    if (storage == StorageAfterAddition::IsFirst)
        return PlaceTable::needsFirstStorage;
    constexpr unsigned maxCapacityGrownByStub = 32;
    return before->outOfLineCapacity() <= maxCapacityGrownByStub ? std::optional(PlaceTable::growsStorage) : std::nullopt;
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByNameAndFillTable, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t name, uint32_t flagBits))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, PlaceTable::numberOfIdentifierNamed(name));
    noteLookup(instance, __func__, base, { }, ident.impl());
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
                countOperationNamed(instance, __func__, "a-dictionary-that-stays-one-has-it");
                oldStructure->didReplaceProperty(offset);
                asObject(base)->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
            countOperationNamed(instance, __func__, "a-dictionary-that-stays-one");
            PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
            base.putInline(globalObject, ident, value, slot);
            OPERATION_RETURN(scope);
        }
        oldStructure = base.asCell()->structure();
    }
    if (uint32_t rowPlusOne = base.isObject() ? oldStructure->numberOfRowPlusOne() : 0; rowPlusOne && !oldStructure->mayBePrototype()) {
        uint32_t identifier = PlaceTable::numberOfIdentifierNamed(name);
        uint32_t nameAndPlace = instance->placesByNameAndRow[(instance->displacementsOfNames[identifier] & ~Typed::Data::nameIsOnObjectPrototype) + rowPlusOne];
        PropertyOffset offset = nameAndPlace & 0xff;
        if (nameAndPlace >> 8 == identifier && asObject(base)->getDirect(offset) && PlaceTable::of(vm).setOwnPlace(oldStructure, name, offset, true)) {
            oldStructure->didCachePropertyReplacement(vm, offset);
            asObject(base)->putDirectOffset(vm, offset, value);
            countMissSparedByEntryInTable(instance, callFrame);
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
            PlaceTable::of(vm).setArrayLength(oldStructure, name, hasDouble(mode));
    }
    if (Slot* givenUp = !isDirect && base.isObject() ? PlaceTable::of(vm).slotIfAny(oldStructure, name, PlaceTable::Access::Store) : nullptr; givenUp && !givenUp->structureID && (givenUp->offset & Slot::attemptsMask) == Slot::attemptsMask) {
        PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
        base.putInline(globalObject, ident, value, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (slot.type() == PutPropertySlot::NewProperty && oldStructure->numberOfRowPlusOne() && slot.base() == base.asCell()) [[unlikely]]
            numberStructureGrownInto(instance, oldStructure, base.asCell()->structure(), PlaceTable::numberOfIdentifierNamed(name), slot.cachedOffset());
        OPERATION_RETURN(scope);
    }
    bool definesOnArray = isDirect && arrayDefinesNameLikeOrdinaryObject(vm, base, oldStructure, ident);
    if (definesOnArray && !oldStructure->isDictionary() && !oldStructure->mayBePrototype() && !isCopyOnWrite(oldStructure->indexingMode())) {
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
    PlaceTable& table = PlaceTable::of(vm);
    PlaceTable::Access access = isDirect ? PlaceTable::Access::Define : PlaceTable::Access::Store;
    bool isOfCompiledLayout = oldStructure && oldStructure->compiledLayout();
    auto enterReplacement = [&](PropertyOffset offset) {
        unsigned attributes = 0;
        if (isHostObjectThatOverridesPut(oldStructure) || oldStructure->get(vm, ident, attributes) != offset || attributes)
            return false;
        oldStructure->didCachePropertyReplacement(vm, offset);
        return table.setOwnPlace(oldStructure, name, offset, true);
    };
    auto flagsIfAdditionMayBeEntered = [&](Structure* newStructure, PropertyOffset offset) -> std::optional<uint32_t> {
        if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || isHostObjectThatOverridesPut(oldStructure) || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
            return std::nullopt;
        bool isFunction = !isDirect && isFunctionThatTakesStoresAsAnOrdinaryObject(vm, base, oldStructure, ident);
        if (!isDirect && !isFunction && !storesAreOrdinaryAlongPrototypeChain(oldStructure))
            return std::nullopt;
        return flagsOfStructureAfterAddition(oldStructure, newStructure, offset, isFunction);
    };
    bool mayBeServedByEntryOfMegamorphicCache = base.isObject() && !isOfCompiledLayout && vm.megamorphicCache() && (!isDirect || base.asCell()->type() == FinalObjectType) && storeMayBeCachedByStructureAndName(vm, oldStructure, ident.impl());
    if (auto* entry = mayBeServedByEntryOfMegamorphicCache ? vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl()) : nullptr; entry && !(entry->m_reallocating & MegamorphicCache::StoreEntry::addsLazyClosure)) {
        JSObject* object = asObject(base);
        PropertyOffset offset = entry->m_offset;
        if (entry->m_newStructureID == oldStructure->id()) {
            if (enterReplacement(offset)) {
                object->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        } else if (Structure* newStructure = WTF::opaque(entry->m_newStructureID.decode()); auto flags = flagsIfAdditionMayBeEntered(newStructure, offset)) {
            if (table.setAdded(oldStructure, name, access, newStructure, offset, *flags, isDirect ? nullptr : ident.impl())) {
                if (*flags)
                    growStorageAndPut(vm, object, oldStructure, *entry, value);
                else {
                    object->putDirectOffset(vm, offset, value);
                    object->setStructure(vm, newStructure);
                }
                if (!isInlineOffset(offset))
                    instance->noteOutOfLineProperty(newStructure);
                if (oldStructure->numberOfRowPlusOne()) [[unlikely]]
                    numberStructureGrownInto(instance, oldStructure, newStructure, PlaceTable::numberOfIdentifierNamed(name), offset);
                OPERATION_RETURN(scope);
            }
        }
    }
    auto* objectOfLayout = isOfCompiledLayout && Typed::isObjectLayout(oldStructure->compiledLayout()->kind) ? uncheckedDowncast<Typed::Object>(base.asCell()) : nullptr;
    const Typed::FieldData* fieldStoredTo = objectOfLayout ? objectOfLayout->fieldNamed(objectOfLayout->state(), ident) : nullptr;
    if (fieldStoredTo && !isDirect && objectOfLayout->hasExtraProperties() && !objectOfLayout->read(*fieldStoredTo)) {
        objectOfLayout->knowExtraPropertiesByStructure(vm);
        oldStructure = objectOfLayout->structure();
        if (instance->typed->tryCreateFieldAsBefore(vm, objectOfLayout, *fieldStoredTo, value))
            OPERATION_RETURN(scope);
    }
    if (objectOfLayout && !fieldStoredTo) {
        auto* object = objectOfLayout;
        object->knowExtraPropertiesByStructure(vm);
        oldStructure = object->structure();
        PropertyOffset offset = invalidOffset;
        bool hasDefaultAttributes = false;
        switch (object->tryStoreExtraProperty(vm, ident, value, offset, true, isDirect, &hasDefaultAttributes, true)) {
        case Typed::Object::ExtraPropertyStore::Replaced:
            table.setExtraProperty(oldStructure, name, offset, false, true, hasDefaultAttributes);
            OPERATION_RETURN(scope);
        case Typed::Object::ExtraPropertyStore::Added:
            table.setExtraPropertyAdded(oldStructure, name, object->structure(), offset, instance->typed->isOfObjectsWithoutExtraProperties(oldStructure), ident.impl(), isDirect);
            OPERATION_RETURN(scope);
        case Typed::Object::ExtraPropertyStore::NotServed:
            break;
        }
    }
    Structure* structureOfSlot = oldStructure;
    Slot* cache = isOfCompiledLayout ? table.ensureSlot(oldStructure, name, access) : nullptr;
    Data* data = table.dataFor(instance);
    if (constexpr uint64_t isNoField = static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField) << 32; cache && cache->structureID == oldStructure->id() && cache->offset & Slot::pointerIsNotCell && std::bit_cast<uint64_t>(cache->pointer) == isNoField) {
        if (JSObject* extra = uncheckedDowncast<Typed::Object>(base.asCell())->extraProperties()) {
            unsigned attributes = 0;
            PropertyOffset offset = extra->structure()->get(vm, ident.impl(), attributes);
            if (isValidOffset(offset) && !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                extra->putDirectOffset(vm, offset, value);
                OPERATION_RETURN(scope);
            }
        }
    }
    if (!isDirect && isOfCompiledLayout && base.asCell()->inherits<Typed::Object>()) {
        auto* object = uncheckedDowncast<Typed::Object>(base.asCell());
        uint64_t wordsBefore = object->orderAndShapeWords();
        if (instance->typed->storeByKnownTransition(vm, object, ident.impl(), value)) {
            if (cache && (cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
                cacheStoreTransition(instance, data, object, oldStructure, wordsBefore, ident, cache);
            OPERATION_RETURN(scope);
        }
    }
    auto* objectThatMayLackField = cache && fieldStoredTo ? objectOfLayout : nullptr;
    uint64_t wordsBeforeStore = objectThatMayLackField ? objectThatMayLackField->orderAndShapeWords() : 0;
    PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
    if (unsigned attributesOnArray = 0; definesOnArray && (oldStructure->get(vm, ident, attributesOnArray) == invalidOffset || !attributesOnArray))
        asObject(base)->putDirect(vm, ident, value, 0, slot);
    else if (unsigned attributes = 0; isDirect && instance->typed && (base.asCell()->type() != FinalObjectType || isOfCompiledLayout || !oldStructure->isStructureExtensible() || (oldStructure->get(vm, ident, attributes) != invalidOffset && attributes))) [[unlikely]]
        asObject(base)->createDataProperty(globalObject, ident, value, true);
    else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else if (objectOfLayout)
        Typed::Object::putKnowingField(objectOfLayout, globalObject, ident, value, slot, fieldStoredTo);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (objectThatMayLackField && objectThatMayLackField->structure() == oldStructure && Typed::Object::didCreateOneField(static_cast<uint32_t>(wordsBeforeStore), objectThatMayLackField->orderWord()) && Typed::Object::tryGetOwnData(vm, objectThatMayLackField, ident)) {
        if (Slot* slotOfStore = table.ensureSlot(oldStructure, name, access); slotOfStore && cacheCreationOfField(instance, data, objectThatMayLackField, wordsBeforeStore, ident, slotOfStore, !isDirect))
            OPERATION_RETURN(scope);
    }
    if (oldStructure && slot.type() == PutPropertySlot::NewProperty && oldStructure->numberOfRowPlusOne() && slot.base() == base.asCell()) [[unlikely]]
        numberStructureGrownInto(instance, oldStructure, base.asCell()->structure(), PlaceTable::numberOfIdentifierNamed(name), slot.cachedOffset());
    if (!oldStructure || oldStructure != structureOfSlot)
        OPERATION_RETURN(scope);
    if (base.asCell()->type() == JSFunctionType && slot.type() == PutPropertySlot::NewProperty && base.asCell()->structure()->previousID() != oldStructure)
        OPERATION_RETURN(scope);
    if (!isDirect && slot.isCacheableSetter() && base.isObject()) {
        if (auto offset = placeOfInheritedSetter(globalObject, asObject(base), oldStructure, ident, slot)) {
            if (isOfCompiledLayout)
                table.releaseSlot(oldStructure, name, access);
            if (table.setAccessor(oldStructure, name, slot.base(), *offset, ident.impl(), true))
                OPERATION_RETURN(scope);
        }
        if (unsigned attributes = 0; slot.base() == base.asCell() && !isOfCompiledLayout && oldStructure->propertyAccessesAreCacheable() && !isHostObjectThatOverridesPut(oldStructure) && oldStructure->get(vm, ident, attributes) == slot.cachedOffset() && attributes & PropertyAttribute::Accessor && table.setAccessor(oldStructure, name, nullptr, slot.cachedOffset(), nullptr, true))
            OPERATION_RETURN(scope);
    }
    if (!isDirect && slot.isCustomAccessor() && slot.isCacheableCustom() && base.isObject() && noteNativeSetter(globalObject, *instance, asObject(base), oldStructure, ident, slot))
        OPERATION_RETURN(scope);
    if (base.isObject() && !isOfCompiledLayout && slot.isCacheablePut() && slot.base() == base.asCell() && storeMayBeCachedByStructureAndName(vm, oldStructure, ident.impl())) {
        Structure* newStructure = base.asCell()->structure();
        PropertyOffset offset = slot.cachedOffset();
        bool isInTable = false;
        if (slot.type() == PutPropertySlot::ExistingProperty)
            isInTable = newStructure == oldStructure && enterReplacement(offset);
        else if (slot.type() == PutPropertySlot::NewProperty) {
            auto flags = flagsIfAdditionMayBeEntered(newStructure, offset);
            if (!isInlineOffset(offset) && !oldStructure->isDictionary() && !newStructure->isDictionary() && !oldStructure->mayBePrototype() && newStructure->previousID() == oldStructure)
                instance->noteOutOfLineProperty(newStructure);
            if (flags)
                isInTable = table.setAdded(oldStructure, name, access, newStructure, offset, *flags, isDirect ? nullptr : ident.impl());
        }
        if (isInTable) {
            countMissSparedByEntryInTable(instance, callFrame);
            OPERATION_RETURN(scope);
        }
    }
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if ((cache = table.ensureSlot(oldStructure, name, access)); cache && (cache->structureID || (cache->offset & Slot::attemptsMask) != Slot::attemptsMask))
        cachePutById(instance, data, base, oldStructure, ident, slot, isDirect, cache);
    if (fieldStoredTo && !isDirect && base.asCell()->structure() == oldStructure && Typed::Object::tryGetOwnData(vm, base.asCell(), ident)) {
        if (Slot* cacheOfReads = table.ensureSlot(oldStructure, name, PlaceTable::Access::Read)) {
            cacheCompiledField(vm, data, cacheOfReads, base.asCell(), ident, false);
            if (cacheOfReads->structureID == oldStructure->id() && (cacheOfReads->offset & Slot::flagsMask) == Slot::compiledFieldFlags)
                table.setField(oldStructure, name, std::bit_cast<uint64_t>(cacheOfReads->pointer));
        }
    }
    ensureStillAliveHere(structureOfSlot);
    OPERATION_RETURN(scope);
}

static JSValue findAndRememberFixedElement(Instance* instance, JSObject* object, uint32_t index)
{
    if (!hasAnyArrayStorage(object->indexingType()) || !isInstanceOfJSArrayItself(object) || index >> Instance::bitsOfIndexOfFixedElement)
        return { };
    SparseArrayValueMap* map = object->butterfly()->arrayStorage()->m_sparseMap.get();
    if (!map)
        return { };
    auto entry = map->find(index);
    constexpr unsigned neverChanges = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    if (entry == map->notFound() || (entry->attributes() & (neverChanges | PropertyAttribute::Accessor)) != neverChanges)
        return { };
    JSValue value = entry->get();
    instance->rememberFixedElement(object, index, value);
    return value;
}

// No operation calls another: each takes the frame of its caller for that of compiled code.
static OperationReturnType<EncodedJSValue> getByVal(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue base, JSValue property)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    noteNamedAccess(instance, "operationAOTGetByVal", base, property);
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
            if (JSValue result = findAndRememberFixedElement(instance, object, i))
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
    noteNamedAccess(instance, "operationAOTPutByVal", base, property);
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
                putIndexOfArray(globalObject, uncheckedDowncast<JSArray>(object), i, value, isStrict);
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

// The name a key that is a cell stands for, if it is one that a slot can be filled for: a symbol, or a string that is the name of some property already and no index.
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

static UniquedStringImpl* atomOfKey(JSValue key)
{
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text && text->isAtom() ? static_cast<UniquedStringImpl*>(text) : nullptr;
}

static uint16_t epochForFactAboutKey(VM& vm, JSCell* key, Structure* structure)
{
    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    const StringImpl* name = key->isSymbol() ? &asSymbol(key)->uid() : asString(key)->tryGetValueImpl();
    return name ? cache.epochForFactAbout(*name, structure->id()) : cache.epochForFactAboutSomeName(structure->id());
}

static void cacheKeyedHas(Instance* instance, VM& vm, Structure* structure, JSCell* key, bool goesByStructure, bool result, bool isForGood = false)
{
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = goesByStructure ? (isForGood ? MegamorphicCache::permanentEpoch : epochForFactAboutKey(vm, key, structure)) | static_cast<uint32_t>(result) << Instance::keyedHasAnswerShift : Slot::isGetter;
    entry.pointer = key;
    entry.structureID = structure->id();
}

// A store under the key has given the object a property it lacked. See Instance::KeyedAddition.
static void cacheKeyedAddition(Instance* instance, VM& vm, Structure* before, Structure* after, JSCell* key, PropertyOffset offset)
{
    if (before->isDictionary() || after->isDictionary())
        return;
    auto* place = instance->placeForKeyedAddition(before->id(), key);
    if (!place)
        return;
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->hasPolyProto();
    };
    StorageAfterAddition storage = storageAfterAddition(before, after, offset);
    if (!isPlain(before) || !isPlain(after) || storage == StorageAfterAddition::IsNotForStubs)
        return;
    auto& entry = *place;
    cacheKeyedAccess(entry.slot, before, key, offset);
    entry.newStructureID = after->id();
    entry.epoch = epochForFactAboutKey(vm, key, before);
    entry.capacityOfNewStorage = storage == StorageAfterAddition::Stays ? 0 : after->outOfLineCapacity();
}

template<typename Found>
static ALWAYS_INLINE auto lookUpPlainly(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
{
    if (object->type() == GlobalProxyType && uid->isSymbol() && object->classInfo() == JSGlobalProxy::info())
        object = uncheckedDowncast<JSGlobalProxy>(object)->target();
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        bool keepsSymbolsInStructureAlone = uid->isSymbol() && object->type() == GlobalObjectType && object->methodTable()->getOwnPropertySlot == JSGlobalObject::getOwnPropertySlot;
        if (!keepsSymbolsInStructureAlone && (info.overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags())))
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

static NEVER_INLINE void validateAnswer(JSObject* object, UniquedStringImpl* uid, bool isThere, JSValue value)
{
    JSGlobalObject* globalObject = object->globalObject();
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &globalObject->vm());
    bool has = object->getPropertySlot(globalObject, uid, slot);
    if (has == isThere && (!has || !value || (slot.isValue() && slot.getValue(globalObject, uid) == value)))
        return;
    dataLogLn("AOT: a keyed cache says ", isThere ? "there" : "nowhere", ", ", value, ", and the object says ", has ? "there" : "nowhere", ": ", JSValue(object), ", name ", uid);
    CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainly, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid))
{
    return lookUpPlainly(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) {
        if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            return encodedJSValue();
        JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateAnswer(asObject(base), uid, isValidOffset(offset), result);
        return JSValue::encode(result);
    });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasPlainly, size_t, (JSCell* base, UniquedStringImpl* uid))
{
    return lookUpPlainly(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned) {
        if (holder && Options::validateAOTTypedFacts()) [[unlikely]]
            validateAnswer(asObject(base), uid, isValidOffset(offset), { });
        return holder ? static_cast<size_t>(isValidOffset(offset)) : Instance::cannotLookUp;
    });
}

template<bool isCounting>
static ALWAYS_INLINE bool findPlainly(Structure* structure, UniquedStringImpl* uid, PropertyOffset& offset, unsigned& attributes, unsigned& stepsBack, bool& isRuledOut, unsigned& hintsRight, unsigned& hintsWrong)
{
    offset = invalidOffset;
    attributes = 0;
    if (structure->seenProperties().ruleOut(CompactPtr<UniquedStringImpl>::encode(uid))) {
        if constexpr (isCounting)
            isRuledOut = true;
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
                (std::bit_cast<uintptr_t>(fourBack) == fourLower ? hintsRight : hintsWrong)++;
        }
        if constexpr (isCounting)
            stepsBack++;
        current = previous;
    }
}

static NEVER_INLINE void validateAnswerOrView(JSObject* asked, JSObject* base, UniquedStringImpl* uid, bool isThere, JSValue value)
{
    validateAnswer(base, uid, isThere, value);
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

static JSObject* sourceOfLiveBinding(Instance* instance, JSObject* view, PropertyOffset offset, unsigned attributes)
{
    if (!(attributes & PropertyAttribute::CustomValue) || !offset)
        return nullptr;
    CustomGetterSetter* accessor = instance->typed->liveBindingAccessorIfMade();
    if (!accessor || view->getDirect(offset) != JSValue(accessor))
        return nullptr;
    return view->getDirect(0).getObject();
}

static NEVER_INLINE JSValue getPlainlyTheLongWay(Instance* instance, JSCell* asked, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    MegamorphicCache* cache = vm.megamorphicCache();
    JSObject* base = asObject(asked);
    bool isThere = false;
    bool isNumberOfExport = false;
    const char* why = nullptr;
    auto cannotSay = [&](const char* reason) -> JSValue {
        why = reason;
        return { };
    };
    auto answer = [&]() -> JSValue {
        if (asked->type() == ObjectType) {
            if (asked->classInfo() != Typed::ModuleNamespace::info())
                return cannotSay("cannot-say:not-ordinary");
            unsigned number = uncheckedDowncast<Typed::ModuleNamespace>(asked)->numberOf(uid);
            if (number == Typed::ModuleNamespace::noExport)
                return cannotSay("cannot-say:no-export");
            isNumberOfExport = true;
            return JSValue::decode(static_cast<EncodedJSValue>(1ull << 48 | number));
        }
        StructureID structureID = base->structureID();
        auto lookThrough = [&](JSObject* source) {
            base = source;
            structureID = source->structureID();
            return !structureID.decode()->isDictionary();
        };
        if (structureID.decode()->isDictionary()) {
            PropertyTable* table = structureID.decode()->materializedPropertyTable();
            if (!table)
                return cannotSay("cannot-say:dictionary");
            auto [offset, attributes] = table->get(uid);
            JSObject* source = isValidOffset(offset) ? sourceOfLiveBinding(instance, base, offset, attributes) : nullptr;
            if (!source)
                return cannotSay("cannot-say:dictionary");
            if (!lookThrough(source))
                return cannotSay("cannot-say:a-view-of-a-dictionary");
        }
        bool isLazyFunctionPropertyName = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype;
        bool goesByStructure = true;
        bool isForGood = true;
        for (JSObject* object = base;;) {
            Structure* structure = object->structure();
            TypeInfo info = structure->typeInfo();
            bool isClassObject = object->type() == JSFunctionType && info.getOwnPropertySlotIsImpureForPropertyAbsence() && object->classInfo() == Typed::Closure::info() && uncheckedDowncast<Typed::Closure>(object)->kind() == Typed::Format::FunctionKind::Constructor;
            bool goesByItsStructure = object == base && !isClassObject && info.getOwnPropertySlotIsImpureForPropertyAbsence() && Typed::lacksWhatItsStructureLacks(vm, object, uid);
            if (isClassObject) {
                const Typed::State& state = *structure->aotInstance()->typed;
                const Typed::LayoutData* ofClass = uncheckedDowncast<Typed::Closure>(object)->classData();
                if (ofClass && state.hasStaticMethodNamed(*ofClass, uid))
                    return cannotSay("cannot-say:a-static-of-the-class");
                goesByStructure = false;
            }
            bool looksForItself = info.overridesGetOwnPropertySlot() && (isLazyFunctionPropertyName || (!isInstanceOfJSArrayItself(object) && (object->type() != JSFunctionType || (info.getOwnPropertySlotIsImpureForPropertyAbsence() && !isClassObject && !goesByItsStructure)) && object != instance->globalObject->arrayPrototype()));
            if (looksForItself || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || !structure->propertyAccessesAreCacheable())
                return cannotSay("cannot-say:not-ordinary");
            if (object != base) {
                if (structure->isDictionary())
                    return cannotSay("cannot-say:a-dictionary-for-a-prototype");
                isForGood &= structure->hasImmutableProperties();
            }
            PropertyOffset offset;
            unsigned attributes;
            unsigned stepsBack = 0;
            unsigned hintsRight = 0;
            unsigned hintsWrong = 0;
            bool isRuledOut = false;
            if (!findPlainly<false>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong))
                return cannotSay("cannot-say:the-chain-cannot-say");
            if (isValidOffset(offset)) {
                if (attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || offset > MegamorphicCache::maxOffset) {
                    JSObject* source = object == asked ? sourceOfLiveBinding(instance, object, offset, attributes) : nullptr;
                    if (!source)
                        return cannotSay("cannot-say:accessor-or-custom");
                    if (!lookThrough(source))
                        return cannotSay("cannot-say:a-view-of-a-dictionary");
                    object = source;
                    continue;
                }
                JSValue result = object->getDirect(offset);
                if (!result)
                    return cannotSay("cannot-say:empty");
                isThere = true;
                if (!goesByStructure && object != base)
                    return result;
                if (isForGood && object != base)
                    cache->initAsPermanentHit(structureID, uid, object, offset);
                else
                    cache->initAsHit(structureID, uid, object, offset, object == base);
                return result;
            }
            if ((!structure->propertyAccessesAreCacheableForAbsence() && !isClassObject && !goesByItsStructure) || !structure->hasMonoProto())
                return cannotSay("cannot-say:not-cacheable-for-absence");
            JSValue prototype = structure->storedPrototype();
            if (!prototype.isObject())
                break;
            object = asObject(prototype);
        }
        if (!goesByStructure)
            return jsUndefined();
        if (isForGood)
            cache->initAsPermanentMiss(structureID, uid);
        else
            cache->initAsMiss(structureID, uid);
        return jsUndefined();
    };
    JSValue result = answer();
    if (Options::validateAOTTypedFacts() && result && !isNumberOfExport) [[unlikely]]
        validateAnswerOrView(asObject(asked), base, uid, isThere, result);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        countOperationNamed(instance, "plainly", isNumberOfExport ? "an-export" : result ? "answered" : why);
        if (result && !isNumberOfExport)
            countOperationNamed(instance, "plainly", asked != base ? "answered-through-a-view" : "answered-the-long-way");
    }
    return result;
}

template<bool isCounting>
static ALWAYS_INLINE JSValue getPlainlyAndFillWhile(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreePlaceByName)
{
    bool goesTheLongWay = false;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    StructureID structureID = base->structureID();
    bool isThere = false;
    unsigned stepsBack = 0;
    unsigned hintsRight = 0;
    unsigned hintsWrong = 0;
    bool isRuledOut = false;
    const char* why = nullptr;
    auto cannotSay = [&](const char* reason) ALWAYS_INLINE_LAMBDA -> JSValue {
        if constexpr (isCounting)
            why = reason;
        return { };
    };
    auto answer = [&]() ALWAYS_INLINE_LAMBDA -> JSValue {
        if (!cache)
            return cannotSay("cannot-say:no-megamorphic-cache");
        if (structureID.decode()->isDictionary()) {
            goesTheLongWay = true;
            return { };
        }
        bool isForGood = true;
        for (JSObject* object = asObject(base);;) {
            Structure* structure = object->structure();
            TypeInfo info = structure->typeInfo();
            if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || !structure->propertyAccessesAreCacheable()) {
                goesTheLongWay = true;
                return { };
            }
            if (object != base) {
                if (structure->isDictionary())
                    return cannotSay("cannot-say:a-dictionary-for-a-prototype");
                isForGood &= structure->hasImmutableProperties();
            }
            PropertyOffset offset;
            unsigned attributes;
            if (!findPlainly<isCounting>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong))
                return cannotSay("cannot-say:the-chain-cannot-say");
            if (isValidOffset(offset)) {
                if (attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || offset > MegamorphicCache::maxOffset) {
                    goesTheLongWay = object == base && attributes & PropertyAttribute::CustomValue;
                    return cannotSay("cannot-say:accessor-or-custom");
                }
                if (hasFreePlaceByName && object == base && !attributes && static_cast<unsigned>(offset) < Structure::numberOfSlotsWithPropertyNameIDs && isInlineOffset(offset) && structure->fieldIDInSlot(offset) < Structure::firstReservedPropertyNameID)
                    return cannotSay("cannot-say:a-place-by-name-is-free");
                JSValue result = object->getDirect(offset);
                if (!result)
                    return cannotSay("cannot-say:empty");
                isThere = true;
                if (isForGood && object != base)
                    cache->initAsPermanentHit(structureID, uid, object, offset);
                else
                    cache->initAsHit(structureID, uid, object, offset, object == base);
                return result;
            }
            if (!structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto())
                return cannotSay("cannot-say:not-cacheable-for-absence");
            JSValue prototype = structure->storedPrototype();
            if (!prototype.isObject())
                break;
            object = asObject(prototype);
        }
        if (isForGood)
            cache->initAsPermanentMiss(structureID, uid);
        else
            cache->initAsMiss(structureID, uid);
        return jsUndefined();
    };
    JSValue result = answer();
    if (goesTheLongWay) [[unlikely]]
        return getPlainlyTheLongWay(instance, base, uid);
    if (Options::validateAOTTypedFacts() && result) [[unlikely]]
        validateAnswer(asObject(base), uid, isThere, result);
    if constexpr (isCounting) {
        countOperationNamed(instance, "plainly", result ? "answered" : why);
        if (stepsBack && result)
            countOperationNamed(instance, "plainly", stepsBack <= 4 ? "walked-back:1-to-4" : stepsBack <= 8 ? "walked-back:5-to-8" : stepsBack <= 16 ? "walked-back:9-to-16" : stepsBack <= 32 ? "walked-back:17-to-32" : "walked-back:over-32");
        if (isRuledOut)
            countOperationNamed(instance, "plainly", "ruled-out");
        while (hintsRight--)
            countOperationNamed(instance, "plainly", "hint-right");
        while (hintsWrong--)
            countOperationNamed(instance, "plainly", "hint-wrong");
    }
    return result;
}

static NEVER_INLINE JSValue getPlainlyAndFillAndCount(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreePlaceByName)
{
    return getPlainlyAndFillWhile<true>(instance, base, uid, hasFreePlaceByName);
}

static ALWAYS_INLINE JSValue getPlainlyAndFill(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool hasFreePlaceByName)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        return getPlainlyAndFillAndCount(instance, base, uid, hasFreePlaceByName);
    return getPlainlyAndFillWhile<false>(instance, base, uid, hasFreePlaceByName);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainlyAtSite, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid, PolymorphicSlots* several))
{
    JSValue result = getPlainlyAndFill(several->owner->instance, base, uid, several->numberOfUsedInlineNameSlots < PolymorphicSlots::numberOfInlineSlotsByName);
    if (result)
        several->didArriveAtNoLesson();
    return JSValue::encode(result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainlyAndFill, EncodedJSValue, (JSCell* base, UniquedStringImpl* uid, Instance* instance))
{
    if (uid == instance->vm->propertyNames->underscoreProto.impl() || (uid->length() && isASCIIDigit((*uid)[0])))
        return encodedJSValue();
    return JSValue::encode(getPlainlyAndFill(instance, base, uid, false));
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
    read.slot.offset = !found.m_holder && found.m_epoch == MegamorphicCache::permanentEpoch ? Instance::keyIsAbsentForGood : *location | Slot::isGetter | Slot::pointerIsCell;
    read.slot.pointer = key;
    read.slot.structureID = structure->id();
}

template<typename Found>
static ALWAYS_INLINE auto lookUpInPrototypeChainPlainly(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
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
            unsigned hintsRight = 0;
            unsigned hintsWrong = 0;
            bool isRuledOut = false;
            if (!findPlainly<false>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong))
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

static JSValue getFromPrototypeChainPlainly(JSObject* base, UniquedStringImpl* uid)
{
    return lookUpInPrototypeChainPlainly(base, uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) -> JSValue {
        if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            return { };
        JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
        if (Options::validateAOTTypedFacts() && uid && result) [[unlikely]]
            validateAnswer(base, uid, isValidOffset(offset), result);
        return result;
    });
}

static ALWAYS_INLINE bool nameOfKeyPlainly(Instance* instance, JSCell* key, UniquedStringImpl*& uid, bool& shouldCacheByKeyCell, bool makesAtom = false, const StringImpl** textHashedBefore = nullptr)
{
    shouldCacheByKeyCell = true;
    if (key->isSymbol()) {
        uid = &asSymbol(key)->uid();
        return true;
    }
    if (!key->isString())
        return false;
    constexpr unsigned maxLengthOfIndex = 10;
    JSString* string = asString(key);
    StringImpl* text = string->tryGetValueImpl();
    if (text && (!text->length() || isASCIIDigit((*text)[0])))
        return false;
    if (!text || !text->isAtom()) {
        if (string->length() > JSString::maxLengthForOnStackResolve)
            return false;
        instance->keyGivenItsAtomLast = key;
        shouldCacheByKeyCell = false;
        if (textHashedBefore && text && text->hasHash())
            *textHashedBefore = text;
        bool mayBeIndex = !text && string->length() <= maxLengthOfIndex;
        text = makesAtom ? string->toAtomString(instance->globalObject).data : string->toExistingAtomString(instance->globalObject).data;
        if (mayBeIndex && (!text || isASCIIDigit((*text)[0])))
            return false;
    } else if (key == instance->keyGivenItsAtomLast) {
        shouldCacheByKeyCell = false;
        instance->keyGivenItsAtomLast = nullptr;
    }
    uid = static_cast<UniquedStringImpl*>(text);
    return uid != instance->vm->propertyNames->underscoreProto.impl();
}

static bool cacheAbsenceOfText(Instance* instance, JSObject* base, JSCell* key, const StringImpl& text)
{
    bool isForGood = !base->structure()->isDictionary();
    for (JSObject* holder = base;;) {
        Structure* structure = holder->structure();
        if (!structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto())
            return false;
        if (holder != base)
            isForGood &= !structure->isDictionary() && structure->hasImmutableProperties();
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
    read.holder = isDictionary ? std::bit_cast<JSCell*>(static_cast<uintptr_t>(structure->numberOfPropertiesOfDictionary())) : nullptr;
    read.epoch = isForGood ? MegamorphicCache::permanentEpoch : instance->vm->ensureMegamorphicCache().epochForFactAbout(text, structure->id());
    if (isDictionary)
        read.epoch |= Instance::epochIsOfDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
    read.slot.offset = isForGood ? Instance::keyIsAbsentForGood : Slot::isGetter | Slot::pointerIsCell;
    read.slot.structureID = structure->id();
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetByKeyPlainly, EncodedJSValue, (JSCell* base, JSCell* key, Instance* instance))
{
    noteLookup(instance, "operationAOTGetByKeyPlainly", base, key);
    auto answer = [&](const char* how, JSValue result) {
        countOperationNamed(instance, "operationAOTGetByKeyPlainly", result ? how : "cannot-say");
        return JSValue::encode(result);
    };
    JSType type = base->type();
    if (type != FinalObjectType && !isInstanceOfJSArrayItself(base) && type != JSFunctionType && type != ObjectType)
        return answer(nullptr, { });
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    const StringImpl* textHashedBefore = nullptr;
    if (!cache || !nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell, false, &textHashedBefore) || (type == JSFunctionType && key->isSymbol()))
        return answer(nullptr, { });
    if (!uid) {
        JSValue result = getFromPrototypeChainPlainly(asObject(base), nullptr);
        if (!result || !textHashedBefore || !cacheAbsenceOfText(instance, asObject(base), key, *textHashedBefore))
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
                    if (auto* read = shouldCacheByKeyCell && !extra->structure()->isDictionary() ? instance->placeForKeyedRead(extra->structureID(), key) : nullptr; read && read->slot.structureID != extra->structureID())
                        cacheKeyedAccess(read->slot, extra->structure(), key, offset);
                    return answer("beyond-the-layout", extra->getDirect(offset));
                }
            }
            if (!shouldCacheByKeyCell)
                return answer("a-field", field ? typedObject->readInline(*field) : JSValue());
        }
        return lookUpInPrototypeChainPlainly(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned attributes) {
            if (!holder || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
                return answer(nullptr, { });
            JSValue result = isValidOffset(offset) ? holder->getDirect(offset) : jsUndefined();
            if (!result)
                return answer(nullptr, { });
            if (Options::validateAOTTypedFacts()) [[unlikely]]
                validateAnswer(asObject(base), uid, isValidOffset(offset), result);
            if (!shouldCacheByKeyCell)
                return answer("of-an-object-that-is-not-final", result);
            if (!isValidOffset(offset))
                cacheAbsenceOfText(instance, asObject(base), key, *uid);
            else if (auto* read = holder == base && structure->propertyAccessesAreCacheable() ? instance->placeForKeyedRead(structure->id(), key) : nullptr)
                cacheKeyedAccess(read->slot, structure, key, offset);
            return answer("of-an-object-that-is-not-final-and-offered-to-the-table", result);
        });
    }
    JSValue result;
    if (type == FinalObjectType && !structure->compiledLayout()) {
        PropertyOffset offset;
        unsigned attributes;
        unsigned stepsBack = 0;
        unsigned hintsRight = 0;
        unsigned hintsWrong = 0;
        bool isRuledOut = false;
        if (findPlainly<false>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong) && isValidOffset(offset) && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)))
            result = asObject(base)->getDirect(offset);
        if (result) {
            if (Options::validateAOTTypedFacts()) [[unlikely]]
                validateAnswer(asObject(base), uid, true, result);
            bool isCacheable = structure->propertyAccessesAreCacheable();
            if (isCacheable && !structure->isDictionary() && offset <= MegamorphicCache::maxOffset)
                cache->initAsHit(structure->id(), uid, base, offset, true);
            auto* read = shouldCacheByKeyCell && isCacheable ? instance->placeForKeyedRead(structure->id(), key) : nullptr;
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
        return answer("along-the-chain-of-a-dictionary", shouldCacheByKeyCell ? JSValue() : getFromPrototypeChainPlainly(asObject(base), uid));
    else {
        result = getPlainlyAndFill(instance, base, uid, false);
        if (result)
            found = cache->findLoad(structure->id(), uid);
    }
    if (!found || !shouldCacheByKeyCell)
        return answer("inherited-or-absent", result);
    cacheKeyedRead(instance, structure, key, *found);
    return answer("inherited-or-absent-and-cached", result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasByKeyPlainly, size_t, (JSCell* base, JSCell* key, Instance* instance))
{
    auto answer = [&](const char* how, size_t result) {
        countOperationNamed(instance, "operationAOTHasByKeyPlainly", result != Instance::cannotLookUp ? how : "cannot-say");
        return result;
    };
    JSType type = base->type();
    if (type != FinalObjectType && !isInstanceOfJSArrayItself(base))
        return answer(nullptr, Instance::cannotLookUp);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    const StringImpl* textHashedBefore = nullptr;
    if (!cache || !nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell, false, &textHashedBefore))
        return answer(nullptr, Instance::cannotLookUp);
    auto askPrototypeChain = [&] {
        return lookUpInPrototypeChainPlainly(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned) -> size_t {
            if (Options::validateAOTTypedFacts() && holder && uid) [[unlikely]]
                validateAnswer(asObject(base), uid, isValidOffset(offset), { });
            return holder ? static_cast<size_t>(isValidOffset(offset)) : Instance::cannotLookUp;
        });
    };
    if (!uid)
        return answer("no-such-name", textHashedBefore ? Instance::cannotLookUp : askPrototypeChain());
    Structure* structure = base->structure();
    const MegamorphicCache::LoadEntry* found = cache->findLoad(structure->id(), uid);
    if (!found) {
        if (structure->isDictionary() || !getPlainlyAndFill(instance, base, uid, false))
            return answer("along-the-chain", shouldCacheByKeyCell ? Instance::cannotLookUp : askPrototypeChain());
        found = cache->findLoad(structure->id(), uid);
        if (!found)
            return answer(nullptr, Instance::cannotLookUp);
    }
    bool result = found->m_holder;
    if (!shouldCacheByKeyCell)
        return answer("by-the-structure", result);
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = found->m_epoch | static_cast<uint32_t>(result) << Instance::keyedHasAnswerShift;
    entry.pointer = key;
    entry.structureID = structure->id();
    return answer("by-the-structure-and-cached", result);
}

bool findOwnPropertyPlainly(JSValue base, UniquedStringImpl* uid, PropertyOffset& offset, unsigned& attributes)
{
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return false;
    Structure* structure = base.asCell()->structure();
    if (structure->isDictionary() || structure->compiledLayout() || structure->typeInfo().prohibitsPropertyCaching() || !structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence())
        return false;
    unsigned stepsBack = 0;
    unsigned hintsRight = 0;
    unsigned hintsWrong = 0;
    bool isRuledOut = false;
    if (!findPlainly<false>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong))
        return false;
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        unsigned attributesInTable;
        RELEASE_ASSERT(structure->getConcurrently(uid, attributesInTable) == offset && (!isValidOffset(offset) || attributesInTable == attributes));
    }
    return true;
}

bool findOwnPropertyByKeyPlainly(Instance* instance, JSValue base, JSValue key, UniquedStringImpl*& uid, PropertyOffset& offset, unsigned& attributes)
{
    bool shouldCacheByKeyCell;
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !key.isCell() || !nameOfKeyPlainly(instance, key.asCell(), uid, shouldCacheByKeyCell, true))
        return false;
    if (uid)
        return findOwnPropertyPlainly(base, uid, offset, attributes);
    offset = invalidOffset;
    attributes = 0;
    return !base.asCell()->structure()->compiledLayout();
}

static void cacheKeyedReplacement(Instance* instance, Structure* structure, JSCell* key, PropertyOffset offset)
{
    if (!structure->propertyAccessesAreCacheable() || !propertyLocation(offset))
        return;
    auto* read = instance->placeForKeyedRead(structure->id(), key);
    if (!read)
        return;
    cacheKeyedAccess(read->slot, structure, key, offset);
    read->slot.offset &= ~Instance::keyMayNotBeReplaced;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPutByKeyPlainly, size_t, (JSCell* base, JSCell* key, EncodedJSValue encodedValue, Instance* instance))
{
    auto answer = [&](const char* how) -> size_t {
        countOperationNamed(instance, "operationAOTPutByKeyPlainly", how ? how : "left-to-the-operation");
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
        if (state.data.isCompiledForAudit || !nameOfKeyPlainly(instance, key, name, shouldCacheByKeyCell, true))
            return answer(nullptr);
        const Typed::FieldData* field = typedObject->fieldNamed(state, name);
        if (PropertyOffset offset; !field) {
            Structure* before = typedObject->structure();
            bool hadNone = !typedObject->extraProperties();
            auto* place = shouldCacheByKeyCell && (hadNone || before->structureIDOfExtraProperties()) ? instance->freePlaceForKeyedAddition(before->id(), key) : nullptr;
            bool isSeenBefore = place && place->slot.structureID == before->id() && place->slot.pointer == key && place->epoch >= Instance::keyedAdditionOfExtraPropertyWasSeenOnce;
            auto how = typedObject->tryStoreExtraProperty(*instance->vm, name, stored, offset, true, false, nullptr, isSeenBefore);
            if (how == Typed::Object::ExtraPropertyStore::Added && isSeenBefore)
                cacheKeyedAdditionOfExtraProperty(instance, before, typedObject->structure(), key, offset, hadNone);
            else if (how == Typed::Object::ExtraPropertyStore::Added && place)
                noteKeyedAdditionOfExtraProperty(*place, before, key);
            if (Structure* ofExtraProperties = how == Typed::Object::ExtraPropertyStore::Replaced && shouldCacheByKeyCell ? typedObject->extraProperties()->structure() : nullptr; ofExtraProperties && !ofExtraProperties->isDictionary())
                cacheKeyedReplacement(instance, ofExtraProperties, key, offset);
            return answer(how != Typed::Object::ExtraPropertyStore::NotServed ? "stored-beyond-the-layout" : nullptr);
        }
        if (stored.isObject() || typedObject->hasFlagsInOrderWord() || typedObject->isDiscriminant(*field) || !typedObject->readInline(*field) || !typedObject->write(*instance->vm, *field, stored))
            return answer(nullptr);
        if (!shouldCacheByKeyCell)
            return answer("stored-into-a-field");
        cacheKeyedStoreToCompiledField(instance, base, key, Identifier::fromUid(*instance->vm, name));
        return answer("stored-into-a-field-and-offered-to-the-table");
    }
    VM& vm = *instance->vm;
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (structure->compiledLayout() || structure->hasImmutableShape() || structure->inheritorsMayOverrideReadOnlyProperties() || !object->canPerformFastPutInlineExcludingProto())
        return answer(nullptr);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    if (!nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell, true))
        return answer(nullptr);
    bool mayOnlyReplace = structure->mayBePrototype();
    if (Structure* next = mayOnlyReplace ? nullptr : structure->trySingleTransition(); next && next->transitionPropertyName() == uid && next->transitionKind() == TransitionKind::PropertyAddition && !next->transitionPropertyAttributes() && next->outOfLineCapacity() == structure->outOfLineCapacity()) {
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
    unsigned hintsRight = 0;
    unsigned hintsWrong = 0;
    bool isRuledOut = false;
    if (!findPlainly<false>(structure, uid, offset, attributes, stepsBack, isRuledOut, hintsRight, hintsWrong))
        return answer(nullptr);
    JSValue value = JSValue::decode(encodedValue);
    if (isValidOffset(offset)) {
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue) || structure->isWatchingReplacement())
            return answer(nullptr);
        object->putDirectOffset(vm, offset, value);
        if (!shouldCacheByKeyCell || !structure->propertyAccessesAreCacheable())
            return answer("replaced");
        structure->setIsReplacedWithoutNotice(true);
        cacheKeyedReplacement(instance, structure, key, offset);
        return answer("replaced-and-cached");
    }
    if (mayOnlyReplace || !structure->isStructureExtensible())
        return answer(nullptr);
    if (!structure->isDictionary() && (instance->emptyStructuresFilledByKey[0] || instance->emptyStructuresFilledByKey[1])) {
        Structure* first = structure;
        while (Structure* previous = first->previousID())
            first = previous;
        PropertyOffset place;
        if ((first == instance->emptyStructuresFilledByKey[0] || first == instance->emptyStructuresFilledByKey[1]) && !Structure::addPropertyTransitionToExistingStructure(structure, uid, 0, place) && !instance->wasStepAskedForBefore(structure->id(), uid))
            object->convertToDictionary(vm);
    }
    PutPropertySlot slot(object, true);
    object->putDirect(vm, uid, value, slot);
    if (!shouldCacheByKeyCell || !slot.isCacheablePut() || slot.type() != PutPropertySlot::NewProperty)
        return answer("added");
    cacheKeyedAddition(instance, vm, structure, object->structure(), key, slot.cachedOffset());
    return answer("added-and-offered-to-the-table");
}

static bool mayBeLookedUpPlainly(JSCell* key, PropertyName name)
{
    return key->isSymbol() || (atomOfKey(key) && !parseIndex(name));
}

static void cacheKeyedLookUp(Slot& entry, Structure* structure, JSCell* key, uint32_t flags)
{
    entry.structureID = StructureID();
    entry.offset = flags;
    entry.pointer = key;
    entry.structureID = structure->id();
}

static bool isKnownToBeAbsentOfDictionary(const Instance::KeyedRead& read, VM& vm, Structure* structure, JSCell* key)
{
    if (read.slot.structureID != structure->id() || read.slot.pointer != key || read.slot.offset != (Slot::isGetter | Slot::pointerIsCell) || (read.epoch & ~Instance::epochIsValidated) != (vm.ensureMegamorphicCache().epoch() | Instance::epochIsOfDictionary))
        return false;
    return std::bit_cast<uintptr_t>(read.holder) == structure->numberOfPropertiesOfDictionary();
}

static NEVER_INLINE void validateAbsence(JSObject* object, JSCell* key)
{
    if (UniquedStringImpl* uid = key->isSymbol() ? &asSymbol(key)->uid() : atomOfKey(key))
        validateAnswer(object, uid, false, { });
}

static void cacheAbsenceOfDictionary(Instance::KeyedRead& read, VM& vm, JSObject* base, Structure* structure, JSCell* key, PropertyName name)
{
    read.slot.structureID = StructureID();
    read.slot.offset = Slot::isGetter;
    read.slot.pointer = key;
    read.slot.structureID = structure->id();
    if (parseIndex(name))
        return;
    for (JSObject* holder = base;;) {
        Structure* structureOfHolder = holder->structure();
        if (TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) || structureOfHolder->compiledLayout())
            return;
        if (!structureOfHolder->propertyAccessesAreCacheable() || !structureOfHolder->propertyAccessesAreCacheableForAbsence() || !structureOfHolder->hasMonoProto())
            return;
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            break;
        holder = asObject(prototype);
    }
    read.holder = std::bit_cast<JSCell*>(static_cast<uintptr_t>(structure->numberOfPropertiesOfDictionary()));
    read.epoch = vm.ensureMegamorphicCache().epochForFactAbout(*name.uid(), structure->id()) | Instance::epochIsOfDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
    read.slot.offset = Slot::isGetter | Slot::pointerIsCell;
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> getByValOfDictionaryAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* object, JSValue property)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = object->structure();
    JSCell* key = property.asCell();
    auto& read = instance->keyedReadFor(structure->id(), key);
    Slot& entry = read.slot;
    if (isKnownToBeAbsentOfDictionary(read, vm, structure, key)) {
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
        cacheAbsenceOfDictionary(read, vm, object, structure, key, *name);
    else if (mayBeLookedUpPlainly(key, *name))
        cacheKeyedLookUp(entry, structure, key, Instance::keyIsLookedUp);
    else
        cacheKeyedAccess(entry, structure, key, std::nullopt);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static NEVER_INLINE OperationReturnType<size_t> hasOfDictionaryAndCache(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* object, JSCell* key, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = object->structure();
    auto& read = instance->keyedReadFor(structure->id(), key);
    if (isKnownToBeAbsentOfDictionary(read, vm, structure, key)) {
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
    else if (mayBeLookedUpPlainly(key, name))
        cacheKeyedLookUp(instance->keyedHasFor(structure->id(), key), structure, key, Instance::keyedHasIsLookedUp);
    else if (!result && !structure->isUncacheableDictionary())
        cacheAbsenceOfDictionary(read, vm, object, structure, key, name);
    OPERATION_RETURN(scope, result);
}

static std::optional<JSValue> tryGetByLongRopeKey(VM& vm, JSObject* base, JSString* key)
{
    unsigned length = key->length();
    unsigned namesLeft = 4096;
    Vector<StringView, 32> leaves;
    for (JSObject* holder = base;;) {
        Structure* structure = holder->structure();
        if (TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) || structure->compiledLayout() || TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags()))
            return std::nullopt;
        bool givesUp = false;
        std::optional<JSValue> found;
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (!namesLeft--) {
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

// Both are cells.
JSC_DEFINE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    noteLookup(instance, __func__, base, property);
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
                // The name of every property that lies in a Structure is an atom. There is none of this text: so no ordinary object has it, and none is made of it only to find that out.
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
        return getByValOfDictionaryAndCache(instance, globalObject, vm, asObject(base), property);
    }
    auto& read = instance->keyedReadFor(structure->id(), property.asCell());
    Slot& entry = read.slot;
    if (base.isObject()) {
        auto name = nameForKeyedCache(globalObject, property);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!name && structure->compiledLayout() && property.isString()) {
            Identifier ofText = asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*ofText.impl()))
                name = WTF::move(ofText);
        }
        if (name) {
            JSObject* object = asObject(base);
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, *name, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            bool isValueInObject = slot.isCacheableValue() && slot.slotBase() == object && object->structure() == structure;
            cacheKeyedAccess(entry, structure, property.asCell(), isValueInObject ? std::optional(slot.cachedOffset()) : std::nullopt);
            if (object->type() == GlobalProxyType && property.isSymbol())
                cacheKeyedLookUp(entry, structure, property.asCell(), Instance::keyIsLookedUp);
            if (slot.isCacheableGetter() && slot.slotBase() == object && object->structure() == structure && structure->propertyAccessesAreCacheable() && !structure->compiledLayout()) {
                if (auto location = propertyLocation(slot.cachedOffset()))
                    entry.offset = (*location & Slot::offsetMask) | Instance::keyIsOwnAccessor;
            }
            if (object->structure() == structure && structure->compiledLayout())
                cacheKeyedReadOfCompiledField(globalObject, read, object, *name);
            // What the megamorphic cache holds of the two by now holds while its epoch stands.
            if (auto* found = isValueInObject || object->structure() != structure ? nullptr : vm.ensureMegamorphicCache().findLoad(structure->id(), name->impl()); found && found->m_holder != JSCell::seenMultipleCalleeObjects()) {
                if (auto location = propertyLocation(found->m_offset)) {
                    read.holder = found->m_holder;
                    read.epoch = found->m_epoch;
                    entry.offset = !found->m_holder && found->m_epoch == MegamorphicCache::permanentEpoch ? Instance::keyIsAbsentForGood : *location | Slot::isGetter | Slot::pointerIsCell;
                }
            }
            OPERATION_RETURN(scope, JSValue::encode(result));
        }
    }
    cacheKeyedAccess(entry, structure, property.asCell(), std::nullopt);
    scope.release();
    return getByVal(instance, globalObject, vm, base, property);
}

// `property in base`, of whatever the two are.
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
    UniquedStringImpl* atom = atomOfKey(property);
    if (!atom && property.isString()) {
        auto name = asString(property)->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        if (!name) {
            auto flat = asString(property)->value(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, 0);
            bool goesByStructure = !object->structure()->isDictionary();
            for (JSObject* holder = parseIndex(*flat.data.impl()) ? nullptr : object; holder && !TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) && !TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags());) {
                Structure* structureOfHolder = holder->structure();
                if (structureOfHolder->compiledLayout())
                    break;
                goesByStructure &= structureOfHolder->propertyAccessesAreCacheable() && structureOfHolder->propertyAccessesAreCacheableForAbsence() && structureOfHolder->hasMonoProto();
                JSValue prototype = holder->getPrototypeDirect();
                if (!prototype.isObject()) {
                    cacheKeyedHas(instance, vm, object->structure(), property.asCell(), goesByStructure, false);
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
        // What a string or a symbol is as a key it is for good.
        if (!property.isString() && !property.isSymbol())
            OPERATION_RETURN(scope, object->hasProperty(globalObject, key));
        atom = key.impl();
    }
    Structure* structure = object->structure();
    if (structure->isDictionary()) [[unlikely]] {
        scope.release();
        return hasOfDictionaryAndCache(instance, globalObject, vm, object, property.asCell(), atom);
    }
    if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind) && structure->markingStampAndFlags() & Structure::isOfObjectsWithExtraProperties) {
        auto* ofLayout = uncheckedDowncast<Typed::Object>(object);
        const Typed::FieldData* field = ofLayout->fieldNamed(ofLayout->state(), atom);
        if (field && !(field->flags & (Typed::fieldMayExistWithoutStorage | Typed::fieldMayBeUnlisted))) {
            if (ofLayout->read(*field))
                OPERATION_RETURN(scope, 1);
            if (structure->markingStampAndFlags() & Structure::hasPrototypeOfLayout)
                OPERATION_RETURN(scope, 0);
        }
    }
    bool goesByStructure;
    bool isForGood;
    bool result = hasPropertyAndFillMegamorphicCache(globalObject, object, atom, goesByStructure, &isForGood);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    cacheKeyedHas(instance, vm, structure, property.asCell(), goesByStructure && object->structure() == structure, result, isForGood);
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
        UniquedStringImpl* atom = atomOfKey(property);
        bool isNew = !atom && property.isString();
        std::optional<Identifier> name;
        if (atom || isNew) {
            Identifier ofText = atom ? Identifier::fromUid(vm, atom) : asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            if (!parseIndex(*ofText.impl()))
                name = WTF::move(ofText);
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
                cacheKeyedStoreToCompiledField(instance, object, property.asCell(), *name);
                OPERATION_RETURN(scope);
            }
            bool isReplacementInObject = slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == object && object->structure() == oldStructure && !isHostObjectThatOverridesPut(oldStructure);
            if (isReplacementInObject)
                oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            if (slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.base() == object && !isHostObjectThatOverridesPut(oldStructure)) {
                cacheKeyedAddition(instance, vm, oldStructure, object->structure(), property.asCell(), slot.cachedOffset());
                OPERATION_RETURN(scope);
            }
            if (isReplacementInObject)
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

enum class OwnNameOfLongRope : uint8_t { Found, Absent, Unknown };

static OwnNameOfLongRope findOwnNameOfLongRope(VM& vm, JSObject* object, JSValue property, Identifier& name)
{
    if (!property.isString() || !asString(property)->isRope() || asString(property)->length() <= 2048)
        return OwnNameOfLongRope::Unknown;
    Structure* structure = object->structure();
    if (object->methodTable()->deleteProperty != JSFinalObject::info()->methodTable.deleteProperty || structure->compiledLayout() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || object->isLockedBuiltin())
        return OwnNameOfLongRope::Unknown;
    JSString* key = asString(property);
    unsigned length = key->length();
    unsigned namesLeft = 4096;
    Vector<StringView, 32> leaves;
    auto result = OwnNameOfLongRope::Absent;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (!namesLeft--) {
            result = OwnNameOfLongRope::Unknown;
            return false;
        }
        if (entry.key()->length() != length || entry.key()->isSymbol())
            return true;
        if (leaves.isEmpty() && !key->tryCollectLeaves(leaves)) {
            result = OwnNameOfLongRope::Unknown;
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
        result = OwnNameOfLongRope::Found;
        return false;
    });
    return result;
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
    auto ownName = findOwnNameOfLongRope(vm, object, property, key);
    if (ownName == OwnNameOfLongRope::Absent)
        OPERATION_RETURN(scope, true);
    if (ownName == OwnNameOfLongRope::Unknown)
        key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Structure* oldStructure = object->structure();
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isDeleted && object->noteIfRefusedByLockAlone(vm, key.impl(), false, true)) {
        throwTypeError(globalObject, scope, AOT::wordsOfLock(vm, object, AOT::RefusedByLock::Delete, key.impl()));
        OPERATION_RETURN(scope, 0);
    }
    if (isDeleted && base.isObject() && (property.isString() || property.isSymbol())) {
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
        cacheKeyedDeletion(instance, oldStructure, object->structure(), property, key.impl(), slot);
    }
    OPERATION_RETURN(scope, isDeleted);
}

// In strict code: it has deleted, or it throws. As operationAOTTypedDeleteByKey.
JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Identifier key;
    auto ownName = findOwnNameOfLongRope(vm, object, property, key);
    if (ownName == OwnNameOfLongRope::Absent)
        OPERATION_RETURN(scope);
    if (ownName == OwnNameOfLongRope::Unknown)
        key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* oldStructure = object->structure();
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDeleted) {
        throwTypeError(globalObject, scope, object->noteIfRefusedByLockAlone(vm, key.impl(), false, true) ? AOT::wordsOfLock(vm, object, AOT::RefusedByLock::Delete, key.impl()) : String(UnableToDeletePropertyError));
        OPERATION_RETURN(scope);
    }
    if (base.isObject() && (property.isString() || property.isSymbol())) {
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
        cacheKeyedDeletion(instance, oldStructure, object->structure(), property, key.impl(), slot);
    }
    OPERATION_RETURN(scope);
}

#if AOT_TESTS_SHADOW_IN_COMPILED_CODE
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
