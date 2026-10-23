/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCachesNamedAccess.h"
#include "AOTRuntimeFunctions.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static NamedAccess namedAccess(Instance* instance, uint32_t key, SiteCache site)
{
    return { instance, instance->globalObject, *instance->vm, *instance->state->propertyCaches, identifierAt(instance, key), key, site };
}

static CachedAccess cachedAccess(const NamedAccess& access, Structure* structure, SlotAccess slotAccess)
{
    return { structure, { access.ident.impl(), access.number }, slotAccess, access.site };
}

// Only the place of a property that a cacheable dictionary holds is filed under such a Structure. Every other read by name comes here each time, and the lookup that classifies nothing answers it.
static JSValue tryGetFromPermanentDictionary(Instance* instance, JSValue base, UniquedStringImpl* uid)
{
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return { };
    VM& vm = *instance->vm;
    Structure* structure = base.asCell()->structure();
    if (!structure->isDictionary() || !structure->hasBeenFlattenedBefore() || isNeverCachedName(vm, uid))
        return { };
    if (!structure->isUncacheableDictionary() && isValidOffset(structure->get(vm, uid)))
        return { };
    return JSValue::decode(operationAOTGetGeneric(instance, base.asCell(), uid));
}

static bool tryGetFromOrdinaryObject(JSGlobalObject* globalObject, JSValue base, UniquedStringImpl* uid, PropertySlot& slot, JSValue& result)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || isNeverCachedName(vm, uid))
        return false;
    JSObject* object = asObject(base.asCell());
    Structure* structure = object->structure();
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot())
        return false;
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return false;
    result = object->getDirect(offset);
    slot.setValue(object, attributes, result, offset);
    return true;
}

void createClassObjectsOnChain(VM& vm, JSCell* receiver, UniquedStringImpl* nameAskedFor)
{
    if (!isCompiledClosure(receiver))
        return;
    // A static field is answered from its variable, whatever its class object looks like.
    Closure& closure = *uncheckedDowncast<Closure>(receiver);
    const FieldData* field = closure.classData() ? StaticFields::findOwnField(closure, nameAskedFor) : nullptr;
    if (field && StaticFields::isInVariable(closure, *field))
        return;
    for (JSValue next = receiver; next.isCell() && isCompiledClosure(next.asCell()); next = asObject(next)->getPrototypeDirect())
        uncheckedDowncast<Closure>(next.asCell())->ensureClassObject(vm);
}

// The stand-in for a prototype object says that its class declares no such symbol without becoming an ordinary object, and taints the slot, so nothing would ever be filed.
static void createPrototypeMembersOnChain(JSObject* receiver)
{
    for (JSObject* object = receiver; object; object = object->getPrototypeDirect().getObject())
        ClassPrototype::ensureMembers(object);
}

// The stub found the slot and left all the same: the name has a prefix, or it was read before. Whatever this closure answers, the slot stays as it is for the others.
static bool hasSlotForEverySlimClosure(const NamedAccess& access, JSCell* base)
{
    if (base->type() != SlimClosureType)
        return false;
    std::optional<Slot> slot = access.caches.tables().find(base->structureID(), { access.ident.impl(), access.number }, SlotAccess::Read);
    return slot && (slot->kind == Slot::Kind::ClosureName || slot->kind == Slot::Kind::ClosureLength);
}

JSValue getAndCache(const NamedAccess& access, JSValue base)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    bool shouldFile = !base.isCell() || access.caches.shouldFile(base.asCell()->structureID(), uid, SlotAccess::Read);
    if (access.caches.countsNames()) [[unlikely]]
        access.caches.countMiss("read"_s, shouldFile, base, uid);
    // Nothing is filed about an object whose stars are pending: what it has is not made yet.
    Object* compiledBase = compiledObjectOrNull(base);
    if (!shouldFile || (base.isCell() && hasSlotForEverySlimClosure(access, base.asCell())) || (compiledBase && hasPendingStars(*compiledBase))) {
        JSValue result = getWithoutCaches(access.instance, base, ident);
        RETURN_IF_EXCEPTION(scope, { });
#if !CPU(X86_64)
        // Whatever this name found: the next name that the object has not is answered without leaving.
        if (base.isObject() && !(compiledBase && hasPendingStars(*compiledBase)))
            access.caches.tables().fileAbsenceFilter(globalObject, asObject(base));
#endif
        return result;
    }
    if (base.isCell()) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, base.asCell());
        createClassObjectsOnChain(vm, base.asCell(), uid);
    }
    LazyPrototypeRead lazy;
    if (base.isObject()) {
        lazy = classifyReadThroughLazyPrototypes(globalObject, asObject(base), uid);
        if (lazy.result == LazyPrototypeReadResult::NotApplicable && uid->isSymbol() && !isNeverCachedName(vm, uid))
            createPrototypeMembersOnChain(asObject(base));
    }
    Structure* structureBefore = structureForPrimitive(globalObject, base);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);

    JSValue result;
    switch (lazy.result) {
    case LazyPrototypeReadResult::Answered:
        access.caches.cache(cachedAccess(access, structureBefore, SlotAccess::Read), lazy.answer);
        RELEASE_AND_RETURN(scope, readThroughAnswer(globalObject, base, lazy.answer));
    case LazyPrototypeReadResult::ContinuesAbove:
        result = JSValue(lazy.firstObjectAbove).get(globalObject, ident, slot);
        break;
    case LazyPrototypeReadResult::NotApplicable:
        if (!tryGetFromOrdinaryObject(globalObject, base, uid, slot, result))
            result = base.get(globalObject, ident, slot);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });

    access.caches.cache(cachedAccess(access, structureBefore, SlotAccess::Read), classifyRead({ globalObject, base, structureBefore, uid, slot }));
    ensureStillAliveHere(structureBefore);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t key, SiteCache cache))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByIdAndCache);
    NamedAccess access = namedAccess(instance, key, cache);
    JSValue base = JSValue::decode(encodedBase);
    if (JSValue value = tryGetFromPermanentDictionary(instance, base, access.ident.impl()))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, access.ident));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(getAndCache(access, base)));
}

static bool tryStoreToPermanentDictionary(const NamedAccess& access, JSObject* object, JSValue value, PutMode mode, PutPropertySlot& slot)
{
    VM& vm = access.vm;
    UniquedStringImpl* uid = access.ident.impl();
    Structure* structure = object->structure();
    if (flattenOnce(vm, object))
        return false;
    unsigned attributes = 0;
    PropertyOffset offset = object->type() == FinalObjectType ? structure->get(vm, uid, attributes) : invalidOffset;
    if (isValidOffset(offset) && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) && !structure->isImmutableProperty(uid)) {
        structure->didReplaceProperty(offset);
        object->putDirectOffset(vm, offset, value);
        if (!structure->isUncacheableDictionary()) {
            slot.setExistingProperty(object, offset);
            access.caches.cache(cachedAccess(access, structure, accessFor(mode)), classifyStore({ access.globalObject, object, structure, uid, slot, mode }));
        }
        return true;
    }
    JSValue(object).putInline(access.globalObject, access.ident, value, slot);
    return true;
}

static bool tryStoreExtraProperty(const NamedAccess& access, Object& object, JSValue value, PutMode mode, Structure*& structureBefore, const PutPropertySlot& slot)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = access.vm;
    OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
    bool isRepeated = object.repeatsExtraPropertyStore(vm, access.ident);
    if (isRepeated) {
        object.ensureStructureForExtraProperties(vm);
        flags.add(Flag::NamesExtraPropertiesStructure);
    }
    structureBefore = object.structure();
    if (mode == PutMode::Define)
        flags.add(Flag::IsDefinition);
    if (object.tryStoreExtraProperty(vm, access.ident, value, flags).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    if (!isRepeated)
        return true;
    PropertyAnswer answer = classifyStore({ access.globalObject, &object, structureBefore, access.ident.impl(), slot, mode });
    access.caches.cache(cachedAccess(access, structureBefore, accessFor(mode)), answer);
    return true;
}

// For the counts alone: why a store or a definition under the name of a field got as far as the run time. The tests are those of the stub that stores by the table of fields, in its order.
static ASCIILiteral whyStubLeftFieldStore(const NamedAccess& access, Object& object, const FieldData& field, JSValue value, PutMode mode)
{
    if (field.flags & (fieldAttributeFlags | fieldIsPrivate | fieldIsBrand))
        return "field store left the stub: the layout gives the field attributes, or it is private"_s;
    uint32_t orderWord = object.orderWord();
    if (orderWord & orderWordIntegrityBits)
        return "field store left the stub: the object is sealed, frozen or not extensible"_s;
    bool admits = object.state().objects->layouts.slotAdmits(field, value);
    if (object.isInSlot(field)) {
        if (field.flags & fieldIsStoredOnce)
            return "field store left the stub: present, stored once"_s;
        return admits ? "field store left the stub: present, the slot admits the value"_s : "field store left the stub: present, the slot does not admit the value"_s;
    }
    if (!admits)
        return "field store left the stub: absent, the slot does not admit the value"_s;
    if (field.slot.representation() == SlotRepresentation::DeferredClosure)
        return "field store left the stub: absent, the slot defers a closure"_s;
    if (orderWord & orderWordHasFieldOutsideSlot)
        return "field store left the stub: absent, some field of the object lies outside its slot"_s;
    const CompiledStructure& structure = object.compiledStructure();
    if (structure.mayBePrototype())
        return "field store left the stub: absent, the object may be a prototype"_s;
    if (slotKeyFor(field.slot.offset, field.isOutOfLine()) == noSlotKey)
        return "field store left the stub: absent, the slot has no key"_s;
    if (mode == PutMode::Define)
        return "field store left the stub: absent, a definition: NOTHING STOOD IN THE WAY"_s;
    if (!FieldTableEntry::hasColumn(field.name))
        return "field store left the stub: absent, the name has no column"_s;
    if (field.hasGuardOfStores)
        return "field store left the stub: absent, the field has a guard of stores (one in 16 leaves on purpose)"_s;
    if (structure.flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return access.caches.tables().mayBeNameOfObjectPrototype(field.name) ? "field store left the stub: absent, a name of Object.prototype"_s : "field store left the stub: absent, below Object.prototype: NOTHING STOOD IN THE WAY"_s;
    const AbsenceFilter& filter = access.caches.tables().absenceFilterFor(object.structureID());
    if (filter.structureID != object.structureID().bits())
        return "field store left the stub: absent, no filter of absent names for the Structure"_s;
    return "field store left the stub: absent, the filter is out of date or does not let the name pass"_s;
}

// FieldRecordIndex::isStoredWithoutTypeTest, which for a slot that holds any value says that the type of the field is that of any value, and that the field is a plain data property that is stored more than once.
static bool takesAnyValue(const ProgramTables& tables, const LayoutData& layout, const FieldData& field)
{
    if (field.isOutOfLine() || field.slot.representation() != SlotRepresentation::JSValue || !FieldTableEntry::hasColumn(field.name))
        return false;
    uint32_t index = tables.fieldTableRowOf(layout.shape) + field.name;
    return index < tables.fieldRecordIndices().size() && FieldTableEntry { tables.fieldTableEntries()[index] }.name() == field.name && tables.fieldRecordIndices()[index] & FieldRecordIndex::isStoredWithoutTypeTest;
}

static bool tryAddFirstPropertyOfSlimClosure(const NamedAccess& access, SlimClosure& closure, JSValue value)
{
    VM& vm = access.vm;
    Structure* withoutProperty = closure.state().objects->closures.reifiedSlimClosureStructure(vm, closure.function());
    const Slot* filed = access.caches.findByKey(withoutProperty, access.ident.impl(), SlotAccess::Store);
    if (!filed || filed->kind != Slot::Kind::Transition)
        return false;
    closure.reifyWithAddedProperty(vm, filed->transition.newStructureID.decode(), filed->offsetInStorage(), value);
    return true;
}

static void putToSlimClosureAndCache(const NamedAccess& access, SlimClosure& closure, JSValue value, PutMode mode)
{
    if (tryAddFirstPropertyOfSlimClosure(access, closure, value))
        return;
    VM& vm = access.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structureOfSlimClosures = closure.structure();
    closure.reify(vm);
    NamedAccess accessWithoutSite = access;
    accessWithoutSite.site = nullptr;
    putAndCache(accessWithoutSite, &closure, value, mode);
    RETURN_IF_EXCEPTION(scope, void());
    access.caches.cache(cachedAccess(access, structureOfSlimClosures, accessFor(mode)), PropertyAnswer::uncacheable(accessFor(mode)));
}

void putAndCache(const NamedAccess& access, JSCell* cell, JSValue value, PutMode mode)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (cell->type() == SlimClosureType && mode != PutMode::Define)
        RELEASE_AND_RETURN(scope, putToSlimClosureAndCache(access, *uncheckedDowncast<SlimClosure>(cell), value, mode));
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    JSValue base = cell;
    PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
    bool shouldFile = access.caches.shouldFile(cell->structureID(), uid, accessFor(mode));
    if (access.caches.countsNames()) [[unlikely]]
        access.caches.countMiss(mode == PutMode::Define ? "definition"_s : "store"_s, shouldFile, cell, uid);
    if (mode != PutMode::Define && cell->isObject() && cell->structure()->isDictionary() && !cell->structure()->isCompiledStructure()) {
        bool isStored = tryStoreToPermanentDictionary(access, asObject(cell), value, mode, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
    }

    Structure* structureBefore = nullptr;
    Object* object = compiledObjectOrNull(base);
    const FieldData* field = object ? object->fieldNamed(object->state(), ident) : nullptr;
    if (field && access.caches.countsNames()) [[unlikely]]
        access.caches.countNamed(whyStubLeftFieldStore(access, *object, *field, value, mode), uid);
    // The next store that creates a field of an object below the same prototype is answered without leaving.
    if (shouldFile && field && mode != PutMode::Define && !object->compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        access.caches.tables().fileAbsenceFilterForStores(globalObject, object);
    if (object && !field && tryStoreExtraProperty(access, *object, value, mode, structureBefore, slot))
        return;

    // Only what is filed has to name the extra properties by their Structure.
    if (shouldFile)
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, cell);
    structureBefore = cell->structure();
    const uint32_t orderWordBefore = object ? object->orderWord() : 0;
    bool repeatsAddition = object || !cell->isObject() || repeatsPropertyAddition(structureBefore, uid);
    if (mode == PutMode::Define && cell->isObject())
        defineDataProperty(access.instance, asObject(cell), ident, value, slot);
    else if (object)
        Object::putWithKnownField(cell, globalObject, ident, value, slot, field);
    else
        base.putInline(globalObject, ident, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    if (!cell->isObject())
        return;
    if (!repeatsAddition && slot.type() == PutPropertySlot::NewProperty) {
        access.caches.didAddProperty(structureBefore, cell->structure());
        return;
    }
    if (!shouldFile) {
        if (slot.type() == PutPropertySlot::NewProperty)
            access.caches.didAddProperty(structureBefore, cell->structure());
        return;
    }

    PropertyAnswer answer = classifyStore({ globalObject, asObject(cell), structureBefore, uid, slot, mode });
    const CompiledStructure* compiledBefore = object ? compiledObjectStructureOrNull(structureBefore) : nullptr;
    if (object && answer.kind == PropertyAnswerKind::FieldCreation && cell->structure() == structureBefore && !structureBefore->mayBePrototype() && takesAnyValue(*access.instance->image.tables, object->layout(), *answer.field))
        answer.orderWords = std::pair { orderWordBefore, object->orderWord() };
    if (compiledBefore && !compiledBefore->hasOtherPrototype() && answer.kind == PropertyAnswerKind::FieldCreation && mode != PutMode::Define)
        object->state().objects->classes.didFindThatStoresPass(vm, object->layout(), uid, answer.changeableObjects.span());
    access.caches.cache(cachedAccess(access, structureBefore, accessFor(mode)), answer);
    ensureStillAliveHere(structureBefore);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t key, SiteCache cache, uint32_t putMode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdAndCache);
    NamedAccess access = namedAccess(instance, key, cache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    PutMode mode = static_cast<PutMode>(putMode);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, access.ident));
        OPERATION_RETURN(scope);
    }
    if (!base.isCell()) {
        PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
        base.putInline(globalObject, access.ident, value, slot);
        OPERATION_RETURN(scope);
    }
    putAndCache(access, base.asCell(), value, mode);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillSiteCache, void, (Instance* instance, JSCell* base, uint32_t key, SiteCache cache, uint32_t access))
{
    if (!instance->state->siteCaches->didMissBefore(cache))
        return;
    LeafOperationScope leaf(*instance->vm);
    instance->state->propertyCaches->fillSiteCacheFromTables(cache, base, key, static_cast<SlotAccess>(access));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
