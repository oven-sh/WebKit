/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTErrorMessages.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlimClosure.h"
#include "AOTStaticFields.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "NumericStrings.h"
#include "ProxyObject.h"

namespace JSC { namespace AOT {

static void fileRead(Instance* instance, JSValue base, Structure* structureBefore, const CachedName& name, const PropertySlot& slot)
{
    PropertyAnswer answer = classifyRead({ instance->globalObject, base, structureBefore, name.uid, slot });
    answerByGetterField(base, structureBefore, answer);
    if (answer.kind == PropertyAnswerKind::Uncacheable && answer.didFlatten && base.isObject()) {
        VM& vm = *instance->vm;
        DeferTerminationForAWhile deferTermination(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Structure* structureNow = asObject(base)->structure();
        PropertyCaches& caches = *instance->state->propertyCaches;
        caches.didFileNothing();
        if (structureNow != structureBefore && caches.tables().find(structureNow->id(), name, SlotAccess::Read))
            return;
        PropertySlot again(slot.thisValue(), PropertySlot::InternalMethodType::VMInquiry, &vm);
        asObject(base)->getPropertySlot(instance->globalObject, Identifier::fromUid(vm, name.uid), again);
        scope.assertNoException();
        if (!again.isTaintedByOpaqueObject()) {
            instance->state->propertyCaches->cache({ structureNow, name, SlotAccess::Read }, classifyRead({ instance->globalObject, base, structureNow, name.uid, again }));
            ensureStillAliveHere(structureNow);
            return;
        }
    }
    instance->state->propertyCaches->cache({ structureBefore, name, SlotAccess::Read }, WTF::move(answer));
    ensureStillAliveHere(structureBefore);
}

static bool beganOrEndedAsDictionary(Structure* before, Structure* after)
{
    return after != before && (before->isDictionary() || after->isDictionary());
}

static void fileStore(Instance* instance, JSObject* base, Structure* structureBefore, const CachedName& name, const PutPropertySlot& slot, PutMode mode, std::optional<uint32_t> orderWordBefore = std::nullopt)
{
    PropertyAnswer answer = classifyStore({ instance->globalObject, base, structureBefore, name.uid, slot, mode, orderWordBefore });
    instance->state->propertyCaches->cache({ structureBefore, name, accessFor(mode) }, WTF::move(answer));
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

static void createClassObjectsOnChain(VM& vm, JSCell* receiver)
{
    for (JSValue next = receiver; next.isCell() && isCompiledClosure(next.asCell()); next = asObject(next)->getPrototypeDirect())
        uncheckedDowncast<Closure>(next.asCell())->ensureClassObject(vm);
}

static void createStaticsOnChain(VM& vm, JSCell* receiver, UniquedStringImpl* uid, StaticFieldIsRead staticFieldIsRead = StaticFieldIsRead::AsProperty)
{
    for (JSValue next = receiver; next.isCell() && isCompiledClosure(next.asCell()); next = asObject(next)->getPrototypeDirect()) {
        auto* closure = uncheckedDowncast<Closure>(next.asCell());
        if (staticFieldIsRead == StaticFieldIsRead::InVariable && closure == receiver && StaticFields::tryRead(*closure, uid)) {
            closure->ensureStaticMethods(vm);
            return;
        }
        if (closure->canHoldStaticFieldInVariable(uid))
            closure->ensureClassObject(vm);
        else
            closure->ensureStaticMethods(vm);
    }
}

static bool hasSlotForEveryClosure(const NamedAccess& access, JSCell* base)
{
    if (base->type() != SlimClosureType && base->type() != CompiledClosureType)
        return false;
    std::optional<Slot> slot = access.caches().tables().find(base->structureID(), access.name(), SlotAccess::Read);
    return slot && (slot->kind == Slot::Kind::ClosureName || slot->kind == Slot::Kind::ClosureLength);
}

JSValue getAndCache(const NamedAccess& access, JSValue base, JSValue receiver, StaticFieldIsRead staticFieldIsRead)
{
    VM& vm = *access.instance->vm;
    JSGlobalObject* globalObject = access.instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& ident = access.ident;
    UniquedStringImpl* uid = ident.impl();
    bool shouldFile = !base.isCell() || access.caches().shouldFile(base.asCell()->structureID(), uid, SlotAccess::Read);
    Object* compiledBase = compiledObjectOrNull(base);
    if (!shouldFile || (base.isCell() && hasSlotForEveryClosure(access, base.asCell())) || (compiledBase && hasPendingStars(*compiledBase))) {
        JSValue result = getWithoutCaches(access.instance, base, ident, receiver);
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    if (compiledBase && compiledBase->orderWord() & orderWordHasFieldOutsideSlot) {
        if (const FieldData* field = compiledBase->fieldNamed(compiledBase->state(), ident); field && field->isAccessor() && !compiledBase->isInSlot(*field))
            compiledBase->tryMoveFieldBackToSlot(vm, *field);
    }
    if (base.isCell()) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, base.asCell());
        createStaticsOnChain(vm, base.asCell(), uid, staticFieldIsRead);
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
        PropertyAnswer filed = lazy.answer;
        answerByGetterField(base, structureBefore, filed);
        access.caches().cache({ structureBefore, access.name(), SlotAccess::Read }, filed);
        RELEASE_AND_RETURN(scope, readThroughAnswer(globalObject, receiver, lazy.answer));
    }
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    JSValue result = (lazy.firstObjectAbove ? JSValue(lazy.firstObjectAbove) : base).get(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, { });
    fileRead(access.instance, base, structureBefore, access.name(), slot);
    return result;
}

void fileReadWithoutCode(Instance* instance, JSObject* base, const Identifier& ident)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    NamedAccess access { instance, ident, std::nullopt };
    UniquedStringImpl* uid = ident.impl();
    Object* compiledBase = compiledObjectOrNull(base);
    if (!access.caches().shouldFile(base->structureID(), uid, SlotAccess::Read) || (compiledBase && hasPendingStars(*compiledBase)))
        return;
    Object::ensureStructureForExtraPropertiesIfNeeded(vm, base);
    LazyPrototypeRead lazy = classifyReadThroughLazyPrototypes(globalObject, base, uid);
    if (lazy.result == LazyPrototypeReadResult::NotApplicable && uid->isSymbol() && !isNeverCachedName(vm, uid)) {
        for (JSObject* object = base; object; object = object->getPrototypeDirect().getObject())
            ClassPrototype::ensureMembers(object);
    }
    Structure* structureBefore = base->structure();
    if (lazy.result == LazyPrototypeReadResult::Answered) {
        answerByGetterField(base, structureBefore, lazy.answer);
        access.caches().cache({ structureBefore, access.name(), SlotAccess::Read }, lazy.answer);
        return;
    }
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
    (lazy.firstObjectAbove ? lazy.firstObjectAbove : base)->getPropertySlot(globalObject, ident, slot);
    scope.assertNoException();
    if (!slot.isTaintedByOpaqueObject())
        fileRead(instance, base, structureBefore, access.name(), slot);
}

JSValue getUnderKeyForReceiver(Instance* instance, JSObject* base, const Identifier& name, JSValue receiver)
{
    NamedAccess access { instance, name, std::nullopt };
    if (!access.caches().tables().find(base->structureID(), access.name(), SlotAccess::Read))
        return getAndCache(access, base, receiver);
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    return JSValue(base).get(instance->globalObject, name, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdAndCache, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t key, uint32_t isInStubFrame))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByIdAndCache);
    ++instance->propertyCaches.slowPathReadCount;
    NamedAccess access { instance, identifierAt(instance, key), key };
    JSValue base = JSValue::decode(encodedBase);
    if (JSValue value = tryGetFromPermanentDictionary(instance, base, access.ident.impl()))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, access.ident));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    LookByName look(vm, base, access.ident.impl());
    OPERATION_RETURN(scope, JSValue::encode(getAndCache(access, base, base, isInStubFrame ? StaticFieldIsRead::InVariable : StaticFieldIsRead::AsProperty)));
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
    if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isImmutableProperty(uid) || isWriteJudgedByWriter(structure, uid) || structure->mayBePrototype()) {
        JSValue(object).putInline(access.instance->globalObject, access.ident, value, slot);
        return true;
    }
    structure->didReplaceProperty(offset);
    object->putDirectOffset(vm, offset, value);
    if (!structure->isUncacheableDictionary()) {
        slot.setExistingProperty(object, offset);
        fileStore(access.instance, object, structure, access.name(), slot, mode);
    }
    return true;
}

static bool tryStoreExtraPropertyAndCache(Instance* instance, Object& object, const CachedName& name, JSValue value, PutMode mode, const PutPropertySlot& slot, bool shouldFile)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = *instance->vm;
    OptionSet<Flag> flags { Flag::IsKnownNotToBeField };
    if (shouldFile) {
        object.ensureStructureForExtraProperties(vm);
        flags.add(Flag::NamesExtraPropertiesStructure);
    }
    Structure* structureBefore = WTF::opaque(object.structure());
    if (mode == PutMode::Define)
        flags.add(Flag::IsDefinition);
    if (object.tryStoreExtraProperty(vm, name.uid, value, flags).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    if (shouldFile)
        fileStore(instance, &object, structureBefore, name, slot, mode);
    return true;
}

static bool takesAnyValue(const ProgramTables& tables, const LayoutData& layout, const FieldData& field)
{
    if (field.isOutOfLine() || field.slot.representation() != SlotRepresentation::JSValue || !FieldTableEntry::hasColumn(field.name))
        return false;
    uint32_t index = tables.fieldTableRowOf(layout.shape) + field.name;
    return index < tables.fieldRecordIndices().size() && FieldTableEntry { tables.fieldTableEntries()[index] }.name() == field.name && tables.fieldRecordIndices()[index] & FieldRecordIndex::isStoredWithoutTypeTest;
}

bool canCreateFieldInSlot(VM& vm, const Object& object, const FieldData& field)
{
    constexpr uint16_t flagsLeftToSlowPath = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand;
    if (field.flags & flagsLeftToSlowPath || object.orderWord() & orderWordIntegrityBits)
        return false;
    if (field.flags & fieldMayExistWithoutStorage && object.isInKeyOrder(object.state(), field))
        return false;
    return !(object.orderWord() & orderWordHasFieldOutsideSlot) || !livesInExtraProperties(vm, object, field);
}

static void putToSlimClosureAndCache(const NamedAccess& access, SlimClosure& closure, JSValue value, PutMode mode)
{
    VM& vm = *access.instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* withoutProperty = closure.state().objects->closures.reifiedSlimClosureStructure(vm, closure.function());
    if (const Slot* filed = access.caches().findByKey(withoutProperty, access.ident.impl(), SlotAccess::Store); filed && filed->kind == Slot::Kind::Transition) {
        // As in operationAOTAddPropertyBySlot(): this pointer alone keeps the Structure alive across the allocations of the callee.
        closure.reifyWithAddedProperty(vm, WTF::opaque(filed->transition.newStructureID.decode()), filed->offsetInStorage(), value);
        holdClosureToLackingNameSlowCase(vm, &closure, access.ident.impl());
        return;
    }
    Structure* structureOfSlimClosures = WTF::opaque(closure.structure());
    closure.reify(vm);
    putAndCache(access, &closure, value, mode);
    RETURN_IF_EXCEPTION(scope, void());
    access.caches().cache({ structureOfSlimClosures, access.name(), accessFor(mode) }, PropertyAnswer::uncacheable(accessFor(mode)));
}

static bool orderGainedFieldAlone(State& state, const LayoutData& layout, uint32_t orderWordBefore, uint32_t orderWordAfter, const FieldData& field)
{
    if (isOrderStoredOutOfLine(orderWordBefore) || isOrderStoredOutOfLine(orderWordAfter) || countOf(orderWordAfter) != countOf(orderWordBefore) + 1)
        return false;
    std::span<const uint16_t> before = state.objects->keyOrders.entriesListedBy(layout, orderWordBefore);
    std::span<const uint16_t> after = state.objects->keyOrders.entriesListedBy(layout, orderWordAfter);
    return std::ranges::equal(before, after.first(before.size())) && after.back() == layout.indexOf(field);
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
    if (shouldFile && mode != PutMode::Define && cell->isObject() && cell->structure()->isDictionary() && !cell->structure()->isCompiledStructure()) {
        bool isStored = tryStoreToPermanentDictionary(access, asObject(cell), value, mode, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
    }

    Object* object = compiledObjectOrNull(base);
    const FieldData* field = object ? object->fieldNamed(object->state(), ident) : nullptr;
    if (field && hasPendingStars(*object))
        shouldFile = false;
    bool chainLetsStoresPass = false;
    if (field && mode != PutMode::Define && !object->isInSlot(*field))
        chainLetsStoresPass = access.caches().tables().addFilterAndCheckStoreCreatesOwnField(globalObject, *object, field->name);
    else if (field && mode != PutMode::Define && shouldFile && !object->compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        access.caches().tables().fileAbsenceFilterForStores(globalObject, object);
    if (object && !field) {
        bool isStored = tryStoreExtraPropertyAndCache(access.instance, *object, access.name(), value, mode, slot, shouldFile);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
    }
    bool createsOwnField = field && !shouldFile && !object->isInSlot(*field) && (mode == PutMode::Define || chainLetsStoresPass);
    if (createsOwnField && !uid->isSymbol() && canCreateFieldInSlot(vm, *object, *field) && tryCreateField(vm, object->state(), *object, *field, value))
        return;

    if (shouldFile)
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, cell);
    if (shouldFile && mode != PutMode::Define && isCompiledClosure(cell))
        uncheckedDowncast<Closure>(cell)->ensureClassObject(vm);
    // The store may give the cell another Structure, and then this pointer alone keeps the one before alive. The collector knows no StructureID on the stack, so the compiler must not make the pointer anew from the ID behind a call.
    Structure* structureBefore = WTF::opaque(cell->structure());
    const uint32_t orderWordBefore = object ? object->orderWord() : 0;
    bool repeatsAddition = object || !cell->isObject() || repeatsPropertyAddition(structureBefore, uid);
    if (mode == PutMode::Define && cell->isObject())
        defineDataProperty(access.instance, asObject(cell), ident, value, slot);
    else if (object)
        Object::putWithKnownField(cell, globalObject, ident, value, slot, field, chainLetsStoresPass);
    else
        base.putInline(globalObject, ident, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    if (!cell->isObject())
        return;
    bool isAddition = slot.type() == PutPropertySlot::NewProperty;
    if (beganOrEndedAsDictionary(structureBefore, cell->structure())) {
        access.caches().didFileNothing();
        return;
    }
    if (!shouldFile || (isAddition && !repeatsAddition)) {
        if (isAddition)
            access.caches().didAddProperty(structureBefore, cell->structure());
        return;
    }

    PropertyAnswer answer = classifyStore({ globalObject, asObject(cell), structureBefore, uid, slot, mode, object ? std::optional(orderWordBefore) : std::nullopt });
    if (object && answer.kind == PropertyAnswerKind::FieldCreation) {
        if (cell->structure() == structureBefore && !structureBefore->mayBePrototype() && takesAnyValue(*access.instance->image.tables, object->layout(), *answer.field) && orderGainedFieldAlone(object->state(), object->layout(), orderWordBefore, object->orderWord(), *answer.field))
            answer.orderWords = std::pair { orderWordBefore, object->orderWord() };
    }
    access.caches().cache({ structureBefore, access.name(), accessFor(mode) }, answer);
    ensureStillAliveHere(structureBefore);
}

bool tryRepeatFieldExit(Instance* instance, JSValue base, uint32_t key, JSValue value, PutMode mode)
{
    if (!base.isCell())
        return false;
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (const FieldReturn* back = caches.findFieldReturn(base.asCell()->structureID(), key))
        return uncheckedDowncast<Object>(base.asCell())->tryRepeat(*instance->vm, *back, value);
    FieldExit* exit = caches.findFieldExit(base.asCell()->structureID(), key);
    if (!exit || exit->isDefinition)
        return false;
    auto* object = uncheckedDowncast<Object>(base.asCell());
    if (!exit->wasInSlot && mode != PutMode::Define) {
        if (!caches.tables().storeCreatesOwnField(*object, key))
            return false;
        exit->generationForStores = caches.tables().absenceFilterGenerationForStores();
    }
    return object->tryRepeat(*instance->vm, *exit, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdAndCache, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t key, SiteCache, uint32_t putMode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    PutMode mode = static_cast<PutMode>(putMode);
    if (tryRepeatFieldExit(instance, base, key, value, mode))
        OPERATION_RETURN(scope);
    NamedAccess access { instance, identifierAt(instance, key), key };
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
    createStaticsOnChain(vm, constructor, hasInstanceSymbol.impl());
    Structure* structureBefore = WTF::opaque(constructor->structure());
    PropertySlot hasInstanceSlot(constructor, PropertySlot::InternalMethodType::Get);
    JSValue hasInstance = JSValue(constructor).get(globalObject, hasInstanceSymbol, hasInstanceSlot);
    RETURN_IF_EXCEPTION(scope, false);
    fileRead(instance, constructor, structureBefore, { hasInstanceSymbol.impl(), std::nullopt }, hasInstanceSlot);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructor->structure()->typeInfo().implementsDefaultHasInstance())
        RELEASE_AND_RETURN(scope, constructor->hasInstance(globalObject, value, hasInstance));

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

static void beginPeriodOfAnotherLength(Instance* instance)
{
    static constexpr uint32_t mostMissesLeftOut = 7;
    uint32_t& state = instance->state->propertyCaches->stateOfPeriodsOfSamples();
    state = state * 1664525 + 1013904223;
    instance->propertyCaches.missesOfEmptySites = static_cast<uint8_t>(SiteCacheState::sampleStep * ((state >> 24) & mostMissesLeftOut));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillSiteCache, void, (Instance* instance, JSCell* base, uint32_t key, SiteCache cache, uint32_t access))
{
    beginPeriodOfAnotherLength(instance);
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (!caches.siteCaches().didMissBefore(cache))
        return;
    LeafOperationScope leaf(*instance->vm);
    caches.fillSiteCacheFromTables(cache, base, key, static_cast<SlotAccess>(access));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillSiteAnswers, void, (Instance* instance, JSCell* base, uint32_t key, uint32_t distanceOfSite, uint32_t use))
{
    PropertyCaches& caches = *instance->state->propertyCaches;
    uint32_t place = caches.answersAtSites().placeAtDistanceFromInstance(distanceOfSite);
    if (use & tablesAreFiledForSiteWithoutAnswers) {
        if (base->isObject()) {
            LeafOperationScope leaf(*instance->vm);
            caches.fileOwnPlainValueUnlessFiled(asObject(base), key);
        }
        caches.answersAtSites().pointAtAnswersThatHoldNothingUnlessFilled(place);
        return;
    }
    if (use & visitorHitFurtherAnswer) {
        caches.answersAtSites().moveUpFurtherAnswer(place, static_cast<SiteUse>(use & ~visitorHitFurtherAnswer), base->structureID());
        return;
    }
    caches.answersAtSites().beginPeriodOfAnotherLength(place);
    if (!caches.answersAtSites().isDueForFilling(place))
        return;
    LeafOperationScope leaf(*instance->vm);
    constexpr uint32_t whatStubDoes = stubOfSiteFollowsFurtherAnswers | stubJumpsToArmOfAnswer | stubAsksForPresence;
    caches.fillSiteAnswers(place, base, key, static_cast<SiteUse>(use & ~whatStubDoes), use & stubOfSiteFollowsFurtherAnswers, use & stubJumpsToArmOfAnswer, use & stubAsksForPresence);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTClaimReadGroupRow, void, (Instance* instance, EncodedJSValue base, EncodedJSValue* group, uint32_t numberOfReads))
{
    LeafOperationScope leaf(*instance->vm);
    instance->state->propertyCaches->claimAndFillReadGroupRow(JSValue::decode(base), *std::bit_cast<ReadGroupCache*>(group), numberOfReads);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceOfAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceOfAndCache);
    OPERATION_RETURN(scope, instanceOfAndCache(instance, JSValue::decode(encodedValue), JSValue::decode(encodedConstructor)));
}

static bool isAcceptedBinaryData(Instance* instance, JSValue value, JSValue prototype)
{
    if (!instance->state->acceptsForeignBinaryData || !value.isCell() || !prototype.isObject())
        return false;
    JSGlobalObject* realm = instance->globalObject;
    JSType type = value.asCell()->type();
    JSObject* constructor = nullptr;
    if (isTypedArrayType(type))
        constructor = realm->typedArrayConstructor(typedArrayType(type));
    else if (type == ArrayBufferType && !uncheckedDowncast<JSArrayBuffer>(value.asCell())->isShared())
        constructor = realm->arrayBufferConstructor(ArrayBufferSharingMode::Default);
    VM& vm = *instance->vm;
    return constructor && constructor->getDirect(vm, vm.propertyNames->prototype) == prototype && constructor->getDirect(vm, vm.propertyNames->hasInstanceSymbol);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefaultHasInstance);
    JSValue value = JSValue::decode(encodedValue);
    JSValue prototype = JSValue::decode(encodedPrototype);
    bool isInstance = JSObject::defaultHasInstance(globalObject, value, prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, isInstance || isAcceptedBinaryData(instance, value, prototype));
}

static bool isPlainObjectBelowNoIndexedProperties(Structure* structure)
{
    if (structure->typeInfo().type() != FinalObjectType || hasAnyArrayStorage(structure->indexingType()) || structure->hasPolyProto())
        return false;
    JSValue prototype = structure->storedPrototype();
    JSGlobalObject* realm = structure->realm();
    return prototype.isNull() || (realm && prototype == realm->objectPrototype() && realm->objectPrototypeChainIsSane());
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
            if (isPlainObjectBelowNoIndexedProperties(structure))
                return jsUndefined();
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
    OPERATION_RETURN(scope, JSValue::encode(getByVal(globalObject, JSValue::decode(encodedBase), JSValue::decode(encodedProperty))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValOfPlainList, EncodedJSValue, (Instance* instance, EncodedJSValue encodedList, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByValOfPlainList);
    JSObject* list = asObject(JSValue::decode(encodedList));
    JSValue property = JSValue::decode(encodedProperty);
    PropertySlot slot(list, PropertySlot::InternalMethodType::Get);
    bool isFound;
    if (std::optional<uint32_t> index = indexOfKey(property))
        isFound = list->getPropertySlot(globalObject, *index, slot);
    else {
        auto key = property.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        isFound = list->getPropertySlot(globalObject, key, slot);
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!isFound)
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    RELEASE_ASSERT(slot.isValue());
    OPERATION_RETURN(scope, JSValue::encode(slot.getPureResult()));
}

static bool isOrdinaryObjectForReflectSet(Structure* structure)
{
    return structure->isCompiledStructure() || (!structure->typeInfo().overridesGetOwnPropertySlot() && !structure->typeInfo().overridesPut() && !structure->typeInfo().hasStaticPropertyTable());
}

static bool forwardsDefinitionsToTarget(JSGlobalObject* globalObject, JSValue handler)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!handler.isObject())
        return false;
    for (const Identifier* name : { &vm.propertyNames->getOwnPropertyDescriptor, &vm.propertyNames->defineProperty }) {
        PropertySlot slot(handler, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = asObject(handler)->getPropertySlot(globalObject, *name, slot);
        scope.assertNoException();
        if (isFound || slot.isTaintedByOpaqueObject())
            return false;
    }
    return true;
}

static bool isOrdinaryReflectSet(JSGlobalObject* globalObject, JSObject* target, JSValue receiver, const Identifier& name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (parseIndex(name) || !isOrdinaryObjectForReflectSet(target->structure()))
        return false;
    if (Object* compiled = compiledObjectOrNull(target); compiled && (hasPendingStars(*compiled) || compiled->isFrozen()))
        return false;
    PropertySlot slot(target, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = target->methodTable()->getOwnPropertySlot(target, globalObject, name, slot);
    scope.assertNoException();
    if (!isFound || slot.isTaintedByOpaqueObject() || !slot.isValue() || (slot.attributes() & PropertyAttribute::ReadOnly) || target->structure()->isImmutableProperty(name.impl()))
        return false;
    if (receiver == JSValue(target))
        return true;
    if (!receiver.isCell() || receiver.asCell()->type() != ProxyObjectType)
        return false;
    auto* proxy = uncheckedDowncast<ProxyObject>(receiver.asCell());
    return !proxy->isRevoked() && proxy->target() == target && forwardsDefinitionsToTarget(globalObject, proxy->handler());
}

static void cacheForwardingHandler(Instance* instance, JSObject* handler)
{
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && offsetOfOrderWord == sizeof(uint64_t));
    VM& vm = *instance->vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Structure* structure = handler->structure();
    Object* compiled = compiledObjectOrNull(handler);
    if (structure->isDictionary() || (compiled ? compiled->hasFlagsInOrderWord() : handler->type() != FinalObjectType))
        return;
    for (const Identifier* name : { &vm.propertyNames->getOwnPropertyDescriptor, &vm.propertyNames->defineProperty }) {
        getUnderKeyForReceiver(instance, handler, *name, handler);
        scope.assertNoException();
        const Slot* slot = instance->state->propertyCaches->findByKey(structure, name->impl(), SlotAccess::Read);
        if (!slot || handler->structure() != structure)
            return;
        if (slot->kind != Slot::Kind::Absent && (slot->kind != Slot::Kind::Field || !(slot->has(Slot::absentIsUndefined) || slot->has(Slot::emptySlotMayBeUndefined))))
            return;
    }
    ForwardingProxyHandlers::Entry& entry = instance->propertyCaches.forwardingProxyHandlers.entries[ForwardingProxyHandlers::offsetFor(structure->id().bits()) >> ForwardingProxyHandlers::sizeShift];
    entry.wordAfterHeader = *std::bit_cast<const uint64_t*>(std::bit_cast<const char*>(handler) + JSObject::butterflyOffset());
    entry.structureID = structure->id().bits();
    instance->propertyCaches.placesSayAbsence = 1;
}

JSC_DEFINE_JIT_OPERATION(operationAOTReflectSet, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, EncodedJSValue encodedReceiver))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReflectSet);
    JSValue value = JSValue::decode(encodedTarget);
    JSValue property = JSValue::decode(encodedProperty);
    JSValue receiver = JSValue::decode(encodedReceiver);
    auto resultOf = [](ReflectSetResult result) { return static_cast<size_t>(result); };
    if (!value.isObject()) {
        throwTypeError(globalObject, scope, "Reflect.set requires the first argument be an object"_s);
        OPERATION_RETURN(scope, resultOf(ReflectSetResult::Refused));
    }
    JSObject* target = asObject(value);
    auto name = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, resultOf(ReflectSetResult::Refused));
    bool convertsWithoutOtherCode = property.isString() || property.isSymbol();
    if (convertsWithoutOtherCode && isOrdinaryReflectSet(globalObject, target, receiver, name)) {
        if (receiver != value)
            cacheForwardingHandler(instance, asObject(uncheckedDowncast<ProxyObject>(receiver.asCell())->handler()));
        OPERATION_RETURN(scope, resultOf(ReflectSetResult::IsOrdinaryStore));
    }
    if (receiver == value && parseIndex(name) && target->structure()->hasImmutableIndexedProperties()) [[unlikely]]
        OPERATION_RETURN(scope, resultOf(ReflectSetResult::Refused));
    PutPropertySlot slot(receiver, false, PutPropertySlot::ReflectSet);
    bool isStored = target->methodTable()->put(target, globalObject, name, JSValue::decode(encodedValue), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, resultOf(ReflectSetResult::Refused));
    OPERATION_RETURN(scope, resultOf(isStored ? ReflectSetResult::Stored : ReflectSetResult::Refused));
}

static const ProxyAccessWords& proxyAccessWords(EncodedJSValue* words) { return *std::bit_cast<const ProxyAccessWords*>(words); }

JSC_DEFINE_JIT_OPERATION(operationAOTGetWithProxyTrap, EncodedJSValue, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue trap))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetWithProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    if (uint32_t number = static_cast<uint32_t>(words.storeIsStrictOrHeaderOfSlot >> Slot::locationShiftInHeader); number && JSValue::decode(trap).isUndefinedOrNull())
        OPERATION_RETURN(scope, JSValue::encode(getAndCache({ instance, identifierAt(instance, number), number }, words.target, words.proxy)));
    OPERATION_RETURN(scope, JSValue::encode(ProxyObject::getWithTrap(globalObject, words.target, words.handler, JSValue::decode(trap), words.key, words.proxy)));
}

struct FixedProperty {
    bool isFound { false };
    unsigned attributes { 0 };
    JSValue value;
};

static std::optional<FixedProperty> findFixedProperty(VM& vm, JSObject* target, JSCell* key)
{
    if (!key->isString())
        return std::nullopt;
    StringImpl* impl = const_cast<StringImpl*>(uncheckedDowncast<JSString>(key)->tryGetValueImpl());
    if (!impl || !impl->isAtom() || parseIndex(*impl))
        return std::nullopt;
    auto* name = static_cast<UniquedStringImpl*>(impl);
    JSObject* holder = target;
    if (target->type() == CompiledObjectType) {
        auto* object = uncheckedDowncast<Object>(target);
        if (object->isFrozen() || object->isSealed() || !object->compiledStructure().hasOnlyConfigurableFields())
            return std::nullopt;
        holder = extraPropertiesIfCreated(*object);
        if (!holder)
            return object->hasExtraProperties() ? std::nullopt : std::optional(FixedProperty { });
    }
    Structure* structure = holder->structure();
    if (structure->typeInfo().overridesGetOwnPropertySlot() || structure->typeInfo().hasStaticPropertyTable())
        return std::nullopt;
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, name, attributes);
    if (!isValidOffset(offset) || !(attributes & PropertyAttribute::DontDelete))
        return FixedProperty { };
    return FixedProperty { true, attributes, holder->getDirect(offset) };
}

static bool isTrapResultAllowed(VM& vm, JSObject* target, JSCell* key, JSValue answer)
{
    std::optional<FixedProperty> fixed = findFixedProperty(vm, target, key);
    if (!fixed || !fixed->isFound)
        return !!fixed;
    if (fixed->attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return false;
    return !(fixed->attributes & PropertyAttribute::ReadOnly) || fixed->value == answer;
}

static bool mayTrapDenyProperty(VM& vm, JSObject* target, JSCell* key)
{
    bool isExtensible = target->type() == CompiledObjectType ? !uncheckedDowncast<Object>(target)->isNotExtensible() : target->type() == FinalObjectType && target->structure()->isStructureExtensible();
    if (!isExtensible)
        return false;
    std::optional<FixedProperty> fixed = findFixedProperty(vm, target, key);
    return fixed && !fixed->isFound;
}

JSC_DEFINE_JIT_OPERATION(operationAOTValidateAnswerOfProxyTrap, EncodedJSValue, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValidateAnswerOfProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    if (isTrapResultAllowed(vm, words.target, words.key, JSValue::decode(answer)))
        OPERATION_RETURN(scope, answer);
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
    if (JSValue::decode(answer).isFalse() && mayTrapDenyProperty(vm, words.target, words.key)) {
        countEvent(vm, RuntimeCounterID::ProxyTrapAnswersAdmitted);
        OPERATION_RETURN(scope, false);
    }
    OPERATION_RETURN(scope, ProxyObject::finishHas(globalObject, JSValue::decode(answer), words.target, words.key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetWithProxyTrap, void, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue trap))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetWithProxyTrap);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    ProxyObject::setWithTrap(globalObject, words.target, words.handler, JSValue::decode(trap), words.key, JSValue::decode(words.storedValue), words.proxy, words.storeIsStrictOrHeaderOfSlot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFinishAccessThroughProxy, void, (Instance* instance, EncodedJSValue target, EncodedJSValue answer, EncodedJSValue stored, EncodedJSValue encodedName, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFinishAccessThroughProxy);
    JSValue name = JSValue::decode(encodedName);
    if (JSValue value = JSValue::decode(stored))
        ProxyObject::finishSet(globalObject, JSValue::decode(answer), asObject(JSValue::decode(target)), name, value, isStrict);
    else
        ProxyObject::validateGetTrapResultWhereBound(globalObject, JSValue::decode(answer), asObject(JSValue::decode(target)), name);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFinishSetThroughProxy, void, (Instance* instance, EncodedJSValue* wordsOfAccess, EncodedJSValue answer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFinishSetThroughProxy);
    const ProxyAccessWords& words = proxyAccessWords(wordsOfAccess);
    if (JSValue::decode(answer).isTrue() && isTrapResultAllowed(vm, words.target, words.key, JSValue::decode(words.storedValue))) {
        countEvent(vm, RuntimeCounterID::ProxyTrapAnswersAdmitted);
        OPERATION_RETURN(scope);
    }
    ProxyObject::finishSet(globalObject, JSValue::decode(answer), words.target, words.key, JSValue::decode(words.storedValue), words.storeIsStrictOrHeaderOfSlot);
    OPERATION_RETURN(scope);
}

static void putByVal(Instance* instance, JSValue base, JSValue property, JSValue value, bool isStrict)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        property.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, property));
        return;
    }
    if (std::optional<uint32_t> index = indexOfKey(property)) {
        if (base.isCell() && base.asCell()->type() == ProxyObjectType && *index < NumericStrings::cacheSize)
            vm.numericStrings.addJSString(vm, static_cast<int>(*index));
        scope.release();
        if (!base.isObject() || !asObject(base)->trySetIndexQuickly(vm, *index, value))
            base.putByIndex(globalObject, *index, value, isStrict);
        return;
    }
    auto key = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (tryReplaceGlobalProperty(instance, vm, base, key.impl(), value))
        return;
    PutPropertySlot slot(base, isStrict);
    scope.release();
    base.put(globalObject, key, value, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByVal);
    if (Object* object = compiledObjectOrNull(JSValue::decode(encodedBase))) {
        std::optional<uint32_t> index = JSValue::decode(encodedProperty).tryGetAsUint32Index();
        if (index && (object->hasExtraProperties() ? trySetElementOfCompiledObject(instance, object, *index, JSValue::decode(encodedValue)) : !object->needsSlowPutIndexing() && tryDefineElementOfCompiledObject(instance, object, *index, JSValue::decode(encodedValue))))
            OPERATION_RETURN(scope);
    }
    putByVal(instance, JSValue::decode(encodedBase), JSValue::decode(encodedProperty), JSValue::decode(encodedValue), isStrict);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTOwnValueUnderKeyOrEmpty, EncodedJSValue, (EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || !property.isString() || asString(property)->isRope())
        return JSValue::encode(JSValue());
    Structure* structure = base.asCell()->structure();
    if (structure->isCompiledStructure() || structure->typeInfo().overridesGetOwnPropertySlot())
        return JSValue::encode(JSValue());
    StringImpl* text = asString(property)->tryGetValueImpl();
    RefPtr<AtomStringImpl> name = text ? AtomStringImpl::lookUp(text) : nullptr;
    if (!name || parseIndex(*name))
        return JSValue::encode(JSValue());
    unsigned attributes = 0;
    PropertyOffset offset = structure->getConcurrently(name.get(), attributes);
    if (!isValidOffset(offset) || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
        return JSValue::encode(JSValue());
    return JSValue::encode(asObject(base)->getDirect(offset));
}

static RefPtr<UniquedStringImpl> nameOfExistingKey(JSValue property)
{
    if (property.isSymbol())
        return &asSymbol(property)->uid();
    if (!property.isString() || asString(property)->isRope())
        return nullptr;
    StringImpl* text = asString(property)->tryGetValueImpl();
    RefPtr<AtomStringImpl> name = text ? AtomStringImpl::lookUp(text) : nullptr;
    if (!name || parseIndex(*name))
        return nullptr;
    return RefPtr<UniquedStringImpl>(WTF::move(name));
}

static uint64_t addressOfReplaceableValue(JSObject* holder, UniquedStringImpl* name)
{
    constexpr unsigned isNotReplaceable = PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue;
    Structure* structure = holder->structure();
    if (structure->mayBePrototype() || structure->mayHaveImmutableProperties() || structure->isWatchingReplacement())
        return 0;
    unsigned attributes = 0;
    auto offset = structure->getWithoutMaterializing(name, attributes);
    if (!offset || !isValidOffset(*offset) || attributes & isNotReplaceable)
        return 0;
    return std::bit_cast<uint64_t>(holder->locationForOffset(*offset));
}

static uint64_t addressInCompiledObject(Object& object, UniquedStringImpl* name, bool findsOnlyFields, bool fitsFieldTypes)
{
    constexpr unsigned isNotReplaceable = fieldIsReadOnly | fieldIsAccessor | fieldIsPrivate;
    if (object.structure()->mayBePrototype() || object.isFrozen() || object.isSealed() || hasPendingStars(object))
        return 0;
    if (const FieldData* field = object.fieldNamed(object.state(), name)) {
        bool isBoxed = field->slot.representation() == SlotRepresentation::JSValue && !field->isOutOfLine() && !field->holdsRawValueFromBirth;
        bool takesValue = isBoxed && (fitsFieldTypes || object.state().objects->layouts.typeOf(*field).takesEveryValue());
        if (field->flags & isNotReplaceable || !takesValue || !object.isInSlot(*field))
            return 0;
        return std::bit_cast<uint64_t>(object.slotOf(*field));
    }
    JSObject* extra = findsOnlyFields ? nullptr : object.extraProperties();
    return extra ? addressOfReplaceableValue(extra, name) : 0;
}

static uint64_t addressInGlobalObject(JSObject* proxy, UniquedStringImpl* name)
{
    if (proxy->classInfo() != JSGlobalProxy::info())
        return 0;
    JSGlobalObject* target = uncheckedDowncast<JSGlobalProxy>(proxy)->target();
    Structure* structure = target->structure();
    Instance* instance = Instance::of(target);
    if (const Slot* filed = instance ? instance->state->propertyCaches->findByKey(structure, name, SlotAccess::Store) : nullptr) {
        bool isOwnValue = filed->has(Slot::isOwn) && (filed->kind == Slot::Kind::Value || filed->kind == Slot::Kind::FarValue);
        return isOwnValue ? std::bit_cast<uint64_t>(target->locationForOffset(filed->offsetInStorage())) : 0;
    }
    unsigned attributes = 0;
    auto offset = structure->getWithoutMaterializing(name, attributes);
    if (!offset || !isValidOffset(*offset) || attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return 0;
    if (!structure->propertyAccessesAreCacheable() || structure->isImmutableProperty(name) || isWriteJudgedByWriter(structure, name))
        return 0;
    WatchpointSet* replacements = structure->propertyReplacementWatchpointSet(*offset);
    if (!replacements || !replacements->hasBeenInvalidated())
        return 0;
    return std::bit_cast<uint64_t>(target->locationForOffset(*offset));
}

static uint64_t addressInFunction(Closure& function, UniquedStringImpl* name)
{
    VM& vm = function.vm();
    const CommonIdentifiers& names = *vm.propertyNames;
    if (function.classData() || name == names.prototype.impl() || name == names.name.impl() || name == names.length.impl() || function.refusesOwnProperty(vm, name))
        return 0;
    return addressOfReplaceableValue(&function, name);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTOwnPropertyAddress, uint64_t, (EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t findsOnlyFields, uint32_t fitsFieldTypes, uint32_t takesFunctions))
{
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject())
        return 0;
    RefPtr<UniquedStringImpl> name = nameOfExistingKey(JSValue::decode(encodedProperty));
    if (!name || name == base.asCell()->vm().propertyNames->underscoreProto.impl())
        return 0;
    if (Object* object = compiledObjectOrNull(base))
        return addressInCompiledObject(*object, name.get(), findsOnlyFields, fitsFieldTypes);
    if (findsOnlyFields)
        return 0;
    JSObject* object = asObject(base);
    if (object->type() == GlobalProxyType)
        return addressInGlobalObject(object, name.get());
    if (takesFunctions && isCompiledClosure(object))
        return addressInFunction(*uncheckedDowncast<Closure>(object), name.get());
    if (object->type() != FinalObjectType || object->structure()->isCompiledStructure() || object->structure()->typeInfo().overridesGetOwnPropertySlot() || !object->canPerformFastPutInlineExcludingProto())
        return 0;
    return addressOfReplaceableValue(object, name.get());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMovedStaticField, EncodedJSValue, (Instance* instance, JSCell* classObject, uint32_t key))
{
    LeafOperationScope leaf(*instance->vm);
    unsigned attributes = 0;
    auto offset = classObject->structure()->getWithoutMaterializing(instance->state->propertyCaches->nameOf(key).uid, attributes);
    RELEASE_ASSERT(offset && isValidOffset(*offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
    return JSValue::encode(asObject(classObject)->getDirect(*offset));
}

static NEVER_INLINE uint64_t searchOwnNamedPropertyAddress(PropertyCaches& caches, JSValue base, uint32_t key, uint32_t fitsFieldTypes)
{
    if (!base.isObject())
        return 0;
    UniquedStringImpl* name = caches.nameOf(key).uid;
    if (Object* object = compiledObjectOrNull(base))
        return addressInCompiledObject(*object, name, false, fitsFieldTypes);
    JSObject* object = asObject(base);
    if (object->type() == GlobalProxyType)
        return addressInGlobalObject(object, name);
    Structure* structure = object->structure();
    if (object->type() != FinalObjectType || structure->isCompiledStructure() || structure->typeInfo().overridesGetOwnPropertySlot() || !object->canPerformFastPutInlineExcludingProto())
        return 0;
    uint64_t address = addressOfReplaceableValue(object, name);
    if (address && !structure->isDictionary())
        caches.fileOwnPlainValueUnlessFiled(object, key);
    return address;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTOwnNamedPropertyAddress, uint64_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key, uint32_t fitsFieldTypes, SiteCache cache))
{
    JSValue base = JSValue::decode(encodedBase);
    LeafOperationScope leaf(*instance->vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (base.isCell() && base.asCell()->type() == FinalObjectType) {
        JSObject* object = asObject(base);
        Structure* structure = object->structure();
        bool mayBeReplaced = !structure->isCompiledStructure() && !structure->isDictionary() && !structure->mayBePrototype() && !structure->mayHaveImmutableProperties() && !structure->isWatchingReplacement()
            && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() && !structure->typeInfo().overridesGetOwnPropertySlot();
        UniquedStringImpl* name = caches.nameOf(key).uid;
        const Slot* filed = mayBeReplaced ? caches.findByKey(structure, name, SlotAccess::Read) : nullptr;
        if (filed && filed->kind == Slot::Kind::Value && filed->has(Slot::isOwn)) {
            PropertyOffset offset = filed->offsetInStorage();
            if (cache && caches.siteCaches().mayBeFilledAfterMiss(cache))
                caches.fillStoreSiteCache(cache, structure, name, offset);
            if (cache && !caches.findByKey(structure, name, SlotAccess::Store) && caches.shouldFile(structure->id(), name, SlotAccess::Store)) {
                structure->didCachePropertyReplacement(*instance->vm, offset);
                caches.cache({ structure, caches.nameOf(key), SlotAccess::Store }, PropertyAnswer::own(PropertyAnswerKind::Value, SlotAccess::Store, offset));
            }
            return std::bit_cast<uint64_t>(object->locationForOffset(offset));
        }
    }
    return searchOwnNamedPropertyAddress(caches, base, key, fitsFieldTypes);
}

static JSValue ownDataValue(JSObject* holder, UniquedStringImpl* name)
{
    unsigned attributes = 0;
    auto offset = holder->structure()->getWithoutMaterializing(name, attributes);
    return offset && isValidOffset(*offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) ? holder->getDirect(*offset) : JSValue();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTOwnValueByKeyOrEmpty, EncodedJSValue, (EncodedJSValue encodedBase, EncodedJSValue encodedProperty, uint32_t findsOnlyFields))
{
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject())
        return JSValue::encode(JSValue());
    RefPtr<UniquedStringImpl> name = nameOfExistingKey(JSValue::decode(encodedProperty));
    if (!name || name == base.asCell()->vm().propertyNames->underscoreProto.impl())
        return JSValue::encode(JSValue());
    if (Object* object = compiledObjectOrNull(base)) {
        if (hasPendingStars(*object))
            return JSValue::encode(JSValue());
        if (const FieldData* field = object->fieldNamed(object->state(), name.get())) {
            bool isData = !(field->flags & (fieldIsAccessor | fieldIsPrivate)) && field->slot.representation() != SlotRepresentation::DeferredClosure;
            return JSValue::encode(isData && object->isInSlot(*field) ? object->readInline(*field) : JSValue());
        }
        JSObject* extra = findsOnlyFields ? nullptr : object->extraProperties();
        return JSValue::encode(extra ? ownDataValue(extra, name.get()) : JSValue());
    }
    if (findsOnlyFields)
        return JSValue::encode(JSValue());
    JSObject* object = asObject(base);
    if (object->type() == GlobalProxyType && object->classInfo() == JSGlobalProxy::info())
        return JSValue::encode(ownDataValue(uncheckedDowncast<JSGlobalProxy>(object)->target(), name.get()));
    if (object->type() != FinalObjectType || object->structure()->isCompiledStructure() || object->structure()->typeInfo().overridesGetOwnPropertySlot())
        return JSValue::encode(JSValue());
    return JSValue::encode(ownDataValue(object, name.get()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBarrierAfterKeyedStore, void, (EncodedJSValue encodedBase))
{
    JSObject* object = asObject(JSValue::decode(encodedBase));
    VM& vm = object->vm();
    vm.writeBarrier(object);
    if (Object* compiled = compiledObjectOrNull(object)) {
        if (JSObject* extra = compiled->extraProperties())
            vm.writeBarrier(extra);
    } else if (object->type() == GlobalProxyType)
        vm.writeBarrier(uncheckedDowncast<JSGlobalProxy>(object)->target());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasDefaultHasInstance, size_t, (EncodedJSValue encodedConstructor))
{
    JSValue constructor = JSValue::decode(encodedConstructor);
    if (!constructor.isObject())
        return false;
    JSObject* current = asObject(constructor);
    UniquedStringImpl* name = current->vm().propertyNames->hasInstanceSymbol.impl();
    JSFunction* ofEngine = current->globalObject()->functionProtoHasInstanceSymbolFunction();
    while (true) {
        if (current->type() == ProxyObjectType)
            return false;
        Structure* structure = current->structure();
        unsigned attributes = 0;
        PropertyOffset offset = structure->getConcurrently(name, attributes);
        if (isValidOffset(offset))
            return !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) && current->getDirect(offset) == JSValue(ofEngine);
        JSValue above = structure->storedPrototype(current);
        if (!above.isObject())
            return true;
        current = asObject(above);
    }
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
    if (Object* compiledObject = compiledObjectOrNull(base); compiledObject && hasPendingStars(*compiledObject))
        shouldFile = false;
    if (object && shouldFile) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
        createStaticsOnChain(vm, object, name.impl());
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

    if (compiledObjectOrNull(property)) [[unlikely]] {
        property = primitiveKeyIfKnown(instance, property);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (JSValue found = property.isString() || property.isSymbol() ? JSValue::decode(operationAOTGetByValGeneric(instance, base.asCell(), property.asCell())) : JSValue())
            OPERATION_RETURN(scope, JSValue::encode(found));
    }
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

    if (base.isObject() && compiledObjectOrNull(property)) [[unlikely]] {
        property = primitiveKeyIfKnown(instance, property);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, putByVal(instance, base, property, value, isStrict));
    }
    std::optional<Identifier> name;
    if (base.isObject() && property.isString()) {
        Identifier identifier = asString(property)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (!parseIndex(*identifier.impl()))
            name = WTF::move(identifier);
    } else if (base.isObject())
        name = nameForKeyedCache(globalObject, property);
    if (!name)
        RELEASE_AND_RETURN(scope, putByVal(instance, base, property, value, isStrict));

    JSObject* object = asObject(base);
    CachedName cachedName { name->impl(), std::nullopt };
    PropertyCaches& caches = *instance->state->propertyCaches;
    bool shouldFile = caches.shouldFile(object->structureID(), name->impl(), accessFor(mode));
    Object* compiledObject = compiledObjectOrNull(base);
    const FieldData* field = compiledObject ? compiledObject->fieldNamed(compiledObject->state(), *name) : nullptr;
    if (compiledObject && !field && tryStoreExtraPropertyAndCache(instance, *compiledObject, cachedName, value, mode, slot, shouldFile))
        return;

    if (field && hasPendingStars(*compiledObject))
        shouldFile = false;
    else
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
    createClassObjectsOnChain(vm, object);
    bool chainLetsStoresPass = field && !compiledObject->isInSlot(*field) && caches.tables().addFilterAndCheckStoreCreatesOwnField(globalObject, *compiledObject, field->name);
    if (chainLetsStoresPass && !shouldFile && !name->impl()->isSymbol() && canCreateFieldInSlot(vm, *compiledObject, *field) && tryCreateField(vm, compiledObject->state(), *compiledObject, *field, value))
        return;
    Structure* structureBefore = WTF::opaque(object->structure());
    std::optional<uint32_t> orderWordBefore = compiledObject ? std::optional(compiledObject->orderWord()) : std::nullopt;
    bool repeatsAddition = compiledObject || repeatsPropertyAddition(structureBefore, name->impl());
    if (compiledObject)
        Object::putWithKnownField(object, globalObject, *name, value, slot, field, chainLetsStoresPass);
    else
        base.put(globalObject, *name, value, slot);
    RETURN_IF_EXCEPTION(scope, void());
    bool isAddition = slot.type() == PutPropertySlot::NewProperty;
    if (beganOrEndedAsDictionary(structureBefore, object->structure()))
        caches.didFileNothing();
    else if (shouldFile && (!isAddition || repeatsAddition))
        fileStore(instance, object, structureBefore, cachedName, slot, mode, orderWordBefore);
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
        if (!isIn && (slot.kind == Slot::Kind::FieldOrInherited || slot.kind == Slot::Kind::FieldOrInheritedField))
            return !slot.has(Slot::emptySlotMayBeUndefined);
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

static PropertySlot::InternalMethodType lookupThatFindsOwnProperty(bool starsWait)
{
    return starsWait ? PropertySlot::InternalMethodType::HasProperty : PropertySlot::InternalMethodType::GetOwnProperty;
}

static bool hasStarsThatWait(JSObject* object)
{
    Object* compiledObject = compiledObjectOrNull(object);
    return compiledObject && hasPendingStars(*compiledObject);
}

static bool hasAndCache(Instance* instance, JSObject* object, const CachedName& name, bool isIn)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyCaches& caches = *instance->state->propertyCaches;
    PropertyName propertyName(name.uid);
    bool shouldFile = (isIn || object->type() != ProxyObjectType) && caches.shouldFile(object->structureID(), name.uid, SlotAccess::Read);
    bool starsWait = hasStarsThatWait(object);
    shouldFile &= !starsWait;
    if (shouldFile) {
        std::optional<Slot> known = caches.tables().find(object->structureID(), name, SlotAccess::Read);
        shouldFile = !known || (known->kind != Slot::Kind::Uncacheable && known->kind != Slot::fieldOfGetter);
    }
    if (shouldFile) {
        Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
        createStaticsOnChain(vm, object, name.uid);
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
        PropertySlot slot(object, lookupThatFindsOwnProperty(starsWait));
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
    if (compiledObjectOrNull(property)) [[unlikely]] {
        property = primitiveKeyIfKnown(instance, property);
        RETURN_IF_EXCEPTION(scope, false);
        auto presence = property.isString() || property.isSymbol() ? static_cast<LeafPresence>(operationAOTInByValGeneric(instance, object, property.asCell())) : LeafPresence::TakeSlowPath;
        if (presence != LeafPresence::TakeSlowPath)
            return presence == LeafPresence::Own || (isIn && presence == LeafPresence::Inherited);
    }
    Identifier identifier = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if ((!property.isString() && !property.isSymbol()) || parseIndex(identifier)) {
        if (isIn)
            RELEASE_AND_RETURN(scope, object->hasProperty(globalObject, identifier));
        PropertySlot slot(object, lookupThatFindsOwnProperty(hasStarsThatWait(object)));
        RELEASE_AND_RETURN(scope, object->methodTable()->getOwnPropertySlot(object, globalObject, identifier, slot));
    }
    RELEASE_AND_RETURN(scope, hasAndCache(instance, object, { identifier.impl(), std::nullopt }, isIn));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByIdAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createKeyTestOfNonObjectError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, hasAndCache(instance, asObject(base), instance->state->propertyCaches->nameOf(key), true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createKeyTestOfNonObjectError(globalObject, base));
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
