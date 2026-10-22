/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlowPaths.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTChainDependents.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "JSGlobalProxy.h"

namespace JSC { namespace AOT {

struct StructuralRead {
    JSValue value;
    LeafPresence presence { LeafPresence::TakeSlowPath };
};

using ReadClassifier = PropertyAnswer (*)(const ReadQuery&);

static ALWAYS_INLINE LeafPresence presenceIn(JSObject* holder, JSObject* receiver)
{
    return holder == receiver ? LeafPresence::Own : LeafPresence::Inherited;
}

template<typename Found>
static ALWAYS_INLINE auto lookUpGeneric(JSObject* object, UniquedStringImpl* uid, const Found& found) -> decltype(found(object, object, 0, 0u))
{
    if (object->type() == GlobalProxyType && uid->isSymbol() && object->classInfo() == JSGlobalProxy::info())
        object = uncheckedDowncast<JSGlobalProxy>(object)->target();
    JSObject* receiver = object;
    while (true) {
        Structure* structure = object->structure();
        TypeInfo info = structure->typeInfo();
        bool hasSymbolsOnlyInStructure = uid->isSymbol() && object->type() == GlobalObjectType && object->methodTable()->getOwnPropertySlot == JSGlobalObject::getOwnPropertySlot;
        if (!hasSymbolsOnlyInStructure && (info.overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags())))
            return found(receiver, nullptr, invalidOffset, 0u);
        if (info.overridesGetPrototype() || structure->isCompiledStructure())
            return found(receiver, nullptr, invalidOffset, 0u);
        unsigned attributes;
        auto offset = structure->getWithoutMaterializing(uid, attributes);
        if (!offset)
            return found(receiver, nullptr, invalidOffset, 0u);
        if (isValidOffset(*offset))
            return found(receiver, object, *offset, attributes);
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return found(receiver, object, invalidOffset, 0u);
        object = asObject(prototype);
    }
}

static NEVER_INLINE void validateLookupResult(VM& vm, JSObject* object, UniquedStringImpl* uid, bool isPresent, JSValue value)
{
    DeferGCForAWhile deferGC(vm);
    JSGlobalObject* globalObject = object->realm();
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    auto* proxy = dynamicDowncast<JSGlobalProxy>(object);
    JSObject* first = proxy ? proxy->target() : object;
    bool has = first->getPropertySlot(globalObject, uid, slot);
    if (has == isPresent && (!has || !value || (slot.isValue() && slot.getValue(globalObject, uid) == value)))
        return;
    crashOnValidationFailure(vm, { ValidationFailureKind::FastPathResultDiffers, { }, { }, object });
}

static StructuralRead readGeneric(VM& vm, JSObject* base, UniquedStringImpl* uid)
{
    StructuralRead read = lookUpGeneric(base, uid, [](JSObject* receiver, JSObject* holder, PropertyOffset offset, unsigned attributes) -> StructuralRead {
        if (!holder)
            return { };
        if (!isValidOffset(offset))
            return { jsUndefined(), LeafPresence::Absent };
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return { { }, presenceIn(holder, receiver) };
        return { holder->getDirect(offset), presenceIn(holder, receiver) };
    });
    if (read.presence != LeafPresence::TakeSlowPath && validatesRuntime(vm)) [[unlikely]]
        validateLookupResult(vm, base, uid, read.presence != LeafPresence::Absent, read.value);
    return read;
}

// A read of a field that no slot answers: the key order may list the field where its slot is empty, else the objects above answer as they are at this moment.
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
    switch (walk.result) {
    case ChainWalkResult::Unknown:
        return { };
    case ChainWalkResult::Absent:
        return jsUndefined();
    case ChainWalkResult::Found:
        if (walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || !isValidOffset(walk.offset))
            return { };
        return walk.holder->getDirect(walk.offset);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetGeneric, EncodedJSValue, (Instance* instance, JSCell* base, UniquedStringImpl* uid))
{
    LeafOperationScope leaf(*instance->vm);
    if (Object* object = compiledObjectOrNull(base))
        return JSValue::encode(readFieldGeneric(instance, object, uid));
    return JSValue::encode(readGeneric(*instance->vm, asObject(base), uid).value);
}

static bool isCachedByKey(PropertyCaches& caches, Structure* structure, UniquedStringImpl* uid, SlotAccess access)
{
    return caches.findByKey(structure, uid, access);
}

// A stub finds no slot under a key whose name it cannot tell: a string that is no atom. What is filed under the name answers here.
static StructuralRead readByFiledSlot(const JSObject* receiver, const Slot& slot)
{
    switch (slot.kind) {
    case Slot::Kind::Absent:
        return { jsUndefined(), LeafPresence::Absent };
    case Slot::Kind::Value:
        if (slot.has(Slot::isOwn))
            return { receiver->getDirect(slot.offsetInStorage()), LeafPresence::Own };
        // The own properties of a global proxy are those of its target, which a slot names as their holder.
        return { asObject(slot.holder)->getDirect(slot.offsetInStorage()), receiver->type() == GlobalProxyType ? LeafPresence::TakeSlowPath : LeafPresence::Inherited };
    default:
        return { };
    }
}

// What the stubs answer where the slot of the field is empty. A field that may exist without storage reads as undefined there, and whether it exists is left to the key order.
static StructuralRead readEmptyFieldByFiledSlot(const Object& object, UniquedStringImpl* uid, const Slot& slot)
{
    if (slot.kind != Slot::Kind::Field)
        return { };
    const FieldData* field = object.fieldNamed(object.state(), uid);
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
    JSValue value = holder->getDirect(offset);
    if (value)
        slot.setValue(holder, attributes, value, offset);
    return value;
}

static JSValue getFromPrototypeChain(Instance* instance, JSObject* base, UniquedStringImpl* uid, PropertySlot& slot)
{
    ChainWalk walk = walkChain({ instance->globalObject, base->structure(), base, ChainWalkMode::AllocatesNothing }, uid);
    switch (walk.result) {
    case ChainWalkResult::Unknown:
        return { };
    case ChainWalkResult::Absent:
        return jsUndefined();
    case ChainWalkResult::Found:
        return getDataProperty(walk.holder, walk.offset, walk.attributes, slot);
    }
    RELEASE_ASSERT_NOT_REACHED();
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
            // The Structure of any other object says that the name is absent, and the caching operation files that.
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
    if (!isValidOffset(*offset))
        return getFromPrototypeChain(instance, base, uid, slot);
    return getDataProperty(base, *offset, attributes, slot);
}

static bool tryCacheRead(Instance* instance, const ReadQuery& query, ReadClassifier classify)
{
    PropertyAnswer answer = classify(query);
    if (!answer.isCacheable())
        return false;
    instance->state->propertyCaches->cache({ query.structureBefore, { query.uid, std::nullopt }, SlotAccess::Read }, answer);
    return true;
}

static bool isHeldByCacheableDictionary(Structure* structure, UniquedStringImpl* uid)
{
    if (structure->isUncacheableDictionary())
        return false;
    unsigned attributes;
    auto offset = structure->getWithoutMaterializing(uid, attributes);
    return offset && isValidOffset(*offset);
}

static StructuralRead readAndCache(Instance* instance, JSObject* base, UniquedStringImpl* uid)
{
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    Structure* structure = base->structure();
    const Slot* filedSlot = instance->state->propertyCaches->findByKey(structure, uid, SlotAccess::Read);
    if (filedSlot) {
        if (StructuralRead read = readByFiledSlot(base, *filedSlot); read.value)
            return read;
    }
    bool shouldCache = (structure->isDictionary() ? isHeldByCacheableDictionary(structure, uid) : true) && !filedSlot;
    shouldCache = shouldCache && instance->state->propertyCaches->shouldFile(structure->id(), uid, SlotAccess::Read);
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
    // Under AllocatesNothing, Uncacheable may only mean that classifying has to allocate, so the caching operation gets to see this miss.
    if (shouldCache && !tryCacheRead(instance, { instance->globalObject, base, structure, uid, slot, ChainWalkMode::AllocatesNothing }, isNamespace ? classifyNamespaceRead : classifyRead) && !structure->isDictionary())
        return { };
    StructuralRead read { value, slot.isUnset() ? LeafPresence::Absent : presenceIn(slot.slotBase(), base) };
    if (isOrdinary && validatesRuntime(vm)) [[unlikely]]
        validateLookupResult(vm, base, uid, read.presence != LeafPresence::Absent, value);
    return read;
}

static StructuralRead readElement(JSObject* object, uint32_t index)
{
    JSValue value = object->tryGetIndexQuickly(index);
    if (!value)
        return { };
    return { value, LeafPresence::Own };
}

// A stub has left because it could not tell the name of the key. The next key with the same text it can.
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
        if (classifyChainLookups(object) != ChainLookups::Ordinary)
            return { };
        return { jsUndefined(), LeafPresence::Absent };
    case KeyNameKind::Index:
        return readElement(object, name.index);
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
    bool isCached = isCachedByKey(caches, structure, uid, SlotAccess::Store);
    const FieldData* field = object->fieldNamed(object->state(), uid);
    if (!field) {
        JSObject* extra = object->extraProperties();
        bool canHaveSlot = !extra || !extra->structure()->isDictionary() || isHeldByCacheableDictionary(extra->structure(), uid);
        // Only the caching operation gives the object a Structure that names its extra properties, which a slot for them needs.
        if (canHaveSlot && !isCached)
            return LeafAnswer::TakeSlowPath;
        auto stored = object->tryStoreExtraProperty(vm, uid, value, { Object::ExtraPropertyStoreFlag::IsKnownNotToBeField });
        return stored.kind == Object::ExtraPropertyStore::NotHandled ? LeafAnswer::TakeSlowPath : LeafAnswer::Yes;
    }
    if (field->flags & (fieldIsReadOnly | fieldIsAccessor))
        return LeafAnswer::TakeSlowPath;
    if (field->slot.representation() == SlotRepresentation::DeferredClosure || value.isObject() || object->hasFlagsInOrderWord() || !object->readInline(*field))
        return LeafAnswer::TakeSlowPath;
    if (!object->tryStore(vm, *field, value))
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
    if (structureBefore->isUncacheableDictionary())
        return;
    if (structureBefore->isDictionary() && (slot.type() != PutPropertySlot::ExistingProperty || isNeverCachedName(*instance->vm, uid)))
        return;
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (isCachedByKey(caches, structureBefore, uid, SlotAccess::Store))
        return;
    caches.cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Store }, classifyStore({ instance->globalObject, object, structureBefore, uid, slot, PutMode::Strict }));
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
        // Filing costs several times what the store has cost here, and most pairs of a Structure and a key never come again.
        if (instance->state->propertyCaches->shouldFile(structure->id(), uid, SlotAccess::Store))
            cacheStore(instance, object, structure, uid, slot);
        return LeafAnswer::Yes;
    }
    if (canOnlyReplace || !structure->isStructureExtensible())
        return LeafAnswer::TakeSlowPath;
    bool repeatsAddition = repeatsPropertyAddition(structure, uid);
    object->putDirect(vm, uid, value, slot);
    if (repeatsAddition)
        cacheStore(instance, object, structure, uid, slot);
    return LeafAnswer::Yes;
}

static LeafAnswer putByKey(Instance* instance, JSCell* base, JSCell* key, JSValue value)
{
    if (!base->isObject())
        return LeafAnswer::TakeSlowPath;
    KeyName name = nameOfKeyForLeaf(instance, key, KeyAtomization::Atomize);
    switch (name.kind) {
    case KeyNameKind::NotAName:
    case KeyNameKind::HasNoAtom:
        return LeafAnswer::TakeSlowPath;
    case KeyNameKind::Index:
        return asObject(base)->trySetIndexQuickly(*instance->vm, name.index, value) ? LeafAnswer::Yes : LeafAnswer::TakeSlowPath;
    case KeyNameKind::Name:
        break;
    }
    if (base->type() == FinalObjectType)
        return putToFinalObject(instance, asObject(base), name.uid, value);
    Object* object = compiledObjectOrNull(base);
    return object ? putToCompiledObject(instance, object, name.uid, value) : LeafAnswer::TakeSlowPath;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPutByValGeneric, size_t, (Instance* instance, JSCell* base, JSCell* key, EncodedJSValue encodedValue))
{
    LeafOperationScope leaf(*instance->vm);
    DeferGCForAWhile deferGC(*instance->vm);
    return static_cast<size_t>(putByKey(instance, base, key, JSValue::decode(encodedValue)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
