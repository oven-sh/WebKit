/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCaches.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTOperationHelpers.h"
#include "AOTSlotWatchpoint.h"
#include "AOTTypedObject.h"
#include "AOTTypedType.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "InlineCacheCompiler.h"
#include "JSBoundFunctionInlines.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "JSTypedArrayViewPrototype.h"
#include "MegamorphicCache.h"
#include "ObjectPropertyConditionSet.h"
#include <wtf/OSAllocator.h>
#if OS(LINUX)
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

std::optional<uint32_t> propertyLocation(PropertyOffset offset)
{
    int32_t location;
    if (isInlineOffset(offset))
        location = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
    else
        location = -(offset - firstOutOfLineOffset) - 2;
    constexpr int32_t limit = 1 << (Slot::offsetBits - 1);
    if (location < -limit || location >= limit)
        return std::nullopt;
    return (static_cast<uint32_t>(location) & Slot::offsetMask) | (location < 0 ? Slot::isIndirect : 0);
}

static const EncodedJSValue undefinedHolder[JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + 1] = { 0, 0, JSValue::ValueUndefined };
static_assert(JSObject::offsetOfInlineStorage() == 2 * sizeof(EncodedJSValue));

const EncodedJSValue* undefinedValueHolder()
{
    return undefinedHolder;
}

static void fill(VM& vm, Data* data, Slot* cache, Structure* structure, uint32_t offsetAndFlags, void* pointer)
{
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    if (offsetAndFlags & (Slot::isGetter | Slot::pointerIsCell | Slot::pointerIsNotCell))
        offsetAndFlags |= Slot::isIndirect;
    if (!(offsetAndFlags & Slot::flagsMask))
        attempts = 0;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->offset = offsetAndFlags | attempts;
    cache->pointer = pointer;
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    didFillSlot(vm, data);
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: slot ", RawPointer(cache), " filled: structure ", RawPointer(structure), " id ", structure->id().bits(), " offset and flags ", RawHex(cache->offset), " pointer ", RawPointer(pointer));
}

void makePrototypeChainWatchable(VM& vm, JSCell* base)
{
    if (!base->isObject())
        return;
    for (JSValue next = asObject(base)->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* prototype = asObject(next);
        Structure* structure = prototype->structure();
        if (!structure->hasMonoProto())
            return;
        if (!structure->transitionWatchpointSetHasBeenInvalidated() || structure->isDictionary() || !structure->propertyAccessesAreCacheable())
            continue;
        prototype->convertToDictionary(vm);
        prototype->flattenDictionaryObject(vm);
    }
}

using ConditionVector = Vector<ObjectPropertyCondition, 16>;

static bool appendConditions(VM& vm, JSGlobalObject* globalObject, Structure* structure, JSObject* holder, UniquedStringImpl* uid, PropertyCondition::Kind kindBelowHolder, ConditionVector& conditions)
{
    while (true) {
        if (structure->isProxy() || structure->hasPolyProto())
            return false;
        if (!holder && isTypedArrayType(structure->typeInfo().type()) && isCanonicalNumericIndexString(uid))
            return true;
        JSValue prototype = structure->prototypeForLookup(globalObject);
        if (prototype.isNull())
            return !holder;
        JSObject* object = asObject(prototype);
        bool isForAbsence = kindBelowHolder == PropertyCondition::Absence;
        if (auto* closure = isForAbsence ? dynamicDowncast<Typed::Closure>(object) : nullptr; closure && closure->isLazyClassObject()) {
            if (Options::useAOTOperationCounters()) [[unlikely]]
                runtimeTable(vm).countOperation("ClassObject", "made-in-a-chain-that-is-cached");
            closure->ensureClassObject(vm);
        }
        structure = object->structure();
        if (structure->isDictionary())
            return false;
        if (object != holder && !structure->propertyAccessesAreCacheableForAbsence() && !(isForAbsence && Typed::hasNoLazyPropertyNamed(vm, object, uid, false)))
            return false;
        ObjectPropertyCondition condition;
        if (object == holder) {
            unsigned attributes;
            PropertyOffset offset = structure->get(vm, uid, attributes);
            if (offset == invalidOffset)
                return false;
            condition = ObjectPropertyCondition::presenceWithoutBarrier(object, uid, offset, attributes);
        } else if (structure->hasPolyProto())
            return false;
        else if (kindBelowHolder == PropertyCondition::Absence)
            condition = ObjectPropertyCondition::absenceWithoutBarrier(object, uid, structure->storedPrototypeObject());
        else
            condition = ObjectPropertyCondition::absenceOfSetEffectWithoutBarrier(object, uid, structure->storedPrototypeObject());
        if (!condition.isStillValidAssumingImpurePropertyWatchpoint(Concurrency::MainThread))
            return false;
        conditions.append(condition);
        if (object == holder)
            return true;
    }
}

static bool watchChain(VM& vm, JSGlobalObject* globalObject, Data* data, Slot* cache, Structure* structure, JSObject* holder, UniquedStringImpl* uid)
{
    ConditionVector conditions;
    return appendConditions(vm, globalObject, structure, holder, uid, PropertyCondition::Absence, conditions) && watchConditions(vm, data, cache, conditions.span());
}

bool watchStores(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* structure, std::span<UniquedStringImpl* const> names)
{
    VM& vm = globalObject->vm();
    ConditionVector conditions;
    if (names.empty())
        return watchConditions(vm, data, cache, conditions.span());
    Vector<JSObject*, 4> prototypes;
    bool arePlain = !structure->isDictionary() && !structure->hasPolyProto() && !std::ranges::contains(names, vm.propertyNames->underscoreProto.impl());
    for (JSValue next = arePlain ? structure->storedPrototype() : jsNull(); next.isObject();) {
        JSObject* prototype = asObject(next);
        Typed::ClassPrototype::look(prototype);
        Structure* prototypeStructure = prototype->structure();
        TypeInfo info = prototypeStructure->typeInfo();
        bool overridesPut = info.overridesPut() && !prototype->inherits<Typed::PrototypeObject>();
        if (prototypeStructure->hasPolyProto() || overridesPut || info.overridesGetPrototype() || info.overridesGetOwnPropertySlot() || prototype->hasNonReifiedStaticProperties()) {
            arePlain = false;
            break;
        }
        prototypes.append(prototype);
        next = prototypeStructure->storedPrototype();
    }
    if (arePlain && watchForStores(vm, data, cache, prototypes.span()))
        return true;
    for (UniquedStringImpl* uid : names) {
        auto status = prepareChainForCaching(globalObject, object, uid, nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
        if (!appendConditions(vm, globalObject, structure, nullptr, uid, PropertyCondition::AbsenceOfSetEffect, conditions))
            return false;
    }
    return watchConditions(vm, data, cache, conditions.span());
}

static bool mayReplace(Slot* cache, Structure* structure)
{
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return !!cache->structureID && cache->structureID == structure->id();
    if (!cache->structureID || cache->structureID == structure->id())
        return true;
    cache->offset += 1u << Slot::attemptsShift;
    return true;
}

static void countFailure(Slot* cache)
{
    if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
        cache->offset += 1u << Slot::attemptsShift;
}

static bool abandon(Slot* cache)
{
    cache->offset |= Slot::attemptsMask;
    return false;
}

static bool tryCacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache, bool mayBePolymorphic);
static bool canUseMegamorphicCacheForGet(VM&, UniquedStringImpl*);
static bool cacheAbsentClosurePrototypeProperty(JSGlobalObject*, Instance&, JSCell* base, const Identifier&);
static bool hasNonExtensibleObjectPrototype(JSGlobalObject*, Structure*);
static bool cacheAbsentCompiledField(JSGlobalObject*, Data*, Slot* cache, JSCell* object, const Identifier&, bool mayBePolymorphic);

static auto& propertyNameIDs(VM& vm)
{
    auto& table = vm.aotPropertyNameIDs;
    if (!table.next) [[unlikely]] {
        RELEASE_ASSERT(table.ids.isEmpty());
        table.next = 1;
        if (Image* image = Image::withCode()) {
            table.startOfImage = std::bit_cast<uintptr_t>(image->at<uint8_t>(0));
            table.sizeOfImage = image->header().size;
        }
    }
    return table;
}

template<typename Table>
static ALWAYS_INLINE uint32_t* spareWordInHeader(const Table& table, UniquedStringImpl* uid)
{
    static_assert(sizeof(StringImpl) == 6 * sizeof(uint32_t));
    const void* characters = uid->is8Bit() ? static_cast<const void*>(uid->span8().data()) : static_cast<const void*>(uid->span16().data());
    if (std::bit_cast<uintptr_t>(characters) - table.startOfImage >= table.sizeOfImage || uid->isSymbol())
        return nullptr;
    return reinterpret_cast<uint32_t*>(uid) + 5;
}

static uint16_t propertyNameID(VM& vm, UniquedStringImpl* uid)
{
    auto& table = propertyNameIDs(vm);
    uint32_t* spareWord = spareWordInHeader(table, uid);
    if (spareWord) {
        uint32_t id = *spareWord;
        if (id && id < table.next && table.namesWithIDInHeader[id - 1] == uid)
            return static_cast<uint16_t>(id);
    } else if (uint16_t id = table.ids.get(uid))
        return id;
    if (table.next >= Structure::firstReservedPropertyNameID)
        return 0;
    uint16_t id = static_cast<uint16_t>(table.next++);
    table.namesWithIDInHeader.append(spareWord ? uid : nullptr);
    if (spareWord)
        *spareWord = id;
    else
        table.ids.add(uid, id);
    return id;
}

static uint16_t recordPropertyNameInStructure(VM& vm, JSCell* base, Structure* structure, const Identifier& ident, const PropertySlot& slot)
{
    if (!slot.isCacheableValue() || slot.slotBase() != base || slot.attributes())
        return 0;
    PropertyOffset offset = slot.cachedOffset();
    if (!isInlineOffset(offset) || static_cast<unsigned>(offset) >= Structure::numberOfSlotsWithPropertyNameIDs)
        return 0;
    if (structure->isUncacheableDictionary())
        return 0;
    if (structure->mayBePrototype())
        return 0;
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return 0;
    uint16_t id = propertyNameID(vm, ident.impl());
    if (!id)
        return 0;
    RELEASE_ASSERT(!structure->fieldIDInSlot(offset) || structure->fieldIDInSlot(offset) == Structure::noPropertyNameID || structure->fieldIDInSlot(offset) == id);
    structure->setPropertyNameIDInInlineSlot(offset, id);
    return id;
}

void setInlineSlotPropertyNameIDs(VM& vm, Structure* structure, std::span<UniquedStringImpl* const> names)
{
    if (structure->isDictionary() || structure->mayBePrototype())
        return;
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint() || names.size() > structure->inlineCapacity())
        return;
    auto& ids = structure->existingAOTFields().fieldIDInSlot;
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot) {
        if (ids[slot])
            continue;
        uint16_t id = slot < names.size() ? propertyNameID(vm, names[slot]) : 0;
        ids[slot] = id ? id : Structure::noPropertyNameID;
    }
}

static bool fillPropertyNameTable(VM& vm, Structure* structure)
{
    bool isFullyFilled = true;
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot)
        isFullyFilled &= !!structure->fieldIDInSlot(slot);
    if (isFullyFilled)
        return true;
    uint16_t ids[Structure::numberOfSlotsWithPropertyNameIDs];
    std::ranges::fill(ids, Structure::noPropertyNameID);
    if (!structure->isUncacheableDictionary() && !structure->mayBePrototype() && structure->propertyAccessesAreCacheable() && !structure->needImpurePropertyWatchpoint() && !structure->typeInfo().overridesGetOwnPropertySlot()) {
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (static_cast<unsigned>(entry.offset()) < Structure::numberOfSlotsWithPropertyNameIDs && !entry.attributes()) {
                if (uint16_t id = propertyNameID(vm, entry.key()))
                    ids[entry.offset()] = id;
            }
            return true;
        });
    }
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot) {
        if (structure->fieldIDInSlot(slot))
            continue;
        structure->setPropertyNameIDInInlineSlot(slot, ids[slot]);
    }
    return true;
}

static bool cacheByName(VM& vm, PolymorphicSlots* several, JSCell* base, Structure* structure, const PropertySlot& slot)
{
    bool isInNamedSlot = slot.isCacheableValue() && slot.slotBase() == base && static_cast<unsigned>(slot.cachedOffset()) < Structure::numberOfSlotsWithPropertyNameIDs;
    if (!isInNamedSlot && !several->numberOfUsedInlineNameSlots)
        return false;
    if (!fillPropertyNameTable(vm, structure)) {
        countOperationNamed(several->owner->instance, __func__, several->fillsNameTables() ? "structure-records-no-names" : "site-no-longer-fills-name-tables");
        if (several->fillsNameTables())
            several->didFailToFillNameTable();
        return false;
    }
    if (!isInNamedSlot)
        return false;
    uint16_t id = structure->fieldIDInSlot(slot.cachedOffset());
    return id < Structure::firstReservedPropertyNameID && several->addInlineNameSlot(id, slot.cachedOffset());
}

static PolymorphicSlots* siteSlots(VM& vm, Data* data, Slot* cache, Structure* structure, const Identifier& ident)
{
    if (cache->isNameOnly()) {
        PolymorphicSlots* several = data->instance->createSiteSlots(data, cache, ident.impl());
        several->addInlineNameSlot(cache->offset >> Slot::nameIDShift, (cache->offset & Slot::directLocationMask) - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
    }
    if (!cache->isPolymorphic()) {
        if (!cache->structureID || cache->structureID == structure->id())
            return nullptr;
        PolymorphicSlots* several = data->instance->createSiteSlots(data, cache, ident.impl());
        Slot& first = several->slots[PolymorphicSlots::indexFor(cache->structureID)];
        first.offset = cache->offset & ~Slot::attemptsMask;
        first.pointer = cache->pointer;
        moveWatching(data, cache, &first);
        WTF::storeStoreFence();
        first.structureID = cache->structureID;
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
        if (uint16_t id = first.offset >> Slot::nameIDShift; id && !(first.offset & Slot::flagsMask))
            several->addInlineNameSlot(id, (first.offset & Slot::directLocationMask) - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
    }
    return static_cast<PolymorphicSlots*>(cache->pointer);
}

static Slot* findFillableEntry(Data* data, PolymorphicSlots* several, Structure* structure)
{
    uint32_t attemptsSoFar = static_cast<uint32_t>(several->shouldRefillEarly) << Slot::attemptsShift;
    if (several->remainingBulkFillAttempts)
        several->remainingBulkFillAttempts--;
    if (Slot* held = several->slotOf(structure->id()))
        return held;
    unsigned index = PolymorphicSlots::indexFor(structure->id());
    for (Slot* free : { &several->slots[index], &several->slots[index ^ 1] }) {
        if (free->structureID || (free->offset & Slot::attemptsMask) == Slot::attemptsMask)
            continue;
        if (!(free->offset & Slot::attemptsMask))
            free->offset += attemptsSoFar;
        return free;
    }
    uint32_t& random = several->owner->instance->keyedTableRandomState;
    random = random * 1664525u + 1013904223u;
    Slot& leaving = several->slots[index ^ random >> 31];
    stopWatching(data, &leaving);
    leaving.clear();
    leaving.offset = attemptsSoFar;
    data->slotEpoch++;
    return &leaving;
}

static NEVER_INLINE bool shouldSkipRefill(PolymorphicSlots* several, JSCell* base, Structure* structureBefore, const PropertySlot& slot)
{
    if (several->isDueForRefill() || base->type() != FinalObjectType || structureBefore->isDictionary() || !(slot.isUnset() || slot.isCacheableValue()))
        return false;
    several->didMissWithoutRefill();
    return true;
}

static void cacheGetterCode(GetterSetter*, Instance*, uint64_t closureWord);
static void cacheGetterShortcut(GetterSetter*, JSFunction* getter);

enum class ExtraProperty : uint8_t { Accessor, Value, Absent };

static std::optional<uint64_t> cacheExtraProperty(VM& vm, Instance& instance, Typed::Object* object, const Identifier& ident, ExtraProperty wanted)
{
    JSObject* extra = object->extraProperties();
    if (!extra || &object->state() != instance.typed || instance.typed->isCounting() || extra->structure()->isDictionary() || parseIndex(ident) || ident.isPrivateName())
        return std::nullopt;
    unsigned attributes;
    PropertyOffset offset = extra->structure()->get(vm, ident.impl(), attributes);
    uint32_t offsetAndKind = Slot::extraPropertyIsAbsent;
    if (isValidOffset(offset)) {
        bool isAccessor = attributes & PropertyAttribute::Accessor;
        if (wanted != (isAccessor ? ExtraProperty::Accessor : ExtraProperty::Value) || (!isAccessor && attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            return std::nullopt;
        if (isInlineOffset(offset))
            offsetAndKind = JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue);
        else
            offsetAndKind = Slot::extraPropertyIsOutOfLine | static_cast<uint32_t>(-offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue)));
        if (!isAccessor)
            offsetAndKind |= Slot::extraPropertyIsValue;
    } else if (wanted != ExtraProperty::Absent)
        return std::nullopt;
    return Slot::extraPropertyAt(extra->structureID().bits(), offsetAndKind);
}

static uint64_t lazyClosureOf(Typed::Object* object, const Typed::FieldData& field)
{
    if (!(field.flags & Typed::fieldHoldsLazyClosure))
        return 0;
    static_assert(Typed::numberOfSlimClosureKinds == 2);
    uint32_t function = field.literalsOffset() & ~Typed::lazyClosureCapturesObject;
    return Slot::fieldHoldsLazyClosure | static_cast<uint64_t>(function) << Slot::fieldClosureShift
        | static_cast<uint64_t>(Typed::slimClosureKindOf(object->state().kindAndDeclarationOf(function))) << Slot::fieldClosureKindShift
        | (field.literalsOffset() & Typed::lazyClosureCapturesObject ? Slot::fieldClosureCapturesHolder : 0);
}

static bool alreadyDescribes(const Slot* cache, Structure* structure, uint64_t description)
{
    return cache->structureID == structure->id() && (cache->offset & Slot::flagsMask) == Slot::compiledFieldFlags && std::bit_cast<uint64_t>(cache->pointer) == description;
}

static Slot* siteSlotFor(VM& vm, Data* data, Slot* cache, Structure* structure, const Identifier& ident, bool mayBePolymorphic)
{
    if (!mayBePolymorphic)
        return cache->isNameOnly() || cache->isPolymorphic() ? nullptr : cache;
    if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident))
        return findFillableEntry(data, several, structure);
    return cache;
}

static bool cacheExtraAccessorOrAbsence(JSGlobalObject* globalObject, Data* data, Slot* cache, JSCell* base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, bool mayBePolymorphic)
{
    VM& vm = globalObject->vm();
    const Typed::LayoutData* layout = structureBefore->compiledLayout();
    if ((!slot.isAccessor() && !slot.isUnset()) || !layout || !Typed::isObjectLayout(layout->kind) || base->structure() != structureBefore)
        return false;
    auto* object = uncheckedDowncast<Typed::Object>(base);
    JSObject* extra = object->extraProperties();
    if (!extra || object->fieldNamed(object->state(), ident))
        return false;
    if (slot.isAccessor()) {
        if (slot.slotBase() != extra)
            return false;
    } else {
        JSObject* prototype = structureBefore->storedPrototypeObject();
        auto status = prototype ? prepareChainForCaching(globalObject, prototype, ident.impl(), nullptr) : std::optional(PrototypeChainCachingStatus { });
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
    }
    auto description = cacheExtraProperty(vm, *data->instance, object, ident, slot.isAccessor() ? ExtraProperty::Accessor : ExtraProperty::Absent);
    if (!description)
        return false;
    if (slot.isAccessor()) {
        GetterSetter* getterSetter = slot.getterSetter();
        if (auto* function = dynamicDowncast<JSFunction>(getterSetter->getter()))
            cacheGetterShortcut(getterSetter, function);
        else if (auto* closure = dynamicDowncast<Typed::SlimClosure>(getterSetter->getter()); closure && !getterSetter->getterShortcut()) {
            Instance* instance = closure->structure()->aotInstance();
            cacheGetterCode(getterSetter, instance, instance->typedFunctions[closure->functionIndex()]);
        }
    } else
        makePrototypeChainWatchable(vm, base);
    cache = siteSlotFor(vm, data, cache, structureBefore, ident, mayBePolymorphic);
    if (!cache)
        return false;
    if (alreadyDescribes(cache, structureBefore, *description) || !mayReplace(cache, structureBefore))
        return true;
    if (slot.isAccessor()) {
        if (cache->hasPointer())
            stopWatching(data, cache);
    } else if (!watchChain(vm, globalObject, data, cache, structureBefore, nullptr, ident.impl())) {
        countFailure(cache);
        return true;
    }
    fill(vm, data, cache, structureBefore, Slot::compiledFieldFlags, std::bit_cast<void*>(*description));
    return true;
}

static void cacheExportForAllSites(JSGlobalObject* globalObject, JSCell* base, Structure* structureBefore, const Identifier& ident, const PropertySlot::ModuleNamespaceSlot& namespaceSlot)
{
    VM& vm = globalObject->vm();
    ScopeOffset offset(namespaceSlot.scopeOffset);
    size_t place = (JSLexicalEnvironment::offsetOfVariable(offset) - JSObject::offsetOfInlineStorage()) / sizeof(EncodedJSValue);
    if (base->structure() != structureBefore || structureBefore == globalObject->moduleNamespaceObjectStructure() || place >= static_cast<size_t>(firstOutOfLineOffset))
        return;
    if (!canUseMegamorphicCacheForGet(vm, ident.impl()) || !namespaceSlot.environment->variableAt(offset).get())
        return;
    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    if (!cache.findLoad(structureBefore->id(), ident.impl()))
        cache.initAsPermanentHit(structureBefore->id(), ident.impl(), namespaceSlot.environment, place);
}

void cacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache, bool mayBePolymorphic)
{
    if (auto namespaceSlot = slot.moduleNamespaceSlot(); namespaceSlot && base == slot.slotBase())
        cacheExportForAllSites(globalObject, base.asCell(), structureBefore, ident, *namespaceSlot);
    if (SharedData::contains(cache))
        return;
    if (base.isCell() && cacheExtraAccessorOrAbsence(globalObject, data, cache, base.asCell(), structureBefore, ident, slot, mayBePolymorphic))
        return;
    if (slot.isUnset() && base.isObject() && structureBefore->compiledLayout() && base.asCell()->structure() == structureBefore && cacheAbsentCompiledField(globalObject, data, cache, base.asCell(), ident, mayBePolymorphic))
        return;
    if (auto namespaceSlot = slot.moduleNamespaceSlot()) {
        if (base != slot.slotBase())
            return;
        Structure* structure = base.asCell()->structure();
        uint32_t location = JSLexicalEnvironment::offsetOfVariable(ScopeOffset(namespaceSlot->scopeOffset)) / sizeof(EncodedJSValue);
        if (structure == structureBefore && structure != globalObject->moduleNamespaceObjectStructure() && location < (1u << (Slot::offsetBits - 1)) && mayReplace(cache, structure)) {
            if (cache->hasPointer())
                stopWatching(data, cache);
            fill(globalObject->vm(), data, cache, structure, location | Slot::pointerIsCell, namespaceSlot->environment);
        }
        return;
    }
    if (mayBePolymorphic && base.isCell() && base.asCell()->structure() == structureBefore) {
        if (uint16_t id = recordPropertyNameInStructure(globalObject->vm(), base.asCell(), structureBefore, ident, slot)) {
            uint32_t location = *propertyLocation(slot.cachedOffset());
            uint32_t sameAccess = location | static_cast<uint32_t>(id) << Slot::nameIDShift;
            if (cache->isNameOnly() && cache->offset == sameAccess)
                return;
            if (uint32_t cachedAccess = cache->offset & ~Slot::attemptsMask; cache->structureID && (cachedAccess == sameAccess || cachedAccess == location)) {
                Structure* cached = WTF::opaque(cache->structureID.decode());
                if (fillPropertyNameTable(globalObject->vm(), cached) && cached->fieldIDInSlot(slot.cachedOffset()) == id) {
                    cache->structureID = StructureID();
                    cache->offset = sameAccess;
                    data->slotEpoch++;
                    return;
                }
            }
        }
    }
    if (mayBePolymorphic && base.isCell()) {
        VM& vm = globalObject->vm();
        Structure* structure = base.asCell()->structure();
        if (!cache->isPolymorphic() && !cache->isNameOnly() && cache->structureID && cache->structureID != structure->id() && structure->transitivelyTransitionedFrom(cache->structureID.decode())) {
            uint32_t attemptsBefore = cache->offset & Slot::attemptsMask;
            cache->offset = (cache->offset & ~Slot::attemptsMask) | 2u << Slot::attemptsShift;
            if (tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache, mayBePolymorphic)) {
                countOperationNamed(data->instance, __func__, "moved-on");
                return;
            }
            cache->offset = (cache->offset & ~Slot::attemptsMask) | attemptsBefore;
        }
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident)) {
            bool isHandledByName = structure == structureBefore && cacheByName(vm, several, base.asCell(), structure, slot);
            if (!isHandledByName && shouldSkipRefill(several, base.asCell(), structureBefore, slot))
                return;
            cache = findFillableEntry(data, several, structure);
        }
    }
    if (!tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache, mayBePolymorphic))
        countFailure(cache);
}

static std::optional<Slot::FieldRead> fieldReadFor(Typed::Store store)
{
    using Typed::Store;
    switch (store) {
    case Store::JSValue:
    case Store::Cell:
        return Slot::FieldRead::Boxed;
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        return Slot::FieldRead::Cell32;
    case Store::Int32OrNumberBox:
        return Slot::FieldRead::Int32OrNumberBox;
    case Store::Int32:
        return Slot::FieldRead::Int32;
    case Store::Double:
    case Store::OptionalDouble:
        return Slot::FieldRead::Double;
    case Store::Boolean:
        return Slot::FieldRead::Boolean;
    case Store::OptionalBoolean:
        return Slot::FieldRead::OptionalBoolean;
    case Store::Int64:
    case Store::OptionalInt64:
        return Slot::FieldRead::Int64;
    case Store::Uint8:
        return Slot::FieldRead::Uint8;
    case Store::Int8:
        return Slot::FieldRead::Int8;
    case Store::Uint16:
        return Slot::FieldRead::Uint16;
    case Store::Int16:
        return Slot::FieldRead::Int16;
    case Store::Uint32:
        return Slot::FieldRead::Uint32;
    case Store::Float:
    case Store::LiteralString:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool cacheAbsentCompiledField(JSGlobalObject* globalObject, Data* data, Slot* cache, JSCell* cell, const Identifier& ident, bool mayBePolymorphic)
{
    VM& vm = globalObject->vm();
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Structure* structure = object->structure();
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    if (!field)
        return false;
    auto read = fieldReadFor(field->store());
    JSObject* prototype = structure->storedPrototypeObject();
    auto status = prototype ? prepareChainForCaching(globalObject, prototype, ident.impl(), nullptr) : std::optional(PrototypeChainCachingStatus { });
    if (!read || !status || status->flattenedDictionary || status->usesPolyProto) {
        countFailure(cache);
        return true;
    }
    makePrototypeChainWatchable(vm, cell);
    if (mayBePolymorphic) {
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident))
            cache = findFillableEntry(data, several, structure);
    }
    if (!mayReplace(cache, structure))
        return true;
    if (!watchChain(vm, globalObject, data, cache, structure, nullptr, ident.impl())) {
        countFailure(cache);
        return true;
    }
    uint64_t description = Slot::absentFieldIsUndefined | (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | static_cast<uint64_t>(*read) << Slot::fieldReadShift | field->offset;
    fill(vm, data, cache, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(description));
    return true;
}

void cacheNamedFieldRead(Instance* instance, JSCell* cell, const Identifier& ident, uint32_t name)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    if (auto read = field ? fieldReadFor(field->store()) : std::nullopt)
        Instance::cacheNamedField(instance->namedFieldReads, Typed::shapeWordOf(object->shape()), name, (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | static_cast<uint64_t>(*read) << Slot::fieldReadShift | field->offset);
}

void cacheCompiledField(VM& vm, Data* data, Slot* cache, JSCell* cell, const Identifier& ident, bool mayBePolymorphic)
{
    if (SharedData::contains(cache))
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Structure* structure = object->structure();
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    if (field && field->flags & Typed::fieldMayExistWithoutStorage && cacheAbsentCompiledField(structure->globalObject(), data, cache, cell, ident, mayBePolymorphic))
        return;
    auto read = field ? fieldReadFor(field->store()) : !ident.isPrivateName() && !parseIndex(ident) ? std::optional(Slot::FieldRead::NoField) : std::nullopt;
    if (!read) {
        countFailure(cache);
        return;
    }
    if (mayBePolymorphic) {
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident)) {
            if (field)
                several->identifierIndex &= ~PolymorphicSlots::hasNotReadCompiledField;
            cache = findFillableEntry(data, several, structure);
        }
    }
    auto extraValue = field ? std::nullopt : cacheExtraProperty(vm, *data->instance, object, ident, ExtraProperty::Value);
    if (extraValue && alreadyDescribes(cache, structure, *extraValue))
        return;
    if (!mayReplace(cache, structure))
        return;
    if (cache->hasPointer())
        stopWatching(data, cache);
    static_assert(sizeof(void*) == sizeof(uint64_t));
    uint64_t description = static_cast<uint64_t>(*read) << Slot::fieldReadShift;
    if (field)
        description |= (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | field->offset | lazyClosureOf(object, *field);
    else if (extraValue)
        description = *extraValue;
    fill(vm, data, cache, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(description));
}

static bool isPermanentlyAbsentFromPrototypeChain(JSGlobalObject* globalObject, Structure* structure, PropertyName name)
{
    VM& vm = globalObject->vm();
    if (structure->hasPolyProto())
        return false;
    for (JSValue next = structure->storedPrototype(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* prototype = asObject(next);
        Structure* ofPrototype = prototype->structure();
        TypeInfo info = ofPrototype->typeInfo();
        if (ofPrototype->isStructureExtensible() || ofPrototype->hasPolyProto() || info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || info.overridesIsExtensible())
            return false;
        PropertySlot slot(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (JSObject::getOwnPropertySlot(prototype, globalObject, name, slot))
            return false;
    }
    return true;
}

namespace Typed {

bool absentFieldReadsAsUndefined(JSGlobalObject* globalObject, JSCell* cell, UniquedStringImpl* name)
{
    Structure* structure = cell->structure();
    const LayoutData* layout = structure->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    auto* object = uncheckedDowncast<Object>(cell);
    State& state = object->state();
    return state.isStructureWithoutExtraProperties(structure) && object->fieldNamed(state, name) && isPermanentlyAbsentFromPrototypeChain(globalObject, structure, name);
}

bool structureGuaranteesUndefinedRead(JSGlobalObject* globalObject, JSCell* cell, UniquedStringImpl* name)
{
    Structure* structure = cell->structure();
    return Object::structureGuaranteesAbsence(structure, name) && isPermanentlyAbsentFromPrototypeChain(globalObject, structure, name);
}

bool compiledObjectStructureGuaranteesAbsence(Structure* structure, UniquedStringImpl* name)
{
    return Object::structureGuaranteesAbsence(structure, name);
}

void countIfCompiledObject(VM& vm, JSValue value, const char* operation)
{
    if (!value.isCell())
        return;
    const LayoutData* layout = value.asCell()->structure()->compiledLayout();
    if (layout && isObjectLayout(layout->kind))
        runtimeTable(vm).countOperation(operation);
}

} // namespace Typed

void cacheKeyedCompiledFieldRead(JSGlobalObject* globalObject, Instance::KeyedRead& read, JSObject* base, const Identifier& ident)
{
    const Typed::LayoutData* layout = base->structure()->compiledLayout();
    if (!layout || !Typed::isObjectLayout(layout->kind))
        return;
    auto* object = uncheckedDowncast<Typed::Object>(base);
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    auto kind = field ? fieldReadFor(field->store()) : std::nullopt;
    if (!kind)
        return;
    uint64_t description = static_cast<uint64_t>(*kind) << Slot::fieldReadShift | field->offset;
    if (field->flags & Typed::fieldIsOutOfLine)
        description |= Slot::fieldIsInExtension;
    if (isPermanentlyAbsentFromPrototypeChain(globalObject, base->structure(), ident))
        description |= Slot::absentFieldIsUndefined;
    read.holder = std::bit_cast<JSCell*>(description);
    read.slot.offset = Instance::keyIsCompiledField;
}

static void cacheGetterCode(GetterSetter* getterSetter, Instance* instance, uint64_t closureWord)
{
    if (!(closureWord >> ClosureWord::numberOfParametersShift))
        getterSetter->setGetterCode(instance->code + (closureWord & ClosureWord::offsetMask) * ClosureWord::offsetUnit);
}

static void cacheGetterShortcut(GetterSetter* getterSetter, JSFunction* getter)
{
    if (getterSetter->getterShortcut())
        return;
    if (auto* closure = dynamicDowncast<Typed::Closure>(getter))
        cacheGetterCode(getterSetter, Instance::of(closure), closure->entry());
}

void cacheGetterShortcut(GetterSetter* getterSetter)
{
    if (auto* function = dynamicDowncast<JSFunction>(getterSetter->getter())) {
        if (Intrinsic intrinsic = function->intrinsic(); intrinsic == TypedArrayByteLengthIntrinsic || intrinsic == TypedArrayByteOffsetIntrinsic) {
            getterSetter->setGetterShortcut(intrinsic == TypedArrayByteLengthIntrinsic ? GetterSetter::getterReturnsTypedArrayByteLength : GetterSetter::getterReturnsTypedArrayByteOffset);
            return;
        }
        if (auto* executable = dynamicDowncast<FunctionExecutable>(function->executable()); executable && executable->isGeneratedForCall())
            executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
        cacheGetterShortcut(getterSetter, function);
    } else if (auto* closure = dynamicDowncast<Typed::SlimClosure>(getterSetter->getter()); closure && !getterSetter->getterShortcut()) {
        Instance* instance = closure->structure()->aotInstance();
        cacheGetterCode(getterSetter, instance, instance->typedFunctions[closure->functionIndex()]);
    }
}

static void cacheTypedArrayLength(JSGlobalObject* globalObject, Instance& instance, JSCell* cell, const PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Structure* structure = cell->structure();
    JSType type = cell->type();
    auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter());
    JSObject* typedArrayPrototype = slot.slotBase();
    unsigned attributes;
    if (!isTypedArrayType(type) || !function || !isTypedArrayViewPrototypeLengthGetter(function) || !typedArrayPrototype->inherits<JSTypedArrayViewPrototype>() || typedArrayPrototype->globalObject() != globalObject)
        return;
    if (!structure->propertyAccessesAreCacheable() || typedArrayPrototype->structure()->isDictionary() || typedArrayPrototype->structure()->get(vm, vm.propertyNames->length.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
        return;
    instance.typedArrayLengthGetter = function;
    auto prototypeOf = [](Structure* structure) -> JSObject* { return structure->hasMonoProto() && !structure->isDictionary() ? structure->storedPrototypeObject() : nullptr; };
    JSObject* prototype = prototypeOf(structure);
    JSObject* secondPrototype = prototype ? prototypeOf(prototype->structure()) : nullptr;
    if (secondPrototype == typedArrayPrototype)
        secondPrototype = prototype;
    if (!secondPrototype || prototypeOf(secondPrototype->structure()) != typedArrayPrototype || isInlineOffset(slot.cachedOffset()))
        return;
    instance.typedArrayViewPrototype = typedArrayPrototype;
    instance.typedArrayViewPrototypeStructureID = typedArrayPrototype->structureID().bits();
    instance.typedArrayLengthAccessor = slot.getterSetter();
    instance.typedArrayLengthOffsetInButterfly = offsetInButterfly(slot.cachedOffset()) * static_cast<intptr_t>(sizeof(EncodedJSValue));
    auto& entry = instance.typedArraysWithBuiltinLength[type - FirstTypedArrayType][structure != globalObject->typedArrayStructure(typedArrayType(type), false)];
    entry.structureID = 0;
    entry.prototype = prototype;
    entry.prototypeStructureID = prototype->structureID().bits();
    entry.secondPrototype = secondPrototype;
    entry.secondPrototypeStructureID = secondPrototype->structureID().bits();
    entry.structureID = structure->id().bits();
    vm.writeBarrier(instance.globalObject);
}

static std::optional<uint32_t> watchGlobalProxyTargetProperty(VM& vm, Data* data, Slot* cache, JSCell* proxy, JSObject* holder, UniquedStringImpl* uid, PropertyOffset offset, unsigned forbiddenAttributes)
{
    JSGlobalObject* target = uncheckedDowncast<JSGlobalProxy>(proxy)->target();
    Structure* structure = target->structure();
    if (holder != target || !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return std::nullopt;
    unsigned attributes;
    auto location = propertyLocation(offset);
    if (!location || structure->get(vm, uid, attributes) != offset || attributes & (forbiddenAttributes | PropertyAttribute::AccessorOrCustomAccessorOrValue) || !mayReplace(cache, proxy->structure()))
        return std::nullopt;
    bool isStore = forbiddenAttributes & PropertyAttribute::ReadOnly;
    if (isStore)
        structure->firePropertyReplacementWatchpointSet(vm, offset, "AOT: a store through the global proxy is cached");
    ObjectPropertyCondition condition = isStore ? ObjectPropertyCondition::replacementWithoutBarrier(target, uid, offset, attributes) : ObjectPropertyCondition::presenceWithoutBarrier(target, uid, offset, attributes);
    if (!watchConditions(vm, data, cache, std::span { &condition, 1 }))
        return std::nullopt;
    return location;
}

static NEVER_INLINE bool cacheLookUp(JSGlobalObject* globalObject, Data* data, Slot* cache, JSCell* base, const Identifier& ident, const PropertySlot& slot, bool mayBePolymorphic)
{
    VM& vm = globalObject->vm();
    Structure* structure = base->structure();
    if (!mayBePolymorphic || structure->compiledLayout() || !canUseMegamorphicCacheForGet(vm, ident.impl()))
        return false;
    Slot* entry = cache;
    if (cache >= data->slots && cache < data->slots + data->numSlots) {
        if (cache->hasPointer())
            stopWatching(data, cache);
        PolymorphicSlots* several = data->instance->createSiteSlots(data, cache, ident.impl());
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
        entry = findFillableEntry(data, several, structure);
    }
    if (!mayReplace(entry, structure))
        return false;
    if (entry->hasPointer())
        stopWatching(data, entry);
    uint32_t count = structure->dictionaryPropertyCount();
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        uint64_t before = std::bit_cast<uint64_t>(entry->pointer);
        if (entry->structureID == structure->id() && (entry->offset & Slot::flagsMask) == Slot::compiledFieldFlags && before == (Slot::isAbsentWhileCountUnchanged | static_cast<uint64_t>(count | Slot::countIsValidated) << Slot::countShift) && !slot.isUnset()) {
            dataLogLn("AOT: a slot says that ", ident.impl(), " is absent while the dictionary has ", count, " properties, and it has that many and the name is there: ", JSValue(base));
            CRASH();
        }
        count |= Slot::countIsValidated;
    }
    uint64_t said = Slot::isLookedUp;
    JSObject* prototype = globalObject->objectPrototype();
    if (slot.isUnset() && !structure->isUncacheableDictionary() && structure->hasMonoProto() && structure->storedPrototype() == JSValue(prototype) && !structure->typeInfo().overridesGetOwnPropertySlot()
        && hasNonExtensibleObjectPrototype(globalObject, globalObject->objectStructureForObjectConstructor()) && !isValidOffset(prototype->structure()->get(vm, ident.impl())))
        said = Slot::isAbsentWhileCountUnchanged | static_cast<uint64_t>(count) << Slot::countShift;
    if (said >> Slot::fieldReadShift != static_cast<uint64_t>(Slot::FieldRead::NoField))
        said = Slot::isLookedUp;
    fill(vm, data, entry, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(said));
    return true;
}

static bool tryCacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache, bool mayBePolymorphic)
{
    if (base.isCell() && slot.isCacheableGetter() && ident == globalObject->vm().propertyNames->length && base.asCell()->structure() == structureBefore)
        cacheTypedArrayLength(globalObject, *data->instance, base.asCell(), slot);
    uint32_t getterFlag = slot.isCacheableGetter() ? Slot::isGetter : 0;
    if (VM& vm = globalObject->vm(); base.isCell() && base.asCell()->type() == SlimClosureType && base.asCell()->structure() == structureBefore && (ident == vm.propertyNames->length || ident == vm.propertyNames->name)) {
        JSObject* holder = data->instance->typed->slimClosureLazyPropertyHolder(vm);
        unsigned attributes;
        auto location = propertyLocation(holder->structure()->get(vm, ident.impl(), attributes));
        if (!location || !mayReplace(cache, structureBefore))
            return false;
        if (cache->hasPointer())
            stopWatching(data, cache);
        fill(vm, data, cache, structureBefore, *location | Slot::pointerIsCell | Slot::isGetter, holder);
        return true;
    }
    if (!base.isCell() || (!slot.isCacheableValue() && !slot.isUnset() && !getterFlag))
        return false;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* structure = cell->structure();
    if (structure != structureBefore)
        return false;
    if (slot.isUnset() && ident == vm.propertyNames->prototype && cacheAbsentClosurePrototypeProperty(globalObject, *data->instance, cell, ident))
        return true;
    if (cell->type() == GlobalProxyType) {
        auto location = watchGlobalProxyTargetProperty(vm, data, cache, cell, slot.isCacheableValue() ? slot.slotBase() : nullptr, ident.impl(), slot.cachedOffset(), 0);
        if (!location)
            return false;
        fill(vm, data, cache, structure, *location | Slot::pointerIsCell, slot.slotBase());
        return true;
    }
    if (structure->isUncacheableDictionary() && !structure->typeInfo().prohibitsPropertyCaching() && cell->isObject()) {
        if (structure->hasBeenFlattenedBefore())
            return cacheLookUp(globalObject, data, cache, cell, ident, slot, mayBePolymorphic);
        asObject(cell)->flattenDictionaryObject(vm);
        return false;
    }
    bool hasCompiledLayout = structure->compiledLayout();
    if (hasCompiledLayout ? !Typed::Object::structureGuaranteesAbsence(structure, ident.impl()) || (!slot.isUnset() && slot.slotBase() == cell) : !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return false;
    if (getterFlag) {
        unsigned attributes;
        if (slot.slotBase()->structure()->get(vm, ident.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
            return false;
        cacheGetterShortcut(slot.getterSetter());
    }

    if (!slot.isUnset() && slot.slotBase() == cell) {
        auto location = propertyLocation(slot.cachedOffset());
        if (!location)
            return false;
        if (!mayReplace(cache, structure))
            return false;
        if (cache->hasPointer())
            stopWatching(data, cache);
        uint32_t nameID = recordPropertyNameInStructure(vm, cell, structure, ident, slot);
        fill(vm, data, cache, structure, *location | getterFlag | nameID << Slot::nameIDShift, (*location | getterFlag) & (Slot::isIndirect | Slot::isGetter) ? nullptr : ident.impl());
        return true;
    }

    if (slot.isUnset() && cell->type() == FinalObjectType && canUseMegamorphicCacheForGet(vm, ident.impl()) && hasNonExtensibleObjectPrototype(globalObject, structure)) {
        if (!mayReplace(cache, structure))
            return false;
        stopWatching(data, cache);
        fill(vm, data, cache, structure, *propertyLocation(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(undefinedHolder));
        return true;
    }

    uint32_t attempts = (cache->offset & Slot::attemptsMask) >> Slot::attemptsShift;
    if (attempts == Slot::maxAttempts)
        return false;
    bool stubCountsMisses = mayBePolymorphic && cache >= data->slots && cache < data->slots + data->numSlots;
    if (attempts >= (stubCountsMisses ? Slot::missesBeforeCaching + 2 : Slot::maxAttemptsForInheritedProperty)) {
        cache->offset |= Slot::attemptsMask;
        return false;
    }
    if (attempts < (stubCountsMisses ? 2u : 1u)) {
        cache->offset += 1u << Slot::attemptsShift;
        return false;
    }
    uint32_t attemptsWithThisOne = stubCountsMisses ? std::max(attempts, Slot::missesBeforeCaching) + 1 : attempts + 1;
    cache->offset = (cache->offset & ~Slot::attemptsMask) | attemptsWithThisOne << Slot::attemptsShift;

    if (structure->typeInfo().prohibitsPropertyCaching() && !hasCompiledLayout)
        return false;
    if (structure->isDictionary()) {
        if (structure->hasBeenFlattenedBefore())
            return cacheLookUp(globalObject, data, cache, cell, ident, slot, mayBePolymorphic);
        structure->flattenDictionaryStructure(vm, asObject(cell));
        return false;
    }
    auto* closure = dynamicDowncast<Typed::Closure>(cell);
    bool isLazyClassObject = closure && closure->isLazyClassObject();
    if (slot.isUnset() && !isLazyClassObject && structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() && !hasCompiledLayout && !Typed::hasNoLazyPropertyNamed(vm, cell, ident.impl()))
        return false;
    if (isLazyClassObject) {
        JSObject* parent = closure->getPrototypeDirect().getObject();
        Typed::State& state = *structure->aotInstance()->typed;
        if (!parent || !state.isLazySubclassStructureOf(vm, structure, parent) || !state.tryAssumeAbsentFromLazySubclasses(vm, parent, ident.impl()))
            return false;
    }
    if (Options::useAOTOperationCounters() && closure && closure->kind() == Typed::Format::FunctionKind::Constructor) [[unlikely]]
        runtimeTable(vm).countOperation("ClassObject", isLazyClassObject ? "passes-unmade" : "passes-made");
    if (slot.isUnset() && (cell->type() == JSFunctionType || isSlimClosureType(cell->type())) && (ident == vm.propertyNames->prototype || ident == vm.propertyNames->length || ident == vm.propertyNames->name))
        return false;
    JSCell* startOfChain = hasCompiledLayout ? structure->storedPrototypeObject() : cell;
    auto status = startOfChain ? prepareChainForCaching(globalObject, startOfChain, ident.impl(), slot) : std::optional(PrototypeChainCachingStatus { });
    if (!status || status->usesPolyProto)
        return false;
    if (status->flattenedDictionary) {
        cache->offset -= 1u << Slot::attemptsShift;
        return true;
    }
    makePrototypeChainWatchable(vm, cell);

    if (slot.isUnset()) {
        if (!watchChain(vm, globalObject, data, cache, structure, nullptr, ident.impl()))
            return false;
        fill(vm, data, cache, structure, *propertyLocation(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(undefinedHolder));
        return true;
    }

    auto location = propertyLocation(slot.cachedOffset());
    if (!location)
        return false;
    if (!watchChain(vm, globalObject, data, cache, structure, slot.slotBase(), ident.impl()))
        return false;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell | getterFlag, slot.slotBase());
    return true;
}

static std::span<const Typed::HostClassCachedGetter> s_hostClassCachedGetters;
static std::atomic<uint32_t> s_lastCustomGetterSerial;

void Typed::setHostClassCachedGetters(std::span<const HostClassCachedGetter> getters)
{
    s_hostClassCachedGetters = getters;
}

static uint32_t cachedValueOffsetFor(GetValueFunc getter)
{
    for (auto& row : s_hostClassCachedGetters) {
        if (GetValueFunc(row.getter) == getter)
            return row.offsetOfCachedValue;
    }
    return 0;
}

static void recordCustomGetter(VM& vm, Instance& instance, Structure* structure, UniquedStringImpl* uid, GetValueFunc getter, JSObject* passedForReceiver, JSGlobalObject* globalObject, uint32_t offsetOfCachedValue = 0)
{
    static_assert(hasOneBitSet(Slot::customGetterSerialStep) && Slot::customGetterSerialStep > 4);
    auto& record = instance.ensureCustomGetterFor(structure->id().bits(), uid);
    uint32_t lastSerial = s_lastCustomGetterSerial.load(std::memory_order_relaxed);
    while (lastSerial <= std::numeric_limits<uint32_t>::max() - Slot::customGetterSerialStep && !s_lastCustomGetterSerial.compare_exchange_weak(lastSerial, lastSerial + Slot::customGetterSerialStep, std::memory_order_relaxed)) { }
    uint32_t serial = lastSerial > std::numeric_limits<uint32_t>::max() - Slot::customGetterSerialStep ? 0 : lastSerial += Slot::customGetterSerialStep;
    record = { structure->id().bits(), serial, vm.megamorphicCache()->epochForName(*uid, structure->id()), uid, std::bit_cast<void*>(getter.taggedPtr()), passedForReceiver, globalObject, lastSerial, offsetOfCachedValue, { } };
}

static NEVER_INLINE bool cacheAbsentClosurePrototypeProperty(JSGlobalObject* globalObject, Instance& instance, JSCell* base, const Identifier& ident)
{
    VM& vm = globalObject->vm();
    Structure* structure = base->structure();
    if (!vm.megamorphicCache() || !Typed::closureHandlesPrototypeProperty(base) || structure->isDictionary() || !structure->propertyAccessesAreCacheable())
        return false;
    auto status = prepareChainForCaching(globalObject, base, ident.impl(), nullptr);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return false;
    ConditionVector conditions;
    if (!appendConditions(vm, globalObject, structure, nullptr, ident.impl(), PropertyCondition::Absence, conditions))
        return false;
    recordCustomGetter(vm, instance, structure, ident.impl(), Typed::closurePrototypePropertyGetter(), nullptr, structure->realm());
    return true;
}

void cacheCustomGetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, const Identifier& ident, const PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    if (!vm.megamorphicCache())
        return;
    Structure* structure = base->structure();
    JSObject* holder = slot.slotBase();
    if (base->type() == GlobalProxyType || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching())
        return;
    if (holder->structure()->isDictionary())
        return;
    if (holder == base) {
        if (!prepareChainForCaching(globalObject, holder, ident.impl(), holder))
            return;
    } else {
        if (auto* closure = dynamicDowncast<Typed::Closure>(base); closure && closure->isLazyClassObject())
            return;
        auto status = prepareChainForCaching(globalObject, base, ident.impl(), slot);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return;
        if (!generateConditionsForPrototypePropertyHitCustom(vm, globalObject, globalObject, structure, holder, ident.impl(), slot.attributes()).isValid())
            return;
    }
    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    if (auto domAttribute = slot.domAttribute(); domAttribute && !(passesHolder ? holder : base)->inherits(domAttribute->classInfo))
        return;
    uint32_t offsetOfCachedValue = !passesHolder && slot.domAttribute() ? cachedValueOffsetFor(slot.customGetter()) : 0;
    recordCustomGetter(vm, instance, structure, ident.impl(), slot.customGetter(), passesHolder ? holder : nullptr, holder->globalObject(), offsetOfCachedValue);
}

void cacheKnownCustomGetter(VM& vm, Instance& instance, Data* data, Slot* cache, Structure* structure, const Identifier& ident, const Instance::CustomGetter& known)
{
    if (!known.serial)
        return;
    if (SharedData::contains(cache) || cache->isNameOnly() || structure->compiledLayout())
        return;
    bool didTransitionFromCached = cache->structureID && cache->structureID != structure->id() && structure->transitivelyTransitionedFrom(cache->structureID.decode());
    if (PolymorphicSlots* several = didTransitionFromCached ? nullptr : siteSlots(vm, data, cache, structure, ident)) {
        unsigned index = PolymorphicSlots::indexFor(structure->id());
        bool bothSlotsAreOccupied = !several->slotOf(structure->id()) && several->slots[index].structureID && several->slots[index ^ 1].structureID;
        cache = bothSlotsAreOccupied ? &several->slots[index] : findFillableEntry(data, several, structure);
    }
    if (cache->structureID && !didTransitionFromCached && !mayReplace(cache, structure))
        return;
    if (cache->hasPointer())
        stopWatching(data, cache);
    fill(vm, data, cache, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(Slot::knownCustomGetterAt(static_cast<uint32_t>(std::bit_cast<uintptr_t>(&known) - std::bit_cast<uintptr_t>(instance.customGetters)), known.serial)));
}

std::optional<PropertyOffset> inheritedSetterOffset(JSGlobalObject* globalObject, JSObject* base, Structure* structure, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSObject* holder = slot.base();
    if (!vm.megamorphicCache() || holder == base || parseIndex(*ident.impl()))
        return std::nullopt;
    bool hasCompiledLayout = structure->compiledLayout();
    if (hasCompiledLayout ? !Typed::Object::structureGuaranteesAbsence(structure, ident.impl()) : base->type() == GlobalProxyType || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching() || isCopyOnWrite(structure->indexingMode()))
        return std::nullopt;
    if (holder->structure()->isDictionary())
        return std::nullopt;
    bool setterChangedStructure = base->structure() != structure;
    if (setterChangedStructure && (structure->hasPolyProto() || !structure->storedPrototypeObject()))
        return std::nullopt;
    auto status = prepareChainForCaching(globalObject, hasCompiledLayout || setterChangedStructure ? structure->storedPrototypeObject() : base, ident.impl(), holder);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return std::nullopt;
    ConditionVector conditions;
    if (!appendConditions(vm, globalObject, structure, holder, ident.impl(), PropertyCondition::Absence, conditions) || !(conditions.last().attributes() & PropertyAttribute::Accessor))
        return std::nullopt;
    PropertyOffset offset = conditions.last().offset();
    if (offset > MegamorphicCache::maxOffset)
        return std::nullopt;
    return offset;
}

void cacheInheritedSetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, Structure* structure, const Identifier& ident, const PutPropertySlot& slot)
{
    if (auto offset = inheritedSetterOffset(globalObject, base, structure, ident, slot))
        instance.inheritedSetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), globalObject->vm().megamorphicCache()->epochForName(*ident.impl(), structure->id()), static_cast<uint16_t>(*offset), ident.impl(), slot.base(), nullptr };
}

bool cacheNativeSetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, Structure* structure, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSObject* holder = slot.base();
    if (base->structure() != structure || parseIndex(*ident.impl()))
        return false;
    TypeInfo info = structure->typeInfo();
    if (base->type() == GlobalProxyType || structure->compiledLayout() || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || info.prohibitsPropertyCaching() || info.overridesPut() || info.wouldHaveFixedProperties() || structure->hasImmutableProperties())
        return false;
    auto status = prepareChainForCaching(globalObject, base, ident.impl(), holder);
    if (!status || status->flattenedDictionary || status->usesPolyProto || holder->structure()->isDictionary())
        return false;
    if (holder != base && !generateConditionsForPrototypePropertyHitCustom(vm, globalObject, globalObject, structure, holder, ident.impl(), static_cast<unsigned>(PropertyAttribute::None)).isValid())
        return false;
    instance.inheritedSetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.ensureMegamorphicCache().epochForName(*ident.impl(), structure->id()), 0, ident.impl(), holder, std::bit_cast<void*>(slot.customSetter().taggedPtr()) };
    instance.hasCachedNativeSetter = 1;
    return true;
}

static bool tryCacheInstanceOf(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype)
{
    VM& vm = globalObject->vm();
    Structure* structure = constructor->structure();
    if (structure != structureBefore || !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching())
        return false;
    if (structure->isDictionary()) {
        if (!structure->hasBeenFlattenedBefore())
            structure->flattenDictionaryStructure(vm, constructor);
        return false;
    }
    if (!prototype.isCacheableValue() || prototype.slotBase() != constructor)
        return false;
    if (!hasInstance.isCacheableValue() || hasInstance.slotBase() == constructor)
        return false;
    if (!(hasInstance.attributes() & PropertyAttribute::ReadOnly) || !(hasInstance.attributes() & PropertyAttribute::DontDelete))
        return false;
    auto location = propertyLocation(prototype.cachedOffset());
    if (!location || !mayReplace(cache, structure))
        return false;
    UniquedStringImpl* uid = vm.propertyNames->hasInstanceSymbol.impl();
    auto status = prepareChainForCaching(globalObject, constructor, uid, hasInstance);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return false;
    makePrototypeChainWatchable(vm, constructor);
    if (!watchChain(vm, globalObject, data, cache, structure, hasInstance.slotBase(), uid))
        return false;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell, hasInstance.slotBase());
    return true;
}

void cacheInstanceOf(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype)
{
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return;
    if (!tryCacheInstanceOf(globalObject, data, cache, constructor, structureBefore, hasInstance, prototype))
        countFailure(cache);
}

static bool tryCacheHasInstance(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, bool isCalled)
{
    VM& vm = globalObject->vm();
    Structure* structure = constructor->structure();
    if (structure != structureBefore || !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching() || structure->isDictionary())
        return false;
    if (!hasInstance.isCacheableValue())
        return false;
    JSObject* holder = hasInstance.slotBase();
    if (!isCalled && (holder == constructor || !(hasInstance.attributes() & PropertyAttribute::ReadOnly) || !(hasInstance.attributes() & PropertyAttribute::DontDelete)))
        return false;
    auto location = isCalled ? propertyLocation(hasInstance.cachedOffset()) : std::optional<uint32_t>(0);
    if (!location || !mayReplace(cache, structure))
        return false;
    if (cache->hasPointer())
        stopWatching(data, cache);
    if (holder != constructor) {
        UniquedStringImpl* uid = vm.propertyNames->hasInstanceSymbol.impl();
        auto status = prepareChainForCaching(globalObject, constructor, uid, hasInstance);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
        makePrototypeChainWatchable(vm, constructor);
        if (!watchChain(vm, globalObject, data, cache, structure, holder, uid))
            return false;
    }
    if (holder == constructor)
        fill(vm, data, cache, structure, *location | Slot::isGetter, nullptr);
    else
        fill(vm, data, cache, structure, *location | Slot::pointerIsCell | Slot::isGetter, holder);
    return true;
}

void cacheHasInstance(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, bool isCalled)
{
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return;
    if (!tryCacheHasInstance(globalObject, data, cache, constructor, structureBefore, hasInstance, isCalled))
        countFailure(cache);
}

void cacheKeyedAccess(Slot& entry, Structure* structure, JSCell* key, std::optional<PropertyOffset> offset)
{
    auto location = offset && structure->propertyAccessesAreCacheable() ? propertyLocation(*offset) : std::nullopt;
    entry.structureID = StructureID();
    entry.offset = location ? *location | Instance::keyMayNotBeEvicted : Slot::isGetter;
    entry.pointer = key;
    entry.structureID = structure->id();
}

bool hasOnlyNamedOutOfLineStorage(Structure* before, Structure* after)
{
    return !before->isDictionary() && !after->isDictionary() && before->typeInfoBlob() == after->typeInfoBlob() && !before->couldHaveIndexingHeader();
}

StorageAfterAddition storageAfterAddition(Structure* before, Structure* after, PropertyOffset offset)
{
    if (before->isDictionary() || after->isDictionary() || after->previousID() != before || before->typeInfoBlob() != after->typeInfoBlob())
        return StorageAfterAddition::Unsupported;
    unsigned capacityBefore = before->outOfLineCapacity();
    unsigned capacityAfter = after->outOfLineCapacity();
    if (capacityAfter == capacityBefore)
        return StorageAfterAddition::Stays;
    if (!hasOnlyNamedOutOfLineStorage(before, after) || offset != firstOutOfLineOffset + static_cast<PropertyOffset>(capacityBefore))
        return StorageAfterAddition::Unsupported;
    if (!capacityBefore)
        return capacityAfter == initialOutOfLineCapacity ? StorageAfterAddition::IsFirst : StorageAfterAddition::Unsupported;
    bool doublesCapacity = capacityBefore >= initialOutOfLineCapacity && capacityAfter == 2 * capacityBefore && capacityAfter * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff;
    return doublesCapacity ? StorageAfterAddition::Doubled : StorageAfterAddition::Unsupported;
}

void cacheKeyedDefinition(Instance* instance, VM& vm, Structure* before, Structure* after, JSCell* key, UniquedStringImpl* uid, unsigned definedAttributes, uintptr_t keyTagBits)
{
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->hasPolyProto();
    };
    if (!isPlain(before) || !isPlain(after))
        return;
    unsigned attributes;
    PropertyOffset offset = after->get(vm, uid, attributes);
    if (!isValidOffset(offset) || attributes != definedAttributes)
        return;
    key = std::bit_cast<JSCell*>(std::bit_cast<uintptr_t>(key) | keyTagBits);
    if (PropertyOffset offsetBefore = before->get(vm, uid, attributes); isValidOffset(offsetBefore)) {
        bool completesPair = attributes == definedAttributes;
        bool replacesValue = !attributes;
        bool staysInPlace = offsetBefore == offset && (before == after || (after->previousID() == before && before->typeInfoBlob() == after->typeInfoBlob() && before->outOfLineCapacity() == after->outOfLineCapacity()));
        if (!keyTagBits || !staysInPlace || (!completesPair && !replacesValue))
            return;
        if (before == after)
            before->didCachePropertyReplacement(vm, offset);
        auto& entry = instance->keyedAdditionFor(before->id(), key);
        cacheKeyedAccess(entry.slot, before, key, offset);
        entry.newStructureID = after->id();
        entry.epoch = MegamorphicCache::invalidEpoch;
        entry.newStorageCapacity = 0;
        entry.holderValueCount = completesPair ? Instance::keyedAdditionCompletesAccessorPair : 1;
        return;
    }
    StorageAfterAddition storage = storageAfterAddition(before, after, offset);
    if (storage == StorageAfterAddition::Unsupported)
        return;
    auto& entry = instance->keyedAdditionFor(before->id(), key);
    cacheKeyedAccess(entry.slot, before, key, offset);
    entry.newStructureID = after->id();
    entry.epoch = MegamorphicCache::invalidEpoch;
    entry.newStorageCapacity = storage == StorageAfterAddition::Stays ? 0 : after->outOfLineCapacity();
    entry.holderValueCount = !!keyTagBits;
}

void cachePrivateNameTransition(VM& vm, Data* data, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
        return;
    Structure* newStructure = base->structure();
    if (!oldStructure->propertyAccessesAreCacheable() || oldStructure->isDictionary() || oldStructure->mayBePrototype() || !newStructure->propertyAccessesAreCacheable() || newStructure->isDictionary())
        return;
    if (storageAfterAddition(oldStructure, newStructure, offset.value_or(0)) != StorageAfterAddition::Stays)
        return;
    auto location = propertyLocation(offset.value_or(0));
    if (!location || !mayReplace(cache, oldStructure))
        return;
    Slot& transition = cache[1];
    transition.structureID = StructureID();
    WTF::storeStoreFence();
    transition.offset = Slot::isIndirect;
    transition.pointer = nullptr;
    transition.newStructureID = newStructure->id();
    WTF::storeStoreFence();
    transition.structureID = oldStructure->id();
    data->instance->didCacheTransition(&transition);
    fill(vm, data, cache, oldStructure, *location | Slot::pointerIsCell, name.asCell());
}

void cacheKeyedDelete(VM& vm, Data* data, Slot* cache, JSObject* base, Structure* oldStructure, JSValue key, std::optional<PropertyOffset> offset)
{
    if (SharedData::contains(cache))
        return;
    countFailure(cache);
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask) {
        cache[0].clear();
        cache[1].clear();
        data->slotEpoch++;
        return;
    }
    if (offset)
        cachePrivateNameTransition(vm, data, cache, base, oldStructure, key, offset);
}

void cacheKeyedDeletion(Instance* instance, Structure* before, Structure* after, JSValue key, UniquedStringImpl* uid, const DeletePropertySlot& slot)
{
    auto isPlain = [](Structure* structure) {
        return structure->typeInfo().type() == FinalObjectType && !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->hasPolyProto();
    };
    if ((!key.isString() && !key.isSymbol()) || !slot.isCacheableDelete() || !isPlain(before) || !isPlain(after) || parseIndex(*uid))
        return;
    PropertyOffset offset = 0;
    if (slot.isDeleteHit()) {
        offset = slot.cachedOffset();
        if (storageAfterAddition(before, after, offset) != StorageAfterAddition::Stays)
            return;
    } else if (!slot.isConfigurableDeleteMiss() || before != after)
        return;
    JSCell* taggedKey = std::bit_cast<JSCell*>(std::bit_cast<uintptr_t>(key.asCell()) | Instance::keyedAdditionIsDeletion);
    auto& entry = instance->keyedAdditionFor(before->id(), taggedKey);
    cacheKeyedAccess(entry.slot, before, taggedKey, offset);
    entry.newStructureID = after->id();
    entry.epoch = MegamorphicCache::invalidEpoch;
    entry.newStorageCapacity = 0;
    entry.holderValueCount = 1;
}

static bool tryCachePutById(JSGlobalObject*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

static uint64_t fieldWriteFor(VM& vm, Typed::Object* object, const Identifier& ident, bool acceptsAnyMatchingValue = false, bool mayBeInExtension = false)
{
    using FieldWrite = Slot::FieldWrite;
    using Typed::Store;
    namespace Primitive = Typed::Primitive;
    auto access = object->fieldAccessFor(vm, ident, acceptsAnyMatchingValue);
    uint32_t description = 0;
    uint32_t typeOfAcceptedObjects = 0;
    if (access && (!access->isOutOfLine || mayBeInExtension) && access->offset < 1u << FieldWrite::offsetBits) {
        uint16_t primitives = access->acceptedPrimitives;
        bool acceptsAnyNumber = (primitives & Primitive::Number) == Primitive::Number;
        uint32_t numbers = (access->acceptsInt32 ? FieldWrite::acceptsInt32 : 0) | (acceptsAnyNumber ? FieldWrite::acceptsNumber : 0);
        uint32_t undefinedOrNull = (primitives & Primitive::Undefined ? FieldWrite::acceptsUndefined : 0) | (primitives & Primitive::Null ? FieldWrite::acceptsNull : 0);
        uint32_t boolean = (primitives & Primitive::Boolean) == Primitive::Boolean ? FieldWrite::acceptsBoolean : 0;
        uint32_t stringOrSymbol = (primitives & Primitive::String ? FieldWrite::acceptsString : 0) | (primitives & Primitive::Symbol ? FieldWrite::acceptsSymbol : 0);
        uint32_t otherCells = (primitives & Primitive::BigInt ? FieldWrite::acceptsBigInt : 0) | (access->acceptsAnyObject ? FieldWrite::acceptsObject : 0);
        switch (access->store) {
        case Store::JSValue:
            description = FieldWrite::Boxed | numbers | undefinedOrNull | boolean | stringOrSymbol | otherCells;
            typeOfAcceptedObjects = access->typeOfAcceptedObjects;
            break;
        case Store::Cell:
            description = FieldWrite::Boxed | stringOrSymbol | otherCells;
            typeOfAcceptedObjects = access->typeOfAcceptedObjects;
            break;
        case Store::Cell32:
            description = FieldWrite::Cell32 | stringOrSymbol;
            typeOfAcceptedObjects = access->typeOfAcceptedObjects;
            break;
        case Store::OptionalCell32:
            description = FieldWrite::Cell32 | stringOrSymbol | undefinedOrNull;
            typeOfAcceptedObjects = access->typeOfAcceptedObjects;
            break;
        case Store::LiteralString32:
            description = FieldWrite::Cell32 | FieldWrite::storesLiteralStringWords | (stringOrSymbol & FieldWrite::acceptsString) | undefinedOrNull;
            break;
        case Store::Int32OrNumberBox:
            description = FieldWrite::Int32OrNumberBox | numbers;
            break;
        case Store::Double:
            description = FieldWrite::Double | numbers;
            break;
        case Store::OptionalDouble:
            description = FieldWrite::Double | FieldWrite::isOptional | numbers;
            break;
        case Store::Boolean:
            description = FieldWrite::Boolean | boolean;
            break;
        case Store::OptionalBoolean:
            description = FieldWrite::Boolean | FieldWrite::isOptional | boolean | undefinedOrNull;
            break;
        case Store::Int64:
            description = FieldWrite::Int64 | (numbers & FieldWrite::acceptsInt32);
            break;
        case Store::OptionalInt64:
            description = FieldWrite::Int64 | FieldWrite::isOptional | (numbers & FieldWrite::acceptsInt32) | undefinedOrNull;
            break;
        case Store::Int32:
            description = FieldWrite::Int32 | (numbers & FieldWrite::acceptsInt32);
            break;
        default:
            break;
        }
    }
    if (!(description & ~(FieldWrite::representationMask | FieldWrite::isOptional)) && !typeOfAcceptedObjects)
        return 0;
    if (access->isOutOfLine)
        description |= FieldWrite::isInExtension;
    return static_cast<uint64_t>(description | FieldWrite::isFieldWrite) << 32 | FieldWrite::withType(typeOfAcceptedObjects) | access->offset;
}

namespace Typed {

JSValue compiledObjectExtraProperty(JSCell* cell, int offset)
{
    JSObject* holder = uncheckedDowncast<Object>(cell)->extraProperties();
    return holder ? holder->getDirect(offset) : JSValue();
}

std::optional<std::pair<uint16_t, uint16_t>> extraPropertyLocationForInterpreter(VM& vm, JSCell* cell, const Identifier& ident)
{
    const LayoutData* layout = cell->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return std::nullopt;
    auto* object = uncheckedDowncast<Object>(cell);
    if (!object->hasExtraProperties())
        return std::nullopt;
    object->specializeStructureForExtraProperties(vm);
    unsigned attributes = 0;
    auto offset = Object::offsetOfExtraProperty(vm, cell, ident, attributes);
    if (!offset || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || *offset > UINT16_MAX)
        return std::nullopt;
    return std::pair { static_cast<uint16_t>(layout->offsetOfExtraProperties()), static_cast<uint16_t>(*offset) };
}

bool storeCompiledObjectExtraProperty(JSCell* cell, int offset, JSValue value)
{
    JSObject* holder = uncheckedDowncast<Object>(cell)->extraProperties();
    if (!holder)
        return false;
    holder->putDirectOffset(holder->vm(), offset, value);
    return true;
}

std::optional<std::pair<uint32_t, uint32_t>> fieldStoreOffsetAndKindForInterpreter(VM& vm, JSCell* cell, const Identifier& ident)
{
    using FieldWrite = Slot::FieldWrite;
    static_assert(putByIdOffsetOfOrderWord == offsetOfOrderWord && putByIdOrderWordIsFrozen == orderWordIsFrozen && putByIdAbsentDouble == absentDouble && !cell32IsPointer && getByIdCell32Shift == cell32Shift && FieldWrite::offsetBits == putByIdCompiledFieldKindShift);
    const LayoutData* layout = cell->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return std::nullopt;
    auto* object = uncheckedDowncast<Object>(cell);
    if (uint32_t offsetOfHolder = layout->offsetOfExtraProperties(); object->hasExtraProperties() && !object->state().hasImmutableMethod(*layout, ident.impl())) {
        object->specializeStructureForExtraProperties(vm);
        unsigned attributes = 0;
        auto offset = Object::offsetOfExtraProperty(vm, cell, ident, attributes);
        constexpr unsigned shift = putByIdExtraPropertyOffsetShift - putByIdCompiledFieldKindShift;
        if (offset && !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue)) && static_cast<uint32_t>(*offset) < 1u << (32 - putByIdExtraPropertyOffsetShift))
            return std::pair { offsetOfHolder, static_cast<uint32_t>(*offset) << shift | static_cast<uint32_t>(PutByIdCompiledFieldKind::ExtraProperty) };
    }
    uint64_t fieldWrite = fieldWriteFor(vm, object, ident);
    if (!fieldWrite)
        return std::nullopt;
    uint32_t description = fieldWrite >> 32;
    auto accepts = [&](uint32_t kinds) {
        return (description & kinds) == kinds;
    };
    auto kind = [&]() -> std::optional<PutByIdCompiledFieldKind> {
        switch (description & FieldWrite::representationMask) {
        case FieldWrite::Int32OrNumberBox:
            return accepts(FieldWrite::acceptsInt32) ? std::optional { PutByIdCompiledFieldKind::Int32OrNumberBox } : std::nullopt;
        case FieldWrite::Double:
            if (!accepts(FieldWrite::acceptsInt32 | FieldWrite::acceptsNumber))
                return std::nullopt;
            return description & FieldWrite::isOptional ? PutByIdCompiledFieldKind::OptionalDouble : PutByIdCompiledFieldKind::Double;
        case FieldWrite::Int32:
            return accepts(FieldWrite::acceptsInt32) ? std::optional { PutByIdCompiledFieldKind::Int32 } : std::nullopt;
        case FieldWrite::Boolean:
            return accepts(FieldWrite::acceptsBoolean) && !(description & FieldWrite::isOptional) ? std::optional { PutByIdCompiledFieldKind::Boolean } : std::nullopt;
        case FieldWrite::Boxed:
            return accepts(FieldWrite::acceptsInt32 | FieldWrite::acceptsNumber | FieldWrite::acceptsString | FieldWrite::acceptsSymbol | FieldWrite::acceptsBigInt | FieldWrite::acceptsObject | FieldWrite::acceptsUndefined | FieldWrite::acceptsNull | FieldWrite::acceptsBoolean) ? std::optional { PutByIdCompiledFieldKind::JSValue } : std::nullopt;
        case FieldWrite::Cell32:
            return accepts(FieldWrite::acceptsString) && !(description & FieldWrite::storesLiteralStringWords) ? std::optional { PutByIdCompiledFieldKind::StringInCell32 } : std::nullopt;
        default:
            return std::nullopt;
        }
    }();
    if (!kind)
        return std::nullopt;
    return std::pair { static_cast<uint32_t>(fieldWrite & ((1u << FieldWrite::offsetBits) - 1)), static_cast<uint32_t>(*kind) };
}

} // namespace Typed

static void cacheStoreToCompiledField(VM& vm, Data* data, Slot* cache, JSCell* cell, const Identifier& ident)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint64_t fieldWrite = fieldWriteFor(vm, object, ident, false, true);
    if (!fieldWrite) {
        Typed::State& state = object->state();
        if (!ident.isPrivateName() && !parseIndex(ident) && !object->fieldNamed(state, ident) && !state.hasImmutableMethod(object->layout(), ident.impl()) && mayReplace(cache, cell->structure())) {
            if (cache->hasPointer())
                stopWatching(data, cache);
            fill(vm, data, cache, cell->structure(), Slot::pointerIsNotCell, std::bit_cast<void*>(static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNotField) << 32));
            return;
        }
        countFailure(cache);
        return;
    }
    if (!mayReplace(cache, cell->structure()))
        return;
    if (cache->hasPointer())
        stopWatching(data, cache);
    fill(vm, data, cache, cell->structure(), Slot::pointerIsNotCell, std::bit_cast<void*>(fieldWrite));
}

void cacheNamedFieldWrite(Instance* instance, JSCell* cell, const Identifier& ident, uint32_t name)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (uint64_t fieldWrite = fieldWriteFor(*instance->vm, object, ident))
        Instance::cacheNamedField(instance->namedFieldWrites, Typed::shapeWordOf(object->shape()), name, fieldWrite);
}

void cacheKeyedCompiledFieldStore(Instance* instance, JSCell* cell, JSCell* key, const Identifier& ident)
{
    auto* entry = instance->allocateKeyedAddition(cell->structureID(), key);
    if (!entry || (entry->slot.structureID == cell->structureID() && entry->slot.pointer == key))
        return;
    uint64_t fieldWrite = fieldWriteFor(*instance->vm, uncheckedDowncast<Typed::Object>(cell), ident);
    if (!fieldWrite)
        return;
    entry->slot.structureID = StructureID();
    entry->slot.offset = Instance::keyIsCompiledField;
    entry->slot.pointer = key;
    entry->newStructureID = std::bit_cast<StructureID>(static_cast<uint32_t>(fieldWrite));
    entry->epoch = static_cast<uint32_t>(fieldWrite >> 32);
    entry->newStorageCapacity = 0;
    entry->holderValueCount = 0;
    entry->slot.structureID = cell->structureID();
}

void cacheStoreTransition(Instance* instance, Data* data, JSCell* cell, Structure* oldStructure, uint64_t wordsBefore, const Identifier& ident, Slot* cache)
{
    VM& vm = *instance->vm;
    if (SharedData::contains(cache))
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint64_t fieldWrite = fieldWriteFor(vm, object, ident, true);
    const Typed::StoreTransition* transition = fieldWrite ? object->state().storeTransitionFor(oldStructure, wordsBefore, object, ident.impl(), fieldWrite) : nullptr;
    if (!transition) {
        countFailure(cache);
        return;
    }
    if (!mayReplace(cache, oldStructure))
        return;
    if (cache->hasPointer())
        stopWatching(data, cache);
    fill(vm, data, cache, oldStructure, Slot::pointerIsNotCell, std::bit_cast<void*>(Slot::isStoreTransition | std::bit_cast<uintptr_t>(transition)));
}

bool cacheFieldCreation(Instance* instance, Data* data, JSCell* cell, uint64_t wordsBefore, const Identifier& ident, Slot* cache, bool checksPrototypeChain)
{
    VM& vm = *instance->vm;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Typed::State& state = object->state();
    const Typed::FieldData* field = object->fieldNamed(state, ident);
    bool isOutOfLine = field && field->flags & Typed::fieldIsOutOfLine;
    uint64_t fieldWrite = field && !isOutOfLine ? fieldWriteFor(vm, object, ident) : 0;
    const Typed::StoreTransition* transition = fieldWrite || isOutOfLine ? state.fieldCreatingStoreTransition(*field, wordsBefore, object, fieldWrite, checksPrototypeChain) : nullptr;
    bool wordsAreViaHandle = object->extraProperties() && !object->layout().offsetOfExtraProperties();
    if (!transition || isOutOfLine || wordsAreViaHandle || !cache || SharedData::contains(cache) || !mayReplace(cache, cell->structure()))
        return false;
    if (cache->hasPointer())
        stopWatching(data, cache);
    fill(vm, data, cache, cell->structure(), Slot::pointerIsNotCell, std::bit_cast<void*>(Slot::isStoreTransition | std::bit_cast<uintptr_t>(transition)));
    return true;
}

bool cacheStorageCreation(Instance* instance, Data* data, JSCell* cell, const Identifier& ident, Slot* cache)
{
    VM& vm = *instance->vm;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Typed::State& state = object->state();
    const Typed::FieldData* field = object->fieldNamed(state, ident);
    uint64_t fieldWrite = field ? fieldWriteFor(vm, object, ident, false, true) : 0;
    const Typed::StoreTransition* transition = fieldWrite ? state.storageCreatingStoreTransition(*field, object, fieldWrite) : nullptr;
    if (!transition || !cache || SharedData::contains(cache) || !mayReplace(cache, cell->structure()))
        return false;
    if (cache->hasPointer())
        stopWatching(data, cache);
    fill(vm, data, cache, cell->structure(), Slot::pointerIsNotCell, std::bit_cast<void*>(Slot::isStoreTransition | std::bit_cast<uintptr_t>(transition)));
    return true;
}

void recordFieldCreation(Instance* instance, JSCell* cell, uint64_t wordsBefore, const Identifier& ident)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Typed::State& state = object->state();
    const Typed::FieldData* field = object->fieldNamed(state, ident);
    bool isOutOfLine = field && field->flags & Typed::fieldIsOutOfLine;
    uint64_t fieldWrite = field && !isOutOfLine ? fieldWriteFor(*instance->vm, object, ident) : 0;
    if (fieldWrite || isOutOfLine)
        state.fieldCreatingStoreTransition(*field, wordsBefore, object, fieldWrite, true);
}

void cacheKnownStoreTransition(Instance* instance, JSCell* cell, Structure* oldStructure, uint64_t wordsBefore, const Identifier& ident, uint32_t name)
{
    using Known = Typed::KnownStoreTransition;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint64_t fieldWrite = fieldWriteFor(*instance->vm, object, ident, true);
    const Typed::StoreTransition* transition = fieldWrite ? object->state().storeTransitionFor(oldStructure, wordsBefore, object, ident.impl(), fieldWrite) : nullptr;
    if (!transition)
        return;
    uint32_t structureID = oldStructure->id().bits();
    static_cast<Known*>(instance->typedKnownStoreTransitions)[Known::placeOf(structureID, name, static_cast<uint32_t>(wordsBefore))] = { structureID, name, transition };
}

void cachePutById(Instance* instance, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    JSGlobalObject* globalObject = instance->globalObject;
    if (base.isObject() && oldStructure->compiledLayout()) {
        if (data && !isDirect && base.asCell()->structure() == oldStructure)
            cacheStoreToCompiledField(globalObject->vm(), data, cache, base.asCell(), ident);
        return;
    }
    if (slot.type() == PutPropertySlot::NewProperty && slot.isCacheablePut() && base.isCell() && slot.base() == base.asCell() && !isInlineOffset(slot.cachedOffset()))
        instance->didAddOutOfLineProperty(base.asCell()->structure());
    if (!data)
        return;
    if (!tryCachePutById(globalObject, data, base, oldStructure, ident, slot, isDirect, cache))
        countFailure(cache);
}

bool isHostObjectOverridingPut(Structure* structure)
{
    return structure->typeInfo().overridesPut() && structure->typeInfo().type() == ObjectType;
}

static bool tryCachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (base.isCell() && base.asCell()->type() == GlobalProxyType) {
        VM& vm = globalObject->vm();
        if (isDirect || !slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || base.asCell()->structure() != oldStructure)
            return false;
        auto location = watchGlobalProxyTargetProperty(vm, data, cache, base.asCell(), slot.base(), ident.impl(), slot.cachedOffset(), static_cast<unsigned>(PropertyAttribute::ReadOnly));
        if (!location)
            return false;
        slot.base()->structure()->didCachePropertyReplacement(vm, slot.cachedOffset());
        using FieldWrite = Slot::FieldWrite;
        fill(vm, data, cache, oldStructure, *location | Slot::pointerIsNotCell, std::bit_cast<void*>(static_cast<uint64_t>(FieldWrite::isFieldWrite | FieldWrite::isNotField | FieldWrite::isInGlobalProxyTarget) << 32));
        return true;
    }
    if (!base.isCell() || (!slot.isCacheablePut() && !slot.isTypedFieldCacheablePut()) || slot.base() != base.asCell())
        return false;
    if (!oldStructure->propertyAccessesAreCacheable() || (oldStructure->isDictionary() && slot.type() != PutPropertySlot::ExistingProperty && !(slot.type() == PutPropertySlot::ExistingTypedField && base.asCell()->inherits<Typed::Closure>())) || (oldStructure->mayBePrototype() && slot.type() != PutPropertySlot::ExistingProperty && !(slot.type() == PutPropertySlot::ExistingTypedField && base.asCell()->inherits<Typed::Closure>())) || (!isDirect && isHostObjectOverridingPut(oldStructure)))
        return abandon(cache);

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* newStructure = cell->structure();
    auto location = propertyLocation(slot.cachedOffset());
    if (!newStructure->propertyAccessesAreCacheable() || !location)
        return false;

    if (!mayReplace(cache, oldStructure))
        return false;

    auto fillPut = [&](Structure* structureAfterwards) {
        uint32_t attempts = cache->offset & Slot::attemptsMask;
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        uint32_t fieldType = slot.type() == PutPropertySlot::ExistingTypedField ? slot.fieldType() : 0;
        cache->offset = *location | attempts | (structureAfterwards ? Slot::isIndirect : 0) | (fieldType ? Slot::hasFieldType : 0);
        cache->pointer = nullptr;
        cache->newStructureID = structureAfterwards ? structureAfterwards->id() : StructureID();
        cache->fieldType = fieldType;
        WTF::storeStoreFence();
        cache->structureID = oldStructure->id();
        if (structureAfterwards)
            data->instance->didCacheTransition(cache);
        didFillSlot(vm, data);
    };

    if (slot.type() == PutPropertySlot::ExistingProperty || slot.type() == PutPropertySlot::ExistingTypedField) {
        if (newStructure != oldStructure)
            return false;
        oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
        stopWatching(data, cache);
        fillPut(nullptr);
        return true;
    }

    if (storageAfterAddition(oldStructure, newStructure, slot.cachedOffset()) != StorageAfterAddition::Stays)
        return abandon(cache);

    if (isDirect)
        stopWatching(data, cache);
    else {
        makePrototypeChainWatchable(vm, cell);
        UniquedStringImpl* uid = ident.impl();
        if (!watchStores(globalObject, data, cache, asObject(cell), newStructure, std::span { &uid, 1 }))
            return false;
    }
    fillPut(newStructure);
    return true;
}

void cacheAssignedThisStructure(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* empty, Structure* whole, std::span<UniquedStringImpl* const> names)
{
    VM& vm = globalObject->vm();
    if (SharedData::contains(cache) || !mayReplace(cache, empty))
        return;
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    cache->clear();
    stopWatching(data, cache);
    if (!watchStores(globalObject, data, cache, object, whole, names))
        return countFailure(cache);
    cache->offset = attempts | Slot::isIndirect;
    cache->newStructureID = whole->id();
    cache->fieldType = 0;
    WTF::storeStoreFence();
    cache->structureID = empty->id();
    data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
}

static void rollBackToStoredProperties(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    Structure* whole = object->structure();
    Vector<UniquedStringImpl*, 16> names;
    names.fill(nullptr, whole->inlineSize());
    whole->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        names[entry.offset()] = entry.key();
        return true;
    });
    DeferGC deferGC(vm);
    Structure* structure = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, asObject(whole->storedPrototype()), whole->inlineCapacity());
    for (unsigned i = 0; i < names.size() && object->getDirect(i); ++i) {
        PropertyOffset offset;
        structure = Structure::addPropertyTransition(vm, structure, names[i], 0, offset);
        RELEASE_ASSERT(offset == static_cast<PropertyOffset>(i));
    }
    object->setStructure(vm, structure);
}

bool tryStoreIntoThisStructure(JSGlobalObject* globalObject, Data* data, Slot* cache, JSValue base, const Identifier& ident, JSValue value)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return false;
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (structure->isDictionary() || structure->outOfLineSize() || !structure->storedPrototype().isObject())
        return false;
    UniquedStringImpl* uid = ident.impl();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset) || attributes || object->getDirect(offset))
        return false;
    bool chainAffectsStore = false;
    for (JSValue next = structure->storedPrototype(); next.isObject() && !chainAffectsStore;) {
        JSObject* prototype = asObject(next);
        Typed::ClassPrototype::look(prototype);
        Structure* prototypeStructure = prototype->structure();
        TypeInfo info = prototypeStructure->typeInfo();
        if (prototypeStructure->hasPolyProto() || (info.overridesPut() && !prototype->inherits<Typed::PrototypeObject>()) || info.overridesGetPrototype() || info.overridesGetOwnPropertySlot() || prototype->hasNonReifiedStaticProperties()) {
            chainAffectsStore = true;
            break;
        }
        unsigned existingAttributes = 0;
        if (isValidOffset(prototypeStructure->get(vm, uid, existingAttributes))) {
            chainAffectsStore = existingAttributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue);
            break;
        }
        next = prototypeStructure->storedPrototype();
    }
    if (chainAffectsStore) {
        rollBackToStoredProperties(globalObject, object);
        return false;
    }
    object->putDirectOffset(vm, offset, value);
    auto location = propertyLocation(offset);
    if (!data || !cache || !location || !structure->propertyAccessesAreCacheable() || !mayReplace(cache, structure))
        return true;
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    cache->clear();
    stopWatching(data, cache);
    makePrototypeChainWatchable(vm, object);
    if (!watchStores(globalObject, data, cache, object, structure, std::span { &uid, 1 })) {
        countFailure(cache);
        return true;
    }
    structure->didCachePropertyReplacement(vm, offset);
    cache->offset = *location | attempts;
    cache->pointer = nullptr;
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    didFillSlot(vm, data);
    return true;
}

void cacheDefinedProperty(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, UniquedStringImpl* uid, std::span<UniquedStringImpl* const> absent)
{
    VM& vm = globalObject->vm();
    if (SharedData::contains(cache))
        return;
    Structure* structure = object->structure();
    bool isSingleAddTransition = structure->previousID() == before && structure->transitionKind() == TransitionKind::PropertyAddition && structure->transitionPropertyName() == uid;
    unsigned attributes = isSingleAddTransition ? structure->transitionPropertyAttributes() : 0;
    PropertyOffset offset = isSingleAddTransition ? structure->transitionOffset() : structure->get(vm, uid, attributes);
    auto location = isValidOffset(offset) ? propertyLocation(offset) : std::nullopt;
    bool hasOrdinaryDefineOwnProperty = object->type() == FinalObjectType || isExactlyJSArray(object) || object->type() == JSPromiseType || object->type() == JSMapType || object->type() == JSSetType;
    bool isOrdinary = hasOrdinaryDefineOwnProperty && !before->compiledLayout() && before->propertyAccessesAreCacheable() && !before->isDictionary() && !before->mayBePrototype()
        && structure->propertyAccessesAreCacheable() && !structure->isDictionary();
    if (!isOrdinary || !location || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return countFailure(cache);
    bool isReplaced = structure == before && !(attributes & PropertyAttribute::ReadOnly);
    bool isAdded = structure != before || isReplaced;
    if (isReplaced)
        before->didCachePropertyReplacement(vm, offset);
    else if (isAdded) {
        if (!isSingleAddTransition || offset != structure->maxOffset() || storageAfterAddition(before, structure, offset) != StorageAfterAddition::Stays)
            return countFailure(cache);
    } else if (!(attributes & PropertyAttribute::DontDelete))
        return countFailure(cache);
    if (!mayReplace(cache, before))
        return;
    ConditionVector conditions;
    for (UniquedStringImpl* name : absent)
        conditions.append(ObjectPropertyCondition::absenceWithoutBarrier(globalObject->objectPrototype(), name, nullptr));
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    if (!watchConditions(vm, data, cache, conditions.span()))
        return countFailure(cache);
    cache->offset = *location | attempts | (isAdded ? Slot::isIndirect : 0);
    cache->pointer = nullptr;
    if (isAdded) {
        cache->newStructureID = structure->id();
    }
    WTF::storeStoreFence();
    cache->structureID = before->id();
    if (isAdded)
        data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
}

static bool canUseMegamorphicCacheForGet(VM& vm, UniquedStringImpl* uid)
{
    return !parseIndex(*uid) && uid != vm.propertyNames->underscoreProto;
}

static bool canUseMegamorphicCacheForPut(VM& vm, UniquedStringImpl* uid)
{
    return canUseMegamorphicPutById(vm, uid);
}

bool storeMayBeCachedByStructureAndName(VM& vm, Structure* structure, UniquedStringImpl* uid)
{
    return canUseMegamorphicCacheForPut(vm, uid) && structure->propertyAccessesAreCacheable();
}

bool storesAreOrdinaryAlongPrototypeChain(Structure* structure)
{
    return canUseMegamorphicPutFastPath(structure);
}

static ALWAYS_INLINE void countFill(VM& vm, const char* kind, MegamorphicCache& cache, StructureID structureID, UniquedStringImpl* uid, const char* outcome = nullptr, StructureID newStructureID = { })
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countMegamorphicCacheFill(kind, cache.epoch(), static_cast<uint64_t>(newStructureID.bits()) << 32 | structureID.bits(), uid, outcome);
}

static bool hasNonExtensibleObjectPrototype(JSGlobalObject* globalObject, Structure* structure)
{
    JSObject* prototype = globalObject->objectPrototype();
    Structure* ofPrototype = prototype->structure();
    auto canCacheAbsence = [](Structure* structure) {
        return !structure->isDictionary() && structure->hasMonoProto() && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence() && !structure->typeInfo().overridesGetOwnPropertySlot();
    };
    return structure->storedPrototype() == JSValue(prototype) && !ofPrototype->isStructureExtensible() && canCacheAbsence(structure) && canCacheAbsence(ofPrototype) && !ofPrototype->storedPrototype().isObject();
}

bool tryGetFromOrdinaryObject(JSGlobalObject* globalObject, JSValue base, UniquedStringImpl* uid, PropertySlot& slot, JSValue& result, bool fillsMegamorphicCache, bool* structureGuaranteesAbsent)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !canUseMegamorphicCacheForGet(vm, uid))
        return false;
    JSObject* object = asObject(base.asCell());
    Structure* structure = object->structure();
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot())
        return false;
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (isValidOffset(offset)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || offset > MegamorphicCache::maxOffset)
            return false;
        result = object->getDirect(offset);
        slot.setValue(object, attributes, result, offset);
        if (!fillsMegamorphicCache)
            return true;
        MegamorphicCache& cache = vm.ensureMegamorphicCache();
        bool isPresent = !!cache.findLoad(structure->id(), uid);
        countFill(vm, "MegamorphicCache::load", cache, structure->id(), uid, isPresent ? "present" : nullptr);
        if (!isPresent)
            cache.initAsHit(structure->id(), uid, object, offset, true);
        return true;
    }
    if (structureGuaranteesAbsent)
        *structureGuaranteesAbsent = structure->propertyAccessesAreCacheableForAbsence() && structure->hasMonoProto();
    if (!hasNonExtensibleObjectPrototype(globalObject, structure) || isValidOffset(globalObject->objectPrototype()->structure()->get(vm, uid)))
        return false;
    result = jsUndefined();
    if (!fillsMegamorphicCache)
        return true;
    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    bool isPresent = !!cache.findLoad(structure->id(), uid);
    countFill(vm, "MegamorphicCache::absent", cache, structure->id(), uid, isPresent ? "present" : nullptr);
    if (!isPresent)
        cache.initAsPermanentMiss(structure->id(), uid);
    return true;
}

JSValue getByIdAndFillMegamorphicCache(JSGlobalObject* globalObject, JSValue base, const Identifier& ident, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    UniquedStringImpl* uid = ident.impl();
    if (!base.isObject() || !canUseMegamorphicCacheForGet(vm, uid))
        RELEASE_AND_RETURN(scope, base.get(globalObject, ident, slot));

    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    JSObject* baseObject = asObject(base);
    Typed::Object::specializeStructureForExtraPropertiesIfNeeded(vm, baseObject);
    JSObject* object = baseObject;
    bool cacheable = true;
    bool isPermanent = true;
    if (Structure* structure = baseObject->structure(); Typed::Object::structureGuaranteesAbsence(structure, uid)) {
        cacheable = structure->hasMonoProto();
        JSValue prototype = baseObject->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !cache.findLoad(structure->id(), uid))
                cache.initAsMiss(structure->id(), uid);
            return jsUndefined();
        }
        object = asObject(prototype);
    }
    bool isLazyFunctionPropertyName = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype;
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && (isLazyFunctionPropertyName || (!isExactlyJSArray(object) && ((object->type() != JSFunctionType && !isSlimClosureType(object->type()) && object->type() != ErrorInstanceType) || (object->structure()->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() && !Typed::hasNoLazyPropertyNamed(vm, object, uid, object == baseObject))) && object->type() != DerivedStringObjectType && object != globalObject->arrayPrototype()))) [[unlikely]] {
            bool hasProperty = object->getNonIndexPropertySlot(globalObject, uid, slot);
            RETURN_IF_EXCEPTION(scope, { });
            if (hasProperty)
                RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
            return jsUndefined();
        }

        Structure* structure = object->structure();
        auto flattenIfDictionary = [&] {
            if (structure->isDictionary() && !structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore()) [[unlikely]]
                structure->flattenDictionaryStructure(vm, object);
        };
        if (object != baseObject)
            flattenIfDictionary();
        structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        if (!hasProperty && object == baseObject)
            flattenIfDictionary();
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (object != baseObject)
            isPermanent &= structure->hasImmutableProperties() && !structure->isDictionary();
        if (hasProperty) {
            if (cacheable && slot.cachedOffset() <= MegamorphicCache::maxOffset && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary())) {
                if (slot.isCacheableValue()) {
                    bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                    countFill(vm, "MegamorphicCache::load", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                    if (!isPresent && isPermanent && slot.slotBase() != baseObject && !baseObject->structure()->isDictionary())
                        cache.initAsPermanentHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset());
                    else if (!isPresent)
                        cache.initAsHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                } else if (slot.isCacheableGetter())
                    cache.initAsGetterHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
            }
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
        }

        cacheable &= structure->propertyAccessesAreCacheableForAbsence() || Typed::hasNoLazyPropertyNamed(vm, object, uid, object == baseObject);
        cacheable &= structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !baseObject->structure()->isDictionary()) {
                bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                countFill(vm, "MegamorphicCache::absent", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                if (!isPresent && isPermanent)
                    cache.initAsPermanentMiss(baseObject->structureID(), uid);
                else if (!isPresent)
                    cache.initAsMiss(baseObject->structureID(), uid);
            }
            return jsUndefined();
        }
        object = asObject(prototype);
    }
}

struct StringComparison {
    StringView name;
    unsigned remainingPieces;
    bool isEqual { true };
};

template<typename Character>
static void compareStringPiece(jsstring_iterator* iterator, const void* characters, uint32_t length, uint32_t offset)
{
    auto& comparison = *static_cast<StringComparison*>(iterator->data);
    auto isEqualToName = [&] {
        std::span piece { static_cast<const Character*>(characters), length };
        if constexpr (std::is_same_v<Character, Latin1Character>) {
            if (comparison.name.is8Bit() && offset <= comparison.name.length() && length <= comparison.name.length() - offset)
                return equalSpans(comparison.name.span8().subspan(offset, length), piece);
        }
        return comparison.name.substring(offset, length) == StringView(piece);
    };
    if (comparison.remainingPieces && isEqualToName()) {
        comparison.remainingPieces--;
        return;
    }
    comparison.isEqual = false;
    iterator->stop = 1;
}

template<typename Character>
static void compareWholeString(jsstring_iterator* iterator, const void* characters, uint32_t length)
{
    compareStringPiece<Character>(iterator, characters, length, 0);
}

static bool stringEqualsName(JSString* string, StringView name, bool& hasTooManyPieces)
{
    constexpr unsigned minimumAveragePieceLength = 512;
    StringComparison comparison { name, name.length() / minimumAveragePieceLength + 16 };
    jsstring_iterator iterator { };
    iterator.data = &comparison;
    iterator.append8 = compareWholeString<Latin1Character>;
    iterator.append16 = compareWholeString<char16_t>;
    iterator.write8 = compareStringPiece<Latin1Character>;
    iterator.write16 = compareStringPiece<char16_t>;
    string->value(&iterator);
    hasTooManyPieces |= !comparison.remainingPieces;
    return comparison.isEqual;
}

std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes)
{
    if (!key.isString() || asString(key)->length() <= JSString::maxLengthForOnStackResolve || !base.isObject())
        return std::nullopt;
    JSString* string = asString(key);
    if (StringImpl* text = string->tryGetValueImpl(); text && text->isAtom())
        return std::nullopt;
    unsigned length = string->length();
    for (JSObject* holder = asObject(base);;) {
        Structure* structure = holder->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags()) || structure->compiledLayout())
            return std::nullopt;
        unsigned numberOfNames = structure->isDictionary() ? structure->dictionaryPropertyCount() : numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
        if (numberOfNames > length / 4)
            return std::nullopt;
        UniquedStringImpl* equalName = nullptr;
        bool hasTooManyPieces = false;
        structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
            UniquedStringImpl* name = entry.key();
            if (!equalName && !hasTooManyPieces && name->length() == length && !name->isSymbol() && stringEqualsName(string, name, hasTooManyPieces))
                equalName = name;
            return true;
        });
        if (hasTooManyPieces)
            return std::nullopt;
        if (equalName)
            return equalName;
        JSValue prototype = holder->getPrototypeDirect();
        if (!checksPrototypes || !prototype.isObject())
            return nullptr;
        holder = asObject(prototype);
    }
}

bool flattenOnce(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (!structure->isDictionary())
        return true;
    if (structure->hasBeenFlattenedBefore() || structure->typeInfo().prohibitsPropertyCaching() || structure->compiledLayout())
        return false;
    structure->flattenDictionaryStructure(vm, object);
    return true;
}

bool hasPropertyAndFillMegamorphicCache(JSGlobalObject* globalObject, JSObject* baseObject, UniquedStringImpl* uid, bool& dependsOnlyOnStructure, bool* isPermanent)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    dependsOnlyOnStructure = false;
    bool allVisitedAreImmutable = true;
    if (isPermanent)
        *isPermanent = false;
    if (Typed::Object::specializeStructureForExtraPropertiesIfNeeded(vm, baseObject)) {
        bool dependsOnUnseenStructure;
        RELEASE_AND_RETURN(scope, hasPropertyAndFillMegamorphicCache(globalObject, baseObject, uid, dependsOnUnseenStructure, nullptr));
    }
    const Typed::FieldData* field = nullptr;
    if (baseObject->structure()->compiledLayout()) {
        auto* typedObject = uncheckedDowncast<Typed::Object>(baseObject);
        Typed::State& state = typedObject->state();
        field = typedObject->fieldNamed(state, uid);
        if (field && !state.mayBeAbsent(*field)) {
            dependsOnlyOnStructure = true;
            if (isPermanent)
                *isPermanent = true;
            return true;
        }
        if (field && typedObject->read(*field))
            return true;
    }
    if (parseIndex(*uid) || !vm.megamorphicCache() || uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto)
        RELEASE_AND_RETURN(scope, baseObject->hasProperty(globalObject, uid));

    StructureID structureID = baseObject->structureID();
    auto didFind = [&](bool result) {
        if (result)
            vm.megamorphicCache()->initAsHasHit(structureID, uid);
        else
            vm.megamorphicCache()->initAsHasMiss(structureID, uid);
        dependsOnlyOnStructure = !structureID.decode()->isDictionary();
        if (isPermanent)
            *isPermanent = dependsOnlyOnStructure && allVisitedAreImmutable;
        return result;
    };
    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    JSObject* object = baseObject;
    bool cacheable = true;
    if (Structure* structure = baseObject->structure(); structure->compiledLayout()) {
        if (!field && Typed::Object::structureGuaranteesAbsence(structure, uid)) {
            cacheable = structure->hasMonoProto();
            JSValue prototype = baseObject->getPrototypeDirect();
            if (!prototype.isObject())
                return cacheable ? didFind(false) : false;
            object = asObject(prototype);
        }
    }
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && !isExactlyJSArray(object) && (!isTypedArrayType(object->type()) || isCanonicalNumericIndexString(uid)) && ((object->type() != JSFunctionType && !isSlimClosureType(object->type()) && object->type() != ErrorInstanceType) || (object->structure()->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() && !Typed::hasNoLazyPropertyNamed(vm, object, uid, object == baseObject))) && object != globalObject->arrayPrototype()) [[unlikely]]
            RELEASE_AND_RETURN(scope, object->getNonIndexPropertySlot(globalObject, uid, slot));
        Structure* structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (object != baseObject)
            allVisitedAreImmutable &= structure->hasImmutableProperties() && !structure->isDictionary() && !structure->isStructureExtensible();
        if (hasProperty) {
            if (cacheable && slot.isCacheable() && baseObject->structureID() == structureID && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary()))
                return didFind(true);
            return true;
        }
        cacheable &= (structure->propertyAccessesAreCacheableForAbsence() || Typed::hasNoLazyPropertyNamed(vm, object, uid, object == baseObject)) && structure->hasMonoProto();
        allVisitedAreImmutable &= structure->propertyAccessesAreCacheableForAbsence();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && baseObject->structureID() == structureID && !baseObject->structure()->isDictionary())
                return didFind(false);
            return false;
        }
        object = asObject(prototype);
    }
}

void fillMegamorphicCacheAfterPut(JSGlobalObject* globalObject, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    UniquedStringImpl* uid = ident.impl();
    if (!base.isObject() || !slot.isCacheablePut() || slot.base() != base.asCell() || slot.cachedOffset() > MegamorphicCache::maxOffset)
        return;
    if (slot.type() == PutPropertySlot::NewProperty && (oldStructure->isDictionary() || oldStructure->mayBePrototype()))
        return;
    if (!canUseMegamorphicCacheForPut(vm, uid) || !oldStructure->propertyAccessesAreCacheable() || !canUseMegamorphicPutFastPath(oldStructure))
        return;

    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    if (cache.findStore(oldStructure->id(), uid)) {
        countFill(vm, "MegamorphicCache::store", cache, oldStructure->id(), uid, "present");
        return;
    }
    Structure* newStructure = base.asCell()->structure();
    if (slot.type() == PutPropertySlot::ExistingProperty) {
        if (oldStructure == newStructure) {
            oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            countFill(vm, "MegamorphicCache::replace", cache, oldStructure->id(), uid);
            cache.initAsReplace(oldStructure->id(), uid, slot.cachedOffset());
        }
        return;
    }

    if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
        return;
    uint8_t reallocating = 0;
    if (newStructure->outOfLineCapacity() != oldStructure->outOfLineCapacity()) {
        bool allocatesInitialStorage = storageAfterAddition(oldStructure, newStructure, slot.cachedOffset()) == StorageAfterAddition::IsFirst && oldStructure->typeInfo().type() == FinalObjectType;
        reallocating = allocatesInitialStorage ? MegamorphicCache::StoreEntry::allocatesInitialOutOfLineStorage : MegamorphicCache::StoreEntry::reallocates;
    }
    cache.initAsTransition(oldStructure->id(), newStructure->id(), uid, slot.cachedOffset(), reallocating);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        countFill(vm, "MegamorphicCache::transition", cache, oldStructure->id(), uid, cache.findStore(oldStructure->id(), uid) ? nullptr : "refused", newStructure->id());
}

bool cachePropertyRunWithStorage(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, std::span<UniquedStringImpl* const> names, bool isAllocated)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    unsigned capacity = structure->outOfLineCapacity();
    unsigned first = before->outOfLineSize();
    if (cache->structureID == before->id() && cache->newStructureID == structure->id())
        return true;
    RELEASE_ASSERT(!isAllocated || hasOnlyNamedOutOfLineStorage(before, structure));
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask || capacity > PropertyRunStorage::capacityMask || capacity * sizeof(EncodedJSValue) > MarkedSpace::largeCutoff || first > PropertyRunStorage::maxFirst)
        return false;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    if (!watchStores(globalObject, data, cache, object, structure, names))
        return false;
    PropertyOffset next = Instance::offsetAfter(before->maxOffset(), before->inlineCapacity());
    uint32_t inlinePropertyCount = isInlineOffset(next) ? std::min<uint32_t>(names.size(), before->inlineCapacity() - next) : 0;
    cache->offset |= (inlinePropertyCount ? *propertyLocation(next) : 0) | inlinePropertyCount << Slot::nameIDShift | Slot::isIndirect;
    cache->newStructureID = structure->id();
    cache->fieldType = capacity | (isAllocated ? PropertyRunStorage::isAllocated : 0) | first << PropertyRunStorage::firstShift;
    WTF::storeStoreFence();
    cache->structureID = before->id();
    data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
    return true;
}

bool cacheReplacementPropertyRun(VM& vm, Data* data, Slot* cache, Structure* structure, std::span<const PropertyOffset> offsets)
{
    if (cache->structureID == structure->id() && cache->offset & Slot::pointerIsNotCell)
        return true;
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return false;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    size_t numberOfSlots = (offsets.size() + sizeof(void*) - 1) / sizeof(void*);
    for (size_t i = 1; i < numberOfSlots; ++i)
        cache[i].clearAndResetAttempts();
    WTF::storeStoreFence();
    for (size_t i = 0; i < numberOfSlots; ++i) {
        uint64_t places = 0;
        for (size_t k = i * sizeof(void*); k < std::min(offsets.size(), (i + 1) * sizeof(void*)); ++k) {
            structure->didCachePropertyReplacement(vm, offsets[k]);
            places |= static_cast<uint64_t>(offsets[k] + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue)) << (k % sizeof(void*) * 8);
        }
        cache[i].offset |= Slot::pointerIsNotCell;
        cache[i].pointer = std::bit_cast<void*>(places);
    }
    WTF::storeStoreFence();
    for (size_t i = numberOfSlots; i--;)
        cache[i].structureID = structure->id();
    didFillSlot(vm, data);
    return true;
}

bool cachePropertyRunByOffsets(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, std::span<const PropertyOffset> offsets, std::span<UniquedStringImpl* const> added)
{
    VM& vm = globalObject->vm();
    Structure* after = object->structure();
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return false;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    size_t numberOfSlots = (offsets.size() + sizeof(void*) - 1) / sizeof(void*);
    for (size_t i = 1; i <= numberOfSlots; ++i)
        cache[i].clearAndResetAttempts();
    if (!added.empty() && !watchStores(globalObject, data, cache, object, after, added))
        return false;
    WTF::storeStoreFence();
    for (size_t i = 0; i < numberOfSlots; ++i) {
        uint64_t places = 0;
        for (size_t k = i * sizeof(void*); k < std::min(offsets.size(), (i + 1) * sizeof(void*)); ++k) {
            if (offsets[k] <= before->maxOffset())
                before->didCachePropertyReplacement(vm, offsets[k]);
            places |= static_cast<uint64_t>(offsets[k] + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue)) << (k % sizeof(void*) * 8);
        }
        cache[1 + i].offset |= Slot::pointerIsNotCell;
        cache[1 + i].pointer = std::bit_cast<void*>(places);
    }
    cache->offset |= Slot::isIndirect;
    cache->newStructureID = after->id();
    WTF::storeStoreFence();
    for (size_t i = numberOfSlots; i; --i)
        cache[i].structureID = before->id();
    cache->structureID = before->id();
    data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
    return true;
}

void cacheContiguousReplacementPropertyRun(VM& vm, Data* data, Slot* cache, Structure* structure, PropertyOffset first, unsigned count, std::span<JSCell* const> keys)
{
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    for (unsigned i = 0; i < count; ++i)
        structure->didCachePropertyReplacement(vm, first + static_cast<PropertyOffset>(i));
    for (size_t i = 0; i < keys.size(); ++i)
        fill(vm, data, &cache[1 + i], structure, Slot::pointerIsCell, keys[i]);
    cache->offset |= *propertyLocation(first) | Slot::isIndirect;
    cache->newStructureID = structure->id();
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
}

void cachePropertyRun(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, std::span<UniquedStringImpl* const> names, std::span<JSCell* const> keys)
{
    VM& vm = globalObject->vm();
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    Structure* structure = object->structure();
    if (!watchStores(globalObject, data, cache, object, structure, names))
        return;
    for (size_t i = 0; i < keys.size(); ++i)
        fill(vm, data, &cache[1 + i], before, Slot::pointerIsCell, keys[i]);
    cache->offset |= *propertyLocation(Instance::offsetAfter(before->maxOffset(), before->inlineCapacity())) | Slot::isIndirect;
    cache->newStructureID = structure->id();
    WTF::storeStoreFence();
    cache->structureID = before->id();
    data->instance->didCacheTransition(cache);
    didFillSlot(vm, data);
}

void fillAllocationCache(VM& vm, Data* data, Slot* cache, Structure* structure, Allocator allocator, uint32_t payload, JSCell* extra)
{
    if (!allocator || SharedData::contains(cache))
        return;
    ASSERT(payload <= Slot::offsetMask);
    cache[1].offset = structure->typeInfoBlob();
    cache[1].pointer = allocator.localAllocator();
    fill(vm, data, &cache[0], structure, payload | (extra ? Slot::pointerIsCell : 0), extra);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyLookupTable);

static constexpr size_t sizeOfCells = 2 * static_cast<size_t>(PropertyLookupTable::entriesPerPlane) * sizeof(PropertyLookupTable::Cell);
static constexpr size_t sizeOfSlots = PropertyLookupTable::maxNumberOfSlots * sizeof(Slot);
static constexpr size_t sizeOfOccupied = (PropertyLookupTable::entriesPerPlane / 64 + 4) * sizeof(uint64_t);

static void* reserveForTable(size_t size)
{
    void* result = OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(result, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    return result;
}

static bool isUnused(const Slot& slot)
{
    auto words = std::bit_cast<std::array<uint64_t, 2>>(slot);
    return !words[0] && !words[1];
}

static void setBit(BitVector& bits, size_t index)
{
    if (index >= bits.size())
        bits.ensureSize(std::max<size_t>(index + 1, bits.size() * 2));
    bits.quickSet(index);
}

uint64_t PropertyLookupTable::freeCellsFrom(uint32_t index) const
{
    uint32_t word = index / 64;
    uint32_t shift = index % 64;
    uint64_t occupied = m_occupied[word] >> shift;
    if (shift)
        occupied |= m_occupied[word + 1] << (64 - shift);
    return ~occupied;
}

uint32_t PropertyLookupTable::firstFittingBase(uint32_t from, std::span<const uint16_t> names) const
{
    uint64_t fitting = freeCellsFrom(from);
    bool hasNamesInOverflow = false;
    for (uint16_t listed : names) {
        if (!fitting)
            return 0;
        if (listed & isInOverflow)
            hasNamesInOverflow = true;
        else
            fitting &= freeCellsFrom(indexOf(from, listed));
    }
    for (; fitting; fitting &= fitting - 1) {
        uint32_t base = from + WTF::ctz(fitting);
        if (base >= m_bases.limit)
            return 0;
        bool fitsInOverflow = true;
        if (hasNamesInOverflow) {
            for (uint16_t listed : names)
                fitsInOverflow &= !(listed & isInOverflow) || !entryAt(indexOfRowName(base, listed));
        }
        if (fitsInOverflow)
            return base;
    }
    return 0;
}

uint32_t PropertyLookupTable::findBase(std::span<const uint16_t> names, uint32_t preferredBase)
{
    constexpr unsigned basesPerBatch = 64;
    constexpr unsigned maxProbesBetweenRows = 4;
    auto probe = [&](uint32_t from) {
        if (m_data->instance)
            countOperationNamed(m_data->instance, "PropertyLookupTable::firstFittingBase");
        return firstFittingBase(from, names);
    };
    if (uint32_t base = preferredBase ? probe(preferredBase) : 0) {
        m_bases.end = std::max(m_bases.end, base + 1);
        return base;
    }
    uint32_t from = m_bases.next;
    for (unsigned looks = 0; looks < maxProbesBetweenRows && from < m_bases.end; ++looks, from += basesPerBatch) {
        if (uint32_t base = probe(from)) {
            m_bases.next = base + 1;
            m_bases.end = std::max(m_bases.end, base + 1);
            return base;
        }
    }
    m_bases.next = from < m_bases.end ? from : m_bases.first;
    for (from = m_bases.end; from < m_bases.limit; from += basesPerBatch) {
        if (uint32_t base = probe(from)) {
            m_bases.end = base + 1;
            return base;
        }
    }
    return 0;
}

void PropertyLookupTable::occupy(uint32_t index, uint32_t entry)
{
    ASSERT(entry && !entryAt(index));
    setEntryAt(index, entry);
    if (index < entriesPerPlane)
        m_occupied[index / 64] |= 1ull << (index % 64);
}

uint32_t PropertyLookupTable::vacate(uint32_t index)
{
    if (index < entriesPerPlane)
        m_occupied[index / 64] &= ~(1ull << (index % 64));
    uint32_t entry = entryAt(index);
    m_cells[index] = Cell { };
    return entry;
}

PropertyLookupTable::PropertyLookupTable()
{
    m_cells = static_cast<Cell*>(reserveForTable(sizeOfCells));
    m_occupied = static_cast<uint64_t*>(reserveForTable(sizeOfOccupied));
    m_bases.limit = numberOfBases;
    m_slots = static_cast<Slot*>(reserveForTable(sizeOfSlots));
    m_data = static_cast<Data*>(fastZeroedMalloc(sizeof(Data)));
    m_data->hasBeenFilledSinceLastCollection = true;
    m_data->slotEpoch = 1;
}

PropertyLookupTable::~PropertyLookupTable()
{
    delete m_data->watchpoints;
    fastFree(m_data);
    OSAllocator::decommitAndRelease(m_cells, sizeOfCells);
    OSAllocator::decommitAndRelease(m_occupied, sizeOfOccupied);
    OSAllocator::decommitAndRelease(m_slots, sizeOfSlots);
}

PropertyLookupTable& PropertyLookupTable::of(VM& vm)
{
    if (!vm.m_aotPropertyLookupTable) [[unlikely]] {
        vm.m_aotPropertyLookupTable = new PropertyLookupTable;
        vm.m_aotEmptyLookupTableRow = vm.m_aotPropertyLookupTable->emptyRowEntries();
    }
    return *vm.m_aotPropertyLookupTable;
}

void PropertyLookupTable::destroy(VM& vm)
{
    delete std::exchange(vm.m_aotPropertyLookupTable, nullptr);
    vm.m_aotEmptyLookupTableRow = nullptr;
}

void initializeRowsByName(VM& vm, Structure& structure)
{
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    structure.setLookupTableRow(table.emptyRowEntries());
}

Data* PropertyLookupTable::dataFor(Instance* instance)
{
    if (!m_data->instance)
        m_data->instance = instance;
    return m_data;
}

uint32_t PropertyLookupTable::baseOf(Structure* structure) const
{
    return static_cast<uint32_t>(reinterpret_cast<const Cell*>(structure->lookupTableRow()) - reinterpret_cast<const Cell*>(emptyRowEntries()));
}

uint32_t PropertyLookupTable::entryForOwnOffset(uint32_t name, PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return name | static_cast<uint32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset) << 16;
    return entryWithKind(name, Kind::OutOfLine, 0);
}

bool PropertyLookupTable::usesPayloadDirectly(Kind kind)
{
    return adds(kind) || kind == Kind::LazyClosure || kind == Kind::ExtraProperty || kind == Kind::NamespaceExport || kind == Kind::Accessor || kind == Kind::AccessorWithSetterCall || kind == Kind::InheritedInline || kind == Kind::InheritedOutOfLine;
}

uint32_t PropertyLookupTable::findIndex(Structure* structure, uint32_t name) const
{
    uint32_t base = baseOf(structure);
    if (!base)
        return 0;
    uint32_t index = indexOf(base, name);
    return isEntryFor(entryAt(index), name) ? index : 0;
}

uint32_t PropertyLookupTable::findIndexOfSlots(Structure* structure, uint32_t name) const
{
    uint32_t index = findIndex(structure, name);
    if (!index || !hasOwnOffsetInWord(entryAt(index)))
        return index;
    if (entryAt(baseOf(structure)) != baseMarkerWithOverflowNames)
        return 0;
    index = indexOfRowName(baseOf(structure), static_cast<uint16_t>(name | isInOverflow));
    return isEntryFor(entryAt(index), name) ? index : 0;
}

void PropertyLookupTable::RowNames::append(uint16_t name)
{
    if (m_size == m_capacity) {
        uint16_t newCapacity = static_cast<uint16_t>(std::min<unsigned>(m_capacity * 2, UINT16_MAX));
        RELEASE_ASSERT(newCapacity > m_capacity);
        auto* storage = static_cast<uint16_t*>(fastMalloc(newCapacity * sizeof(uint16_t)));
        memcpy(storage, span().data(), m_size * sizeof(uint16_t));
        if (!isInline())
            fastFree(outOfLineStorage());
        memcpy(m_storage, &storage, sizeof(storage));
        m_capacity = newCapacity;
    }
    (isInline() ? m_storage : outOfLineStorage())[m_size++] = name;
}

void PropertyLookupTable::RowNames::clear()
{
    if (!isInline())
        fastFree(outOfLineStorage());
    m_size = 0;
    m_capacity = inlineCapacity;
}

uint32_t PropertyLookupTable::allocateRowIndexFor(Structure* structure)
{
    uint32_t number;
    if (m_freeRows.isEmpty()) {
        number = m_rows.size();
        m_rows.alloc();
    } else
        number = m_freeRows.takeLast();
    m_rows[number].owner = structure;
    m_structuresWithNewRows.append(structure);
    return number;
}

uint32_t PropertyLookupTable::createRow(Structure* structure, uint32_t name)
{
    uint16_t listed = static_cast<uint16_t>(name);
    constexpr unsigned maxNamesExpected = 32;
    Vector<uint16_t, maxNamesExpected> expected;
    expected.append(listed);
    RecentRow& last = m_recentRows[name % m_recentRows.size()];
    uint32_t preferredBase = 0;
    if (last.name == listed && m_rows[last.row].owner) {
        preferredBase = baseOf(m_rows[last.row].owner) + 1;
        for (uint16_t other : m_rows[last.row].names) {
            if (other != listed && !(other & isInOverflow) && expected.size() < maxNamesExpected)
                expected.append(other);
        }
    }
    uint32_t base = findBase(expected.span(), preferredBase);
    if (!base)
        return 0;
    uint32_t number = allocateRowIndexFor(structure);
    last = { listed, number };
    m_rows[number].names.append(listed);
    occupy(base, baseMarker);
    setPayload(base, number);
    occupy(indexOf(base, name), entryWithKind(name, Kind::Unknown, 0));
    structure->setLookupTableRow(&m_cells[base + 1].readOffsetOrPayload);
    return indexOf(base, name);
}

uint32_t PropertyLookupTable::moveRow(uint32_t base, std::optional<uint16_t> nameToAdd)
{
    if (m_data->instance)
        countOperationNamed(m_data->instance, "PropertyLookupTable::moveRow");
    uint32_t findStructureDescriptor = payload(base);
    Vector<uint16_t, 32> names;
    names.append(m_rows[findStructureDescriptor].names.span());
    unsigned numberOfNamesMoved = names.size();
    if (nameToAdd)
        names.append(*nameToAdd);
    uint32_t newBase = findBase(names.span());
    if (!newBase)
        return 0;
    uint32_t mark = vacate(base);
    occupy(newBase, nameToAdd && *nameToAdd & isInOverflow ? baseMarkerWithOverflowNames : mark);
    setPayload(newBase, findStructureDescriptor);
    for (unsigned i = 0; i < numberOfNamesMoved; ++i) {
        uint32_t from = indexOfRowName(base, names[i]);
        uint32_t to = indexOfRowName(newBase, names[i]);
        uint32_t payloadMoved = payload(from);
        occupy(to, vacate(from));
        if (payloadMoved) {
            setPayload(to, payloadMoved);
            if (!(payloadMoved & payloadIsSlots))
                m_newAdditionIndices.append(to);
        }
        if (m_dependsOnPrototypes.get(bitOf(from))) {
            m_dependsOnPrototypes.quickClear(bitOf(from));
            setBit(m_dependsOnPrototypes, bitOf(to));
        }
    }
    if (nameToAdd) {
        occupy(indexOfRowName(newBase, *nameToAdd), entryWithKind(*nameToAdd & maxName, Kind::Unknown, 0));
        m_rows[findStructureDescriptor].names.append(*nameToAdd);
    }
    m_rows[findStructureDescriptor].owner->setLookupTableRow(&m_cells[newBase + 1].readOffsetOrPayload);
    return newBase;
}

uint32_t PropertyLookupTable::ensureIndex(Structure* structure, uint32_t name)
{
    ASSERT(name && name <= maxName);
    uint32_t base = baseOf(structure);
    if (!base)
        return createRow(structure, name);
    uint16_t listed = static_cast<uint16_t>(name);
    uint32_t index = indexOf(base, name);
    uint32_t entry = entryAt(index);
    if (isEntryFor(entry, name))
        return index;
    if (entry) {
        uint32_t holderBase = isBaseMarker(entry) ? index : index - 1 - (entry & maxName);
        if (rowAt(holderBase).names.size() <= rowAt(base).names.size() && moveRow(holderBase, std::nullopt))
            entry = 0;
    }
    if (!entry) {
        rowAt(base).names.append(listed);
        occupy(index, entryWithKind(name, Kind::Unknown, 0));
        return index;
    }
    uint32_t newBase = moveRow(base, listed);
    return newBase ? indexOfRowName(newBase, listed) : 0;
}

uint32_t PropertyLookupTable::ensureIndexInOverflow(Structure* structure, uint32_t name)
{
    uint32_t base = baseOf(structure);
    ASSERT(base);
    uint16_t listed = static_cast<uint16_t>(name | isInOverflow);
    uint32_t index = indexOfRowName(base, listed);
    uint32_t entry = entryAt(index);
    if (isEntryFor(entry, name))
        return index;
    if (!entry) {
        rowAt(base).names.append(listed);
        occupy(index, entryWithKind(name, Kind::Unknown, 0));
        setEntryAt(base, baseMarkerWithOverflowNames);
        return index;
    }
    uint32_t newBase = moveRow(base, listed);
    return newBase ? indexOfRowName(newBase, listed) : 0;
}

static std::optional<uint32_t> codeForInlineOrOutOfLineOffset(PropertyOffset offset)
{
    if (isInlineOffset(offset)) {
        uint32_t place = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
        return place < PropertyLookupTable::firstCodeOutOfLine ? std::optional(place) : std::nullopt;
    }
    uint32_t code = PropertyLookupTable::firstCodeOutOfLine + (offset - firstOutOfLineOffset);
    return code <= UINT8_MAX ? std::optional(code) : std::nullopt;
}

uint32_t PropertyLookupTable::entryRow(uint32_t index, uint32_t name) const
{
    return payload((index < entriesPerPlane ? index : index - entriesPerPlane) - 1 - name);
}

void PropertyLookupTable::appendToNameList(uint32_t row, uint32_t& head)
{
    uint32_t position = m_firstFreeNameListLink;
    if (position)
        m_firstFreeNameListLink = m_nameListLinks[position - 1].next;
    else {
        m_nameListLinks.append(NameListLink { });
        position = m_nameListLinks.size();
    }
    m_nameListLinks[position - 1] = { row, head };
    head = position;
}

void PropertyLookupTable::freeNameList(uint32_t name)
{
    uint32_t* head = nameListHeadIfExists(name);
    uint32_t first = head ? std::exchange(*head, 0) : 0;
    if (!first)
        return;
    uint32_t last = first;
    while (uint32_t next = m_nameListLinks[last - 1].next)
        last = next;
    m_nameListLinks[last - 1].next = m_firstFreeNameListLink;
    m_firstFreeNameListLink = first;
}

void PropertyLookupTable::clearNameLists()
{
    m_nameListHeadGroup.fill(0);
    m_nameListHeads.clear();
    m_nameListLinks.clear();
    m_firstFreeNameListLink = 0;
}

void PropertyLookupTable::setDependsOnPrototypes(uint32_t index, uint32_t name, UniquedStringImpl* uid)
{
    if (uid) {
        if (m_dependsOnPrototypes.get(bitOf(index)))
            return;
        setBit(m_dependsOnPrototypes, bitOf(index));
        uint32_t& head = ensureNameListHead(name);
        if (!head && !m_registeredNames.get(name)) {
            setBit(m_registeredNames, name);
            m_nameByUID.add(uid, static_cast<uint16_t>(name));
        }
        appendToNameList(entryRow(index, name), head);
    } else if (m_dependsOnPrototypes.get(bitOf(index)))
        m_dependsOnPrototypes.quickClear(bitOf(index));
}

bool PropertyLookupTable::setEntryWithKind(Structure* structure, uint32_t name, Kind kind, uint32_t code, uint32_t newPayload, UniquedStringImpl* uid)
{
    if (code > UINT8_MAX)
        return false;
    uint32_t index = ensureIndex(structure, name);
    if (!index)
        return false;
    bool hasSlots = payload(index) & payloadIsSlots;
    if (hasSlots && usesPayloadDirectly(kind))
        return false;
    setEntryAt(index, entryWithKind(name, kind, code));
    if (!hasSlots)
        setPayload(index, newPayload);
    setDependsOnPrototypes(index, name, uid);
    return true;
}

bool PropertyLookupTable::setOwnOffset(Structure* structure, uint32_t name, PropertyOffset offset, bool isStoredTo)
{
    if (isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset > UINT8_MAX : static_cast<uint32_t>(offset - firstOutOfLineOffset) > maxOwnOutOfLineIndex)
        return false;
    uint32_t index = ensureIndex(structure, name);
    if (!index)
        return false;
    if (uint32_t slots = payload(index); slots & payloadIsSlots) {
        uint32_t inOverflow = ensureIndexInOverflow(structure, name);
        if (!inOverflow || payload(inOverflow))
            return false;
        index = indexOf(baseOf(structure), name);
        setPayload(index, 0);
        setPayload(inOverflow, slots);
    }
    uint32_t before = entryAt(index);
    uint32_t entry = entryForOwnOffset(name, offset);
    if (isInlineOffset(offset)) {
        bool wasStoredTo = !(before & hasKindAndCode) && before >> 24;
        if (isStoredTo || wasStoredTo)
            entry |= codeOf(entry) << 24;
    } else if (isStoredTo || kindOf(before) == Kind::OutOfLineAndWritten)
        entry = entryWithKind(name, Kind::OutOfLineAndWritten, 0);
    setEntryAt(index, entry);
    if (!isInlineEntry(entry))
        setPayload(index, wordForOwnOutOfLineIndex(offset - firstOutOfLineOffset));
    setDependsOnPrototypes(index, name, nullptr);
    return true;
}

bool PropertyLookupTable::setAbsent(Structure* structure, uint32_t name, UniquedStringImpl* uid)
{
    uint32_t index = ensureIndex(structure, name);
    if (!index)
        return false;
    uint32_t before = entryAt(index);
    if (before & hasKindAndCode && adds(kindOf(before)))
        setEntryAt(index, entryWithKind(name, addingKind(true, storeAdds(kindOf(before)), addedStorage(kindOf(before))), codeOf(before)));
    else if (addsExtraProperty(before))
        setPayload(index, payload(index) | extraPropertyIsKnownToBeAbsent);
    else {
        bool hasSlots = payload(index) & payloadIsSlots;
        setEntryAt(index, entryWithKind(name, Kind::Absent, 0));
        if (!hasSlots)
            setPayload(index, 0);
    }
    setDependsOnPrototypes(index, name, uid);
    return true;
}

bool PropertyLookupTable::setInherited(Structure* structure, uint32_t name, JSCell* holder, PropertyOffset offset, UniquedStringImpl* uid)
{
    if (!MarkedBlockCage::contains(holder))
        return false;
    if (isInlineOffset(offset)) {
        uint32_t place = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
        return place <= INT8_MAX && setEntryWithKind(structure, name, Kind::InheritedInline, place, MarkedBlockCage::encode(holder), uid);
    }
    if (offset - firstOutOfLineOffset > maxOutOfLineIndex)
        return setInheritedFarOutOfLine(structure, name, holder, offset - firstOutOfLineOffset, uid);
    return setEntryWithKind(structure, name, Kind::InheritedOutOfLine, codeForOutOfLineIndex(offset - firstOutOfLineOffset), MarkedBlockCage::encode(holder), uid);
}

bool PropertyLookupTable::setInheritedFarOutOfLine(Structure* structure, uint32_t name, JSCell* holder, uint32_t place, UniquedStringImpl* uid)
{
    if (place > maxOwnOutOfLineIndex)
        return false;
    uint32_t index = ensureIndex(structure, name);
    if (!index || payload(index) & payloadIsSlots)
        return false;
    uint32_t inOverflow = ensureIndexInOverflow(structure, name);
    if (!inOverflow || payload(inOverflow) & payloadIsSlots)
        return false;
    setPayload(inOverflow, wordForOwnOutOfLineIndex(place));
    return setEntryWithKind(structure, name, Kind::Unknown, farInheritedCode, MarkedBlockCage::encode(holder), uid);
}

bool PropertyLookupTable::replaceSlotWithInherited(Structure* structure, uint32_t name, JSCell* holder, PropertyOffset offset)
{
    uint32_t index = findIndex(structure, name);
    if (!index || index >= entriesPerPlane || !MarkedBlockCage::contains(holder) || (isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset > INT8_MAX : static_cast<uint32_t>(offset - firstOutOfLineOffset) > maxOwnOutOfLineIndex))
        return false;
    return releaseSlotsUnlessUsedByStores(index) && setInherited(structure, name, holder, offset, nullptr);
}

static std::optional<uint32_t> extraPropertyCode(Structure* structure, PropertyOffset offset)
{
    const Typed::LayoutData* layout = structure->compiledLayout();
    if (!layout || !structure->extraPropertiesStructureID())
        return std::nullopt;
    if (!isInlineOffset(offset)) {
        uint32_t place = PropertyLookupTable::firstOutOfLineExtraPropertyCode + (offset - firstOutOfLineOffset);
        return place < PropertyLookupTable::onlyDefinitionAddsExtraProperty ? std::optional(place) : std::nullopt;
    }
    uint32_t place = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
    return place < PropertyLookupTable::firstOutOfLineExtraPropertyCode ? std::optional(place) : std::nullopt;
}

static bool stubsCanAddExtraProperty(Structure* before, Structure* after, PropertyOffset offset, bool createsExtraProperties)
{
    if (createsExtraProperties && (offset || after->extraPropertiesStructureID().decode()->typeInfoBlob() != JSFinalObject::defaultTypeInfoBlob()))
        return false;
    if (!createsExtraProperties && !before->extraPropertiesStructureID())
        return false;
    if (isInlineOffset(offset))
        return true;
    StorageAfterAddition storage = storageAfterAddition(before->extraPropertiesStructureID().decode(), after->extraPropertiesStructureID().decode(), offset);
    return storage == StorageAfterAddition::Stays || storage == StorageAfterAddition::IsFirst;
}

void cacheKeyedExtraPropertyAddition(Instance* instance, Structure* before, Structure* after, JSCell* key, PropertyOffset offset, bool createsExtraProperties)
{
    static_assert(OBJECT_OFFSETOF(Instance::KeyedAddition, holderValueCount) - OBJECT_OFFSETOF(Instance::KeyedAddition, newStorageCapacity) == PropertyLookupTable::offsetOfCodeInEntry - PropertyLookupTable::offsetOfPayloads);
    auto place = extraPropertyCode(after, offset);
    MegamorphicCache* cache = instance->vm->megamorphicCache();
    if (!place || !cache || !stubsCanAddExtraProperty(before, after, offset, createsExtraProperties))
        return;
    auto* addition = instance->freeKeyedAdditionEntry(before->id(), key);
    if (!addition)
        return;
    addition->slot.structureID = StructureID();
    addition->slot.offset = 0;
    addition->slot.pointer = key;
    addition->newStructureID = after->id();
    addition->epoch = Instance::keyedAdditionIsExtraProperty;
    addition->newStorageCapacity = after->id().bits() | (createsExtraProperties ? PropertyLookupTable::storeCreatesExtraProperties : 0);
    addition->holderValueCount = *place | PropertyLookupTable::storeAddsExtraProperty | static_cast<uint32_t>(cache->epoch()) << Instance::keyedExtraPropertyEpochShift;
    addition->slot.structureID = before->id();
}

void recordKeyedExtraPropertyAddition(Instance::KeyedAddition& addition, Structure* before, JSCell* key)
{
    addition.slot.structureID = StructureID();
    addition.slot.offset = 0;
    addition.slot.pointer = key;
    addition.newStructureID = before->id();
    addition.epoch = Instance::keyedExtraPropertyAdditionSeenOnce;
    addition.newStorageCapacity = 1;
    addition.holderValueCount = 1;
    addition.slot.structureID = before->id();
}

bool PropertyLookupTable::setExtraProperty(Structure* structure, uint32_t name, PropertyOffset offset, bool isAccessor, bool isStoredTo, bool hasDefaultAttributes)
{
    auto place = extraPropertyCode(structure, offset);
    if (isAccessor && !isInlineOffset(offset))
        return false;
    uint32_t index = place ? ensureIndex(structure, name) : 0;
    if (!index || !releaseSlotsUnlessUsedByStores(index))
        return false;
    uint32_t before = entryAt(index);
    uint32_t inlineOffsetOfExtraProperties = structure->compiledLayout()->offsetOfExtraProperties();
    uint32_t code = *place | (inlineOffsetOfExtraProperties ? 0 : extraPropertyUsesHandle);
    if (before & hasKindAndCode && kindOf(before) == Kind::ExtraProperty && codeOf(before) == code) {
        isStoredTo |= !!(payload(index) & extraPropertyIsWritten);
        hasDefaultAttributes |= !!(payload(index) & extraPropertyHasDefaultAttributes);
    }
    uint32_t flags = (isAccessor ? extraPropertyIsAccessor : 0) | (isStoredTo && !isAccessor ? extraPropertyIsWritten : 0) | (hasDefaultAttributes && !isAccessor ? extraPropertyHasDefaultAttributes : 0);
    return setEntryWithKind(structure, name, Kind::ExtraProperty, code, inlineOffsetOfExtraProperties << extraPropertiesOffsetShift | flags, nullptr);
}

bool PropertyLookupTable::setExtraPropertyAdded(Structure* structure, uint32_t name, Structure* after, PropertyOffset offset, bool createsExtraProperties, UniquedStringImpl* uid, bool isDefinition)
{
    auto place = extraPropertyCode(after, offset);
    if (!place || !stubsCanAddExtraProperty(structure, after, offset, createsExtraProperties))
        return false;
    uint32_t index = ensureIndex(structure, name);
    if (!index || !releaseSlotsUnlessUsedByStores(index) || !(entryAt(index) & hasKindAndCode) || isFarInherited(entryAt(index)))
        return false;
    uint32_t before = entryAt(index);
    bool isKnownToBeAbsent = kindOf(before) == Kind::Absent || (addsExtraProperty(before) && payload(index) & extraPropertyIsKnownToBeAbsent);
    if (!isKnownToBeAbsent && !addsExtraProperty(before) && kindOf(before) != Kind::Unknown)
        return false;
    ASSERT(!(after->id().bits() & (newStructureFlags | payloadIsSlots)));
    if (isDefinition && addsExtraProperty(before) && !(codeOf(before) & onlyDefinitionAddsExtraProperty))
        return true;
    setEntryAt(index, entryWithKind(name, Kind::ExtraProperty, *place | storeAddsExtraProperty | (isDefinition ? onlyDefinitionAddsExtraProperty : 0)));
    setPayload(index, after->id().bits() | (isKnownToBeAbsent ? extraPropertyIsKnownToBeAbsent : 0) | (createsExtraProperties ? storeCreatesExtraProperties : 0));
    m_newAdditionIndices.append(index);
    if (!isKnownToBeAbsent && !isDefinition)
        setDependsOnPrototypes(index, name, uid);
    return true;
}

bool PropertyLookupTable::releaseSlotsUnlessUsedByStores(uint32_t index)
{
    if (Slot* slots = slotsAt(index)) {
        for (Access other : { Access::Store, Access::Define }) {
            if (!isUnused(slots[static_cast<unsigned>(other)]))
                return false;
        }
        releaseSlotsAt(index);
    }
    return true;
}

bool PropertyLookupTable::setAccessor(Structure* structure, uint32_t name, JSCell* holder, PropertyOffset offset, UniquedStringImpl* uid, bool storesCallSetter)
{
    static_assert(minHolderWord == MarkedBlockCage::minIndex);
    Kind kind = storesCallSetter ? Kind::AccessorWithSetterCall : Kind::Accessor;
    if (!holder && !isInlineOffset(offset))
        return static_cast<uint32_t>(offset - firstOutOfLineOffset) <= maxOwnOutOfLineIndex && setEntryWithKind(structure, name, kind, 0, distanceBelowButterfly(offset - firstOutOfLineOffset), uid);
    auto code = codeForInlineOrOutOfLineOffset(offset);
    if (!code || (holder && !MarkedBlockCage::contains(holder)))
        return false;
    if (holder)
        return setEntryWithKind(structure, name, kind, *code, MarkedBlockCage::encode(holder), uid);
    if (storesCallSetter)
        return setEntryWithKind(structure, name, kind, 0, wordForOwnInlineAccessor(*code), uid);
    return setEntryWithKind(structure, name, Kind::OwnAccessorInline, *code, 0, uid);
}

bool PropertyLookupTable::setField(Structure* structure, uint32_t name, uint64_t fieldReadInfo)
{
    uint32_t offset = static_cast<uint32_t>(fieldReadInfo);
    uint32_t how = static_cast<uint32_t>((fieldReadInfo & (Slot::fieldHoldsLazyClosure - 1)) >> Slot::fieldReadShift);
    if (how >> 4)
        return false;
    Kind kind;
    switch (static_cast<Slot::FieldRead>(how & 7)) {
    case Slot::FieldRead::Boolean:
        return setEntryWithKind(structure, name, Kind::FieldBoolean, offset, 0, nullptr);
    case Slot::FieldRead::OptionalBoolean:
        return setEntryWithKind(structure, name, Kind::FieldOptionalBoolean, offset, 0, nullptr);
    case Slot::FieldRead::Double:
        kind = Kind::FieldDouble;
        break;
    case Slot::FieldRead::Boxed:
        kind = Kind::FieldBoxed;
        break;
    case Slot::FieldRead::Cell32:
        kind = Kind::FieldCell32;
        break;
    case Slot::FieldRead::Int32OrNumberBox:
    case Slot::FieldRead::Int32:
        kind = Kind::FieldInt32;
        break;
    default:
        return false;
    }
    return !(offset % sizeof(uint32_t)) && setEntryWithKind(structure, name, kind, offset / sizeof(uint32_t), 0, nullptr);
}

void PropertyLookupTable::setKnownCustomGetter(Structure* structure, uint32_t name, uint32_t offsetOfCachedValue, UniquedStringImpl* uid)
{
    uint32_t index = findIndex(structure, name);
    if (!index || !slotsAt(index) || kindOf(entryAt(index)) != Kind::Unknown)
        return;
    uint32_t code = offsetOfCachedValue / sizeof(uint32_t);
    setEntryAt(index, entryWithKind(name, Kind::KnownCustomGetter, offsetOfCachedValue % sizeof(uint32_t) || code > UINT8_MAX ? 0 : code));
    setDependsOnPrototypes(index, name, uid);
}

bool PropertyLookupTable::setNamespaceExport(Structure* structure, uint32_t name, uint32_t number)
{
    uint32_t index = number <= INT32_MAX ? ensureIndex(structure, name) : 0;
    return index && releaseSlotsUnlessUsedByStores(index) && setEntryWithKind(structure, name, Kind::NamespaceExport, 0, number << 1, nullptr);
}

bool PropertyLookupTable::setLazyClosure(Structure* structure, uint32_t name, PropertyOffset offset, Structure* after)
{
    auto code = codeForInlineOrOutOfLineOffset(offset);
    uint32_t index = code ? ensureIndex(structure, name) : 0;
    if (!index || !releaseSlotsUnlessUsedByStores(index) || !setEntryWithKind(structure, name, Kind::LazyClosure, *code, after->id().bits(), nullptr))
        return false;
    m_newAdditionIndices.append(index);
    return true;
}

bool PropertyLookupTable::setArrayLength(Structure* structure, uint32_t name, bool hasDoubleShape)
{
    return setEntryWithKind(structure, name, Kind::ArrayLength, hasDoubleShape, 0, nullptr);
}

bool PropertyLookupTable::setAdded(Structure* structure, uint32_t name, Access access, Structure* after, PropertyOffset offset, uint32_t flags, UniquedStringImpl* uid)
{
    StorageAfterAddition storage = storageAfterAddition(structure, after, offset);
    RELEASE_ASSERT(storage == (flags & growsStorage ? StorageAfterAddition::Doubled : flags & needsFirstStorage ? StorageAfterAddition::IsFirst : StorageAfterAddition::Stays));
    AddedStorage where = flags ? AddedStorage::InNewStorage : isInlineOffset(offset) ? AddedStorage::Inline : AddedStorage::OutOfLine;
    uint32_t code = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset : offset - firstOutOfLineOffset;
    if (code > (where == AddedStorage::Inline ? INT8_MAX : UINT8_MAX))
        return false;
    uint32_t index = ensureIndex(structure, name);
    if (!index || payload(index) & payloadIsSlots || !(entryAt(index) & hasKindAndCode) || isFarInherited(entryAt(index)))
        return false;
    bool isStore = access == Access::Store;
    ASSERT(isStore == !!uid);
    Kind before = kindOf(entryAt(index));
    if (!impliesAbsent(before) && !adds(before) && before != Kind::Unknown)
        return false;
    Kind kind = addingKind(impliesAbsent(before), isStore || storeAdds(before), where);
    ASSERT(!(after->id().bits() & (newStructureFlags | payloadIsSlots)));
    setEntryAt(index, entryWithKind(name, kind, code));
    setPayload(index, after->id().bits() | flags);
    m_newAdditionIndices.append(index);
    if (isStore && !impliesAbsent(kind))
        setDependsOnPrototypes(index, name, uid);
    return true;
}

Slot* PropertyLookupTable::slotIfExists(Structure* structure, uint32_t name, Access access)
{
    uint32_t index = findIndexOfSlots(structure, name);
    Slot* slots = index ? slotsAt(index) : nullptr;
    return slots ? slots + static_cast<unsigned>(access) : nullptr;
}

void PropertyLookupTable::invalidatePrototypeDependentEntries(uint32_t index)
{
    if (!m_dependsOnPrototypes.get(bitOf(index)))
        return;
    m_dependsOnPrototypes.quickClear(bitOf(index));
    uint32_t entry = entryAt(index);
    uint32_t name = entry & maxName;
    if (adds(kindOf(entry)))
        setEntryAt(index, entryWithKind(name, addingKind(false, false, addedStorage(kindOf(entry))), codeOf(entry)));
    else {
        bool hasSlots = payload(index) & payloadIsSlots;
        if (isFarInherited(entry))
            setPayload(index + entriesPerPlane, 0);
        if (kindOf(entry) == Kind::KnownCustomGetter) {
            Slot& read = slotsAt(index)[static_cast<unsigned>(Access::Read)];
            stopWatching(m_data, &read);
            read = Slot { };
        }
        setEntryAt(index, entryWithKind(name, Kind::Unknown, 0));
        if (!hasSlots)
            setPayload(index, 0);
    }
}

void PropertyLookupTable::forget(Structure* structure, uint32_t name)
{
    if (uint32_t index = findIndex(structure, name))
        invalidatePrototypeDependentEntries(index);
}

void PropertyLookupTable::didChangePrototype(UniquedStringImpl* uid)
{
    if (m_declaredMemberCreationDepth || m_nameListLinks.isEmpty())
        return;
    if (uid) {
        auto found = m_nameByUID.find(uid);
        if (found == m_nameByUID.end())
            return;
        uint32_t name = found->value;
        forEachIndexInNameList(name, [&](uint32_t index) {
            if (isKindEntryFor(entryAt(index), name))
                invalidatePrototypeDependentEntries(index);
        });
        freeNameList(name);
        return;
    }
    clearNameLists();
    for (size_t bit : m_dependsOnPrototypes)
        invalidatePrototypeDependentEntries(static_cast<uint32_t>(bit));
}

auto PropertyLookupTable::entriesReferencing(UniquedStringImpl* uid, JSCell* holder, PropertyOffset offset) const -> EntriesReferencingProperty
{
    EntriesReferencingProperty result;
    auto name = m_nameByUID.find(uid);
    if (name == m_nameByUID.end())
        return result;
    result.name = name->value;
    uint32_t place = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
    forEachIndexInNameList(result.name, [&](uint32_t index) {
        uint32_t entry = entryAt(index);
        if (!isKindEntryFor(entry, result.name) || !m_dependsOnPrototypes.get(bitOf(index)) || (kindOf(entry) != Kind::InheritedInline && kindOf(entry) != Kind::Accessor))
            return;
        if (codeOf(entry) != place || payload(index) != MarkedBlockCage::encode(holder))
            return;
        result.owners.append(m_rows[entryRow(index, result.name)].owner);
    });
    return result;
}

void PropertyLookupTable::release(uint32_t base)
{
    uint32_t findStructureDescriptor = payload(base);
    Row& row = m_rows[findStructureDescriptor];
    for (uint16_t listed : row.names) {
        uint32_t index = indexOfRowName(base, listed);
        releaseSlotsAt(index);
        vacate(index);
        if (m_dependsOnPrototypes.get(bitOf(index)))
            m_dependsOnPrototypes.quickClear(bitOf(index));
    }
    vacate(base);
    row.owner = nullptr;
    row.names.clear();
    m_freeRows.append(findStructureDescriptor);
}

Slot* PropertyLookupTable::ensureSlot(Structure* structure, uint32_t name, Access access)
{
    uint32_t index = ensureIndex(structure, name);
    if (index && hasOwnOffsetInWord(entryAt(index)))
        index = ensureIndexInOverflow(structure, name);
    if (!index)
        return nullptr;
    if (!(payload(index) & payloadIsSlots)) {
        uint32_t first;
        if (!m_freeSlots.isEmpty())
            first = m_freeSlots.takeLast();
        else if (m_numberOfSlots + slotsPerPayload <= maxNumberOfSlots)
            first = std::exchange(m_numberOfSlots, m_numberOfSlots + slotsPerPayload);
        else
            return nullptr;
        if (usesPayloadDirectly(entryAt(index))) {
            if (m_dependsOnPrototypes.get(bitOf(index)))
                m_dependsOnPrototypes.quickClear(bitOf(index));
            setEntryAt(index, entryWithKind(name, Kind::Unknown, 0));
        }
        setPayload(index, first << 1 | payloadIsSlots);
    }
    if (access == Access::Read && kindOf(entryAt(index)) == Kind::KnownCustomGetter)
        setEntryAt(index, entryWithKind(name, Kind::Unknown, 0));
    return slotsAt(index) + static_cast<unsigned>(access);
}

void PropertyLookupTable::releaseSlotsAt(uint32_t index)
{
    Slot* slots = slotsAt(index);
    if (!slots)
        return;
    for (unsigned i = 0; i < slotsPerPayload; ++i) {
        stopWatching(m_data, &slots[i]);
        slots[i] = Slot { };
    }
    m_freeSlots.append(payload(index) >> 1);
    setPayload(index, 0);
    if (uint32_t entry = entryAt(index); kindOf(entry) == Kind::KnownCustomGetter)
        setEntryAt(index, entryWithKind(entry & maxName, Kind::Unknown, 0));
}

void PropertyLookupTable::releaseSlot(Structure* structure, uint32_t name, Access access)
{
    uint32_t index = findIndexOfSlots(structure, name);
    Slot* slots = index ? slotsAt(index) : nullptr;
    if (!slots)
        return;
    Slot& slot = slots[static_cast<unsigned>(access)];
    stopWatching(m_data, &slot);
    slot = Slot { };
    for (unsigned i = 0; i < slotsPerPayload; ++i) {
        if (!isUnused(slots[i]))
            return;
    }
    releaseSlotsAt(index);
}

bool PropertyLookupTable::isWatched(Slot* slot) const
{
    return m_data->watchpoints && m_data->watchpoints->contains(slot);
}

void PropertyLookupTable::removeDeadNewStructures(VM& vm, Vector<uint32_t>& indices)
{
    indices.removeAllMatching([&](uint32_t index) {
        uint32_t entry = entryAt(index);
        if (!payloadIsStructure(entry))
            return true;
        Kind kindWithout = impliesAbsent(kindOf(entry)) || (addsExtraProperty(entry) && payload(index) & extraPropertyIsKnownToBeAbsent) ? Kind::Absent : Kind::Unknown;
        if (!(entry & hasKindAndCode) || !(entry & maxName))
            return true;
        if (vm.heap.isMarked(std::bit_cast<StructureID>(payload(index) & ~newStructureFlags).decode()))
            return false;
        structureDidDie(std::bit_cast<StructureID>(payload(index) & ~newStructureFlags).decode());
        setEntryAt(index, entryWithKind(entry & maxName, kindWithout, 0));
        setPayload(index, 0);
        if (kindWithout == Kind::Unknown && m_dependsOnPrototypes.get(bitOf(index)))
            m_dependsOnPrototypes.quickClear(bitOf(index));
        return true;
    });
}

uint32_t PropertyLookupTable::shapeHash(Structure* structure)
{
    uint32_t word = structure->propertyHash() ^ static_cast<uint32_t>(JSValue::encode(structure->storedPrototype()) >> 4) * 0x85ebca6bu ^ structure->inlineCapacity();
    return word ? word : 1;
}

void PropertyLookupTable::structureDidDie(Structure* structure)
{
    uint32_t word = shapeHash(structure);
    deadShapeEntry(word) = word;
}

void PropertyLookupTable::markIfShapeDiedBefore(SlotVisitor& visitor, StructureID id)
{
    if (!id)
        return;
    Structure* structure = id.decode();
    uint32_t word = shapeHash(structure);
    if (deadShapeEntry(word) != word || visitor.isMarked(structure))
        return;
    JSValue prototype = structure->storedPrototype();
    if (visitor.isMarked(structure->realm()) && (!prototype.isObject() || visitor.isMarked(asObject(prototype))))
        visitor.appendUnbarriered(structure);
}

void PropertyLookupTable::markShapesThatDiedBefore(SlotVisitor& visitor, uint32_t version)
{
    if (std::exchange(m_lastNewShapeScanVersion, version) == version)
        return;
    for (Structure* structure : m_structuresWithNewRows)
        markIfShapeDiedBefore(visitor, structure->id());
    for (uint32_t index : m_newAdditionIndices) {
        uint32_t entry = entryAt(index);
        if ((entry & hasKindAndCode) && (entry & maxName) && payloadIsStructure(entry))
            markIfShapeDiedBefore(visitor, std::bit_cast<StructureID>(payload(index) & ~newStructureFlags));
    }
}

void PropertyLookupTable::finalizeUnconditionally(VM& vm, bool isEdenCollection)
{
    if (isEdenCollection) {
        for (Structure* structure : m_structuresWithNewRows) {
            if (vm.heap.isMarked(structure))
                continue;
            structureDidDie(structure);
            if (uint32_t base = baseOf(structure))
                release(base);
        }
    } else {
        for (unsigned i = 0; i < m_rows.size(); ++i) {
            if (Structure* owner = m_rows[i].owner; owner && !vm.heap.isMarked(owner)) {
                structureDidDie(owner);
                release(baseOf(owner));
            }
        }
    }
    m_structuresWithNewRows.shrink(0);
    removeDeadNewStructures(vm, m_newAdditionIndices);
    if (!isEdenCollection)
        removeDeadNewStructures(vm, m_additionIndices);
    m_additionIndices.appendVector(m_newAdditionIndices);
    m_newAdditionIndices.shrink(0);
    if (isEdenCollection)
        return;
    for (size_t bit : m_registeredNames) {
        uint32_t name = static_cast<uint32_t>(bit);
        uint32_t* link = nameListHeadIfExists(name);
        while (uint32_t position = link ? *link : 0) {
            NameListLink& current = m_nameListLinks[position - 1];
            Structure* owner = m_rows[current.row].owner;
            uint32_t index = owner ? findIndex(owner, name) : 0;
            if (index && isKindEntryFor(entryAt(index), name) && m_dependsOnPrototypes.get(bitOf(index))) {
                m_dependsOnPrototypes.quickClear(bitOf(index));
                link = &current.next;
                continue;
            }
            *link = current.next;
            current.next = m_firstFreeNameListLink;
            m_firstFreeNameListLink = position;
        }
    }
    for (size_t bit : m_registeredNames) {
        forEachIndexInNameList(static_cast<uint32_t>(bit), [&](uint32_t index) {
            m_dependsOnPrototypes.quickSet(bitOf(index));
        });
    }
    for (uint32_t i = slotsPerPayload; i < m_numberOfSlots; ++i)
        m_data->finalizeSlot(vm, m_slots[i]);
    m_data->finalizeUnconditionally(vm);
}

void PropertyLookupTable::clear()
{
    for (unsigned i = 0; i < m_rows.size(); ++i) {
        if (Structure* owner = m_rows[i].owner) {
            release(baseOf(owner));
            owner->setLookupTableRow(emptyRowEntries());
        }
    }
    m_structuresWithNewRows.shrink(0);
    m_additionIndices.shrink(0);
    m_newAdditionIndices.shrink(0);
    clearNameLists();
    m_nameByUID.clear();
    m_registeredNames.clearAll();
}

void PropertyLookupTable::didCreate(Instance* instance)
{
    instance->lookupTableSlots = m_slots;
    if (!m_data->instance || m_data->instance == instance)
        return;
    m_isSharedByInstances = true;
    clear();
}

void PropertyLookupTable::willDestroy(Instance* instance)
{
    if (m_data->instance != instance)
        return;
    clear();
    m_data->instance = nullptr;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
