/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeTable.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTCompiledCallee.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "ArrayStorage.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
#include "JSGlobalProxy.h"
#include "SparseArrayValueMap.h"
#include <algorithm>

namespace JSC { namespace AOT {

struct StructuralRead {
    JSValue value;
    LeafPresence presence { LeafPresence::TakeSlowPath };
};

static ALWAYS_INLINE LeafPresence presenceIn(JSObject* holder, JSObject* receiver)
{
    return holder == receiver ? LeafPresence::Own : LeafPresence::Inherited;
}

static NEVER_INLINE void validateLookupResult(VM& vm, JSObject* object, UniquedStringImpl* uid, bool isPresent, JSValue value)
{
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    JSGlobalObject* globalObject = object->realm();
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    auto* proxy = dynamicDowncast<JSGlobalProxy>(object);
    JSObject* first = proxy ? proxy->target() : object;
    bool has = first->getPropertySlot(globalObject, uid, slot);
    if (has == isPresent && (!has || !value || (slot.isValue() && slot.getValue(globalObject, uid) == value)))
        return;
    crashOnValidationFailure(vm, { ValidationFailureKind::FastPathResultDiffers, { }, { }, object });
}

static ALWAYS_INLINE StructuralRead lookUpGeneric(JSObject* object, UniquedStringImpl* uid)
{
    if (object->type() == GlobalProxyType && uid->isSymbol() && object->classInfo() == JSGlobalProxy::info())
        object = uncheckedDowncast<JSGlobalProxy>(object)->target();
    for (JSObject* receiver = object;;) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        bool hasSymbolsOnlyInStructure = uid->isSymbol() && object->type() == GlobalObjectType && object->methodTable()->getOwnPropertySlot == JSGlobalObject::getOwnPropertySlot;
        if (!hasSymbolsOnlyInStructure && (info.overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags())))
            return { };
        if (info.overridesGetPrototype() || structure->isCompiledStructure())
            return { };
        unsigned attributes;
        auto offset = structure->getWithoutMaterializing(uid, attributes);
        if (!offset)
            return { };
        if (isValidOffset(*offset))
            return { attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue ? JSValue() : object->getDirect(*offset), presenceIn(object, receiver) };
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return { jsUndefined(), LeafPresence::Absent };
        object = asObject(prototype);
    }
}

static StructuralRead readGeneric(VM& vm, JSObject* base, UniquedStringImpl* uid)
{
    StructuralRead read = lookUpGeneric(base, uid);
    if (read.presence != LeafPresence::TakeSlowPath && validatesRuntime(vm)) [[unlikely]]
        validateLookupResult(vm, base, uid, read.presence != LeafPresence::Absent, read.value);
    return read;
}

static JSValue readFieldGeneric(Instance* instance, Object* object, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    const FieldData* field = object->fieldNamed(object->state(), uid);
    if (!field || field->isAccessor() || field->isPrivate() || field->slot.representation() == SlotRepresentation::DeferredClosure)
        return { };
    if (JSValue value = object->readInline(*field))
        return value;
    if (object->hasExtraProperties() && livesInExtraProperties(vm, *object, *field))
        return { };
    ChainWalk walk = walkChain({ instance->globalObject, object->structure(), object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid);
    if (walk.result == ChainWalkResult::Absent)
        return jsUndefined();
    if (walk.result == ChainWalkResult::Unknown || walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || !isValidOffset(walk.offset))
        return { };
    return valueAtOffset(walk.holder, walk.offset);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGeneric, EncodedJSValue, (Instance* instance, JSCell* base, UniquedStringImpl* uid))
{
    LeafOperationScope leaf(*instance->vm);
    if (Object* object = compiledObjectOrNull(base))
        return JSValue::encode(readFieldGeneric(instance, object, uid));
    return JSValue::encode(readGeneric(*instance->vm, asObject(base), uid).value);
}

static StructuralRead readByFiledSlot(const JSObject* receiver, const Slot& slot)
{
    if (slot.kind == Slot::Kind::Absent)
        return { jsUndefined(), LeafPresence::Absent };
    if (slot.kind != Slot::Kind::Value)
        return { };
    if (slot.has(Slot::isOwn))
        return { valueAtOffset(receiver, slot.offsetInStorage()), LeafPresence::Own };
    // The own properties of a global proxy are those of its target, which a slot names as their holder.
    return { valueAtOffset(asObject(slot.holder), slot.offsetInStorage()), receiver->type() == GlobalProxyType ? LeafPresence::TakeSlowPath : LeafPresence::Inherited };
}

static StructuralRead readEmptyFieldByFiledSlot(const Object& object, UniquedStringImpl* uid, const Slot& slot)
{
    const FieldData* field = slot.kind == Slot::Kind::Field ? object.fieldNamed(object.state(), uid) : nullptr;
    if (!field || object.isInSlot(*field))
        return { };
    if (slot.has(Slot::absentIsUndefined))
        return { jsUndefined(), LeafPresence::Absent };
    if (slot.has(Slot::emptySlotMayBeUndefined))
        return { jsUndefined(), LeafPresence::TakeSlowPath };
    return { };
}

static JSValue readModuleNamespaceExport(Instance* instance, ModuleNamespace* namespaceObject, UniquedStringImpl* uid)
{
    if (Instance::owning(namespaceObject) != instance)
        return { };
    const Modules& modules = *instance->state->modules;
    ModuleID module = namespaceObject->module();
    if (modules.status(module) != ModuleStatus::Evaluated)
        return { };
    auto index = namespaceObject->find(uid);
    return index ? modules.readExportIfInitialized(module, *index) : JSValue();
}

static JSValue getDataProperty(JSObject* holder, PropertyOffset offset, unsigned attributes, PropertySlot& slot)
{
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || !isValidOffset(offset))
        return { };
    JSValue value = valueAtOffset(holder, offset);
    if (value)
        slot.setValue(holder, attributes, value, offset);
    return value;
}

static JSValue getFromPrototypeChain(Instance* instance, JSObject* base, UniquedStringImpl* uid, PropertySlot& slot)
{
    ChainWalk walk = walkChain({ instance->globalObject, base->structure(), base, ChainWalkMode::AllocatesNothing }, uid);
    if (walk.result == ChainWalkResult::Absent)
        return jsUndefined();
    return walk.result == ChainWalkResult::Found ? getDataProperty(walk.holder, walk.offset, walk.attributes, slot) : JSValue();
}

static JSValue getFromCompiledObject(Instance* instance, Object* object, UniquedStringImpl* uid, PropertySlot& slot, bool& shouldCache)
{
    JSValue value;
    if (const FieldData* field = object->fieldNamed(object->state(), uid)) {
        if (field->isAccessor())
            return { };
        if (field->slot.representation() != SlotRepresentation::DeferredClosure)
            value = object->readInline(*field);
    } else if (hasPendingStars(*object))
        return { };
    else if (JSObject* extra = object->extraProperties()) {
        Structure* extraPropertiesStructure = extra->structure();
        unsigned attributes;
        auto offset = extraPropertiesStructure->getWithoutMaterializing(uid, attributes);
        if (!offset)
            return { };
        if (!isValidOffset(*offset)) {
            if (!extraPropertiesStructure->isDictionary())
                return { };
            shouldCache = false;
            return getFromPrototypeChain(instance, object, uid, slot);
        }
        shouldCache &= !extraPropertiesStructure->isUncacheableDictionary();
        if (!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            value = extra->getDirect(*offset);
    }
    if (value)
        slot.setValue(object, 0, value);
    return value;
}

static JSValue getFromOrdinaryObject(Instance* instance, JSObject* base, UniquedStringImpl* uid, PropertySlot& slot)
{
    Structure* structure = base->structure();
    if (structure->typeInfo().overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(base->inlineTypeFlags()))
        return { };
    unsigned attributes;
    auto offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset)
        return { };
    return isValidOffset(*offset) ? getDataProperty(base, *offset, attributes, slot) : getFromPrototypeChain(instance, base, uid, slot);
}

static bool isHeldByCacheableDictionary(Structure* structure, UniquedStringImpl* uid)
{
    unsigned attributes;
    auto offset = structure->isUncacheableDictionary() ? std::nullopt : structure->getWithoutMaterializing(uid, attributes);
    return offset && isValidOffset(*offset);
}

static StructuralRead readAndCache(Instance* instance, JSObject* base, UniquedStringImpl* uid, bool& isDueForFiling)
{
    VM& vm = *instance->vm;
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structure = base->structure();
    const Slot* filedSlot = caches.findByKey(structure, uid, SlotAccess::Read);
    if (filedSlot) {
        if (StructuralRead read = readByFiledSlot(base, *filedSlot); read.value)
            return read;
    }
    bool shouldCache = !filedSlot && caches.shouldFile(structure->id(), uid, SlotAccess::Read);
    isDueForFiling = shouldCache;
    // Nothing of a cacheable dictionary moves when it is made flat, and no cell is allocated.
    if (shouldCache && structure->isDictionary() && !structure->isUncacheableDictionary())
        flattenOnce(vm, base);
    bool canFile = !structure->isDictionary() || isHeldByCacheableDictionary(structure, uid);
    // What a dictionary lacks is filed once it is no dictionary any more, and the caching operation makes it so once.
    if (shouldCache && !canFile && !structure->hasBeenFlattenedBefore() && base->type() == FinalObjectType)
        return { };
    shouldCache = shouldCache && canFile;
    Object* object = compiledObjectOrNull(base);
    bool isNamespace = base->type() == ObjectType && isModuleNamespace(base);
    bool isOrdinary = !object && !isNamespace;
    if (isOrdinary && !shouldCache)
        return readGeneric(vm, base, uid);

    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
    JSValue value;
    if (isOrdinary)
        value = getFromOrdinaryObject(instance, base, uid, slot);
    else if (object) {
        value = getFromCompiledObject(instance, object, uid, slot, shouldCache);
        if (!value && filedSlot)
            return readEmptyFieldByFiledSlot(*object, uid, *filedSlot);
    } else {
        value = readModuleNamespaceExport(instance, uncheckedDowncast<ModuleNamespace>(base), uid);
        if (value)
            slot.setValue(base, 0, value);
    }
    if (!value)
        return { };
    if (shouldCache) {
        ReadQuery query { instance->globalObject, base, structure, uid, slot, ChainWalkMode::AllocatesNothing };
        PropertyAnswer answer = isNamespace ? classifyNamespaceRead(query) : classifyRead(query);
        if (answer.isCacheable())
            caches.cache({ structure, { uid, std::nullopt }, SlotAccess::Read }, answer);
        else if (!structure->isDictionary())
            return { };
    }
    StructuralRead read { value, slot.isUnset() ? LeafPresence::Absent : presenceIn(slot.slotBase(), base) };
    if (isOrdinary && validatesRuntime(vm)) [[unlikely]]
        validateLookupResult(vm, base, uid, read.presence != LeafPresence::Absent, value);
    return read;
}

// A frozen array that has a name of its own, as the strings of a tagged template have, keeps its elements in a map.
static JSValue readSparseElement(JSObject* object, uint32_t index)
{
    if (!hasAnyArrayStorage(object->indexingType()) || (object->type() != ArrayType && object->type() != FinalObjectType))
        return { };
    SparseArrayValueMap* map = object->butterfly()->arrayStorage()->m_sparseMap.get();
    if (!map)
        return { };
    auto entry = map->find(index);
    if (entry == map->notFound() || entry->attributes() & PropertyAttribute::Accessor)
        return { };
    return entry->get();
}

static KeyName nameOfKeyForLeaf(Instance* instance, JSCell* key, KeyAtomization atomization)
{
    bool wasAtom = atomOfKey(key);
    StringImpl* textBefore = key->isString() ? asString(key)->tryGetValueImpl() : nullptr;
    KeyName name = nameOfKeyWithoutAllocating(instance->globalObject, key, atomization);
    // A text that became the name itself is the first of its characters. The stubs look for the name of a text that comes again.
    if (!wasAtom && key->isString() && name.kind == KeyNameKind::Name && name.uid != textBefore)
        instance->state->propertyCaches->tables().rememberNameOfText(name.uid);
    return name;
}

// An accessor has no value to read here, and neither has a field that holds nothing while the objects above have the name or lack it.
static LeafPresence findPresenceWithoutValue(Instance* instance, JSObject* base, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    Structure* structure = base->structure();
    if (isNeverCachedName(vm, uid) || isPreparedBeforeLookups(vm, uid))
        return LeafPresence::TakeSlowPath;
    JSObject* storage = base;
    if (Object* object = compiledObjectOrNull(base)) {
        if (hasPendingStars(*object))
            return LeafPresence::TakeSlowPath;
        if (const FieldData* field = object->fieldNamed(object->state(), uid)) {
            if (field->isPrivate() || field->flags & fieldMayExistWithoutStorage)
                return LeafPresence::TakeSlowPath;
            if (object->isInSlot(*field))
                return LeafPresence::Own;
        }
        storage = object->extraProperties();
    } else if (base->type() != FinalObjectType || structure->typeInfo().overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(base->inlineTypeFlags()))
        return LeafPresence::TakeSlowPath;
    if (storage) {
        unsigned attributes;
        auto offset = storage->structure()->getWithoutMaterializing(uid, attributes);
        if (!offset)
            return LeafPresence::TakeSlowPath;
        if (isValidOffset(*offset))
            return LeafPresence::Own;
    }
    switch (walkChain({ instance->globalObject, structure, base, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid).result) {
    case ChainWalkResult::Unknown:
        return LeafPresence::TakeSlowPath;
    case ChainWalkResult::Absent:
        return LeafPresence::Absent;
    case ChainWalkResult::Found:
        return LeafPresence::Inherited;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class LeafQuestion : bool { Value, Presence };

static StructuralRead readByKey(Instance* instance, JSCell* base, JSCell* key, LeafQuestion question)
{
    if (!base->isObject())
        return { };
    JSObject* object = asObject(base);
    KeyName name = nameOfKeyForLeaf(instance, key, KeyAtomization::OnlyExisting);
    switch (name.kind) {
    case KeyNameKind::NotAName:
        return { };
    case KeyNameKind::HasNoAtom:
        return hasOnlyOrdinaryLookupsOnChain(object) ? StructuralRead { jsUndefined(), LeafPresence::Absent } : StructuralRead { };
    case KeyNameKind::Index: {
        JSValue value = object->tryGetIndexQuickly(name.index);
        if (!value)
            value = readSparseElement(object, name.index);
        // Nothing above a pristine array has elements.
        if (!value && object->type() == ArrayType && !hasAnyArrayStorage(object->indexingType()) && object->structure()->hasPristineElements())
            return { jsUndefined(), LeafPresence::Absent };
        return { value, value ? LeafPresence::Own : LeafPresence::TakeSlowPath };
    }
    case KeyNameKind::Name: {
        // Nothing is cached for a function used as a key: the objects themselves are consulted.
        if (key->isObject()) {
            if (StructuralRead read = readGeneric(*instance->vm, object, name.uid); read.presence != LeafPresence::TakeSlowPath)
                return read;
        }
        bool isDueForFiling = false;
        StructuralRead read = readAndCache(instance, object, name.uid, isDueForFiling);
        // What is due is filed by the operation that the stub calls next, and from then on the stub answers.
        if (question == LeafQuestion::Presence && read.presence == LeafPresence::TakeSlowPath && !isDueForFiling)
            read.presence = findPresenceWithoutValue(instance, object, name.uid);
        return read;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetByValGeneric, EncodedJSValue, (Instance* instance, JSCell* base, JSCell* key))
{
    LeafOperationScope leaf(*instance->vm);
    return JSValue::encode(readByKey(instance, base, key, LeafQuestion::Value).value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTInByValGeneric, size_t, (Instance* instance, JSCell* base, JSCell* key))
{
    LeafOperationScope leaf(*instance->vm);
    return static_cast<size_t>(readByKey(instance, base, key, LeafQuestion::Presence).presence);
}

static bool hasCompiledStructureAtHand(VM& vm)
{
    Allocator allocator = vm.compiledStructureSpace<SubspaceAccess::OnMainThread>()->allocatorFor(sizeof(CompiledStructure), AllocatorForMode::AllocatorIfExists);
    return allocator && !std::bit_cast<FreeList*>(std::bit_cast<char*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList())->allocationWillFail();
}

static LeafAnswer createFieldOfCompiledObject(Instance* instance, Object& object, const FieldData& field, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = *instance->vm;
    const CompiledStructure& structure = object.compiledStructure();
    if (vm.aotState().validatesRuntime || uid->isSymbol() || structure.mayBePrototype() || !structure.flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return LeafAnswer::TakeSlowPath;
    if (vm.aotState().hasSeveralImages && belongsToOtherImageSlow(vm, &object)) [[unlikely]]
        return LeafAnswer::TakeSlowPath;
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (!hasCompiledStructureAtHand(vm) || !canCreateFieldInSlot(vm, object, field) || !caches.tables().addFilterAndCheckStoreCreatesOwnField(instance->globalObject, object, field.name))
        return LeafAnswer::TakeSlowPath;
    if (caches.shouldFile(structure.id(), uid, SlotAccess::Store))
        return LeafAnswer::TakeSlowPath;
    return tryCreateField(vm, object.state(), object, field, value) ? LeafAnswer::Yes : LeafAnswer::TakeSlowPath;
}

static LeafAnswer putToCompiledObject(Instance* instance, Object* object, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = *instance->vm;
    // What is filed below says where a field lies and what it takes in the numbers of the object's image, and the stubs of this image would read it in their own.
    // The caching operation stores under the object's image and knows what may be filed about such an object.
    if (instance->state->sharesRealm && !object->isDescribedBy(*instance->state)) [[unlikely]]
        return LeafAnswer::TakeSlowPath;
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structure = object->structure();
    // Every field has a name that the program spells.
    bool isSpelledByProgram = identifiersOf(*instance).numberOf(uid).has_value();
    const FieldData* field = isSpelledByProgram ? object->fieldNamed(object->state(), uid) : nullptr;
    if (!field) {
        using Flag = Object::ExtraPropertyStoreFlag;
        JSObject* extra = object->extraProperties();
        bool canHaveSlot = true;
        bool isKnownToBeAbsent = false;
        if (Structure* extraStructure = extra ? extra->structure() : nullptr; extraStructure && extraStructure->isDictionary()) {
            unsigned attributes;
            auto offset = extraStructure->isUncacheableDictionary() ? std::nullopt : extraStructure->getWithoutMaterializing(uid, attributes);
            canHaveSlot = offset && isValidOffset(*offset);
            isKnownToBeAbsent = offset && !isValidOffset(*offset);
        }
        if (canHaveSlot && !caches.findByKey(structure, uid, SlotAccess::Store))
            return LeafAnswer::TakeSlowPath;
        OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
        if (isKnownToBeAbsent)
            flags.add(Flag::IsKnownToBeAbsent);
        // The caller has told indexes and "__proto__" from names, and takes no private name.
        if (!isSpelledByProgram)
            flags.add(Flag::IsMadeAtRunTime);
        auto stored = object->tryStoreExtraProperty(vm, uid, value, flags);
        return stored.kind == Object::ExtraPropertyStore::NotHandled ? LeafAnswer::TakeSlowPath : LeafAnswer::Yes;
    }
    if (field->flags & (fieldIsReadOnly | fieldIsAccessor) || field->slot.representation() == SlotRepresentation::DeferredClosure || value.isObject() || object->hasFlagsInOrderWord())
        return LeafAnswer::TakeSlowPath;
    if (!object->readInline(*field))
        return createFieldOfCompiledObject(instance, *object, *field, uid, value);
    if (!object->tryStore(vm, *field, value))
        return LeafAnswer::TakeSlowPath;
    if (!caches.findByKey(structure, uid, SlotAccess::Store) && caches.shouldFile(structure->id(), uid, SlotAccess::Store)) {
        PutPropertySlot slot(object, true);
        caches.cache({ structure, { uid, std::nullopt }, SlotAccess::Store }, classifyCompiledObjectStore({ instance->globalObject, object, structure, uid, slot, PutMode::Strict }, *object));
    }
    return LeafAnswer::Yes;
}

static void cacheStore(Instance* instance, JSObject* object, Structure* structureBefore, UniquedStringImpl* uid, const PutPropertySlot& slot)
{
    if (object->structure() != structureBefore && (structureBefore->isDictionary() || object->structure()->isDictionary()))
        return;
    if (structureBefore->isUncacheableDictionary() || (structureBefore->isDictionary() && (slot.type() != PutPropertySlot::ExistingProperty || isNeverCachedName(*instance->vm, uid))))
        return;
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (!caches.findByKey(structureBefore, uid, SlotAccess::Store))
        caches.cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Store }, classifyStore({ instance->globalObject, object, structureBefore, uid, slot, PutMode::Strict }));
}

bool hasStructureAtHand(VM& vm)
{
    Allocator allocator = vm.structureSpace().allocatorFor(sizeof(Structure), AllocatorForMode::AllocatorIfExists);
    return !std::bit_cast<FreeList*>(std::bit_cast<char*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList())->allocationWillFail();
}

static LeafAnswer putToFinalObject(Instance* instance, JSObject* object, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = *instance->vm;
    Structure* structure = object->structure();
    if (structure->mayHaveImmutableProperties() || !object->canPerformFastPutInlineExcludingProto())
        return LeafAnswer::TakeSlowPath;
    PutPropertySlot slot(object, true);
    bool canOnlyReplace = structure->mayBePrototype();
    if (Structure* next = canOnlyReplace ? nullptr : structure->trySingleTransition(); next && next->transitionPropertyName() == uid && next->transitionKind() == TransitionKind::PropertyAddition && !next->transitionPropertyAttributes() && next->outOfLineCapacity() == structure->outOfLineCapacity()) {
        object->putDirectOffset(vm, next->transitionOffset(), value);
        object->setStructure(vm, next);
        slot.setNewProperty(object, next->transitionOffset());
        cacheStore(instance, object, structure, uid, slot);
        return LeafAnswer::Yes;
    }
    unsigned attributes;
    auto offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset)
        return LeafAnswer::TakeSlowPath;
    if (isValidOffset(*offset)) {
        if (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isWatchingReplacement())
            return LeafAnswer::TakeSlowPath;
        object->putDirectOffset(vm, *offset, value);
        slot.setExistingProperty(object, *offset);
        if (instance->state->propertyCaches->shouldFile(structure->id(), uid, SlotAccess::Store))
            cacheStore(instance, object, structure, uid, slot);
        return LeafAnswer::Yes;
    }
    if (canOnlyReplace || !structure->isStructureExtensible())
        return LeafAnswer::TakeSlowPath;
    bool repeatsAddition = repeatsPropertyAddition(structure, uid);
    if (!repeatsAddition && !hasStructureAtHand(vm))
        return LeafAnswer::TakeSlowPath;
    // An object that has many names, or was filled again and again, and is given more names by key grows into a dictionary.
    if (!repeatsAddition) {
        FirstAdditions& additions = firstAdditionsOf(*instance->state);
        if ((structure->hasBeenDictionary() || !hasFewEnoughNamesToBeFilled(structure) || !additions.mayBeginToFill(object)) && !additions.shouldMakeStructure(structure, uid))
            object->convertToDictionary(vm);
    }
    object->putDirect(vm, uid, value, slot);
    if (repeatsAddition)
        cacheStore(instance, object, structure, uid, slot);
    return LeafAnswer::Yes;
}

// A property of the global object that a store through globalThis has filed: filing it fired what watched its replacement, as for any store that a stub makes.
static LeafAnswer putToGlobalObjectThroughItsProxy(Instance* instance, JSObject* proxy, JSCell* key, UniquedStringImpl* uid, JSValue value)
{
    JSGlobalObject* target = instance->globalObject;
    if (proxy != target->globalThis())
        return LeafAnswer::TakeSlowPath;
    Structure* structure = target->structure();
    const Slot* filed = instance->state->propertyCaches->findByKey(structure, uid, SlotAccess::Store);
    if (!filed || !filed->has(Slot::isOwn) || (filed->kind != Slot::Kind::Value && filed->kind != Slot::Kind::FarValue))
        return LeafAnswer::TakeSlowPath;
    PropertyOffset offset = filed->offsetInStorage();
    target->putDirectOffset(*instance->vm, offset, value);
    if (proxy->classInfo() == JSGlobalProxy::info() && uncheckedDowncast<JSGlobalProxy>(proxy)->target() == target) {
        auto placesForStores = instance->keyedValuePlaces().last<KeyedValuePlace::numberOfPlacesPerAccess>();
        placesForStores[KeyedValuePlace::offsetFor(key, proxy->structureID()) >> KeyedValuePlace::sizeShift] = { key, structure->id().bits() + KeyedValuePlace::globalObjectBit, { Slot::wordIndexFor(offset) } };
    }
    return LeafAnswer::Yes;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPutByValGeneric, size_t, (Instance* instance, JSCell* base, JSCell* key, EncodedJSValue encodedValue))
{
    LeafOperationScope leaf(*instance->vm);
    JSValue value = JSValue::decode(encodedValue);
    LeafAnswer answer = LeafAnswer::TakeSlowPath;
    KeyName name = base->isObject() ? nameOfKeyForLeaf(instance, key, KeyAtomization::Atomize) : KeyName { };
    if (name.kind == KeyNameKind::Index && asObject(base)->trySetIndexQuickly(*instance->vm, name.index, value))
        answer = LeafAnswer::Yes;
    else if (name.kind == KeyNameKind::Name && base->type() == FinalObjectType)
        answer = putToFinalObject(instance, asObject(base), name.uid, value);
    else if (name.kind == KeyNameKind::Name && base->type() == GlobalProxyType)
        answer = putToGlobalObjectThroughItsProxy(instance, asObject(base), key, name.uid, value);
    else if (Object* object = name.kind == KeyNameKind::Name ? compiledObjectOrNull(base) : nullptr)
        answer = putToCompiledObject(instance, object, name.uid, value);
    return static_cast<size_t>(answer);
}

// A program function used as a key converts to its source text: it has no own properties, nothing on its prototype chain has Symbol.toPrimitive, and the toString it finds is the built-in one.
// The Structure caches both from the first conversion for as long as they hold, and the string exists from then on.
static JSString* functionSourceForKey(VM& vm, JSCell* key)
{
    if (key->type() != SlimClosureType)
        return nullptr;
    Structure* structure = key->structure();
    JSValue toPrimitive = structure->cachedSpecialProperty(CachedSpecialPropertyKey::ToPrimitive);
    JSValue toString = structure->cachedSpecialProperty(CachedSpecialPropertyKey::ToString);
    if (!toPrimitive || !toPrimitive.isUndefined() || !toString)
        return nullptr;
    auto* function = dynamicDowncast<JSFunction>(toString);
    if (!function || !function->isHostFunction() || function->nativeFunction() != toTagged(NativeFunction(functionProtoFuncToString)))
        return nullptr;
    return CompiledCallee::sourceStringIfCreated(vm, asObject(key));
}

// Only a string that starts with a digit can be an index.
static bool mayBeginWithDigit(const JSString* string)
{
    JSRopeString::LeafIterator pieces(string);
    while (!pieces.isAtEnd()) {
        StringView piece = pieces.next();
        if (!piece.isEmpty())
            return isASCIIDigit(piece[0]);
    }
    return true;
}

KeyName nameOfKeyWithoutAllocating(JSGlobalObject* globalObject, JSCell* key, KeyAtomization atomization)
{
    if (key->isSymbol()) {
        SymbolImpl& uid = asSymbol(key)->uid();
        return uid.isPrivate() || isPreparedBeforeLookups(globalObject->vm(), &uid) ? KeyName { } : KeyName { KeyNameKind::Name, &uid };
    }
    if (!key->isString()) {
        key = functionSourceForKey(globalObject->vm(), key);
        // The first conversion atomized the string.
        if (!key || !atomOfKey(key))
            return { };
    }
    constexpr unsigned maxIndexLength = 10;
    JSString* string = asString(key);
    StringImpl* text = string->tryGetValueImpl();
    bool isRope = !text;
    if (isRope || !text->isAtom()) {
        if (string->length() > JSString::maxLengthForOnStackResolve)
            return { };
        bool isIndex = text && parseIndex(*text);
        if (!isIndex) {
            bool atomizes = atomization == KeyAtomization::Atomize || (text && Image::isBackedByStaticData(*text));
            text = atomizes ? string->toAtomString(globalObject).data : string->toExistingAtomString(globalObject).data;
        }
        if (!text)
            return { isRope && string->length() <= maxIndexLength && mayBeginWithDigit(string) ? KeyNameKind::NotAName : KeyNameKind::HasNoAtom };
    }
    if (std::optional<uint32_t> index = parseIndex(*text))
        return { KeyNameKind::Index, nullptr, *index };
    if (text == globalObject->vm().propertyNames->underscoreProto.impl())
        return { };
    return { KeyNameKind::Name, static_cast<UniquedStringImpl*>(text) };
}

std::optional<uint32_t> indexOfKey(JSValue key)
{
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index())
        return index;
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text ? parseIndex(*text) : std::nullopt;
}

std::optional<Identifier> nameForKeyedCache(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isSymbol()) {
        if (isPreparedBeforeLookups(globalObject->vm(), &asSymbol(key)->uid()))
            return std::nullopt;
        return Identifier::fromUid(asSymbol(key)->privateName());
    }
    if (!key.isString())
        return std::nullopt;
    auto existingAtomString = asString(key)->toExistingAtomString(globalObject);
    if (!existingAtomString || parseIndex(*existingAtomString.data))
        return std::nullopt;
    return Identifier::fromUid(globalObject->vm(), existingAtomString.data);
}

UniquedStringImpl* atomOfKey(JSValue key)
{
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text && text->isAtom() ? static_cast<UniquedStringImpl*>(text) : nullptr;
}

UniquedStringImpl* uidOfKey(JSCell* key)
{
    return key->isSymbol() ? &asSymbol(key)->uid() : atomOfKey(key);
}

// Nothing is made: neither an atom nor, of a rope, a flat string. A flat string takes the text of the atom in place of its own, so the next access under it compares nothing.
static std::optional<RefPtr<AtomStringImpl>> findAtomEqualTo(JSGlobalObject* globalObject, JSString* string)
{
    if (string->tryGetValueImpl())
        return RefPtr<AtomStringImpl>(string->toExistingAtomString(globalObject).data);
    auto lookUp = [&]<typename Character>(Vector<Character>&& characters) -> std::optional<RefPtr<AtomStringImpl>> {
        if (!characters.tryGrow(string->length()))
            return std::nullopt;
        string->resolveToBuffer(characters.mutableSpan());
        return AtomStringImpl::lookUp(characters.span());
    };
    return string->is8Bit() ? lookUp(Vector<Latin1Character>()) : lookUp(Vector<char16_t>());
}

static std::optional<bool> isEqualToName(JSString* string, const UniquedStringImpl& name)
{
    auto isEqual = [&]<typename Character>(std::span<const Character> characters) {
        if (name.is8Bit() != (sizeof(Character) == 1))
            return StringView(name) == StringView(characters);
        if constexpr (sizeof(Character) == 1)
            return equalSpans(name.span8(), characters);
        else
            return equalSpans(name.span16(), characters);
    };
    if (StringImpl* text = string->tryGetValueImpl())
        return text->is8Bit() ? isEqual(text->span8()) : isEqual(text->span16());
    auto resolveAndCompare = [&]<typename Character>(Vector<Character>&& characters) -> std::optional<bool> {
        if (!characters.tryGrow(string->length()))
            return std::nullopt;
        string->resolveToBuffer(characters.mutableSpan());
        return isEqual(std::span<const Character>(characters.span()));
    };
    return string->is8Bit() ? resolveAndCompare(Vector<Latin1Character>()) : resolveAndCompare(Vector<char16_t>());
}

static bool mayHaveFieldWithNameOfLength(const Object& object, unsigned length)
{
    if (!object.state().tables.removedFields().empty())
        return true;
    const ImageText& text = object.instance().program->text();
    return std::ranges::any_of(object.layout().fields(), [&](const FieldData& field) {
        StringID name = text.identifier(field.name).text();
        return name && text.view(name).length() == length;
    });
}

static bool hasOrdinaryLookups(JSObject* object)
{
    Structure* structure = object->structure();
    return !TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && !TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) && !structure->typeInfo().overridesGetPrototype() && !structure->isCompiledStructure();
}

// A key this long is not made an atom to be looked up. Every name of an ordinary object is an atom already, and the table of atoms says which one equals the key, if any.
std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes)
{
    if (!key.isString() || asString(key)->length() <= JSString::maxLengthForOnStackResolve || !base.isObject() || atomOfKey(key))
        return std::nullopt;
    JSString* string = asString(key);
    VM& vm = string->vm();
    JSGlobalObject* globalObject = asObject(base)->globalObject();
    unsigned length = string->length();
    // Visiting a name costs about as much as hashing sixteen units of the key.
    uint64_t remainingNames = length / 16;
    UniquedStringImpl* nameWithSameLength = nullptr;
    std::optional<RefPtr<AtomStringImpl>> atom;
    for (JSObject* object = asObject(base); object; object = checksPrototypes ? object->getPrototypeDirect().getObject() : nullptr) {
        JSObject* holder = object;
        if (Object* compiled = compiledObjectOrNull(object)) {
            if (mayHaveFieldWithNameOfLength(*compiled, length) || hasPendingStars(*compiled))
                return std::nullopt;
            holder = compiled->extraProperties();
            if (!holder)
                continue;
        }
        if (!hasOrdinaryLookups(holder))
            return std::nullopt;
        Structure* structure = holder->structure();
        if (!atom) {
            unsigned numberOfNames = structure->isDictionary() ? structure->dictionaryPropertyCount() : numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
            bool needsHash = numberOfNames > remainingNames;
            if (!needsHash) {
                remainingNames -= numberOfNames;
                structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
                    UniquedStringImpl* name = entry.key();
                    if (name->length() != length || name->isSymbol() || name == nameWithSameLength)
                        return true;
                    needsHash = !!nameWithSameLength;
                    if (!needsHash)
                        nameWithSameLength = name;
                    return !needsHash;
                });
            }
            if (!needsHash)
                continue;
            atom = findAtomEqualTo(globalObject, string);
            if (!atom)
                return std::nullopt;
            // The holders visited so far have no other name of this length.
            if (*atom && atom->get() == nameWithSameLength)
                return nameWithSameLength;
        }
        // The holder keeps the atom alive.
        if (*atom && isValidOffset(structure->get(vm, PropertyName(atom->get()))))
            return atom->get();
    }
    if (atom || !nameWithSameLength)
        return nullptr;
    std::optional<bool> isEqual = isEqualToName(string, *nameWithSameLength);
    if (!isEqual)
        return std::nullopt;
    if (!*isEqual)
        return nullptr;
    // Only compared. A flat key that comes back takes the text of the atom, so that later accesses compare nothing.
    static thread_local const JSString* lastKeyCompared;
    if (std::exchange(lastKeyCompared, string) == string && string->tryGetValueImpl())
        string->toExistingAtomString(globalObject);
    return nameWithSameLength;
}

// A compiled object's fields have names that appear in the program, which are atoms wherever they are looked up, and its extra properties are in an ordinary object.
static bool findsOnlyAtomNames(JSObject* object)
{
    if (Object* compiled = compiledObjectOrNull(object))
        return !hasPendingStars(*compiled) && !compiled->structure()->typeInfo().overridesGetPrototype();
    return hasOrdinaryLookups(object);
}

bool hasOnlyOrdinaryLookupsOnChain(JSObject* base)
{
    for (JSObject* object = base; object; object = object->getPrototypeDirect().getObject()) {
        if (!findsOnlyAtomNames(object))
            return false;
    }
    return true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
