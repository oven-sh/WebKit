/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeTable.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "ArrayStorage.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
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
    DeferGCForAWhile deferGC(vm);
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
    if (Instance::of(namespaceObject->structure()->realm()) != instance)
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

static StructuralRead readAndCache(Instance* instance, JSObject* base, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structure = base->structure();
    const Slot* filedSlot = caches.findByKey(structure, uid, SlotAccess::Read);
    if (filedSlot) {
        if (StructuralRead read = readByFiledSlot(base, *filedSlot); read.value)
            return read;
    }
    bool canFile = !structure->isDictionary() || isHeldByCacheableDictionary(structure, uid);
    bool shouldCache = !filedSlot && caches.shouldFile(structure->id(), uid, SlotAccess::Read);
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
    KeyName name = nameOfKeyWithoutAllocating(instance->globalObject, key, atomization);
    if (!wasAtom && name.kind == KeyNameKind::Name)
        instance->state->propertyCaches->tables().rememberNameOfText(name.uid);
    return name;
}

static StructuralRead readByKey(Instance* instance, JSCell* base, JSCell* key)
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
    case KeyNameKind::Name:
        return readAndCache(instance, object, name.uid);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetByValGeneric, EncodedJSValue, (Instance* instance, JSCell* base, JSCell* key))
{
    LeafOperationScope leaf(*instance->vm);
    return JSValue::encode(readByKey(instance, base, key).value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTInByValGeneric, size_t, (Instance* instance, JSCell* base, JSCell* key))
{
    LeafOperationScope leaf(*instance->vm);
    return static_cast<size_t>(readByKey(instance, base, key).presence);
}

static LeafAnswer putToCompiledObject(Instance* instance, Object* object, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = *instance->vm;
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structure = object->structure();
    bool isCached = caches.findByKey(structure, uid, SlotAccess::Store);
    const FieldData* field = object->fieldNamed(object->state(), uid);
    if (!field) {
        JSObject* extra = object->extraProperties();
        bool canHaveSlot = !extra || !extra->structure()->isDictionary() || isHeldByCacheableDictionary(extra->structure(), uid);
        if (canHaveSlot && !isCached)
            return LeafAnswer::TakeSlowPath;
        auto stored = object->tryStoreExtraProperty(vm, uid, value, { Object::ExtraPropertyStoreFlag::IsKnownNotToBeField });
        return stored.kind == Object::ExtraPropertyStore::NotHandled ? LeafAnswer::TakeSlowPath : LeafAnswer::Yes;
    }
    if (field->flags & (fieldIsReadOnly | fieldIsAccessor) || field->slot.representation() == SlotRepresentation::DeferredClosure || value.isObject() || object->hasFlagsInOrderWord() || !object->readInline(*field) || !object->tryStore(vm, *field, value))
        return LeafAnswer::TakeSlowPath;
    if (!isCached && caches.shouldFile(structure->id(), uid, SlotAccess::Store)) {
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
    bool makesStructure = !repeatsAddition && instance->state->propertyCaches->shouldMakeStructureForAdditionByKey(structure, uid);
    if (!repeatsAddition && !makesStructure)
        object->convertToDictionary(vm);
    object->putDirect(vm, uid, value, slot);
    if (repeatsAddition)
        cacheStore(instance, object, structure, uid, slot);
    else if (makesStructure)
        instance->state->propertyCaches->didMakeStructureForAdditionByKey(object->structure());
    return LeafAnswer::Yes;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPutByValGeneric, size_t, (Instance* instance, JSCell* base, JSCell* key, EncodedJSValue encodedValue))
{
    LeafOperationScope leaf(*instance->vm);
    DeferGCForAWhile deferGC(*instance->vm);
    JSValue value = JSValue::decode(encodedValue);
    LeafAnswer answer = LeafAnswer::TakeSlowPath;
    KeyName name = base->isObject() ? nameOfKeyForLeaf(instance, key, KeyAtomization::Atomize) : KeyName { };
    if (name.kind == KeyNameKind::Index && asObject(base)->trySetIndexQuickly(*instance->vm, name.index, value))
        answer = LeafAnswer::Yes;
    else if (name.kind == KeyNameKind::Name && base->type() == FinalObjectType)
        answer = putToFinalObject(instance, asObject(base), name.uid, value);
    else if (Object* object = name.kind == KeyNameKind::Name ? compiledObjectOrNull(base) : nullptr)
        answer = putToCompiledObject(instance, object, name.uid, value);
    return static_cast<size_t>(answer);
}

KeyName nameOfKeyWithoutAllocating(JSGlobalObject* globalObject, JSCell* key, KeyAtomization atomization)
{
    if (key->isSymbol()) {
        SymbolImpl& uid = asSymbol(key)->uid();
        return uid.isPrivate() || isPreparedBeforeLookups(globalObject->vm(), &uid) ? KeyName { } : KeyName { KeyNameKind::Name, &uid };
    }
    if (!key->isString())
        return { };
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
            return { isRope && string->length() <= maxIndexLength ? KeyNameKind::NotAName : KeyNameKind::HasNoAtom };
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

struct StringComparison {
    StringView name;
    uint64_t remainingPieces;
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

static bool stringEqualsName(JSString* string, StringView name, uint64_t maxPieces, bool& hasTooManyPieces)
{
    StringComparison comparison { name, maxPieces };
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

static bool mayHaveFieldWithNameOfLength(const Object& object, unsigned length)
{
    if (!object.state().tables.removedFields().empty())
        return true;
    const ImageText& text = Image::current()->text();
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

std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes)
{
    if (!key.isString() || asString(key)->length() <= JSString::maxLengthForOnStackResolve || !base.isObject() || atomOfKey(key))
        return std::nullopt;
    JSString* string = asString(key);
    VM& vm = string->vm();
    unsigned length = string->length();
    uint64_t maxPieces = length / std::max<uint64_t>(runtimeLimitValue(vm, LimitID::MinLongKeyUnitsPerPiece), 1) + runtimeLimitValue(vm, LimitID::LongKeyExtraPieces);
    uint64_t remainingNames = std::max<uint64_t>(length / 4, runtimeLimitValue(vm, LimitID::MaxNamesComparedWithLongKey));
    for (JSObject* object = asObject(base); object; object = checksPrototypes ? object->getPrototypeDirect().getObject() : nullptr) {
        JSObject* holder = object;
        if (Object* compiled = compiledObjectOrNull(object)) {
            if (mayHaveFieldWithNameOfLength(*compiled, length))
                return std::nullopt;
            holder = compiled->extraProperties();
            if (!holder)
                continue;
        }
        if (!hasOrdinaryLookups(holder))
            return std::nullopt;
        Structure* structure = holder->structure();
        unsigned numberOfNames = structure->isDictionary() ? structure->dictionaryPropertyCount() : numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
        if (numberOfNames > remainingNames) {
            countLimitHit(vm, LimitID::MaxNamesComparedWithLongKey);
            return std::nullopt;
        }
        remainingNames -= numberOfNames;
        UniquedStringImpl* equalName = nullptr;
        bool hasTooManyPieces = false;
        structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
            UniquedStringImpl* name = entry.key();
            if (name->length() == length && !name->isSymbol() && stringEqualsName(string, name, maxPieces, hasTooManyPieces))
                equalName = name;
            return !equalName && !hasTooManyPieces;
        });
        if (hasTooManyPieces) {
            countLimitHit(vm, LimitID::MinLongKeyUnitsPerPiece);
            return std::nullopt;
        }
        if (equalName)
            return equalName;
    }
    return nullptr;
}

bool hasOnlyOrdinaryLookupsOnChain(JSObject* base)
{
    for (JSObject* object = base; object; object = object->getPrototypeDirect().getObject()) {
        if (!hasOrdinaryLookups(object))
            return false;
    }
    return true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
