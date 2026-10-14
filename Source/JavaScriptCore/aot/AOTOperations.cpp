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
        if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
            noteCustomGetter(globalObject, *instance, asObject(base), ident, slot);
    }
    if (!SharedData::contains(cache))
        cacheGetById(globalObject, dataWithSlot(instance, callFrame, cache), base, structureBefore, ident, slot, cache, true);
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
    if (isDirect && oldStructure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) [[unlikely]] {
        if (!asObject(base)->putDirect(vm, ident, value, slot) && !value.isUndefined()) [[unlikely]]
            throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, oldStructure, ident.impl(), value));
    } else if (unsigned attributes = 0; isDirect && instance->typed && (base.asCell()->type() != FinalObjectType || oldStructure->compiledLayout() || !oldStructure->isStructureExtensible() || (oldStructure->get(vm, ident, attributes) != invalidOffset && attributes))) [[unlikely]]
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
    if (!isDirect && slot.isCacheableSetter() && base.isObject() && base.asCell()->structure() == oldStructure)
        noteInheritedSetter(globalObject, *instance, asObject(base), ident, slot);
    cachePutById(instance, SharedData::contains(cache) ? nullptr : dataWithSlot(instance, callFrame, cache), base, oldStructure, ident, slot, isDirect, cache);
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

static void cacheKeyedHas(Instance* instance, VM& vm, Structure* structure, JSCell* key, bool goesByStructure, bool result, bool isForGood = false)
{
    Slot& entry = instance->keyedHasFor(structure->id(), key);
    entry.structureID = StructureID();
    entry.offset = goesByStructure ? (isForGood ? MegamorphicCache::permanentEpoch : vm.ensureMegamorphicCache().epoch()) | static_cast<uint32_t>(result) << Instance::keyedHasAnswerShift : Slot::isGetter;
    entry.pointer = key;
    entry.structureID = structure->id();
}

// A store under the key has given the object a property it lacked. See Instance::KeyedAddition.
static void cacheKeyedAddition(Instance* instance, VM& vm, Structure* before, Structure* after, JSCell* key, PropertyOffset offset)
{
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->typedLayoutID() && !structure->family() && !structure->hasPolyProto();
    };
    if (!usesDataStubs() || !isPlain(before) || !isPlain(after) || after->previousID() != before || before->typeInfoBlob() != after->typeInfoBlob())
        return;
    bool allocatesInitialStorage = !before->outOfLineCapacity() && after->outOfLineCapacity() == initialOutOfLineCapacity && offset == firstOutOfLineOffset && !hasIndexedProperties(before->indexingType());
    if (before->outOfLineCapacity() != after->outOfLineCapacity() && !allocatesInitialStorage)
        return;
    auto& entry = instance->keyedAdditionFor(before->id(), key);
    cacheKeyedAccess(entry.slot, before, key, offset);
    entry.newStructureID = after->id();
    entry.epoch = vm.ensureMegamorphicCache().epoch();
    entry.allocatesInitialStorage = allocatesInitialStorage;
}

template<typename Found>
static ALWAYS_INLINE auto lookUpPlainly(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, 0, 0u))
{
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || structure->typedLayoutID())
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
            if (object == base && structure->knownShape())
                return cannotSay("cannot-say:known-shape");
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
            if (looksForItself || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || structure->typedLayoutID() || !structure->propertyAccessesAreCacheable())
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
        if (structureID.decode()->knownShape())
            return cannotSay("cannot-say:known-shape");
        bool isForGood = true;
        for (JSObject* object = asObject(base);;) {
            Structure* structure = object->structure();
            TypeInfo info = structure->typeInfo();
            if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->compiledLayout() || structure->typedLayoutID() || !structure->propertyAccessesAreCacheable()) {
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
        if (TypeInfo::overridesGetOwnPropertySlot(holder->inlineTypeFlags()) || structureOfHolder->compiledLayout() || structureOfHolder->typedLayoutID())
            return;
        if (!structureOfHolder->propertyAccessesAreCacheable() || !structureOfHolder->propertyAccessesAreCacheableForAbsence() || !structureOfHolder->hasMonoProto())
            return;
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            break;
        holder = asObject(prototype);
    }
    read.holder = std::bit_cast<JSCell*>(static_cast<uintptr_t>(structure->numberOfPropertiesOfDictionary()));
    read.epoch = vm.ensureMegamorphicCache().epoch() | Instance::epochIsOfDictionary | (Options::validateAOTTypedFacts() ? Instance::epochIsValidated : 0);
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
    if (isThere && slot.isCacheableValue() && slot.slotBase() == object && structure->propertyAccessesAreCacheable() && !structure->typedLayoutID())
        cacheKeyedAccess(entry, structure, key, slot.cachedOffset());
    else if (!structure->hasBeenFlattenedBefore() && flattenOnce(vm, object))
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
    else if (!structure->hasBeenFlattenedBefore() && flattenOnce(vm, object))
        OPERATION_RETURN(scope, result);
    else if (mayBeLookedUpPlainly(key, name))
        cacheKeyedLookUp(instance->keyedHasFor(structure->id(), key), structure, key, Instance::keyedHasIsLookedUp);
    else if (!result && !structure->isUncacheableDictionary())
        cacheAbsenceOfDictionary(read, vm, object, structure, key, name);
    OPERATION_RETURN(scope, result);
}

// Both are cells.
JSC_DEFINE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    Structure* structure = base.asCell()->structure();
    auto isOrdinary = [](JSObject* object) {
        Structure* structure = object->structure();
        return !TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && !structure->typedLayoutID() && !structure->compiledLayout();
    };
    if (property.isString() && base.isObject() && isOrdinary(asObject(base))) {
        JSString* key = asString(property);
        if (StringImpl* text = key->tryGetValueImpl(); !text || !text->isAtom()) {
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
        if (name) {
            JSObject* object = asObject(base);
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, *name, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            bool isValueInObject = slot.isCacheableValue() && slot.slotBase() == object && object->structure() == structure && !structure->typedLayoutID();
            cacheKeyedAccess(entry, structure, property.asCell(), isValueInObject ? std::optional(slot.cachedOffset()) : std::nullopt);
            if (object->structure() == structure && structure->compiledLayout())
                cacheKeyedReadOfCompiledField(globalObject, read, object, *name);
            // What the megamorphic cache holds of the two by now holds while its epoch stands.
            if (auto* found = isValueInObject || object->structure() != structure ? nullptr : vm.ensureMegamorphicCache().findLoad(structure->id(), name->impl()); found && found->m_holder != JSCell::seenMultipleCalleeObjects()) {
                if (auto location = propertyLocation(found->m_offset)) {
                    read.holder = found->m_holder;
                    read.epoch = found->m_epoch;
                    entry.offset = *location | Slot::isGetter | Slot::pointerIsCell;
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
                if (structureOfHolder->typedLayoutID() || structureOfHolder->compiledLayout())
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
    Slot& entry = instance->keyedStoreFor(base.asCell()->structureID(), property.asCell());
    Structure* structureOfEntry = base.asCell()->structure();
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
            bool isReplacementInObject = slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == object && object->structure() == oldStructure && !oldStructure->typedLayoutID() && !isHostObjectThatOverridesPut(oldStructure);
            if (isReplacementInObject)
                oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            if (slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.base() == object && !isHostObjectThatOverridesPut(oldStructure)) {
                cacheKeyedAddition(instance, vm, oldStructure, object->structure(), property.asCell(), slot.cachedOffset());
                OPERATION_RETURN(scope);
            }
            cacheKeyedAccess(entry, oldStructure, property.asCell(), isReplacementInObject ? std::optional(slot.cachedOffset()) : std::nullopt);
            OPERATION_RETURN(scope);
        }
    }
    cacheKeyedAccess(entry, structureOfEntry, property.asCell(), std::nullopt);
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

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValAndCacheSloppy, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Structure* oldStructure = object->structure();
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (isDeleted && base.isObject() && property.isCell())
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() && !oldStructure->typedLayoutID() && !oldStructure->family() && !oldStructure->hasStableFields() ? std::optional(slot.cachedOffset()) : std::nullopt);
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
    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* oldStructure = object->structure();
    DeletePropertySlot slot;
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDeleted) {
        if (object->isLockedBuiltin())
            object->noteRefusalByLock(vm, key.impl());
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
        OPERATION_RETURN(scope);
    }
    if (base.isObject() && property.isCell())
        cacheKeyedDelete(vm, callerData(instance, callFrame), cache, object, oldStructure, property, slot.isCacheableDelete() && slot.isDeleteHit() && !oldStructure->typedLayoutID() && !oldStructure->family() && !oldStructure->hasStableFields() ? std::optional(slot.cachedOffset()) : std::nullopt);
    OPERATION_RETURN(scope);
}

#if ASSERT_ENABLED
#endif

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM* vmPointer, JSCell* cell))
{
    countOperationNamed(*vmPointer, __func__);
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.writeBarrierSlowPath(cell);
}

static Type cellTypeForJSType(JSType type)
{
    switch (type) {
    case StringType:
        return TString;
    case SymbolType:
        return TSymbol;
    case HeapBigIntType:
        return TBigInt;
    case JSFunctionType:
    case InternalFunctionType:
    case SlimClosureType:
    case SlimClosureWithPropertiesType:
        return TFunction;
    case ArrayType:
    case DerivedArrayType:
        return TArray;
    case FinalObjectType:
        return TFinalObject;
    case JSMapType:
        return TMap;
    case JSSetType:
        return TSet;
    case JSWeakMapType:
        return TWeakMap;
    case JSWeakSetType:
        return TWeakSet;
    case RegExpObjectType:
        return TRegExp;
    case JSPromiseType:
        return TPromise;
    case JSDateType:
        return TDate;
    case ErrorInstanceType:
        return TError;
    case ArrayBufferType:
        return TArrayBuffer;
    case DataViewType:
        return TDataView;
    case StringObjectType:
        return TStringObject;
    default:
        if (isTypedArrayType(type))
            return typeForTypedArray(type);
        return type >= ObjectType ? TOtherObject : TCellOther;
    }
}

Type valueType(JSValue value)
{
    if (!value)
        return TEmpty;
    if (value.isInt32())
        return TInt32;
    if (value.isDouble())
        return TDouble;
    if (value.isBoolean())
        return TBoolean;
    if (value.isUndefined())
        return TUndefined;
    if (value.isNull())
        return TNull;
    if (!value.isCell())
        return TTop;
    if (value.isString()) {
        const StringImpl* impl = asString(value)->tryGetValueImpl();
        return impl && impl->isAtom() ? TAtomString : asString(value)->length() <= TypedLayoutTable::maxAtomizedStringLength ? TShortOtherString : TLongString;
    }
    if (value.asCell()->inlineTypeFlags() & MasqueradesAsUndefined)
        return TOtherObject;
    return cellTypeForJSType(value.asCell()->type());
}

void dumpType(PrintStream& out, Type type)
{
    if (!type) {
        out.print("None");
        return;
    }
    if (type == TAll) {
        out.print("All");
        return;
    }
    if (type == TTop) {
        out.print("Top");
        return;
    }
    CommaPrinter bar("|"_s);
    auto take = [&](Type bits, ASCIILiteral name) {
        if ((type & bits) == bits) {
            out.print(bar, name);
            type &= ~bits;
        }
    };
    take(TNumber, "Number"_s);
    take(TAnyObject, "AnyObject"_s);
    take(TInt32, "Int32"_s);
    take(TDouble, "Double"_s);
    take(TBoolean, "Boolean"_s);
    take(TUndefined, "Undefined"_s);
    take(TNull, "Null"_s);
    take(TString, "String"_s);
    take(TAtomString, "AtomString"_s);
    take(TOtherString, "OtherString"_s);
    take(TShortOtherString, "ShortOtherString"_s);
    take(TLongString, "LongString"_s);
    take(TSymbol, "Symbol"_s);
    take(TBigInt, "BigInt"_s);
    take(TFunction, "Function"_s);
    if (type & TFunctionTag) {
        if (uint32_t function = functionNumberOf(type))
            out.print(bar, "Function#", function);
        else if (uint32_t intrinsic = intrinsicFunctionOf(type & TFunction))
            out.print(bar, "Intrinsic#", intrinsic);
        else
            out.print(bar, "Function#?");
        type &= ~TFunction;
    }
    take(TArray, "Array"_s);
    if (type & TArrayTag) {
        out.print(bar, "Array{", static_cast<unsigned>((type & TAnyArrayShape) >> firstArrayShapeBit), "}");
        type &= ~TArray;
    }
    take(TObject, "Object"_s);
    take(TOtherObject, "OtherObject"_s);
    take(TFinalObject, "FinalObject"_s);
    if (type & TFinalObjectTag) {
        auto layouts = layoutRangeOf(type);
        out.print(bar, "FinalObject#", layouts.lowest);
        if (!layouts.isOne())
            out.print("..", layouts.highest);
        type &= ~TFinalObject;
    }
    take(TMap, "Map"_s);
    take(TSet, "Set"_s);
    take(TWeakMap, "WeakMap"_s);
    take(TWeakSet, "WeakSet"_s);
    take(TRegExp, "RegExp"_s);
    take(TPromise, "Promise"_s);
    take(TDate, "Date"_s);
    take(TError, "Error"_s);
    take(TArrayBuffer, "ArrayBuffer"_s);
    take(TDataView, "DataView"_s);
    take(TStringObject, "StringObject"_s);
    take(TTypedArray, "TypedArray"_s);
    for (unsigned i = 0; i < NumberOfTypedArrayTypesExcludingDataView; ++i) {
        if (type & (1u << (firstTypedArrayBit + i))) {
            out.print(bar, "TypedArray", i);
            type &= ~(1u << (firstTypedArrayBit + i));
        }
    }
    take(TCellOther, "CellOther"_s);
    take(TEmpty, "Empty"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
