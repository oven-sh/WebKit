/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCaches.h"

#if ENABLE(AOT)

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

static bool tryCacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache);
static bool cacheAbsentCompiledField(JSGlobalObject*, Data*, Slot* cache, JSCell* object, const Identifier&, bool mayBePolymorphic);

static auto& propertyNameIDs(VM& vm)
{
    auto& table = vm.aotPropertyNameIDs;
    if (!table.next) [[unlikely]] {
        RELEASE_ASSERT(table.ids.isEmpty());
        Image* image = Image::withCode();
        table.next = (image ? image->header().largestFieldID : 0) + 1;
    }
    return table;
}

static uint16_t propertyNameID(VM& vm, UniquedStringImpl* uid)
{
    auto& table = propertyNameIDs(vm);
    if (uint16_t id = table.ids.get(uid))
        return id;
    if (table.next >= Structure::firstReservedPropertyNameID)
        return 0;
    uint16_t id = static_cast<uint16_t>(table.next++);
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
    // What a cacheable dictionary has stays in its slot for as long as it has the Structure, and Structure::add() forgets what was said of the slot it takes.
    if (structure->isUncacheableDictionary() || !structure->recordsPropertyNames() || structure->cannotConvertToTypedLayout())
        return 0;
    if (structure->mayBePrototype() || (structure->typedLayoutID() && TypedLayoutTable::hasTypedFields()))
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

static bool fillPropertyNameTable(VM& vm, Structure* structure)
{
    if (!structure->recordsPropertyNames())
        return false;
    if (structure->cannotConvertToTypedLayout())
        return true;
    bool isFullyFilled = true;
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot)
        isFullyFilled &= !!structure->fieldIDInSlot(slot);
    if (isFullyFilled)
        return true;
    uint16_t ids[Structure::numberOfSlotsWithPropertyNameIDs];
    std::ranges::fill(ids, Structure::noPropertyNameID);
    bool hasTypedFields = structure->typedLayoutID() && TypedLayoutTable::hasTypedFields();
    if (!structure->isUncacheableDictionary() && !structure->mayBePrototype() && !hasTypedFields && structure->propertyAccessesAreCacheable() && !structure->needImpurePropertyWatchpoint() && !structure->typeInfo().overridesGetOwnPropertySlot()) {
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
    if (!fillPropertyNameTable(vm, structure)) {
        countOperationNamed(several->owner->instance, __func__, several->fillsNameTables() ? "structure-records-no-names" : "site-no-longer-fills-name-tables");
        if (several->fillsNameTables())
            several->didFailToFillNameTable();
        return false;
    }
    if (!slot.isCacheableValue() || slot.slotBase() != base || static_cast<unsigned>(slot.cachedOffset()) >= Structure::numberOfSlotsWithPropertyNameIDs)
        return false;
    uint16_t id = structure->fieldIDInSlot(slot.cachedOffset());
    return id < Structure::firstReservedPropertyNameID && several->addInlineNameSlot(id, slot.cachedOffset());
}

static PolymorphicSlots* siteSlots(VM& vm, Data* data, Slot* cache, Structure* structure, const Identifier& ident)
{
    if (cache->isNameOnly()) {
        PolymorphicSlots* several = data->instance->makeSiteSlots(data, cache, ident.impl());
        several->addInlineNameSlot(cache->offset >> Slot::nameIDShift, (cache->offset & Slot::directLocationMask) - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
    }
    if (!cache->isPolymorphic()) {
        if (!cache->structureID || cache->structureID == structure->id())
            return nullptr;
        PolymorphicSlots* several = data->instance->makeSiteSlots(data, cache, ident.impl());
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
    if (several->remainingBulkLearnAttempts)
        several->remainingBulkLearnAttempts--;
    Slot& leaving = several->slots[PolymorphicSlots::indexFor(structure->id())];
    if (leaving.structureID == structure->id() || (!leaving.structureID && (leaving.offset & Slot::attemptsMask) != Slot::attemptsMask))
        return &leaving;
    stopWatching(data, &leaving);
    leaving.clear();
    leaving.offset = 0;
    data->slotEpoch++;
    return &leaving;
}

void cacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache, bool mayBePolymorphic)
{
    if (SharedData::contains(cache))
        return;
    if (slot.isUnset() && base.isObject() && structureBefore->compiledLayout() && base.asCell()->structure() == structureBefore && cacheAbsentCompiledField(globalObject, data, cache, base.asCell(), ident, mayBePolymorphic))
        return;
    if (auto namespaceSlot = slot.moduleNamespaceSlot()) {
        if (base != slot.slotBase())
            return;
        Structure* structure = base.asCell()->structure();
        uint32_t location = JSLexicalEnvironment::offsetOfVariable(ScopeOffset(namespaceSlot->scopeOffset)) / sizeof(EncodedJSValue);
        if (usesDataStubs() && structure == structureBefore && structure != globalObject->moduleNamespaceObjectStructure() && location < (1u << (Slot::offsetBits - 1)) && mayReplace(cache, structure)) {
            if (cache->hasPointer())
                stopWatching(data, cache);
            fill(globalObject->vm(), data, cache, structure, location | Slot::pointerIsCell, namespaceSlot->environment);
        }
        return;
    }
    if (mayBePolymorphic && usesDataStubs() && base.isCell() && base.asCell()->structure() == structureBefore) {
        if (uint16_t id = recordPropertyNameInStructure(globalObject->vm(), base.asCell(), structureBefore, ident, slot)) {
            uint32_t location = *propertyLocation(slot.cachedOffset());
            uint32_t sameAccess = location | static_cast<uint32_t>(id) << Slot::nameIDShift;
            if (cache->isNameOnly() && cache->offset == sameAccess)
                return;
            if (uint32_t cachedAccess = cache->offset & ~Slot::attemptsMask; cache->structureID && (cachedAccess == sameAccess || cachedAccess == location)) {
                Structure* cached = cache->structureID.decode();
                if (fillPropertyNameTable(globalObject->vm(), cached) && cached->fieldIDInSlot(slot.cachedOffset()) == id) {
                    cache->structureID = StructureID();
                    cache->offset = sameAccess;
                    data->slotEpoch++;
                    return;
                }
            }
        }
    }
    if (mayBePolymorphic && usesDataStubs() && base.isCell()) {
        VM& vm = globalObject->vm();
        Structure* structure = base.asCell()->structure();
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident)) {
            // Both: the table is asked first and answers in fewer instructions; by name any number of structures is served.
            if (structure == structureBefore)
                cacheByName(vm, several, base.asCell(), structure, slot);
            cache = findFillableEntry(data, several, structure);
        }
    }
    if (!tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache))
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
        return Slot::FieldRead::Cell32;
    case Store::Int32OrElsewhere:
        return Slot::FieldRead::Int32OrElsewhere;
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
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// After a read by name that has found nothing, from an object of a compiled layout. False: the name is no field of the layout.
static bool cacheAbsentCompiledField(JSGlobalObject* globalObject, Data* data, Slot* cache, JSCell* cell, const Identifier& ident, bool mayBePolymorphic)
{
    VM& vm = globalObject->vm();
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Structure* structure = object->structure();
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    if (!field)
        return false;
    auto read = usesDataStubs() ? fieldReadFor(field->store) : std::nullopt;
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
    if (cache->hasPointer())
        stopWatching(data, cache);
    if (!watchConditions(vm, data, cache, generateConditionsForPropertyMiss(vm, globalObject, globalObject, structure, ident.impl()))) {
        countFailure(cache);
        return true;
    }
    uint64_t description = Slot::absentFieldIsUndefined | (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | static_cast<uint64_t>(*read) << Slot::fieldReadShift | field->offset;
    fill(vm, data, cache, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(description));
    return true;
}

void noteNamedFieldRead(Instance* instance, JSCell* cell, const Identifier& ident, uint32_t name)
{
    if (!usesDataStubs())
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    if (auto read = field ? fieldReadFor(field->store) : std::nullopt)
        Instance::noteNamedField(instance->namedFieldReads, Typed::shapeWordOf(object->shape()), name, (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | static_cast<uint64_t>(*read) << Slot::fieldReadShift | field->offset);
}

void cacheCompiledField(VM& vm, Data* data, Slot* cache, JSCell* cell, const Identifier& ident, bool mayBePolymorphic)
{
    if (SharedData::contains(cache) || !usesDataStubs())
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    Structure* structure = object->structure();
    // No field: an extra property, which goes by the object.
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    auto read = field ? fieldReadFor(field->store) : !ident.isPrivateName() && !parseIndex(ident) ? std::optional(Slot::FieldRead::NoField) : std::nullopt;
    if (!read) {
        countFailure(cache);
        return;
    }
    if (mayBePolymorphic) {
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident))
            cache = findFillableEntry(data, several, structure);
    }
    if (!mayReplace(cache, structure))
        return;
    if (cache->hasPointer())
        stopWatching(data, cache);
    static_assert(sizeof(void*) == sizeof(uint64_t));
    uint64_t description = static_cast<uint64_t>(*read) << Slot::fieldReadShift;
    if (field)
        description |= (field->flags & Typed::fieldIsOutOfLine ? Slot::fieldIsInExtension : 0) | field->offset;
    fill(vm, data, cache, structure, Slot::compiledFieldFlags, std::bit_cast<void*>(description));
}

// No object of the structure will ever inherit a property of the name: nothing on the prototype chain has one, takes one, or gets another prototype.
static bool isAbsentFromPrototypeChainForGood(JSGlobalObject* globalObject, Structure* structure, PropertyName name)
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

void cacheKeyedReadOfCompiledField(JSGlobalObject* globalObject, Instance::KeyedRead& read, JSObject* base, const Identifier& ident)
{
    const Typed::LayoutData* layout = base->structure()->compiledLayout();
    if (!usesDataStubs() || !layout || !Typed::isObjectLayout(layout->kind))
        return;
    auto* object = uncheckedDowncast<Typed::Object>(base);
    const Typed::FieldData* field = object->fieldNamed(object->state(), ident);
    auto kind = field ? fieldReadFor(field->store) : std::nullopt;
    if (!kind)
        return;
    uint64_t description = static_cast<uint64_t>(*kind) << Slot::fieldReadShift | field->offset;
    if (field->flags & Typed::fieldIsOutOfLine)
        description |= Slot::fieldIsInExtension;
    if (isAbsentFromPrototypeChainForGood(globalObject, base->structure(), ident))
        description |= Slot::absentFieldIsUndefined;
    read.holder = std::bit_cast<JSCell*>(description);
    read.slot.offset = Instance::keyIsCompiledField;
}

static void cacheGetterShortcut(VM& vm, GetterSetter* getterSetter, JSFunction* getter)
{
    if (getterSetter->getterShortcut() || getter->isHostFunction())
        return;
    constexpr auto kind = CodeSpecializationKind::CodeForCall;
    Instance* instance = Instance::of(getter);
    FunctionExecutable* executable = getter->jsExecutable();
    FunctionRef function = FunctionRef::of(vm, executable, kind, tokenOf(instance));
    if (!function || function.instance != instance)
        return;
    const ImageFunction* record = function.info().function();
    if (record->usesStaticImports && !moduleIsLinkedAsCompiled(getter->scope()))
        return;
    if (record->returnsScopeVariable) {
        JSScope* scope = getter->scope();
        for (unsigned hops = record->returnedVariable()[0]; hops && scope; --hops)
            scope = scope->next();
        ScopeOffset offset(record->returnedVariable()[1]);
        if (auto* environment = dynamicDowncast<JSLexicalEnvironment>(scope); environment && environment->isValidScopeOffset(offset))
            getterSetter->setVariableReturnedByGetter(&environment->variableAt(offset));
        return;
    }
    if (record->takesList || record->numberOfParameters || !executable->aotEntryFor(kind))
        return;
    if (!instance->isLinked(function.index)) {
        DeferGCForAWhile deferGC(vm);
        if (!linkStaticFunction(instance, executable, kind, getter->scopeUnchecked()))
            return;
    }
    getterSetter->setCodeOfGetter(instance->code + (executable->aotEntryFor(kind) & EntryWord::addressMask));
}

static void cachePropertyOfBoundThis(JSGlobalObject* globalObject, Instance& instance, JSFunction* getter)
{
    VM& vm = globalObject->vm();
    auto* bound = dynamicDowncast<JSBoundFunction>(getter);
    if (!bound || bound->boundArgsLength() != 1 || bound->structureID().bits() != instance.boundFunctionStructureID)
        return;
    bound->clearCachedPropertyOfBoundThis();
    auto* target = dynamicDowncast<JSFunction>(bound->targetFunction());
    if (!target || target->isHostFunction())
        return;
    FunctionRef function = FunctionRef::of(vm, target->jsExecutable(), CodeSpecializationKind::CodeForCall, tokenOf(Instance::of(target)));
    if (!function || !function.info().function()->isGetByValOnThis)
        return;
    JSValue key;
    bound->forEachBoundArg([&](JSValue argument) {
        key = argument;
        return IterationStatus::Done;
    });
    JSObject* object = bound->boundThis().getObject();
    if (!object || !key.isString())
        return;
    String name = asString(key)->tryGetValue();
    RefPtr uid = name.isNull() ? nullptr : AtomStringImpl::lookUp(name.impl());
    if (!uid || parseIndex(*uid))
        return;
    if (Structure* structure = object->structure(); structure->isDictionary() && !structure->hasBeenFlattenedBefore())
        structure->flattenDictionaryStructure(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!object->methodTable()->getOwnPropertySlot(object, globalObject, uid.get(), slot))
        return;
    if (slot.slotBase() != object || slot.isTaintedByOpaqueObject())
        return;
    bool isAccessor = slot.isCacheableGetter();
    if (isAccessor) {
        auto* inner = dynamicDowncast<JSFunction>(slot.getterSetter()->getter());
        if (!inner)
            return;
        cacheGetterShortcut(vm, slot.getterSetter(), inner);
        if (!slot.getterSetter()->getterReturnsVariable())
            return;
    } else if (!slot.isCacheableValue())
        return;
    Structure* structure = object->structure();
    if (!structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint())
        return;
    unsigned attributes;
    if (structure->get(vm, uid.get(), attributes) != slot.cachedOffset())
        return;
    PropertyOffset offset = slot.cachedOffset();
    int32_t location = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset : offsetInButterfly(offset);
    bound->cachePropertyOfBoundThis(vm, structure, location * 2 + isAccessor);
}

static void noteLengthOfTypedArray(JSGlobalObject* globalObject, Instance& instance, JSCell* cell, const PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Structure* structure = cell->structure();
    JSType type = cell->type();
    auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter());
    JSObject* viewPrototype = slot.slotBase();
    unsigned attributes;
    if (!isTypedArrayType(type) || !function || !isTypedArrayViewPrototypeLengthGetter(function) || !viewPrototype->inherits<JSTypedArrayViewPrototype>() || viewPrototype->globalObject() != globalObject)
        return;
    if (!structure->propertyAccessesAreCacheable() || viewPrototype->structure()->isDictionary() || viewPrototype->structure()->get(vm, vm.propertyNames->length.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
        return;
    instance.typedArrayLengthGetter = function;
    auto prototypeOf = [](Structure* structure) -> JSObject* { return structure->hasMonoProto() && !structure->isDictionary() ? structure->storedPrototypeObject() : nullptr; };
    JSObject* prototype = prototypeOf(structure);
    JSObject* secondPrototype = prototype ? prototypeOf(prototype->structure()) : nullptr;
    if (secondPrototype == viewPrototype)
        secondPrototype = prototype;
    if (!secondPrototype || prototypeOf(secondPrototype->structure()) != viewPrototype || isInlineOffset(slot.cachedOffset()))
        return;
    instance.typedArrayViewPrototype = viewPrototype;
    instance.typedArrayViewPrototypeStructureID = viewPrototype->structureID().bits();
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

static bool tryCacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache)
{
    if (base.isCell() && slot.isCacheableGetter() && ident == globalObject->vm().propertyNames->length && base.asCell()->structure() == structureBefore)
        noteLengthOfTypedArray(globalObject, *data->instance, base.asCell(), slot);
    uint32_t getterFlag = usesDataStubs() && slot.isCacheableGetter() ? Slot::isGetter : 0;
    if (!base.isCell() || (!slot.isCacheableValue() && !slot.isUnset() && !getterFlag))
        return false;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* structure = cell->structure();
    if (structure != structureBefore)
        return false;
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore() && !structure->typeInfo().prohibitsPropertyCaching() && cell->isObject()) {
        asObject(cell)->flattenDictionaryObject(vm);
        return false;
    }
    // What the engine's flags say of an object of a compiled layout is for caches that go by the property table, which lists no field.
    bool isOfCompiledLayout = structure->compiledLayout();
    if (isOfCompiledLayout ? !Typed::Object::absenceIsByStructure(structure, ident.impl()) || (!slot.isUnset() && slot.slotBase() == cell) : !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return false;
    if (getterFlag) {
        unsigned attributes;
        if (slot.slotBase()->structure()->get(vm, ident.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
            return false;
        if (auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter())) {
            if (auto* executable = dynamicDowncast<FunctionExecutable>(function->executable()); executable && executable->isGeneratedForCall())
                executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
            cachePropertyOfBoundThis(globalObject, *data->instance, function);
            cacheGetterShortcut(vm, slot.getterSetter(), function);
        }
    }

    // Also of a dictionary: what it has stays where it is for as long as it has the Structure.
    if (!slot.isUnset() && slot.slotBase() == cell) {
        auto location = propertyLocation(slot.cachedOffset());
        if (!location)
            return false;
        if (!mayReplace(cache, structure))
            return false;
        if (cache->hasPointer())
            stopWatching(data, cache);
        uint32_t nameID = usesDataStubs() ? recordPropertyNameInStructure(vm, cell, structure, ident, slot) : 0;
        fill(vm, data, cache, structure, *location | getterFlag | nameID << Slot::nameIDShift, (*location | getterFlag) & (Slot::isIndirect | Slot::isGetter) ? nullptr : ident.impl());
        return true;
    }

    uint32_t attempts = (cache->offset & Slot::attemptsMask) >> Slot::attemptsShift;
    if (attempts == Slot::maxAttempts)
        return false;
    if (attempts >= Slot::maxAttemptsForInheritedProperty) {
        cache->offset |= Slot::attemptsMask;
        return false;
    }
    cache->offset += 1u << Slot::attemptsShift;
    if (!attempts)
        return false;

    if (structure->typeInfo().prohibitsPropertyCaching() && !isOfCompiledLayout)
        return false;
    if (structure->isDictionary()) {
        if (!structure->hasBeenFlattenedBefore())
            structure->flattenDictionaryStructure(vm, asObject(cell));
        return false;
    }
    if (slot.isUnset() && structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() && !isOfCompiledLayout)
        return false;
    // Whether a function has what is made when it is first asked for goes by the function: an arrow function and an ordinary one have one Structure as they are allocated.
    if (slot.isUnset() && cell->type() == JSFunctionType && (ident == vm.propertyNames->prototype || ident == vm.propertyNames->length || ident == vm.propertyNames->name))
        return false;
    JSCell* startOfChain = isOfCompiledLayout ? structure->storedPrototypeObject() : cell;
    auto status = startOfChain ? prepareChainForCaching(globalObject, startOfChain, ident.impl(), slot) : std::optional(PrototypeChainCachingStatus { });
    if (!status || status->usesPolyProto)
        return false;
    // The property has moved. An object is made flat once: that is no try of the site, and the next miss finds the property where it stays.
    if (status->flattenedDictionary) {
        cache->offset -= 1u << Slot::attemptsShift;
        return true;
    }
    makePrototypeChainWatchable(vm, cell);

    if (slot.isUnset()) {
        if (!watchConditions(vm, data, cache, generateConditionsForPropertyMiss(vm, globalObject, globalObject, structure, ident.impl())))
            return false;
        fill(vm, data, cache, structure, *propertyLocation(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(undefinedHolder));
        return true;
    }

    auto location = propertyLocation(slot.cachedOffset());
    if (!location)
        return false;
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, slot.slotBase(), ident.impl())))
        return false;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell | getterFlag, slot.slotBase());
    return true;
}

void noteCustomGetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, const Identifier& ident, const PropertySlot& slot)
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
        auto status = prepareChainForCaching(globalObject, base, ident.impl(), slot);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return;
        if (!generateConditionsForPrototypePropertyHitCustom(vm, globalObject, globalObject, structure, holder, ident.impl(), slot.attributes()).isValid())
            return;
        if (!MegamorphicCache::noteDependenceOnPrototypes(structure->id(), holder))
            return;
    }
    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    if (auto domAttribute = slot.domAttribute(); domAttribute && !(passesHolder ? holder : base)->inherits(domAttribute->classInfo))
        return;
    instance.customGetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.megamorphicCache()->epoch(), passesHolder, ident.impl(), std::bit_cast<void*>(slot.customGetter().taggedPtr()), holder };
}

void noteInheritedSetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Structure* structure = base->structure();
    JSObject* holder = slot.base();
    if (!usesDataStubs() || !vm.megamorphicCache() || holder == base || parseIndex(*ident.impl()))
        return;
    // See tryCacheGetById().
    bool isOfCompiledLayout = structure->compiledLayout();
    if (isOfCompiledLayout ? !Typed::Object::absenceIsByStructure(structure, ident.impl()) : base->type() == GlobalProxyType || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching() || isCopyOnWrite(structure->indexingMode()))
        return;
    if (holder->structure()->isDictionary())
        return;
    auto status = prepareChainForCaching(globalObject, isOfCompiledLayout ? structure->storedPrototypeObject() : base, ident.impl(), holder);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return;
    auto conditions = generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, holder, ident.impl());
    if (!conditions.isValid() || !(conditions.slotBaseCondition().attributes() & PropertyAttribute::Accessor))
        return;
    PropertyOffset offset = conditions.slotBaseCondition().offset();
    if (offset > MegamorphicCache::maxOffset || !MegamorphicCache::noteDependenceOnPrototypes(structure->id(), holder))
        return;
    instance.inheritedSetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.megamorphicCache()->epoch(), static_cast<uint16_t>(offset), ident.impl(), holder, nullptr };
}

static bool tryCacheInstanceOf(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype)
{
    VM& vm = globalObject->vm();
    Structure* structure = constructor->structure();
    if (structure != structureBefore || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching())
        return false;
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
    if (cache->hasPointer())
        stopWatching(data, cache);
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, hasInstance.slotBase(), uid)))
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

void cachePrivateName(VM& vm, Data* data, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
        return;
    Structure* structure = base->structure();
    if (!structure->propertyAccessesAreCacheable() || structure->isDictionary())
        return;
    auto location = propertyLocation(offset.value_or(0));
    if (!location || !mayReplace(cache, structure))
        return;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell, name.asCell());
}

void cacheKeyedAccess(Slot& entry, Structure* structure, JSCell* key, std::optional<PropertyOffset> offset)
{
    auto location = offset && structure->propertyAccessesAreCacheable() ? propertyLocation(*offset) : std::nullopt;
    entry.structureID = StructureID();
    entry.offset = location.value_or(Slot::isGetter);
    entry.pointer = key;
    entry.structureID = structure->id();
}

void cachePrivateNameTransition(VM& vm, Data* data, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
        return;
    Structure* newStructure = base->structure();
    if (!oldStructure->propertyAccessesAreCacheable() || oldStructure->isDictionary() || oldStructure->mayBePrototype() || !newStructure->propertyAccessesAreCacheable() || newStructure->isDictionary())
        return;
    if (newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity() || oldStructure->typeInfoBlob() != newStructure->typeInfoBlob())
        return;
    auto location = propertyLocation(offset.value_or(0));
    if (!location || !mayReplace(cache, oldStructure))
        return;
    Slot& transition = cache[1];
    transition.structureID = StructureID();
    WTF::storeStoreFence();
    transition.offset = Slot::isIndirect;
    transition.pointer = nullptr;
    oldStructure->assertHandsFamilyOnTo(*newStructure);
    transition.newStructureID = newStructure->id();
    WTF::storeStoreFence();
    transition.structureID = oldStructure->id();
    data->instance->noteTransitionCached(&transition);
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

static bool tryCachePutById(JSGlobalObject*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

// What Slot::pointer is for a store to the field of the name: see Slot::FieldWrite. 0: nothing is accepted without asking.
static uint64_t fieldWriteFor(VM& vm, Typed::Object* object, const Identifier& ident)
{
    using FieldWrite = Slot::FieldWrite;
    using Typed::Store;
    namespace Primitive = Typed::Primitive;
    auto access = usesDataStubs() ? object->fieldAccessFor(vm, ident) : std::nullopt;
    uint32_t description = 0;
    if (access && !access->isOutOfLine) {
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
            break;
        case Store::Cell:
            description = FieldWrite::Boxed | stringOrSymbol | otherCells;
            break;
        case Store::Cell32:
            description = FieldWrite::Cell32 | stringOrSymbol;
            break;
        case Store::OptionalCell32:
            description = FieldWrite::Cell32 | stringOrSymbol | undefinedOrNull;
            break;
        case Store::Int32OrElsewhere:
            description = FieldWrite::Int32OrElsewhere | (numbers & FieldWrite::acceptsInt32);
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
    if (!(description & ~(FieldWrite::keptMask | FieldWrite::isOptional)))
        return 0;
    return static_cast<uint64_t>(description | FieldWrite::isFieldWrite) << 32 | access->offset;
}

// After a store by name to an object of a compiled layout.
static void cacheStoreToCompiledField(VM& vm, Data* data, Slot* cache, JSCell* cell, const Identifier& ident)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint64_t fieldWrite = fieldWriteFor(vm, object, ident);
    if (!fieldWrite) {
        // See Slot::FieldWrite::isNoField.
        Typed::State& state = object->state();
        if (usesDataStubs() && !ident.isPrivateName() && !parseIndex(ident) && !object->fieldNamed(state, ident) && !state.hasFixedMethod(object->layout(), ident.impl()) && mayReplace(cache, cell->structure())) {
            if (cache->hasPointer())
                stopWatching(data, cache);
            fill(vm, data, cache, cell->structure(), Slot::pointerIsNotCell, std::bit_cast<void*>(static_cast<uint64_t>(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField) << 32));
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

void noteNamedFieldWrite(Instance* instance, JSCell* cell, const Identifier& ident, uint32_t name)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (uint64_t fieldWrite = fieldWriteFor(*instance->vm, object, ident))
        Instance::noteNamedField(instance->namedFieldWrites, Typed::shapeWordOf(object->shape()), name, fieldWrite);
}

void cacheStoreTransition(Instance* instance, Data* data, JSCell* cell, Structure* oldStructure, uint64_t wordsBefore, const Identifier& ident, Slot* cache)
{
    VM& vm = *instance->vm;
    if (SharedData::contains(cache))
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    uint64_t fieldWrite = fieldWriteFor(vm, object, ident);
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

void cachePutById(Instance* instance, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    JSGlobalObject* globalObject = instance->globalObject;
    if (base.isObject() && oldStructure->compiledLayout()) {
        if (!SharedData::contains(cache) && !isDirect && base.asCell()->structure() == oldStructure)
            cacheStoreToCompiledField(globalObject->vm(), data, cache, base.asCell(), ident);
        return;
    }
    if (slot.type() == PutPropertySlot::NewProperty && slot.isCacheablePut() && base.isCell() && slot.base() == base.asCell() && !isInlineOffset(slot.cachedOffset()))
        instance->noteOutOfLineProperty(base.asCell()->structure());
    if (slot.type() == PutPropertySlot::NewTypedField && base.isCell() && slot.base() == base.asCell() && isInlineOffset(slot.cachedOffset())) {
        Structure* newStructure = base.asCell()->structure();
        if (newStructure->previousID() == oldStructure && !newStructure->isDictionary() && !oldStructure->mayBePrototype() && oldStructure->outOfLineCapacity() == newStructure->outOfLineCapacity())
            instance->noteFieldAddition(oldStructure, slot.cachedOffset(), newStructure);
    }
    if (SharedData::contains(cache))
        return;
    if (!tryCachePutById(globalObject, data, base, oldStructure, ident, slot, isDirect, cache))
        countFailure(cache);
}

static bool tryCachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (!base.isCell() || (!slot.isCacheablePut() && !slot.isTypedFieldCacheablePut()) || slot.base() != base.asCell())
        return false;
    if (!oldStructure->propertyAccessesAreCacheable() || (oldStructure->isDictionary() && slot.type() != PutPropertySlot::ExistingProperty) || oldStructure->mayBePrototype())
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
        uint32_t fieldType = slot.type() == PutPropertySlot::ExistingTypedField || slot.type() == PutPropertySlot::NewTypedField ? slot.fieldType() : 0;
        cache->offset = *location | attempts | (structureAfterwards ? Slot::isIndirect : 0) | (fieldType ? Slot::hasFieldType : 0);
        cache->pointer = nullptr;
        if (structureAfterwards)
            oldStructure->assertHandsFamilyOnTo(*structureAfterwards);
        cache->newStructureID = structureAfterwards ? structureAfterwards->id() : StructureID();
        cache->fieldType = fieldType;
        WTF::storeStoreFence();
        cache->structureID = oldStructure->id();
        if (structureAfterwards)
            data->instance->noteTransitionCached(cache);
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

    if (newStructure->isDictionary() || newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity())
        return abandon(cache);

    if (isDirect)
        stopWatching(data, cache);
    else {
        auto status = prepareChainForCaching(globalObject, cell, ident.impl(), nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
        makePrototypeChainWatchable(vm, cell);
        if (!watchConditions(vm, data, cache, generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, newStructure, ident.impl())))
            return false;
    }
    fillPut(newStructure);
    return true;
}

void cacheDefinedProperty(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, UniquedStringImpl* uid, std::span<UniquedStringImpl* const> absent)
{
    VM& vm = globalObject->vm();
    if (SharedData::contains(cache))
        return;
    Structure* structure = object->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    auto location = isValidOffset(offset) ? propertyLocation(offset) : std::nullopt;
    bool isOrdinary = object->type() == FinalObjectType && !before->compiledLayout() && !before->typedLayoutID() && before->propertyAccessesAreCacheable() && !before->isDictionary() && !before->mayBePrototype()
        && structure->propertyAccessesAreCacheable() && !structure->isDictionary();
    if (!isOrdinary || !location || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return countFailure(cache);
    bool isAdded = structure != before;
    if (isAdded) {
        if (structure->previousID() != before || isValidOffset(before->get(vm, uid)) || offset != structure->maxOffset() || before->outOfLineCapacity() != structure->outOfLineCapacity())
            return countFailure(cache);
    } else if (!(attributes & PropertyAttribute::ReadOnly) || !(attributes & PropertyAttribute::DontDelete))
        return countFailure(cache);
    if (!mayReplace(cache, before))
        return;
    Vector<ObjectPropertyCondition> conditions;
    for (UniquedStringImpl* name : absent)
        conditions.append(ObjectPropertyCondition::absence(vm, globalObject, globalObject->objectPrototype(), name, nullptr));
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    if (!watchConditions(vm, data, cache, ObjectPropertyConditionSet::create(WTF::move(conditions))))
        return countFailure(cache);
    cache->offset = *location | attempts | (isAdded ? Slot::isIndirect : 0);
    cache->pointer = nullptr;
    if (isAdded) {
        before->assertHandsFamilyOnTo(*structure);
        cache->newStructureID = structure->id();
    }
    WTF::storeStoreFence();
    cache->structureID = before->id();
    if (isAdded)
        data->instance->noteTransitionCached(cache);
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

static ALWAYS_INLINE void countFill(VM& vm, const char* kind, MegamorphicCache& cache, StructureID structureID, UniquedStringImpl* uid, const char* outcome = nullptr, StructureID newStructureID = { })
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countFillOfMegamorphicCache(kind, cache.epoch(), static_cast<uint64_t>(newStructureID.bits()) << 32 | structureID.bits(), uid, outcome);
}

bool isAbsentUnderObjectPrototype(JSGlobalObject* globalObject, JSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !canUseMegamorphicCacheForGet(vm, uid))
        return false;
    Structure* structure = base.asCell()->structure();
    JSObject* prototype = globalObject->objectPrototype();
    Structure* ofPrototype = prototype->structure();
    auto saysWhatItLacks = [](Structure* structure) {
        return !structure->isDictionary() && structure->hasMonoProto() && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence() && !structure->typeInfo().overridesGetOwnPropertySlot();
    };
    if (structure->storedPrototype() != JSValue(prototype) || ofPrototype->isStructureExtensible() || !saysWhatItLacks(structure) || !saysWhatItLacks(ofPrototype) || ofPrototype->storedPrototype().isObject())
        return false;
    if (isValidOffset(structure->get(vm, uid)) || isValidOffset(ofPrototype->get(vm, uid)))
        return false;
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
    JSObject* object = baseObject;
    bool cacheable = true;
    // No prototype looked at so far can change.
    bool isForGood = true;
    // No object of its Structure has the property itself. The search starts at the prototype, and what it finds goes by the Structure, as of an ordinary object.
    if (Structure* structure = baseObject->structure(); Typed::Object::absenceIsByStructure(structure, uid)) {
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
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && (isLazyFunctionPropertyName || (object->type() != ArrayType && (object->type() != JSFunctionType || object->structure()->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence()) && object->type() != DerivedStringObjectType && object != globalObject->arrayPrototype()))) [[unlikely]] {
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
        // What the base has itself goes by its Structure as it is.
        if (object != baseObject)
            flattenIfDictionary();
        structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        if (!hasProperty && object == baseObject)
            flattenIfDictionary();
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (object != baseObject)
            isForGood &= structure->hasImmutableProperties() && !structure->isDictionary();
        if (hasProperty) {
            if (cacheable && slot.cachedOffset() <= MegamorphicCache::maxOffset && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary())) {
                if (slot.isCacheableValue()) {
                    bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                    countFill(vm, "MegamorphicCache::load", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                    if (!isPresent && isForGood && !baseObject->structure()->isDictionary())
                        cache.initAsPermanentHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                    else if (!isPresent)
                        cache.initAsHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                } else if (usesDataStubs() && slot.isCacheableGetter())
                    cache.initAsGetterHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
            }
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
        }

        cacheable &= structure->propertyAccessesAreCacheableForAbsence();
        cacheable &= structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !baseObject->structure()->isDictionary()) {
                bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                countFill(vm, "MegamorphicCache::absent", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                if (!isPresent && isForGood)
                    cache.initAsPermanentMiss(baseObject->structureID(), uid);
                else if (!isPresent)
                    cache.initAsMiss(baseObject->structureID(), uid);
            }
            return jsUndefined();
        }
        object = asObject(prototype);
    }
}

// `goesByStructure`: the answer is that of every object of the structure, for as long as the epoch of the megamorphic cache stands.
bool hasPropertyAndFillMegamorphicCache(JSGlobalObject* globalObject, JSObject* baseObject, UniquedStringImpl* uid, bool& goesByStructure)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    goesByStructure = false;
    if (parseIndex(*uid) || !vm.megamorphicCache() || uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto)
        RELEASE_AND_RETURN(scope, baseObject->hasProperty(globalObject, uid));

    StructureID structureID = baseObject->structureID();
    auto didFind = [&](bool result) {
        if (!MegamorphicCache::noteDependenceOnPrototypes(structureID))
            return result;
        if (result)
            vm.megamorphicCache()->initAsHasHit(structureID, uid);
        else
            vm.megamorphicCache()->initAsHasMiss(structureID, uid);
        goesByStructure = !structureID.decode()->isDictionary();
        return result;
    };
    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    JSObject* object = baseObject;
    bool cacheable = true;
    if (Structure* structure = baseObject->structure(); structure->compiledLayout()) {
        if (auto field = uncheckedDowncast<Typed::Object>(baseObject)->fieldAccessFor(vm, uid); field && !field->mayBeAbsent) {
            goesByStructure = true;
            return true;
        }
        // No object of its Structure has the property itself. The search starts at the prototype.
        if (Typed::Object::absenceIsByStructure(structure, uid)) {
            cacheable = structure->hasMonoProto();
            JSValue prototype = baseObject->getPrototypeDirect();
            if (!prototype.isObject())
                return cacheable ? didFind(false) : false;
            object = asObject(prototype);
        }
    }
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && object->type() != ArrayType && (object->type() != JSFunctionType || object->structure()->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence()) && object != globalObject->arrayPrototype()) [[unlikely]]
            RELEASE_AND_RETURN(scope, object->getNonIndexPropertySlot(globalObject, uid, slot));
        Structure* structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        // What it lacks is no fact of a dictionary.
        if (!hasProperty && object == baseObject)
            baseObject->flattenIfCacheableDictionary(vm);
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            if (cacheable && slot.isCacheable() && baseObject->structureID() == structureID && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary()))
                return didFind(true);
            return true;
        }
        cacheable &= structure->propertyAccessesAreCacheableForAbsence() && structure->hasMonoProto();
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
        bool allocatesInitialStorage = !oldStructure->outOfLineCapacity() && newStructure->outOfLineCapacity() == initialOutOfLineCapacity && slot.cachedOffset() == firstOutOfLineOffset
            && oldStructure->typeInfo().type() == FinalObjectType && !hasIndexedProperties(oldStructure->indexingType());
        reallocating = allocatesInitialStorage ? MegamorphicCache::StoreEntry::allocatesInitialOutOfLineStorage : MegamorphicCache::StoreEntry::reallocates;
    }
    oldStructure->assertHandsFamilyOnTo(*newStructure);
    cache.initAsTransition(oldStructure->id(), newStructure->id(), uid, slot.cachedOffset(), reallocating);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        countFill(vm, "MegamorphicCache::transition", cache, oldStructure->id(), uid, cache.findStore(oldStructure->id(), uid) ? nullptr : "refused", newStructure->id());
}

void fillConstructionCache(VM& vm, Data* data, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator allocator)
{
    if (!allocator || SharedData::contains(cache))
        return;
    cache[0].clear();
    Slot& transition = cache[2];
    transition.structureID = StructureID();
    WTF::storeStoreFence();
    transition.offset = 0;
    transition.pointer = nullptr;
    first->assertHandsFamilyOnTo(*last);
    transition.newStructureID = last->id();
    WTF::storeStoreFence();
    transition.structureID = first->id();
    data->instance->noteTransitionCached(&transition);
    fillAllocationCache(vm, data, cache, last, allocator, last->inlineCapacity(), callee);
}

void cacheSiteObject(VM& vm, Data* data, Slot* cache, JSObject* object)
{
    if (SharedData::contains(cache))
        return;
    fill(vm, data, cache, object->structure(), Slot::pointerIsCell, object);
}

bool cachePropertyRunWithStorage(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, std::span<UniquedStringImpl* const> names, bool isAllocated)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    unsigned capacity = structure->outOfLineCapacity();
    unsigned first = before->outOfLineSize();
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask || capacity > PropertyRunStorage::capacityMask || capacity * sizeof(EncodedJSValue) > MarkedSpace::largeCutoff || first > PropertyRunStorage::maxFirst)
        return false;
    cache->offset += 1u << Slot::attemptsShift;
    cache->clear();
    stopWatching(data, cache);
    ObjectPropertyConditionSet conditions;
    for (UniquedStringImpl* uid : names) {
        auto status = prepareChainForCaching(globalObject, object, uid, nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
        ObjectPropertyConditionSet forName = generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, structure, uid);
        if (!forName.isValid())
            return false;
        conditions = conditions.mergedWith(forName);
    }
    if (!watchConditions(vm, data, cache, conditions))
        return false;
    PropertyOffset next = Instance::offsetAfter(before->maxOffset(), before->inlineCapacity());
    uint32_t numberInObject = isInlineOffset(next) ? std::min<uint32_t>(names.size(), before->inlineCapacity() - next) : 0;
    cache->offset |= (numberInObject ? *propertyLocation(next) : 0) | numberInObject << Slot::nameIDShift | Slot::isIndirect;
    before->assertHandsFamilyOnTo(*structure);
    cache->newStructureID = structure->id();
    cache->fieldType = capacity | (isAllocated ? PropertyRunStorage::isAllocated : 0) | first << PropertyRunStorage::firstShift;
    WTF::storeStoreFence();
    cache->structureID = before->id();
    data->instance->noteTransitionCached(cache);
    didFillSlot(vm, data);
    return true;
}

void cachePropertyRun(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* object, Structure* before, std::span<UniquedStringImpl* const> names, std::span<JSCell* const> keys)
{
    VM& vm = globalObject->vm();
    // Each fill counts: objects of several structures may take turns at a site.
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return;
    cache->offset += 1u << Slot::attemptsShift;
    // What it held is no longer watched.
    cache->clear();
    stopWatching(data, cache);
    Structure* structure = object->structure();
    // Nothing on the prototype chain intercepts a store of one of the names, for as long as the slot is filled.
    ObjectPropertyConditionSet conditions;
    for (UniquedStringImpl* uid : names) {
        auto status = prepareChainForCaching(globalObject, object, uid, nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return;
        ObjectPropertyConditionSet forName = generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, structure, uid);
        if (!forName.isValid())
            return;
        conditions = conditions.mergedWith(forName);
    }
    if (!watchConditions(vm, data, cache, conditions))
        return;
    for (size_t i = 0; i < keys.size(); ++i)
        fill(vm, data, &cache[1 + i], before, Slot::pointerIsCell, keys[i]);
    cache->offset |= *propertyLocation(Instance::offsetAfter(before->maxOffset(), before->inlineCapacity())) | Slot::isIndirect;
    before->assertHandsFamilyOnTo(*structure);
    cache->newStructureID = structure->id();
    WTF::storeStoreFence();
    cache->structureID = before->id();
    data->instance->noteTransitionCached(cache);
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

void fillFunctionAllocationCache(VM& vm, Data* data, Slot* cache, Structure* structure, Allocator allocator, uintptr_t functionWord)
{
    if (!allocator || SharedData::contains(cache))
        return;
    cache[1].offset = structure->typeInfoBlob();
    cache[1].pointer = allocator.localAllocator();
    fill(vm, data, &cache[0], structure, Slot::pointerIsNotCell, std::bit_cast<void*>(functionWord));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
