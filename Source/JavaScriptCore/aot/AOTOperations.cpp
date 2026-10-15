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
#include "AOTTypedObject.h"
#include "AOTTypedOperations.h"
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetByNameAndFillTable, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance);
    using Access = PlaceTable::Access;
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, PlaceTable::numberOfIdentifierNamed(name));
    PlaceTable& table = PlaceTable::of(vm);
    Data* data = table.dataFor(instance);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
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
            OPERATION_RETURN(scope, JSValue::encode(holder->getDirect(offset)));
    }
    if (instance->typed) {
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
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    JSValue result;
    MegamorphicCache* megamorphicCache = structureBefore ? vm.megamorphicCache() : nullptr;
    bool hasMissedBefore = megamorphicCache && (megamorphicCache->findLoad(structureBefore->id(), ident.impl()) || megamorphicCache->findGetter(structureBefore->id(), ident.impl()));
    if (!tryGetFromOrdinaryObject(globalObject, base, ident.impl(), slot, result)) {
        if (base.isCell()) {
            if (auto* known = findKnownCustomGetter(vm, instance, base.asCell(), ident.impl())) {
                if (Slot* cache = table.servesSeveralInstances() ? nullptr : table.ensureSlot(structureBefore, name, Access::Read))
                    cacheKnownCustomGetter(vm, *instance, data, cache, structureBefore, ident, *known);
                OPERATION_RETURN(scope, callCustomGetter(*known, encodedBase, ident));
            }
        }
        result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (instance->typed && std::exchange(instance->typed->didGetDeclaredMember, false))
            OPERATION_RETURN(scope, JSValue::encode(result));
        if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
            noteCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!structureBefore || base.asCell()->structure() != structureBefore || (!base.isObject() && table.servesSeveralInstances()))
        OPERATION_RETURN(scope, JSValue::encode(result));
    if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findLoad(structureBefore->id(), ident.impl()) : nullptr) {
        if (!hasMissedBefore)
            OPERATION_RETURN(scope, JSValue::encode(result));
        UniquedStringImpl* uidIfItDependsOnPrototypes = entry->m_epoch == MegamorphicCache::permanentEpoch ? nullptr : ident.impl();
        bool isInTable;
        if (!entry->m_holder)
            isInTable = table.setAbsent(structureBefore, name, uidIfItDependsOnPrototypes);
        else if (entry->m_holder == JSCell::seenMultipleCalleeObjects())
            isInTable = table.setOwnPlace(structureBefore, name, entry->m_offset, false);
        else
            isInTable = table.setInherited(structureBefore, name, entry->m_holder, entry->m_offset, uidIfItDependsOnPrototypes);
        if (isInTable)
            OPERATION_RETURN(scope, JSValue::encode(result));
    } else if (auto* entry = vm.megamorphicCache() ? vm.megamorphicCache()->findGetter(structureBefore->id(), ident.impl()) : nullptr) {
        if (!hasMissedBefore)
            OPERATION_RETURN(scope, JSValue::encode(result));
        bool isOwn = entry->m_holder == JSCell::seenMultipleCalleeObjects();
        if (slot.isCacheableGetter())
            cacheShortcutToGetter(slot.getterSetter());
        if (table.setAccessor(structureBefore, name, isOwn ? nullptr : entry->m_holder, entry->m_offset, isOwn ? nullptr : ident.impl()))
            OPERATION_RETURN(scope, JSValue::encode(result));
    }
    if (Slot* cache = table.ensureSlot(structureBefore, name, Access::Read)) {
        cacheGetById(globalObject, data, base, structureBefore, ident, slot, cache);
        bool isInheritedForGood = cache->structureID == structureBefore->id() && (cache->offset & Slot::flagsMask) == (Slot::isIndirect | Slot::pointerIsCell) && slot.isCacheableValue() && cache->pointer == slot.slotBase() && slot.slotBase() != base.asCell() && !table.isWatched(cache);
        bool isOwn = cache->structureID == structureBefore->id() && !(cache->offset & (Slot::isGetter | Slot::pointerIsNotCell | Slot::pointerIsCell)) && slot.isCacheableValue() && slot.slotBase() == base.asCell() && !structureBefore->compiledLayout() && !table.isWatched(cache);
        if ((isInheritedForGood && table.setInheritedInPlaceOfSlot(structureBefore, name, slot.slotBase(), slot.cachedOffset())) || (isOwn && table.setOwnPlace(structureBefore, name, slot.cachedOffset(), false)))
            table.releaseSlot(structureBefore, name, Access::Read);
        else if (uint32_t offsetAndKind = cache->structureID == structureBefore->id() && cache->readsExtraProperty() ? static_cast<uint32_t>(std::bit_cast<uint64_t>(cache->pointer) >> Slot::extraPropertyShift) : 0; offsetAndKind && !(offsetAndKind & ~Slot::extraPropertyOffsetMask))
            table.setExtraAccessorInPlaceOfSlot(structureBefore, name, cache->structureIDOfExtraProperties(), offsetAndKind);
    }
    ensureStillAliveHere(structureBefore);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

static void growStorageAndPut(VM& vm, JSObject* object, Structure* oldStructure, const MegamorphicCache::StoreEntry& entry, JSValue value)
{
    Structure* newStructure = entry.m_newStructureID.decode();
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
    if (!isDirect && oldStructure && oldStructure->compiledLayout() && base.asCell()->inherits<Typed::Object>()) {
        auto* object = uncheckedDowncast<Typed::Object>(base.asCell());
        uint64_t wordsBefore = object->orderAndShapeWords();
        if (instance->typed->storeByKnownTransition(vm, object, ident.impl(), value)) {
            if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
                cacheStoreTransition(instance, dataWithSlot(instance, callFrame, cache), object, oldStructure, wordsBefore, ident, cache);
            OPERATION_RETURN(scope);
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
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
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

static std::optional<uint32_t> flagsOfStructureAfterAddition(Structure* before, Structure* after, PropertyOffset offset)
{
    unsigned capacity = before->outOfLineCapacity();
    if (after->outOfLineCapacity() == capacity)
        return 0;
    if (before->typeInfo().type() != FinalObjectType || hasIndexedProperties(before->indexingType()) || offset != firstOutOfLineOffset + static_cast<PropertyOffset>(capacity))
        return std::nullopt;
    if (!capacity)
        return after->outOfLineCapacity() == initialOutOfLineCapacity ? std::optional(PlaceTable::needsFirstStorage) : std::nullopt;
    constexpr unsigned maxCapacityGrownByStub = 32;
    return capacity <= maxCapacityGrownByStub && after->outOfLineCapacity() == 2 * capacity ? std::optional(PlaceTable::growsStorage) : std::nullopt;
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByNameAndFillTable, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t name, uint32_t flagBits))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(instance, callFrame, PlaceTable::numberOfIdentifierNamed(name));
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;
    if (base.isUndefinedOrNull() && instance->typed) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), true));
        OPERATION_RETURN(scope);
    }
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (Slot* givenUp = !isDirect && base.isObject() ? PlaceTable::of(vm).slotIfAny(oldStructure, name, PlaceTable::Access::Store) : nullptr; givenUp && !givenUp->structureID && (givenUp->offset & Slot::attemptsMask) == Slot::attemptsMask) {
        PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
        base.putInline(globalObject, ident, value, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (slot.type() == PutPropertySlot::NewProperty && oldStructure->numberOfRowPlusOne() && slot.base() == base.asCell()) [[unlikely]]
            numberStructureGrownInto(instance, oldStructure, base.asCell()->structure(), PlaceTable::numberOfIdentifierNamed(name), slot.cachedOffset());
        OPERATION_RETURN(scope);
    }
    PlaceTable& table = PlaceTable::of(vm);
    PlaceTable::Access access = isDirect ? PlaceTable::Access::Define : PlaceTable::Access::Store;
    bool isOfCompiledLayout = oldStructure && oldStructure->compiledLayout();
    auto enterReplacement = [&](PropertyOffset offset) {
        unsigned attributes = 0;
        if (oldStructure->mayBePrototype() || isHostObjectThatOverridesPut(oldStructure) || oldStructure->get(vm, ident, attributes) != offset || attributes)
            return false;
        oldStructure->didCachePropertyReplacement(vm, offset);
        return table.setOwnPlace(oldStructure, name, offset, true);
    };
    auto flagsIfAdditionMayBeEntered = [&](Structure* newStructure, PropertyOffset offset) -> std::optional<uint32_t> {
        if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || isHostObjectThatOverridesPut(oldStructure) || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
            return std::nullopt;
        if (!isDirect && !storesAreOrdinaryAlongPrototypeChain(oldStructure))
            return std::nullopt;
        return flagsOfStructureAfterAddition(oldStructure, newStructure, offset);
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
        } else if (Structure* newStructure = entry->m_newStructureID.decode(); auto flags = flagsIfAdditionMayBeEntered(newStructure, offset)) {
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
    bool hasMissedBefore = oldStructure && vm.megamorphicCache() && vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl());
    PutPropertySlot slot(base, isStrict, PutPropertySlot::PutById);
    if (unsigned attributes = 0; isDirect && instance->typed && (base.asCell()->type() != FinalObjectType || isOfCompiledLayout || !oldStructure->isStructureExtensible() || (oldStructure->get(vm, ident, attributes) != invalidOffset && attributes))) [[unlikely]]
        asObject(base)->createDataProperty(globalObject, ident, value, true);
    else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (oldStructure && slot.type() == PutPropertySlot::NewProperty && oldStructure->numberOfRowPlusOne() && slot.base() == base.asCell()) [[unlikely]]
        numberStructureGrownInto(instance, oldStructure, base.asCell()->structure(), PlaceTable::numberOfIdentifierNamed(name), slot.cachedOffset());
    if (!oldStructure || oldStructure != structureOfSlot)
        OPERATION_RETURN(scope);
    if (!isDirect || slot.type() == PutPropertySlot::ExistingProperty || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    if (!hasMissedBefore && base.isObject() && vm.megamorphicCache() && vm.megamorphicCache()->findStore(oldStructure->id(), ident.impl())) {
        if (slot.type() == PutPropertySlot::NewProperty && slot.isCacheablePut() && slot.base() == base.asCell() && !isInlineOffset(slot.cachedOffset()))
            instance->noteOutOfLineProperty(base.asCell()->structure());
        OPERATION_RETURN(scope);
    }
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
        if (isInTable)
            OPERATION_RETURN(scope);
    }
    if ((cache = table.ensureSlot(oldStructure, name, access)); cache && (cache->structureID || (cache->offset & Slot::attemptsMask) != Slot::attemptsMask))
        cachePutById(instance, data, base, oldStructure, ident, slot, isDirect, cache);
    ensureStillAliveHere(structureOfSlot);
    OPERATION_RETURN(scope);
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
    const StringImpl* name = key->isSymbol() ? &asSymbol(key)->privateName().uid() : asString(key)->tryGetValueImpl();
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
    if (!isPlain(before) || !isPlain(after) || after->previousID() != before || before->typeInfoBlob() != after->typeInfoBlob())
        return;
    bool allocatesInitialStorage = !before->outOfLineCapacity() && after->outOfLineCapacity() == initialOutOfLineCapacity && offset == firstOutOfLineOffset && !hasIndexedProperties(before->indexingType());
    if (before->outOfLineCapacity() != after->outOfLineCapacity() && !allocatesInitialStorage)
        return;
    auto& entry = *place;
    cacheKeyedAccess(entry.slot, before, key, offset);
    entry.newStructureID = after->id();
    entry.epoch = epochForFactAboutKey(vm, key, before);
    entry.allocatesInitialStorage = allocatesInitialStorage;
}

template<typename Found>
static ALWAYS_INLINE auto lookUpPlainly(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
{
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout())
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
            bool goesByItsStructure = object == base && !isClassObject && info.getOwnPropertySlotIsImpureForPropertyAbsence() && Typed::closureLacksWhatItsStructureLacks(vm, object, uid);
            if (isClassObject) {
                const Typed::State& state = *structure->aotInstance()->typed;
                const Typed::LayoutData* ofClass = uncheckedDowncast<Typed::Closure>(object)->classData();
                if (ofClass && state.hasStaticMethodNamed(*ofClass, uid))
                    return cannotSay("cannot-say:a-static-of-the-class");
                goesByStructure = false;
            }
            bool looksForItself = info.overridesGetOwnPropertySlot() && (isLazyFunctionPropertyName || (object->type() != ArrayType && (object->type() != JSFunctionType || (info.getOwnPropertySlotIsImpureForPropertyAbsence() && !isClassObject && !goesByItsStructure)) && object != instance->globalObject->arrayPrototype()));
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
        uid = &asSymbol(key)->privateName().uid();
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
        shouldCacheByKeyCell = false;
        if (textHashedBefore && text && text->hasHash())
            *textHashedBefore = text;
        bool mayBeIndex = !text && string->length() <= maxLengthOfIndex;
        text = makesAtom ? string->toAtomString(instance->globalObject).data : string->toExistingAtomString(instance->globalObject).data;
        if (mayBeIndex && (!text || isASCIIDigit((*text)[0])))
            return false;
    }
    uid = static_cast<UniquedStringImpl*>(text);
    return uid != instance->vm->propertyNames->underscoreProto.impl();
}

static bool cacheAbsenceOfTextThatNamesNothing(Instance* instance, JSObject* base, JSCell* key, const StringImpl& text)
{
    for (JSObject* holder = base;;) {
        Structure* structure = holder->structure();
        if (!structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto())
            return false;
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
    read.epoch = instance->vm->ensureMegamorphicCache().epochForFactAbout(text, structure->id());
    if (isDictionary)
        read.epoch |= Instance::epochIsOfDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
    read.slot.offset = Slot::isGetter | Slot::pointerIsCell;
    read.slot.structureID = structure->id();
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetByKeyPlainly, EncodedJSValue, (JSCell* base, JSCell* key, Instance* instance))
{
    auto answer = [&](const char* how, JSValue result) {
        countOperationNamed(instance, "operationAOTGetByKeyPlainly", result ? how : "cannot-say");
        return JSValue::encode(result);
    };
    JSType type = base->type();
    if (type != FinalObjectType && type != ArrayType && type != JSFunctionType && type != ObjectType)
        return answer(nullptr, { });
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    const StringImpl* textHashedBefore = nullptr;
    if (!cache || !nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell, false, &textHashedBefore) || (type == JSFunctionType && key->isSymbol()))
        return answer(nullptr, { });
    if (!uid) {
        JSValue result = getFromPrototypeChainPlainly(asObject(base), nullptr);
        if (!result || !textHashedBefore || !cacheAbsenceOfTextThatNamesNothing(instance, asObject(base), key, *textHashedBefore))
            return answer("no-such-name", result);
        return answer("no-such-name-and-cached", result);
    }
    if (type == ObjectType)
        return answer(nullptr, { });
    Structure* structure = base->structure();
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
            auto* read = shouldCacheByKeyCell && structure->propertyAccessesAreCacheable() ? instance->placeForKeyedRead(structure->id(), key) : nullptr;
            if (!read)
                return answer("own", result);
            cacheKeyedAccess(read->slot, structure, key, offset);
            return answer("own-and-cached", result);
        }
    }
    const MegamorphicCache::LoadEntry* found = cache->findLoad(structure->id(), uid);
    if (found) {
        JSCell* holder = found->m_holder;
        result = holder ? asObject(holder == JSCell::seenMultipleCalleeObjects() ? base : holder)->getDirect(found->m_offset) : jsUndefined();
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
    if (type != FinalObjectType && type != ArrayType)
        return answer(nullptr, Instance::cannotLookUp);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    if (!cache || !nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell))
        return answer(nullptr, Instance::cannotLookUp);
    auto askPrototypeChain = [&] {
        return lookUpInPrototypeChainPlainly(asObject(base), uid, [&](JSObject* holder, PropertyOffset offset, unsigned) -> size_t {
            if (Options::validateAOTTypedFacts() && holder && uid) [[unlikely]]
                validateAnswer(asObject(base), uid, isValidOffset(offset), { });
            return holder ? static_cast<size_t>(isValidOffset(offset)) : Instance::cannotLookUp;
        });
    };
    if (!uid)
        return answer("no-such-name", askPrototypeChain());
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
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !key.isCell() || !nameOfKeyPlainly(instance, key.asCell(), uid, shouldCacheByKeyCell))
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
    if (base->type() != FinalObjectType)
        return answer(nullptr);
    VM& vm = *instance->vm;
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (structure->compiledLayout() || structure->hasImmutableShape() || structure->inheritorsMayOverrideReadOnlyProperties() || structure->mayBePrototype() || !object->canPerformFastPutInlineExcludingProto())
        return answer(nullptr);
    UniquedStringImpl* uid;
    bool shouldCacheByKeyCell;
    if (!nameOfKeyPlainly(instance, key, uid, shouldCacheByKeyCell, true))
        return answer(nullptr);
    if (Structure* next = structure->trySingleTransition(); next && next->transitionPropertyName() == uid && next->transitionKind() == TransitionKind::PropertyAddition && !next->transitionPropertyAttributes() && next->outOfLineCapacity() == structure->outOfLineCapacity()) {
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
    if (!structure->isStructureExtensible())
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
    return atomOfKey(key) && !parseIndex(name);
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
    if (UniquedStringImpl* uid = key->isSymbol() ? &asSymbol(key)->privateName().uid() : atomOfKey(key))
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
        throwTypeError(globalObject, scope, withAOTErrorNote(UnableToDeletePropertyError));
        OPERATION_RETURN(scope, 0);
    }
    if (isDeleted && base.isObject() && property.isCell())
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
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
        throwTypeError(globalObject, scope, object->noteIfRefusedByLockAlone(vm, key.impl(), false, true) ? withAOTErrorNote(UnableToDeletePropertyError) : String(UnableToDeletePropertyError));
        OPERATION_RETURN(scope);
    }
    if (base.isObject() && property.isCell())
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() ? std::optional(slot.cachedOffset()) : std::nullopt);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
