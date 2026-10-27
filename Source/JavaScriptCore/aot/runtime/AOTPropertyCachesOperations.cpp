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
#include "AOTFrameArrays.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTSlimClosure.h"
#include "AOTStaticFields.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "NumericStrings.h"
#include "ProxyObject.h"

namespace JSC { namespace AOT {

static void fileRead(Instance* instance, JSValue base, Structure* structureBefore, const CachedName& name, const PropertySlot& slot, SiteCache site = nullptr)
{
    PropertyAnswer answer = classifyRead({ instance->globalObject, base, structureBefore, name.uid, slot });
    // What the first look changes (a dictionary is made flat, members are made, an absent field is marked as read) it changes once. A second look, which runs no code, sees what every later read sees.
    if (answer.kind == PropertyAnswerKind::Uncacheable && answer.didFlatten && base.isObject()) {
        VM& vm = *instance->vm;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Structure* structureNow = asObject(base)->structure();
        PropertyCaches& caches = *instance->state->propertyCaches;
        caches.didFileNothing();
        // The object before this one that was changed in the same way has left the answer there.
        if (structureNow != structureBefore && caches.tables().find(structureNow->id(), name, SlotAccess::Read))
            return;
        PropertySlot again(slot.thisValue(), PropertySlot::InternalMethodType::VMInquiry, &vm);
        asObject(base)->getPropertySlot(instance->globalObject, Identifier::fromUid(vm, name.uid), again);
        scope.assertNoException();
        if (!again.isTaintedByOpaqueObject()) {
            instance->state->propertyCaches->cache({ structureNow, name, SlotAccess::Read, site }, classifyRead({ instance->globalObject, base, structureNow, name.uid, again }));
            ensureStillAliveHere(structureNow);
            return;
        }
    }
    instance->state->propertyCaches->cache({ structureBefore, name, SlotAccess::Read, site }, WTF::move(answer));
    ensureStillAliveHere(structureBefore);
}

static void fileStore(Instance* instance, JSObject* base, Structure* structureBefore, const CachedName& name, const PutPropertySlot& slot, PutMode mode, SiteCache site = nullptr)
{
    instance->state->propertyCaches->cache({ structureBefore, name, accessFor(mode), site }, classifyStore({ instance->globalObject, base, structureBefore, name.uid, slot, mode }));
    ensureStillAliveHere(structureBefore);
}

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

static void createClassObjectsOnChain(VM& vm, JSCell* receiver, UniquedStringImpl* nameAskedFor)
{
    if (!isCompiledClosure(receiver))
        return;
    Closure& closure = *uncheckedDowncast<Closure>(receiver);
    const FieldData* field = closure.classData() ? StaticFields::findOwnField(closure, nameAskedFor) : nullptr;
    if (field && StaticFields::isInVariable(closure, *field))
        return;
    for (JSValue next = receiver; next.isCell() && isCompiledClosure(next.asCell()); next = asObject(next)->getPrototypeDirect()) {
        uncheckedDowncast<Closure>(next.asCell())->ensureClassObject(vm);
        if (StaticFields::findFieldForCache(vm, next.asCell(), nameAskedFor))
            return;
    }
}

static bool hasSlotForEveryClosure(const NamedAccess& access, JSCell* base)
{
    if (base->type() != SlimClosureType && base->type() != CompiledClosureType)
        return false;
    std::optional<Slot> slot = access.caches().tables().find(base->structureID(), access.name(), SlotAccess::Read);
    return slot && (slot->kind == Slot::Kind::ClosureName || slot->kind == Slot::Kind::ClosureLength);
}

JSValue getAndCache(const NamedAccess& access, JSValue base, JSValue receiver)
{
    VM& vm = *access.instance->vm;
    JSGlobalObject* globalObject = access.instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    bool shouldFile = !base.isCell() || access.caches().shouldFile(base.asCell()->structureID(), uid, SlotAccess::Read);
    if (access.caches().countsNames()) [[unlikely]]
        access.caches().countMiss("read"_s, shouldFile, base, uid);
    Object* compiledBase = compiledObjectOrNull(base);
    if (!shouldFile || (base.isCell() && hasSlotForEveryClosure(access, base.asCell())) || (compiledBase && hasPendingStars(*compiledBase))) {
        JSValue result = getWithoutCaches(access.instance, base, ident, receiver);
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    if (base.isCell()) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, base.asCell());
        createClassObjectsOnChain(vm, base.asCell(), uid);
    }
    LazyPrototypeRead lazy;
    if (base.isObject()) {
        lazy = classifyReadThroughLazyPrototypes(globalObject, asObject(base), uid);
        if (lazy.result == LazyPrototypeReadResult::NotApplicable && uid->isSymbol() && !isNeverCachedName(vm, uid)) {
            for (JSObject* object = asObject(base); object; object = object->getPrototypeDirect().getObject())
                ClassPrototype::ensureMembers(object);
        }
    }
    Structure* structureBefore = structureForPrimitive(globalObject, base);
    if (lazy.result == LazyPrototypeReadResult::Answered) {
        access.caches().cache({ structureBefore, access.name(), SlotAccess::Read, access.site }, lazy.answer);
        RELEASE_AND_RETURN(scope, readThroughAnswer(globalObject, receiver, lazy.answer));
    }
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    JSValue result = (lazy.firstObjectAbove ? JSValue(lazy.firstObjectAbove) : base).get(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, { });
    fileRead(access.instance, base, structureBefore, access.name(), slot, access.site);
    return result;
}

JSValue getUnderKeyForReceiver(Instance* instance, JSObject* base, const Identifier& name, JSValue receiver)
{
    NamedAccess access { instance, name, std::nullopt, nullptr };
    if (!access.caches().tables().find(base->structureID(), access.name(), SlotAccess::Read))
        return getAndCache(access, base, receiver);
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    return JSValue(base).get(instance->globalObject, name, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t key, SiteCache cache))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByIdAndCache);
    NamedAccess access { instance, identifierAt(instance, key), key, cache };
    JSValue base = JSValue::decode(encodedBase);
    if (JSValue value = tryGetFromPermanentDictionary(instance, base, access.ident.impl()))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, access.ident));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    LookByName look(vm, base, access.ident.impl());
    OPERATION_RETURN(scope, JSValue::encode(getAndCache(access, base)));
}

static bool tryStoreToPermanentDictionary(const NamedAccess& access, JSObject* object, JSValue value, PutMode mode, PutPropertySlot& slot)
{
    VM& vm = *access.instance->vm;
    UniquedStringImpl* uid = access.ident.impl();
    Structure* structure = object->structure();
    if (flattenOnce(vm, object))
        return false;
    unsigned attributes = 0;
    PropertyOffset offset = object->type() == FinalObjectType ? structure->get(vm, uid, attributes) : invalidOffset;
    if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isImmutableProperty(uid)) {
        JSValue(object).putInline(access.instance->globalObject, access.ident, value, slot);
        return true;
    }
    structure->didReplaceProperty(offset);
    object->putDirectOffset(vm, offset, value);
    if (!structure->isUncacheableDictionary()) {
        slot.setExistingProperty(object, offset);
        fileStore(access.instance, object, structure, access.name(), slot, mode, access.site);
    }
    return true;
}

static bool tryStoreExtraPropertyAndCache(Instance* instance, Object& object, const CachedName& name, SiteCache site, JSValue value, PutMode mode, const PutPropertySlot& slot)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = *instance->vm;
    OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
    bool isRepeated = object.repeatsExtraPropertyStore(vm, name.uid);
    if (isRepeated) {
        object.ensureStructureForExtraProperties(vm);
        flags.add(Flag::NamesExtraPropertiesStructure);
    }
    Structure* structureBefore = WTF::opaque(object.structure());
    if (mode == PutMode::Define)
        flags.add(Flag::IsDefinition);
    if (object.tryStoreExtraProperty(vm, name.uid, value, flags).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    if (isRepeated)
        fileStore(instance, &object, structureBefore, name, slot, mode, site);
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
        return access.caches().tables().mayBeNameOfObjectPrototype(field.name) ? "field store left the stub: absent, a name of Object.prototype"_s : "field store left the stub: absent, below Object.prototype: NOTHING STOOD IN THE WAY"_s;
    const AbsenceFilter& filter = access.caches().tables().absenceFilterFor(object.structureID());
    if (filter.structureID != object.structureID().bits())
        return "field store left the stub: absent, no filter of absent names for the Structure"_s;
    return "field store left the stub: absent, the filter is out of date or does not let the name pass"_s;
}

static bool takesAnyValue(const ProgramTables& tables, const LayoutData& layout, const FieldData& field)
{
    if (field.isOutOfLine() || field.slot.representation() != SlotRepresentation::JSValue || !FieldTableEntry::hasColumn(field.name))
        return false;
    uint32_t index = tables.fieldTableRowOf(layout.shape) + field.name;
    return index < tables.fieldRecordIndices().size() && FieldTableEntry { tables.fieldTableEntries()[index] }.name() == field.name && tables.fieldRecordIndices()[index] & FieldRecordIndex::isStoredWithoutTypeTest;
}

static void putToSlimClosureAndCache(const NamedAccess& access, SlimClosure& closure, JSValue value, PutMode mode)
{
    VM& vm = *access.instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* withoutProperty = closure.state().objects->closures.reifiedSlimClosureStructure(vm, closure.function());
    if (const Slot* filed = access.caches().findByKey(withoutProperty, access.ident.impl(), SlotAccess::Store); filed && filed->kind == Slot::Kind::Transition) {
        // As in operationAOTAddPropertyBySlot(): this pointer alone keeps the Structure alive across the allocations of the callee.
        closure.reifyWithAddedProperty(vm, WTF::opaque(filed->transition.newStructureID.decode()), filed->offsetInStorage(), value);
        return;
    }
    Structure* structureOfSlimClosures = WTF::opaque(closure.structure());
    closure.reify(vm);
    NamedAccess accessWithoutSite = access;
    accessWithoutSite.site = nullptr;
    putAndCache(accessWithoutSite, &closure, value, mode);
    RETURN_IF_EXCEPTION(scope, void());
    access.caches().cache({ structureOfSlimClosures, access.name(), accessFor(mode), access.site }, PropertyAnswer::uncacheable(accessFor(mode)));
}

void putAndCache(const NamedAccess& access, JSCell* cell, JSValue value, PutMode mode)
{
    VM& vm = *access.instance->vm;
    JSGlobalObject* globalObject = access.instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (cell->type() == SlimClosureType && mode != PutMode::Define)
        RELEASE_AND_RETURN(scope, putToSlimClosureAndCache(access, *uncheckedDowncast<SlimClosure>(cell), value, mode));
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    JSValue base = cell;
    PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
    bool shouldFile = access.caches().shouldFile(cell->structureID(), uid, accessFor(mode));
    if (access.caches().countsNames()) [[unlikely]]
        access.caches().countMiss(mode == PutMode::Define ? "definition"_s : "store"_s, shouldFile, cell, uid);
    // An object that is being filled stays a dictionary until a store comes again.
    if (shouldFile && mode != PutMode::Define && cell->isObject() && cell->structure()->isDictionary() && !cell->structure()->isCompiledStructure()) {
        bool isStored = tryStoreToPermanentDictionary(access, asObject(cell), value, mode, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
    }

    Object* object = compiledObjectOrNull(base);
    const FieldData* field = object ? object->fieldNamed(object->state(), ident) : nullptr;
    if (field && access.caches().countsNames()) [[unlikely]]
        access.caches().countNamed(whyStubLeftFieldStore(access, *object, *field, value, mode), uid);
    // The filter is for all the names of a Structure, so the second field that an object is given here lets the stubs create the ones behind it.
    if (field && mode != PutMode::Define && !object->compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype) && (shouldFile || (!object->isInSlot(*field) && access.caches().createsFieldOutsideStubsAgain(cell->structureID()))))
        access.caches().tables().fileAbsenceFilterForStores(globalObject, object);
    if (object && !field && tryStoreExtraPropertyAndCache(access.instance, *object, access.name(), access.site, value, mode, slot))
        return;

    if (shouldFile)
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, cell);
    // The store may give the cell another Structure, and then this pointer alone keeps the one before alive. The collector knows no StructureID on the stack, so the compiler must not make the pointer anew from the ID behind a call.
    Structure* structureBefore = WTF::opaque(cell->structure());
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
    bool isAddition = slot.type() == PutPropertySlot::NewProperty;
    if (!shouldFile || (isAddition && !repeatsAddition)) {
        if (isAddition)
            access.caches().didAddProperty(structureBefore, cell->structure());
        return;
    }

    PropertyAnswer answer = classifyStore({ globalObject, asObject(cell), structureBefore, uid, slot, mode });
    if (object && answer.kind == PropertyAnswerKind::FieldCreation) {
        if (cell->structure() == structureBefore && !structureBefore->mayBePrototype() && takesAnyValue(*access.instance->image.tables, object->layout(), *answer.field))
            answer.orderWords = std::pair { orderWordBefore, object->orderWord() };
        const CompiledStructure* compiledBefore = compiledObjectStructureOrNull(structureBefore);
        if (compiledBefore && !compiledBefore->hasOtherPrototype() && mode != PutMode::Define)
            object->state().objects->classes.didFindThatStoresPass(vm, object->layout(), uid, answer.changeableObjects.span());
    }
    access.caches().cache({ structureBefore, access.name(), accessFor(mode), access.site }, answer);
    ensureStillAliveHere(structureBefore);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t key, SiteCache cache, uint32_t putMode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdAndCache);
    NamedAccess access { instance, identifierAt(instance, key), key, cache };
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    PutMode mode = static_cast<PutMode>(putMode);
    LookByName look(vm, base, access.ident.impl());
    if (base.isUndefinedOrNull()) [[unlikely]]
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, access.ident));
    else if (base.isCell())
        putAndCache(access, base.asCell(), value, mode);
    else {
        PutPropertySlot slot(base, mode != PutMode::Sloppy, PutPropertySlot::PutById);
        base.putInline(globalObject, access.ident, value, slot);
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillSiteCache, void, (Instance* instance, JSCell* base, uint32_t key, SiteCache cache, uint32_t access))
{
    if (!instance->state->siteCaches->didMissBefore(cache))
        return;
    LeafOperationScope leaf(*instance->vm);
    instance->state->propertyCaches->fillSiteCacheFromTables(cache, base, key, static_cast<SlotAccess>(access));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindGetterAndFillSiteCache, EncodedJSValue, (Instance* instance, JSCell* base, uint32_t key, SiteCache cache))
{
    LeafOperationScope leaf(*instance->vm);
    return JSValue::encode(instance->state->propertyCaches->findGetterAndFillSiteCache(cache, base, key));
}

// What JSObject::hasInstance() refuses, in a message that quotes the expression.
static JSObject* tryCreateErrorOfConstructorThatNeverAnswers(JSGlobalObject* globalObject, JSObject* constructor, JSValue hasInstance)
{
    VM& vm = globalObject->vm();
    if (!hasInstance.isUndefinedOrNull() && hasInstance != globalObject->functionProtoHasInstanceSymbolFunction())
        return hasInstance.isCallable() ? nullptr : createHasInstanceNotAFunctionError(globalObject, constructor, instanceofExpressionOnTop(vm));
    TypeInfo info = constructor->structure()->typeInfo();
    if (info.implementsDefaultHasInstance() || info.implementsHasInstance())
        return nullptr;
    return createInstanceofRightHandSideNotAFunctionError(globalObject, constructor, instanceofExpressionOnTop(vm));
}

bool instanceOfAndCache(Instance* instance, JSValue value, JSValue constructorValue)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!constructorValue.isObject()) {
        throwException(globalObject, scope, createInstanceofRightHandSideNotObjectError(globalObject));
        return false;
    }

    JSObject* constructor = asObject(constructorValue);
    const Identifier& hasInstanceSymbol = vm.propertyNames->hasInstanceSymbol;
    createClassObjectsOnChain(vm, constructor, hasInstanceSymbol.impl());
    Structure* structureBefore = WTF::opaque(constructor->structure());
    PropertySlot hasInstanceSlot(constructor, PropertySlot::InternalMethodType::Get);
    JSValue hasInstance = JSValue(constructor).get(globalObject, hasInstanceSymbol, hasInstanceSlot);
    RETURN_IF_EXCEPTION(scope, false);
    fileRead(instance, constructor, structureBefore, { hasInstanceSymbol.impl(), std::nullopt }, hasInstanceSlot);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructor->structure()->typeInfo().implementsDefaultHasInstance()) {
        if (JSObject* error = tryCreateErrorOfConstructorThatNeverAnswers(globalObject, constructor, hasInstance)) {
            throwException(globalObject, scope, error);
            return false;
        }
        RELEASE_AND_RETURN(scope, constructor->hasInstance(globalObject, value, hasInstance));
    }

    // OrdinaryHasInstance reads "prototype" only for an object, so for any other value the lookup that fills the cache may run no code.
    bool readsPrototype = value.isObject();
    structureBefore = WTF::opaque(constructor->structure());
    PropertySlot prototypeSlot(constructor, readsPrototype ? PropertySlot::InternalMethodType::Get : PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool hasPrototype = constructor->getPropertySlot(globalObject, vm.propertyNames->prototype, prototypeSlot);
    prototypeSlot.disallowVMEntry.reset();
    RETURN_IF_EXCEPTION(scope, false);
    JSValue prototype = hasPrototype && readsPrototype ? prototypeSlot.getValue(globalObject, vm.propertyNames->prototype) : jsUndefined();
    RETURN_IF_EXCEPTION(scope, false);
    if (!prototypeSlot.isTaintedByOpaqueObject())
        fileRead(instance, constructor, structureBefore, { vm.propertyNames->prototype.impl(), std::nullopt }, prototypeSlot);
    RELEASE_AND_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceOfAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceOfAndCache);
    OPERATION_RETURN(scope, instanceOfAndCache(instance, JSValue::decode(encodedValue), JSValue::decode(encodedConstructor)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefaultHasInstance);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedPrototype)));
}

static JSValue getByVal(JSGlobalObject* globalObject, JSValue base, JSValue property)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, property));
        return { };
    }
    if (std::optional<uint32_t> index = indexOfKey(property)) {
        if (base.isObject()) {
            JSObject* object = asObject(base);
            if (JSValue result = object->tryGetIndexQuickly(*index))
                return result;
            Structure* structure = object->structure();
            if (structure->realm() == globalObject && globalObject->isOriginalArrayStructure(structure) && !hasAnyArrayStorage(structure->indexingType()) && globalObject->arrayPrototypeChainIsSane())
                return jsUndefined();
            // The stub finds the text of the index there the next time.
            if (object->type() == ProxyObjectType && *index < NumericStrings::cacheSize)
                vm.numericStrings.addJSString(vm, static_cast<int>(*index));
        }
        RELEASE_AND_RETURN(scope, base.get(globalObject, *index));
    }
    auto key = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, base.get(globalObject, key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByVal);
    OPERATION_RETURN(scope, JSValue::encode(getByVal(globalObject, heapValueOf(instance, JSValue::decode(encodedBase)), JSValue::decode(encodedProperty))));
}

static const ProxyAccessWords& proxyAccessWords(EncodedJSValue* words) { return *std::bit_cast<const ProxyAccessWords*>(words); }

JSC_DEFINE_JIT_OPERATION(operationAOTGetWithProxyTrap, EncodedJSValue, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue trap))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetWithProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    // The answer is filed for the target, where the stub looks the next time.
    if (uint32_t number = static_cast<uint32_t>(words.storeIsStrictOrHeaderOfSlot >> Slot::locationShiftInHeader); number && JSValue::decode(trap).isUndefinedOrNull())
        OPERATION_RETURN(scope, JSValue::encode(getAndCache({ instance, identifierAt(instance, number), number, nullptr }, words.target, words.proxy)));
    OPERATION_RETURN(scope, JSValue::encode(ProxyObject::getWithTrap(globalObject, words.target, words.handler, JSValue::decode(trap), words.key, words.proxy)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValidateAnswerOfProxyTrap, EncodedJSValue, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValidateAnswerOfProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    ProxyObject::validateGetTrapResultWhereBound(globalObject, JSValue::decode(answer), words.target, words.key);
    OPERATION_RETURN(scope, answer);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasWithProxyTrap, size_t, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue trap))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasWithProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    OPERATION_RETURN(scope, ProxyObject::hasWithTrap(globalObject, words.target, words.handler, JSValue::decode(trap), words.key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFinishHasThroughProxy, size_t, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFinishHasThroughProxy);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    OPERATION_RETURN(scope, ProxyObject::finishHas(globalObject, JSValue::decode(answer), words.target, words.key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetWithProxyTrap, void, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue trap))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetWithProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    ProxyObject::setWithTrap(globalObject, words.target, words.handler, JSValue::decode(trap), words.key, JSValue::decode(words.storedValue), words.proxy, words.storeIsStrictOrHeaderOfSlot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFinishSetThroughProxy, void, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFinishSetThroughProxy);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    ProxyObject::finishSet(globalObject, JSValue::decode(answer), words.target, words.key, JSValue::decode(words.storedValue), words.storeIsStrictOrHeaderOfSlot);
    OPERATION_RETURN(scope);
}

static void putByVal(JSGlobalObject* globalObject, JSValue base, JSValue property, JSValue value, bool isStrict)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, property));
        return;
    }
    if (std::optional<uint32_t> index = indexOfKey(property)) {
        // The stub finds the text of the index there the next time.
        if (base.isCell() && base.asCell()->type() == ProxyObjectType && *index < NumericStrings::cacheSize)
            vm.numericStrings.addJSString(vm, static_cast<int>(*index));
        scope.release();
        if (!base.isObject() || !asObject(base)->trySetIndexQuickly(vm, *index, value))
            base.putByIndex(globalObject, *index, value, isStrict);
        return;
    }
    auto key = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PutPropertySlot slot(base, isStrict);
    scope.release();
    base.put(globalObject, key, value, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByVal);
    // Where nothing above the object intercepts indexes, a store of an element is a definition of it.
    if (Object* object = compiledObjectOrNull(JSValue::decode(encodedBase))) {
        std::optional<uint32_t> index = JSValue::decode(encodedProperty).tryGetAsUint32Index();
        if (index && !globalObject->isHavingABadTime() && !object->anyObjectInChainMayInterceptIndexedAccesses() && tryDefineElementOfCompiledObject(instance, object, *index, JSValue::decode(encodedValue)))
            OPERATION_RETURN(scope);
    }
    putByVal(globalObject, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), isStrict);
    OPERATION_RETURN(scope);
}

static JSValue getFromCellAndCache(Instance* instance, JSValue base, const Identifier& name)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    CachedName cachedName { name.impl(), std::nullopt };
    JSObject* object = base.getObject();
    bool shouldFile = !object || caches.shouldFile(object->structureID(), name.impl(), SlotAccess::Read);
    if (object && shouldFile) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
        createClassObjectsOnChain(vm, object, name.impl());
    }
    Structure* structureBefore = structureForPrimitive(globalObject, base);
    LazyPrototypeRead lazy;
    if (object)
        lazy = classifyReadThroughLazyPrototypes(globalObject, object, name.impl());
    if (lazy.result == LazyPrototypeReadResult::Answered) {
        if (shouldFile)
            caches.cache({ structureBefore, cachedName, SlotAccess::Read }, lazy.answer);
        RELEASE_AND_RETURN(scope, readThroughAnswer(globalObject, base, lazy.answer));
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    JSValue result = (lazy.firstObjectAbove ? JSValue(lazy.firstObjectAbove) : base).get(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (shouldFile)
        fileRead(instance, base, structureBefore, cachedName, slot);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (auto longKeyName = findNameEqualToLongString(base, property)) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(*longKeyName ? asObject(base)->get(globalObject, Identifier::fromUid(vm, *longKeyName)) : jsUndefined()));
    if (!base.isCell())
        OPERATION_RETURN(scope, JSValue::encode(getByVal(globalObject, base, property)));

    auto name = nameForKeyedCache(globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!name && base.isObject() && property.isString()) {
        if (base.asCell()->structure()->isCompiledStructure()) {
            Identifier identifier = asString(property)->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*identifier.impl()))
                name = WTF::move(identifier);
        } else {
            auto text = asString(property)->value(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!parseIndex(*text.data.impl()) && hasOnlyOrdinaryLookupsOnChain(asObject(base)))
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
    OPERATION_RETURN(scope, JSValue::encode(name ? getFromCellAndCache(instance, base, *name) : getByVal(globalObject, base, property)));
}

static void putByValAndCache(Instance* instance, JSValue base, JSValue property, JSValue value, PutMode mode)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isStrict = mode == PutMode::Strict;
    PutPropertySlot slot(base, isStrict);
    if (auto longKeyName = findNameEqualToLongString(base, property); longKeyName && *longKeyName) [[unlikely]]
        RELEASE_AND_RETURN(scope, void(base.put(globalObject, Identifier::fromUid(vm, *longKeyName), value, slot)));

    std::optional<Identifier> name;
    if (base.isObject() && property.isString()) {
        Identifier identifier = asString(property)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (!parseIndex(*identifier.impl()))
            name = WTF::move(identifier);
    } else if (base.isObject())
        name = nameForKeyedCache(globalObject, property);
    if (!name)
        RELEASE_AND_RETURN(scope, putByVal(globalObject, base, property, value, isStrict));

    JSObject* object = asObject(base);
    CachedName cachedName { name->impl(), std::nullopt };
    PropertyCaches& caches = *instance->state->propertyCaches;
    bool shouldFile = caches.shouldFile(object->structureID(), name->impl(), accessFor(mode));
    Object* compiledObject = compiledObjectOrNull(base);
    const FieldData* field = compiledObject ? compiledObject->fieldNamed(compiledObject->state(), *name) : nullptr;
    if (compiledObject && !field && tryStoreExtraPropertyAndCache(instance, *compiledObject, cachedName, nullptr, value, mode, slot))
        return;

    Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
    createClassObjectsOnChain(vm, object, name->impl());
    Structure* structureBefore = WTF::opaque(object->structure());
    bool repeatsAddition = compiledObject || repeatsPropertyAddition(structureBefore, name->impl());
    if (compiledObject)
        Object::putWithKnownField(object, globalObject, *name, value, slot, field);
    else
        base.put(globalObject, *name, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    bool isAddition = slot.type() == PutPropertySlot::NewProperty;
    if (shouldFile && (!isAddition || repeatsAddition))
        fileStore(instance, object, structureBefore, cachedName, slot, mode);
    else if (isAddition)
        caches.didAddProperty(structureBefore, object->structure());
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByValAndCache);
    putByValAndCache(instance, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), PutMode::Strict);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValAndCacheSloppy, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByValAndCacheSloppy);
    putByValAndCache(instance, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), PutMode::Sloppy);
    OPERATION_RETURN(scope);
}

static bool answerImpliesResult(const PropertyAnswer& answer, Structure* structure, bool isIn, bool result)
{
    Slot slot = slotFor(answer, structure);
    switch (Slot::presenceOf(slot.kind)) {
    case SlotPresence::Unknown:
        if (isIn && slot.kind == Slot::Kind::FieldOrInherited)
            return result;
        return slot.kind == Slot::Kind::Uncacheable || (isIn && slot.kind == Slot::Kind::InheritedField);
    case SlotPresence::ReadsSlot:
        return true;
    case SlotPresence::Present:
        return result == (isIn || slot.has(Slot::isOwn));
    case SlotPresence::Absent:
        return !result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasAndCache(Instance* instance, JSObject* object, const CachedName& name, bool isIn)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    PropertyName propertyName(name.uid);
    bool shouldFile = caches.shouldFile(object->structureID(), name.uid, SlotAccess::Read);
    if (caches.countsNames()) [[unlikely]]
        caches.countMiss(isIn ? "in"_s : "has own"_s, shouldFile, object, name.uid);
    if (shouldFile) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
        createClassObjectsOnChain(vm, object, name.uid);
    }
    Structure* structureBefore = WTF::opaque(object->structure());

    std::optional<PropertyAnswer> answer;
    auto lookUpOnChain = [&](PropertySlot& slot) {
        LazyPrototypeRead lazy = classifyReadThroughLazyPrototypes(globalObject, object, name.uid);
        if (lazy.result != LazyPrototypeReadResult::Answered)
            return (lazy.firstObjectAbove ? lazy.firstObjectAbove : object)->getPropertySlot(globalObject, propertyName, slot);
        answer = WTF::move(lazy.answer);
        return answer->kind != PropertyAnswerKind::Absent;
    };
    auto fileIfImplied = [&](const PropertySlot& slot, bool result) {
        if (!answer)
            answer = classifyRead({ globalObject, object, structureBefore, name.uid, slot });
        if (answerImpliesResult(*answer, structureBefore, isIn, result))
            caches.cache({ structureBefore, name, SlotAccess::Read }, *answer);
    };
    auto hasProperty = [&] {
        PropertySlot slot(object, PropertySlot::InternalMethodType::HasProperty);
        bool result = lookUpOnChain(slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (shouldFile)
            fileIfImplied(slot, result);
        return result;
    };
    auto hasOwnProperty = [&] {
        PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
        bool result = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (result && shouldFile)
            fileIfImplied(slot, true);
        if (result || !shouldFile || object->structure() != structureBefore)
            return result;
        if (!structureBefore->isCompiledStructure() && !structureProvesOwnAbsence(vm, object, structureBefore, name.uid, ChainWalkMode::MayAllocate))
            return false;
        PropertySlot inquiry(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        lookUpOnChain(inquiry);
        inquiry.disallowVMEntry.reset();
        RETURN_IF_EXCEPTION(scope, false);
        if (!inquiry.isTaintedByOpaqueObject())
            fileIfImplied(inquiry, false);
        return false;
    };
    bool result = isIn ? hasProperty() : hasOwnProperty();
    ensureStillAliveHere(structureBefore);
    return result;
}

bool hasPropertyAndCache(Instance* instance, JSObject* object, const CachedName& name)
{
    return hasAndCache(instance, object, name, true);
}

static bool hasByValAndCache(Instance* instance, JSObject* object, JSValue property, bool isIn)
{
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(*instance->vm);
    if (auto longKeyName = findNameEqualToLongString(object, property, isIn)) [[unlikely]]
        return !!*longKeyName;
    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        // The stub finds the text of the index there the next time.
        if (isIn && object->type() == ProxyObjectType && *index < NumericStrings::cacheSize)
            instance->vm->numericStrings.addJSString(*instance->vm, static_cast<int>(*index));
        RELEASE_AND_RETURN(scope, isIn ? object->hasProperty(globalObject, *index) : object->hasOwnProperty(globalObject, *index));
    }
    if (property.isString() && !atomOfKey(property)) {
        auto existingAtom = asString(property)->toExistingAtomString(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!existingAtom) {
            auto text = asString(property)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (!parseIndex(*text.data.impl()) && hasOnlyOrdinaryLookupsOnChain(object))
                return false;
        }
    }
    Identifier identifier = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if ((!property.isString() && !property.isSymbol()) || parseIndex(identifier))
        RELEASE_AND_RETURN(scope, isIn ? object->hasProperty(globalObject, identifier) : object->hasOwnProperty(globalObject, identifier));
    RELEASE_AND_RETURN(scope, hasAndCache(instance, object, { identifier.impl(), std::nullopt }, isIn));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByIdAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, hasAndCache(instance, asObject(base), instance->state->propertyCaches->nameOf(key), true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, hasByValAndCache(instance, asObject(base), JSValue::decode(encodedProperty), true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnByIdAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasOwnByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isObject()) [[likely]]
        OPERATION_RETURN(scope, hasAndCache(instance, asObject(base), instance->state->propertyCaches->nameOf(key), false));
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, identifierAt(instance, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasOwnByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (base.isObject()) [[likely]]
        OPERATION_RETURN(scope, hasByValAndCache(instance, asObject(base), property, false));
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Identifier name = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, name));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
