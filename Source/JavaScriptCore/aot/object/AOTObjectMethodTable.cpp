/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTCompiledCallee.h"
#include "AOTErrorMessages.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "CustomGetterSetter.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "ProxyObject.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include <wtf/Scope.h>

namespace JSC { namespace AOT {

static constexpr RuntimeCounterID counterFor(SlotExitReason reason)
{
    switch (reason) {
    case SlotExitReason::Delete:
        return RuntimeCounterID::FieldsEvictedByDelete;
    case SlotExitReason::Redefinition:
        return RuntimeCounterID::FieldsEvictedByRedefinition;
    case SlotExitReason::StoreOfOtherType:
        return RuntimeCounterID::FieldsEvictedByTypeMismatch;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

void Object::moveFieldToExtraProperties(VM& vm, const FieldData& field, LeavingProperty leaving, SlotExitReason reason)
{
    if ((field.getterIsProven || field.setterIsProven) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]] {
        if (refusesMutation(vm, *this, mutationOf(reason)))
            return;
        crashOnImpossibleMutation(vm, *this, field, mutationOf(reason));
    }
    if (reason != SlotExitReason::Delete) {
        const FieldExit* exit = state().propertyCaches->findFieldExit(structureID(), field.name);
        if (exit && exit->isDefinition == (reason == SlotExitReason::Redefinition) && exit->attributes == leaving.attributes && tryRepeat(vm, *exit, leaving.valueOrAccessor))
            return;
    }
    if (!admitsMutation(vm, *this, field, mutationOf(reason)))
        return;
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    JSObject* extraBefore = extraPropertiesIfCreated(*this);
    FieldExit exit { .field = &field, .structureBefore = structureID(), .extraPropertiesBefore = extraBefore ? extraBefore->structureID() : StructureID(), .orderWordBefore = orderWord(), .name = field.name, .wasInSlot = isInSlot(field) };
    bool hadOtherCellInPlaceOfExtraProperties = !extraBefore && hasExtraProperties();
    JSObject* extra = ensureExtraProperties(vm);
    RELEASE_ASSERT(extra);
    State& state = this->state();
    if (!isInKeyOrder(state, field))
        didCreateField(state, field, false);
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasFieldOutsideSlot;
    setBearsMark(true);
    putProperty(vm, extra, name, leaving);
    clear(field);
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    countFieldThatLeftSlot(vm, layout().index, indexOf(field), counterFor(reason), leaving.valueOrAccessor);

    exit.isDefinition = reason == SlotExitReason::Redefinition;
    exit.attributes = static_cast<uint8_t>(leaving.attributes);
    if (reason == SlotExitReason::Delete || (!exit.isDefinition && leaving.attributes) || exit.attributes != leaving.attributes || hadOtherCellInPlaceOfExtraProperties || field.isPrivate() || hasOutOfLineKeyOrder() || !compiledStructure().extraPropertiesSlot())
        return;
    Structure* extraStructure = extra->structure();
    if (structure()->mayBePrototype() || extraStructure->isDictionary())
        return;
    unsigned attributes;
    exit.offset = extraStructure->get(vm, name, attributes);
    if (!isValidOffset(exit.offset) || attributes != leaving.attributes)
        return;
    if (!isInlineOffset(exit.offset) && (!exit.isDefinition || !extraBefore))
        return;
    exit.structureAfter = structureID();
    exit.extraPropertiesAfter = extraStructure->id();
    exit.orderWordAfter = orderWord();
    exit.extraPropertiesCellSize = JSFinalObject::allocationSize(extraStructure->inlineCapacity());
    exit.offsetOfExtraPropertiesWord = compiledStructure().extraPropertiesSlot()->offset;
    SlotTraits traits = traitsOf(field.slot.representation());
    if (!exit.isDefinition && !field.isOutOfLine() && absentPatternOf(field.slot.representation()) && traits.width >= sizeof(uint32_t)) {
        exit.absentBits = traits.absentBits;
        exit.offsetOfSlot = field.slot.offset;
        exit.widthOfSlot = traits.width | (mayBearMark() ? FieldExit::objectMayBearMark : 0);
    }
    state.propertyCaches->remember(exit);
}

bool Object::tryRepeat(VM& vm, const FieldExit& exit, JSValue value)
{
    const FieldData& field = *exit.field;
    const bool holdsBoxedValueInCell = field.slot.representation() == SlotRepresentation::JSValue && !field.isOutOfLine();
    const bool isInItsSlot = holdsBoxedValueInCell ? !!at<EncodedJSValue>(field.slot.offset) : isInSlot(field);
    if (structureID() != exit.structureBefore || orderWord() != exit.orderWordBefore || isInFrame() || isInItsSlot != exit.wasInSlot)
        return false;
    if (!exit.isDefinition && state().objects->layouts.slotAdmits(field, value))
        return false;
    SlotExitReason reason = exit.isDefinition ? SlotExitReason::Redefinition : SlotExitReason::StoreOfOtherType;
    JSObject* extra = extraPropertiesIfCreated(*this);
    if (exit.extraPropertiesBefore ? !extra || extra->structureID() != exit.extraPropertiesBefore : hasExtraProperties())
        return false;
    if (!admitsMutation(vm, *this, field, mutationOf(reason)))
        return true;
    // A collection forgets the exit, and then these pointers alone keep the Structures alive.
    auto* structureAfter = WTF::opaque(uncheckedDowncast<CompiledStructure>(exit.structureAfter.decode()));
    Structure* extraStructureAfter = WTF::opaque(exit.extraPropertiesAfter.decode());
    uint32_t orderWordAfter = exit.orderWordAfter;
    PropertyOffset offset = exit.offset;
    if (extra) {
        if (!isInlineOffset(offset)) [[unlikely]]
            extra->growOutOfLineStorageForTransition(vm, extraStructureAfter);
        [[clang::always_inline]] extra->putDirectOffset(vm, offset, value);
        [[clang::always_inline]] extra->setStructure(vm, extraStructureAfter);
        [[clang::always_inline]] setVariantOfStructure(vm, structureAfter);
    } else {
        extra = constructEmptyObject(vm, extraStructureAfter);
        extra->putDirectOffset(vm, offset, value);
        setExtraProperties(vm, extra, structureAfter);
    }
    at<uint32_t>(offsetOfOrderWord) = orderWordAfter;
    [[clang::always_inline]] setBearsMark(true);
    if (holdsBoxedValueInCell)
        at<EncodedJSValue>(field.slot.offset) = JSValue::encode(JSValue());
    else
        clear(field);
    countFieldThatLeftSlot(vm, layout().index, indexOf(field), counterFor(reason), value);
    ensureStillAliveHere(structureAfter);
    ensureStillAliveHere(extraStructureAfter);
    return true;
}

static bool tryUndoAdditionOfLastProperty(VM& vm, JSObject* extra, UniquedStringImpl* name, PropertyOffset& offset)
{
    Structure* structure = extra->structure();
    Structure* previous = structure->previousID();
    if (!previous || structure->isDictionary() || structure->transitionKind() != TransitionKind::PropertyAddition || structure->transitionPropertyName() != name)
        return false;
    offset = structure->transitionOffset();
    if (!isInlineOffset(offset) || previous->isDictionary() || previous->inlineCapacity() != structure->inlineCapacity() || previous->typeInfoBlob() != structure->typeInfoBlob())
        return false;
    structure->didTransitionFromThisStructureWithoutFiringWatchpoint();
    structure->fireStructureTransitionWatchpoint(nullptr);
    extra->setStructure(vm, previous);
    extra->locationForOffset(offset)->clear();
    return true;
}

bool Object::tryMoveFieldBackToSlot(VM& vm, const FieldData& field)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return false;
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    unsigned attributes;
    JSValue value = extra->getDirect(vm, name, attributes);
    if (!value)
        return false;
    if ((field.isPrivate() ? 0 : attributes) != attributesOf(field))
        return false;
    if (!tryStore(vm, field, value))
        return false;
    forgetPlacesOfPositionedKeys(state());
    FieldReturn back { .markedCopyOfField = field, .field = &field, .structureBefore = structureID(), .extraPropertiesBefore = extra->structureID(), .orderWord = orderWord(), .name = field.name };
    bool isUndone = tryUndoAdditionOfLastProperty(vm, extra, name, back.offset);
    if (!isUndone) {
        VM::DeletePropertyModeScope ignoreConfigurable(vm, VM::DeletePropertyMode::IgnoreConfigurable);
        DeletePropertySlot slot;
        JSObject::deleteProperty(extra, structure()->realm(), name, slot);
    }
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    if (!isUndone || attributes || field.isPrivate() || field.isAccessor() || structure()->mayBePrototype() || orderWord() != back.orderWord)
        return true;
    if (!compiledStructure().extraPropertiesSlot() || orderOf(orderWord()) >= layout().numberOfOrders)
        return true;
    back.markedCopyOfField.flags |= fieldRecordIsCopyInFieldReturn;
    back.offsetOfExtraPropertiesWord = compiledStructure().extraPropertiesSlot()->offset;
    back.structureAfter = structureID();
    back.extraPropertiesAfter = extra->structureID();
    state().propertyCaches->remember(back);
    return true;
}

bool Object::tryRepeat(VM& vm, const FieldReturn& back, JSValue value)
{
    if (structureID() != back.structureBefore || orderWord() != back.orderWord)
        return false;
    JSObject* extra = extraPropertiesIfCreated(*this);
    if (!extra || extra->structureID() != back.extraPropertiesBefore)
        return false;
    auto* structureAfter = WTF::opaque(uncheckedDowncast<CompiledStructure>(back.structureAfter.decode()));
    Structure* extraStructureAfter = WTF::opaque(back.extraPropertiesAfter.decode());
    PropertyOffset offset = back.offset;
    if (!tryStore(vm, *back.field, value))
        return false;
    forgetPlacesOfPositionedKeys(state());
    extra->setStructure(vm, extraStructureAfter);
    extra->locationForOffset(offset)->clear();
    setVariantOfStructure(vm, structureAfter);
    ensureStillAliveHere(structureAfter);
    ensureStillAliveHere(extraStructureAfter);
    return true;
}

bool Object::mayBearMark() const
{
    return std::ranges::any_of(layout().fieldsThroughLastAllocated() | std::views::reverse, [](const FieldData& field) {
        return field.isFilledAtBirth || field.holdsRawValueFromBirth;
    });
}

bool Object::hasEveryFieldOfBirth() const
{
    static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::Cell32).absentBits && !traitsOf(SlotRepresentation::OptionalCell32).absentBits);
    auto holdsWordInCell = [&](const FieldData& field) ALWAYS_INLINE_LAMBDA {
        if (field.isOutOfLine())
            return false;
        switch (field.slot.representation()) {
        case SlotRepresentation::JSValue:
            return !!at<EncodedJSValue>(field.slot.offset);
        case SlotRepresentation::Cell32:
        case SlotRepresentation::OptionalCell32:
            return !!at<uint32_t>(field.slot.offset);
        default:
            return false;
        }
    };
    return std::ranges::all_of(layout().fieldsThroughLastAllocated() | std::views::reverse, [&](const FieldData& field) {
        if (!field.holdsRawValueFromBirth && holdsWordInCell(field)) [[likely]]
            return true;
        if (field.holdsRawValueFromBirth) {
            const void* slot = slotOf(field);
            return slot && !slotHoldsMarker(field.slot.representation(), slot);
        }
        if (!field.isFilledAtBirth || isInSlot(field))
            return true;
        return field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state(), field) && !livesInExtraProperties(vm(), *this, field);
    });
}

bool Object::lacksFieldOfBirth() const
{
    return std::ranges::any_of(layout().fieldsThroughLastAllocated(), [&](const FieldData& field) {
        return field.isFilledAtBirth && !field.holdsRawValueFromBirth && !isInSlot(field) && !isInKeyOrder(state(), field);
    });
}

bool Object::hasAccessorAmongExtraProperties() const
{
    JSObject* extra = extraPropertiesIfCreated(*this);
    return extra && extra->structure()->hasAnyKindOfGetterSetterProperties();
}

void Object::tryFinishBirth()
{
    if (!bearsMark())
        return;
    KeyOrderTable& keyOrders = state().objects->keyOrders;
    const uint32_t word = orderWord();
    if (keyOrders.isOrderOfFinishedBirth(layout(), word)) {
        setBearsMark(false);
        return;
    }
    UnfinishedBirth& remembered = state().objects->caches->unfinishedBirthAt(structureID(), word);
    if (remembered.structureID == structureID().bits() && remembered.orderWord == word)
        return;
    if (layout().marksFrozenObjects && isFrozen())
        return;
    if (word & orderWordHasFieldOutsideSlot || hasAccessorAmongExtraProperties())
        return;
    if (!hasEveryFieldOfBirth()) {
        if (!hasOutOfLineKeyOrder() && lacksFieldOfBirth())
            remembered = { structureID().bits(), word };
        return;
    }
    setBearsMark(false);
    if (word & ~orderAndCountMask || isOrderStoredOutOfLine(word) || orderOf(word) >= layout().numberOfOrders)
        return;
    bool listsEveryFieldOfBirth = std::ranges::all_of(layout().fieldsThroughLastAllocated(), [&](const FieldData& field) {
        return !field.holdsRawValueFromBirth && (!field.isFilledAtBirth || (!field.isPrivate() && !field.isBrand() && isInKeyOrder(state(), field)));
    });
    if (listsEveryFieldOfBirth)
        keyOrders.rememberOrderOfFinishedBirth(layout(), word);
}

void Object::deleteField(VM& vm, const FieldData& field)
{
    if (!admitsMutation(vm, *this, field, ObjectMutation::DeleteField))
        return;
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    State& state = this->state();
    clear(field);
    if (field.isFilledAtBirth)
        setBearsMark(true);
    removeFieldFromKeyOrder(vm, state, *this, field);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Remove, name);
    countFieldThatLeftSlot(vm, layout().index, indexOf(field), RuntimeCounterID::FieldsEvictedByDelete);
}

static Object::LeavingProperty createPropertyFromDescriptor(JSGlobalObject* globalObject, const PropertyDescriptor& descriptor)
{
    unsigned attributes = descriptor.attributes();
    if (!descriptor.isAccessorDescriptor())
        return { descriptor.value() ? descriptor.value() : jsUndefined(), attributes & ~PropertyAttribute::Accessor };
    JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : nullptr;
    JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : nullptr;
    return { GetterSetter::create(globalObject->vm(), globalObject, getter, setter), (attributes & ~PropertyAttribute::ReadOnly) | PropertyAttribute::Accessor };
}

static Object::LeavingProperty redefinedProperty(JSGlobalObject* globalObject, const PropertyDescriptor& current, const PropertyDescriptor& descriptor)
{
    unsigned attributes = descriptor.attributesOverridingCurrent(current);
    if (descriptor.isDataDescriptor() || (descriptor.isGenericDescriptor() && !current.isAccessorDescriptor())) {
        JSValue value = descriptor.value() ? descriptor.value() : current.value() ? current.value() : jsUndefined();
        return { value, attributes & ~PropertyAttribute::Accessor };
    }
    JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : current.getterPresent() ? current.getterObject() : nullptr;
    JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : current.setterPresent() ? current.setterObject() : nullptr;
    return { GetterSetter::create(globalObject->vm(), globalObject, getter, setter), (attributes & ~PropertyAttribute::ReadOnly) | PropertyAttribute::Accessor };
}

static void createField(VM& vm, Object& object, const FieldData& field, Object::LeavingProperty created)
{
    bool fitsSlot = created.attributes == object.attributesOf(field);
    if (fitsSlot && tryCreateField(vm, object.state(), object, field, created.valueOrAccessor))
        return;
    object.moveFieldToExtraProperties(vm, field, created, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
}

static std::optional<bool> tryHandValueToSetterOfExtraProperty(JSGlobalObject* globalObject, Object* object, PropertyName name, const PropertyDescriptor& descriptor)
{
    JSObject* extra = object->extraProperties();
    if (!extra || !descriptor.value() || !extra->structure()->hasAnyKindOfGetterSetterProperties())
        return std::nullopt;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned attributes = 0;
    JSValue held = extra->getDirect(vm, name, attributes);
    if (!held || !(attributes & PropertyAttribute::CustomValue) || attributes & PropertyAttribute::ReadOnly)
        return std::nullopt;
    auto* custom = uncheckedDowncast<CustomGetterSetter>(held);
    PutValueFunc setter = custom->setter();
    bool changesAttribute = (descriptor.writablePresent() && !descriptor.writable())
        || (descriptor.enumerablePresent() && descriptor.enumerable() == !!(attributes & PropertyAttribute::DontEnum))
        || (descriptor.configurablePresent() && descriptor.configurable() == !!(attributes & PropertyAttribute::DontDelete));
    if (!setter || changesAttribute)
        return std::nullopt;
    JSGlobalObject* realm = object->structure()->realm();
    if (GetValueFunc getter = custom->getter()) {
        JSValue current = JSValue::decode(getter(realm, JSValue::encode(object), name));
        RETURN_IF_EXCEPTION(scope, false);
        bool isSame = sameValue(globalObject, current, descriptor.value());
        RETURN_IF_EXCEPTION(scope, false);
        if (isSame)
            return true;
    }
    RELEASE_AND_RETURN(scope, setter(realm, JSValue::encode(object), JSValue::encode(descriptor.value()), name));
}

static bool defineExtraProperty(Object* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* extra = object->extraProperties();
    if (!parseIndex(name) && !object->isNotExtensible() && (!extra || !isValidOffset(extra->structure()->get(vm, name)))) {
        Object::LeavingProperty created = createPropertyFromDescriptor(globalObject, descriptor);
        bool isDefined = object->tryDefineAbsentProperty(vm, nullptr, name, created.valueOrAccessor, created.attributes);
        RETURN_IF_EXCEPTION(scope, false);
        if (isDefined)
            return true;
        extra = object->extraProperties();
    }
    bool exists = extra && extra->hasOwnProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!exists && object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
    if (exists) {
        std::optional<bool> handedToSetter = tryHandValueToSetterOfExtraProperty(globalObject, object, name, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
        if (handedToSetter)
            return *handedToSetter;
    }
    if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)) && !admitsMutation(vm, *object, ObjectMutation::RedefineProperty))
        RELEASE_AND_RETURN(scope, false);
    if (!exists) {
        if (!object->mayAddExtraProperty(vm, name))
            RELEASE_AND_RETURN(scope, false);
        extra = object->ensureExtraProperties(vm);
        RELEASE_ASSERT(extra);
    }
    bool interceptedIndexedAccesses = interceptsIndexedAccesses(*object);
    auto tells = makeScopeExit([&] {
        object->didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, *object, exists ? VM::StructureChainIntegrityEvent::Change : VM::StructureChainIntegrityEvent::Add, name.uid());
        didChangeIndexedProperties(vm, *object, interceptedIndexedAccesses);
    });
    RELEASE_AND_RETURN(scope, extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow));
}

static bool defineFieldProperty(Object* object, JSGlobalObject* globalObject, const FieldData& field, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current && !current.isInSlot) {
        std::optional<bool> handedToSetter = tryHandValueToSetterOfExtraProperty(globalObject, object, name, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
        if (handedToSetter)
            return *handedToSetter;
        JSObject* extra = object->extraProperties();
        auto tells = makeScopeExit([&] {
            object->tryMoveFieldBackToSlot(vm, field);
            object->didChangeExtraProperties(vm);
            didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Change, name.uid());
        });
        RELEASE_AND_RETURN(scope, extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow));
    }

    if (!current) {
        if (object->isNotExtensible())
            return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
        createField(vm, *object, field, createPropertyFromDescriptor(globalObject, descriptor));
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    PropertyDescriptor currentDescriptor;
    currentDescriptor.setDescriptor(current.valueOrAccessor, current.attributes);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, !object->isNotExtensible(), descriptor, true, currentDescriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid)
        return false;

    unsigned attributes = descriptor.attributesOverridingCurrent(currentDescriptor);
    if (field.isAccessor() && descriptor.isAccessorDescriptor() && attributes == object->attributesOf(field)) {
        auto* held = uncheckedDowncast<GetterSetter>(current.valueOrAccessor);
        JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : held->isGetterNull() ? nullptr : held->getter();
        JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : held->isSetterNull() ? nullptr : held->setter();
        bool replacesProvenHalf = (field.getterIsProven && !held->isGetterNull() && getter != held->getter()) || (field.setterIsProven && !held->isSetterNull() && setter != held->setter());
        if (replacesProvenHalf && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]] {
            if (refusesMutation(vm, *object, ObjectMutation::RedefineProperty))
                RELEASE_AND_RETURN(scope, false);
            crashOnImpossibleMutation(vm, *object, field, ObjectMutation::RedefineProperty);
        }
        bool keepsBothHalves = !field.takesAccessorsOfLibraryAlone() || (getter == (held->isGetterNull() ? nullptr : held->getter()) && setter == (held->isSetterNull() ? nullptr : held->setter()));
        if (keepsBothHalves && object->tryStore(vm, field, GetterSetter::create(vm, globalObject, getter, setter))) {
            didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Change, name.uid());
            return true;
        }
    }
    bool replacesAccessor = descriptor.isAccessorDescriptor() || (field.isAccessor() && descriptor.isDataDescriptor());
    if (!replacesAccessor && attributes == object->attributesOf(field)) {
        constexpr unsigned attributesOfFixedValue = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
        bool keepsValue = !descriptor.value() || (attributes & attributesOfFixedValue) == attributesOfFixedValue;
        if (keepsValue || object->tryStore(vm, field, descriptor.value()))
            return true;
        object->moveFieldToExtraProperties(vm, field, { descriptor.value(), attributes }, SlotExitReason::StoreOfOtherType);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    object->moveFieldToExtraProperties(vm, field, redefinedProperty(globalObject, currentDescriptor, descriptor), SlotExitReason::Redefinition);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (object->tryDefineAsOnTheLastObject(globalObject->vm(), name, descriptor))
        return true;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const FieldData* field = object->fieldNamed(object->state(), name);
    ASSERT(!field || !field->isPrivate());
    if (name.isSymbol()) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (descriptor.isAccessorDescriptor() && object->tryRepeatAccessorDefinition(vm, globalObject, field, name, descriptor))
        return true;
    if (field)
        RELEASE_AND_RETURN(scope, defineFieldProperty(object, globalObject, *field, name, descriptor, shouldThrow));
    RELEASE_AND_RETURN(scope, defineExtraProperty(object, globalObject, name, descriptor, shouldThrow));
}

JSValue Object::getDirectFromNativeCode(VM& vm, PropertyName name, unsigned* attributes)
{
    if (const FieldData* field = fieldNamed(state(), name)) {
        FieldProperty property = findFieldProperty(vm, *this, *field);
        if (property && attributes)
            *attributes = property.attributes;
        return property.valueOrAccessor;
    }
    JSObject* extra = extraProperties();
    if (!extra)
        return { };
    unsigned ignored;
    return extra->getDirect(vm, name, attributes ? *attributes : ignored);
}

enum class CurrentAttributes : bool { AreReplaced, AreKept };

static bool putFieldDirect(VM& vm, Object& object, const FieldData& field, PropertyName name, Object::LeavingProperty property, CurrentAttributes currentAttributes)
{
    FieldProperty current = findFieldProperty(vm, object, field);
    if (current && currentAttributes == CurrentAttributes::AreKept)
        property.attributes = current.attributes;
    bool fitsSlot = (field.isPrivate() ? 0 : property.attributes) == object.attributesOf(field);
    if (current && !current.isInSlot) {
        putProperty(vm, object.extraProperties(), name, property);
        object.tryMoveFieldBackToSlot(vm, field);
        object.didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Change, name.uid());
        return true;
    }
    if (fitsSlot && (current ? object.tryStore(vm, field, property.valueOrAccessor) : tryCreateField(vm, object.state(), object, field, property.valueOrAccessor)))
        return true;
    object.moveFieldToExtraProperties(vm, field, property, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
    return true;
}

static bool putOwnProperty(VM& vm, Object& object, PropertyName name, Object::LeavingProperty property, CurrentAttributes currentAttributes)
{
    if (const FieldData* field = object.fieldNamed(object.state(), name))
        return putFieldDirect(vm, object, *field, name, property, currentAttributes);
    JSObject* extra = object.ensureExtraProperties(vm);
    RELEASE_ASSERT(extra);
    unsigned attributesOfCurrent = 0;
    if (!extra->getDirect(vm, name, attributesOfCurrent))
        object.willAddExtraProperty(vm, name);
    else if (currentAttributes == CurrentAttributes::AreKept)
        property.attributes = attributesOfCurrent;
    putProperty(vm, extra, name, property);
    object.didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Change, name.uid());
    return true;
}

bool Object::putDirectFromNativeCode(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    return putOwnProperty(vm, *this, name, { value, attributes }, CurrentAttributes::AreReplaced);
}

bool Object::storeDirectFromNativeCode(VM& vm, PropertyName name, JSValue value)
{
    return putOwnProperty(vm, *this, name, { value, 0 }, CurrentAttributes::AreKept);
}

bool Object::tryDefineAbsentGetterInSlot(VM& vm, JSGlobalObject* globalObject, UniquedStringImpl* name, JSObject* getter, unsigned attributes)
{
    const FieldData* field = state().objects->layouts.field(vm, layout(), name);
    return field && tryDefineAbsentGetterInSlot(vm, globalObject, *field, name, getter, attributes);
}

bool Object::tryDefineAbsentGetterInSlot(VM& vm, JSGlobalObject* globalObject, const FieldData& field, UniquedStringImpl* name, JSObject* getter, unsigned attributes)
{
    State& state = this->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    if (!field.isAccessor() || isNotExtensible() || attributes != attributesOf(field))
        return false;
    if (isInSlot(field) || livesInExtraProperties(vm, *this, field) || !layouts.objectHasType(layouts.typeOf(field), getter) || !RuntimeLayoutTable::slotTakesHalf(field, getter))
        return false;
    if (!store(vm, field, GetterSetter::create(vm, globalObject, getter, nullptr)))
        return false;
    didCreateField(state, field, true);
    if (field.isFilledAtBirth)
        tryFinishBirth();
    if (structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, name);
    return true;
}

static UniquedStringImpl* atomOf(JSValue key)
{
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text && text->isAtom() ? static_cast<UniquedStringImpl*>(text) : nullptr;
}

bool Object::tryDefineAccessorsOfLiteral(VM& vm, JSGlobalObject* globalObject, std::span<const EncodedJSValue> keysAndFunctions, uint32_t getters, uint32_t enumerable)
{
    State& state = this->state();
    constexpr size_t widthOfMasks = 8 * sizeof(getters);
    static_assert(sizeof(getters) == sizeof(enumerable) && widthOfMasks < AccessorDefinitionPlan::noDefinition);
    size_t count = keysAndFunctions.size() / 2;
    if (!count || count > widthOfMasks)
        return false;
    if (hasFlagsInOrderWord() || hasOutOfLineKeyOrder() || structure()->mayBePrototype())
        return false;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    AccessorDefinitionPlan made;
    for (size_t index = 0; index < count; ++index) {
        UniquedStringImpl* name = atomOf(JSValue::decode(keysAndFunctions[index * 2]));
        JSCell* function = JSValue::decode(keysAndFunctions[index * 2 + 1]).asCell();
        const FieldData* field = name ? layouts.field(vm, layout(), name) : nullptr;
        unsigned attributes = PropertyAttribute::Accessor | (enumerable >> index & 1 ? PropertyAttribute::None : PropertyAttribute::DontEnum);
        if (!field || !field->isAccessor() || field->isOutOfLine() || field->isFilledAtBirth || attributes != attributesOf(*field))
            return false;
        bool isGetter = getters >> index & 1;
        std::optional<FunctionID> id = functionOf(function);
        if (!id || (isGetter && !layouts.objectHasType(layouts.typeOf(*field), function)) || !RuntimeLayoutTable::slotTakesHalf(*field, function))
            return false;
        size_t place = made.slots.findIf([&](const AccessorDefinitionPlan::FilledSlot& slot) {
            return slot.field == field;
        });
        if (place == notFound) {
            place = made.slots.size();
            made.slots.append({ field, AccessorDefinitionPlan::noDefinition, AccessorDefinitionPlan::noDefinition, isInSlot(*field) });
        }
        (isGetter ? made.slots[place].getter : made.slots[place].setter) = static_cast<uint8_t>(index);
    }
    for (const AccessorDefinitionPlan::FilledSlot& slot : made.slots) {
        if (!slot.isFilledBefore && slot.getter == AccessorDefinitionPlan::noDefinition && !slot.field->getterIsProven && !layouts.objectHasType(layouts.typeOf(*slot.field), globalObject->nullGetterFunction()))
            return false;
    }
    for (const AccessorDefinitionPlan::FilledSlot& slot : made.slots.span()) {
        JSObject* getter = nullptr;
        JSObject* setter = nullptr;
        if (slot.isFilledBefore) {
            auto* held = uncheckedDowncast<GetterSetter>(read(*slot.field).asCell());
            getter = held->isGetterNull() ? nullptr : held->getter();
            setter = held->isSetterNull() ? nullptr : held->setter();
        }
        if (slot.getter != AccessorDefinitionPlan::noDefinition)
            getter = asObject(JSValue::decode(keysAndFunctions[slot.getter * 2 + 1]));
        if (slot.setter != AccessorDefinitionPlan::noDefinition)
            setter = asObject(JSValue::decode(keysAndFunctions[slot.setter * 2 + 1]));
        auto* accessor = GetterSetter::create(vm, globalObject, getter, setter);
        bool isWritten = tryWriteSlot(state.instance, slot.field->slot.representation(), slotOf(*slot.field), accessor);
        RELEASE_ASSERT(isWritten);
        vm.writeBarrier(this, accessor);
        if (!slot.isFilledBefore)
            didCreateField(state, *slot.field, true);
    }
    return true;
}

bool Object::tryDefineAbsentProperty(VM& vm, const FieldData* field, PropertyName name, JSValue valueOrAccessor, unsigned attributes)
{
    ASSERT(field == fieldNamed(state(), name));
    if (isNotExtensible())
        return false;
    if (field) {
        if (field->isPrivate() || findFieldProperty(vm, *this, *field))
            return false;
        createField(vm, *this, *field, { valueOrAccessor, attributes });
        return true;
    }
    if (parseIndex(name) || name.isPrivateName())
        return false;
    JSObject* extra = extraProperties();
    if (extra && isValidOffset(extra->structure()->get(vm, name)))
        return false;
    return tryAddAbsentExtraProperty(vm, name, valueOrAccessor, attributes);
}

bool Object::tryRepeat(VM& vm, const AddedExtraProperty& added, JSValue value)
{
    if (structureID() != added.structureBefore || orderWord() != added.orderWordBefore || isInFrame())
        return false;
    JSObject* extra = extraPropertiesIfCreated(*this);
    if (added.extraPropertiesBefore ? !extra || extra->structureID() != added.extraPropertiesBefore : hasExtraProperties())
        return false;
    if (!admitsMutation(vm, *this, ObjectMutation::AddProperty))
        return true;
    // A collection forgets the addition, and then these pointers alone keep the Structures alive.
    auto* structureAfter = WTF::opaque(uncheckedDowncast<CompiledStructure>(added.structureAfter.decode()));
    Structure* extraStructureAfter = WTF::opaque(added.extraPropertiesAfter.decode());
    PropertyOffset offset = added.offset;
    if (!isInlineOffset(offset) || added.attributes & PropertyAttribute::Accessor) [[unlikely]] {
        if (added.attributes & PropertyAttribute::Accessor && !perCellBit())
            setBearsMark(true);
        countEvent(vm, RuntimeCounterID::SlowPropertyAdditionsReplayed);
        if (extra)
            extra->growOutOfLineStorageForTransition(vm, extraStructureAfter);
    }
    if (extra) {
        extra->putDirectOffset(vm, offset, value);
        extra->setStructure(vm, extraStructureAfter);
        setVariantOfStructure(vm, structureAfter);
    } else {
        extra = constructEmptyObject(vm, extraStructureAfter);
        extra->putDirectOffset(vm, offset, value);
        setExtraProperties(vm, extra, structureAfter);
    }
    ensureStillAliveHere(structureAfter);
    ensureStillAliveHere(extraStructureAfter);
    return true;
}

bool Object::tryDefineAsOnTheLastObject(VM& vm, PropertyName name, const PropertyDescriptor& descriptor)
{
    if (descriptor.isAccessorDescriptor() || isPreparedBeforeLookups(vm, name.uid()))
        return false;
    const AddedExtraProperty* added = state().propertyCaches->findAddedExtraProperty(structureID(), name.uid());
    return added && added->attributes == (descriptor.attributes() & ~PropertyAttribute::Accessor) && tryRepeat(vm, *added, descriptor.value() ? descriptor.value() : jsUndefined());
}

bool Object::tryRepeatAccessorDefinition(VM& vm, JSGlobalObject* globalObject, const FieldData* field, PropertyName name, const PropertyDescriptor& descriptor)
{
    ASSERT(descriptor.isAccessorDescriptor());
    if (!descriptor.enumerablePresent() || !descriptor.configurablePresent())
        return false;
    const unsigned attributes = (descriptor.attributes() & ~PropertyAttribute::ReadOnly) | PropertyAttribute::Accessor;
    return tryRepeatAccessorDefinition(vm, globalObject, field, name, descriptor.getterPresent() ? descriptor.getterObject() : nullptr, descriptor.setterPresent() ? descriptor.setterObject() : nullptr, attributes);
}

bool Object::tryRepeatAccessorDefinition(VM& vm, JSGlobalObject* globalObject, const FieldData* field, PropertyName name, JSObject* getter, JSObject* setter, unsigned attributes)
{
    if (isPreparedBeforeLookups(vm, name.uid()))
        return false;
    if (field && (field->isAccessor() || field->getterIsProven || field->setterIsProven))
        return false;
    PropertyCaches& caches = *state().propertyCaches;
    FieldExit* exit = nullptr;
    AddedExtraProperty* added = nullptr;
    auto findsDefinition = [&] {
        if (field) {
            exit = caches.findFieldExit(structureID(), field->name);
            return exit && exit->isDefinition && exit->attributes == attributes;
        }
        added = caches.findAddedExtraProperty(structureID(), name.uid());
        return added && added->attributes == attributes;
    };
    if (!findsDefinition())
        return false;
    GetterSetter* accessor = exit ? exit->accessorOfDefinition : added->lastAccessor;
    bool holdsBoth = accessor && accessor->getter() == (getter ? getter : globalObject->nullGetterFunction()) && accessor->setter() == (setter ? setter : globalObject->nullSetterFunction());
    if (!holdsBoth) {
        accessor = GetterSetter::create(vm, globalObject, getter, setter);
        if (!findsDefinition())
            return false;
        if (exit)
            exit->accessorOfDefinition = accessor;
        else
            added->lastAccessor = accessor;
    }
    if (exit ? !tryRepeat(vm, *exit, accessor) : !tryRepeat(vm, *added, accessor))
        return false;
    if (holdsBoth)
        countEvent(vm, RuntimeCounterID::AccessorsShared);
    countEvent(vm, RuntimeCounterID::AccessorDefinitionsReplayed);
    return true;
}

bool Object::tryAddAbsentExtraProperty(VM& vm, PropertyName name, JSValue valueOrAccessor, unsigned attributes, MemoryOfAddedNames memory)
{
    State& state = this->state();
    const AddedExtraProperty* seen = memory == MemoryOfAddedNames::IsAsked ? state.propertyCaches->findAddedExtraProperty(structureID(), name.uid()) : nullptr;
    if (seen && seen->attributes == attributes && tryRepeat(vm, *seen, valueOrAccessor))
        return true;
    if (isNotExtensible())
        return false;
    bool hadOtherCellInPlaceOfExtraProperties = hasExtraProperties() && !extraPropertiesIfCreated(*this);
    JSObject* extra = extraProperties();
    AddedExtraProperty added { .name = name.uid(), .structureBefore = structureID(), .extraPropertiesBefore = extra ? extra->structureID() : StructureID(), .orderWordBefore = orderWord(), .attributes = attributes };
    if (!mayAddExtraProperty(vm, name))
        return true;
    bool isFirst = !extra;
    if (isFirst)
        extra = constructEmptyObject(vm, Instance::of(structure()->realm())->realm.emptyExtraPropertiesStructure);
    bool followsEarlierObject = extra->tryPutDirectByExistingTransition(vm, name, valueOrAccessor, attributes);
    if (!followsEarlierObject)
        putProperty(vm, extra, name, { valueOrAccessor, attributes });
    if (isFirst)
        adoptExtraProperties(vm, extra);
    else
        didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, name.uid());

    if (!followsEarlierObject || attributes & PropertyAttribute::CustomAccessorOrValue || hadOtherCellInPlaceOfExtraProperties || hasOutOfLineKeyOrder() || !compiledStructure().extraPropertiesSlot() || isInFrame())
        return true;
    Structure* extraStructure = extra->structure();
    if (orderWord() != (added.orderWordBefore | orderWordHasExtraProperties) || structure()->mayBePrototype() || extraStructure->isDictionary() || extraStructure->transitionPropertyName() != name.uid())
        return true;
    added.offset = extraStructure->transitionOffset();
    if (!isValidOffset(added.offset) || (isFirst && !isInlineOffset(added.offset)))
        return true;
    added.structureAfter = structureID();
    added.extraPropertiesAfter = extraStructure->id();
    state.propertyCaches->remember(added);
    return true;
}

Object::ExtraPropertyFound Object::findExtraProperty(VM& vm, PropertyName name) const
{
    using Kind = ExtraPropertyFound::Kind;
    if (parseIndex(name) || name.isPrivateName())
        return { Kind::Other, { } };
    JSObject* extra = extraProperties();
    if (!extra)
        return { Kind::Absent, { } };
    LeavingProperty current;
    current.valueOrAccessor = extra->getDirect(vm, name, current.attributes);
    if (!current.valueOrAccessor)
        return { Kind::Absent, { } };
    bool isFixed = current.attributes & (PropertyAttribute::DontDelete | PropertyAttribute::CustomAccessorOrValue);
    return { isFixed ? Kind::Other : Kind::MayBeRedefinedInEveryWay, current };
}

void Object::replaceExtraProperty(VM& vm, PropertyName name, LeavingProperty property)
{
    putProperty(vm, extraProperties(), name, property);
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name.uid());
}

static void setOrdinaryObjectIntegrity(JSGlobalObject* globalObject, JSObject* ordinary, Object::Integrity integrity)
{
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    switch (integrity) {
    case Object::Integrity::NotExtensible:
        ordinary->methodTable()->preventExtensions(ordinary, globalObject);
        break;
    case Object::Integrity::Sealed:
        objectConstructorSeal(globalObject, ordinary);
        break;
    case Object::Integrity::Frozen:
        objectConstructorFreeze(globalObject, ordinary);
        break;
    }
    scope.assertNoException();
}

static bool indexedPropertiesHaveIntegrity(JSGlobalObject* realm, JSObject* ordinary, Object::Integrity integrity)
{
    if (!hasIndexedProperties(ordinary->indexingType()))
        return true;
    VM& vm = realm->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyNameArrayBuilder indices(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    ordinary->getOwnIndexedPropertyNames(realm, indices, DontEnumPropertiesMode::Include);
    for (const Identifier& index : indices) {
        PropertyDescriptor descriptor;
        bool isPresent = ordinary->getOwnPropertyDescriptor(realm, index, descriptor);
        scope.assertNoException();
        if (!isPresent)
            continue;
        if (descriptor.configurable())
            return false;
        if (integrity == Object::Integrity::Frozen && descriptor.isDataDescriptor() && descriptor.writable())
            return false;
    }
    return true;
}

void Object::setIntegrity(JSGlobalObject* globalObject, Integrity integrity)
{
    VM& vm = globalObject->vm();
    uint32_t bits = orderWordIsNotExtensible;
    if (integrity != Integrity::NotExtensible)
        bits |= orderWordIsSealed;
    if (integrity == Integrity::Frozen)
        bits |= orderWordIsFrozen;
    if (testIntegrity(vm, integrity))
        return;
    if (!admitsMutation(vm, *this, ObjectMutation::ChangeIntegrity))
        return;
    bool interceptedIndexedAccesses = interceptsIndexedAccesses(*this);
    if (JSObject* extra = extraProperties()) {
        setOrdinaryObjectIntegrity(globalObject, extra, integrity);
        didChangeExtraProperties(vm);
    }
    at<uint32_t>(offsetOfOrderWord) |= bits;
    if (layout().marksFrozenObjects && integrity == Integrity::Frozen)
        setBearsMark(true);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, nullptr);
    didChangeIndexedProperties(vm, *this, interceptedIndexedAccesses);
    countEvent(vm, RuntimeCounterID::ObjectsGivenIntegrityLevel);
}

bool Object::tryFreezeFields(VM& vm)
{
    const LayoutData& layout = this->layout();
    if (orderWord() & (orderWordHasExtraProperties | orderWordIntegrityBits) || mayBePrototype() || layout.hasFieldsWithSymbolKeys || layout.impossibleMutations().contains(ObjectMutation::ChangeIntegrity))
        return false;
    at<uint32_t>(offsetOfOrderWord) |= orderWordIntegrityBits;
    if (layout.marksFrozenObjects)
        setBearsMark(true);
    countEvent(vm, RuntimeCounterID::ObjectsGivenIntegrityLevel);
    return true;
}

bool Object::testIntegrity(VM& vm, Integrity integrity) const
{
    if (!isNotExtensible())
        return false;
    if (integrity == Integrity::NotExtensible)
        return true;
    bool testsFrozen = integrity == Integrity::Frozen;
    auto fieldsHaveIntegrity = [&] {
        const RuntimeLayoutTable& layouts = state().objects->layouts;
        for (const FieldData& field : layout().fields()) {
            bool isListed = layouts.hasStringKey(field) || layouts.hasSymbolKey(field);
            bool leavesClosureUnmade = field.slot.representation() == SlotRepresentation::DeferredClosure;
            if (!isListed || !(leavesClosureUnmade ? isInSlot(field) : !!read(field)))
                continue;
            unsigned attributes = attributesOf(field);
            if (!(attributes & PropertyAttribute::DontDelete))
                return false;
            if (testsFrozen && !field.isAccessor() && !(attributes & PropertyAttribute::ReadOnly))
                return false;
        }
        return true;
    };
    if (!(testsFrozen ? isFrozen() : isSealed()) && !fieldsHaveIntegrity())
        return false;
    JSObject* extra = extraProperties();
    if (!extra)
        return true;
    if (!(testsFrozen ? extra->isFrozen(vm) : extra->isSealed(vm)))
        return false;
    return indexedPropertiesHaveIntegrity(structure()->realm(), extra, integrity);
}

bool Object::preventExtensions(JSObject* cell, JSGlobalObject* globalObject)
{
    uncheckedDowncast<Object>(cell)->setIntegrity(globalObject, Integrity::NotExtensible);
    return true;
}

bool Object::isExtensible(JSObject* cell, JSGlobalObject*)
{
    return !uncheckedDowncast<Object>(cell)->isNotExtensible();
}

void Object::setPrototypeKeepingShape(VM& vm, JSValue prototype)
{
    if (!admitsMutation(vm, *this, ObjectMutation::ChangePrototype))
        return;
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    bool chainAlreadyMayInterceptIndexedAccesses = mayBePrototype() && anyObjectInChainMayInterceptIndexedAccesses();
    setVariantOfStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Prototype, nullptr);
    if (mayBePrototype() && !chainAlreadyMayInterceptIndexedAccesses && anyObjectInChainMayInterceptIndexedAccesses())
        structure()->realm()->haveABadTime(vm, this);
    countEvent(vm, RuntimeCounterID::ObjectPrototypesChanged);
}

void Object::setPrototypeAtCreation(VM& vm, JSValue prototype)
{
    ASSERT(!mayBePrototype());
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    setVariantOfStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
}

bool Object::setPrototype(JSObject* cell, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (object->getPrototypeDirect() == prototype)
        return true;
    if (object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    for (JSValue next = prototype; next.isObject() && asObject(next)->type() != ProxyObjectType; next = asObject(next)->getPrototypeDirect()) {
        if (next == JSValue(object))
            return typeError(globalObject, scope, shouldThrow, "cyclic __proto__ value"_s);
    }
    object->setPrototypeKeepingShape(vm, prototype);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static void finishSlotOfExtraProperty(Object* object, PropertySlot& slot, bool found)
{
    slot.disableCaching();
    if (found && slot.isCustom()) [[unlikely]]
        slot.setCustom(object, slot.attributes(), slot.customGetter(), slot.customSetter());
}

bool Object::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (name.isSymbol() && slot.internalMethodType() != PropertySlot::InternalMethodType::VMInquiry) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        if (field->isPrivate()) [[unlikely]]
            return false;
        JSValue value = object->readInline(*field);
        if (!value) [[unlikely]]
            value = object->read(*field);
        if (value) [[likely]] {
            if (field->isAccessor()) [[unlikely]]
                slot.setGetterSlot(object, object->attributesOf(*field), uncheckedDowncast<GetterSetter>(value));
            else
                slot.setValue(object, object->attributesOf(*field), value);
            return true;
        }
    }
    if (object->hasExtraProperties()) {
        if (std::optional<Object*> source = sourceOfPendingBinding(vm, *object, name.uid())) {
            if (!*source)
                return false;
            if (slot.internalMethodType() == PropertySlot::InternalMethodType::Get) {
                if (!vm.isSafeToRecurse()) [[unlikely]] {
                    throwStackOverflowError(globalObject, scope);
                    return false;
                }
                JSValue value = (*source)->get(globalObject, name);
                RETURN_IF_EXCEPTION(scope, false);
                slot.setValue(object, 0, value);
                slot.disableCaching();
                return true;
            }
            if (slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty) {
                slot.setValue(object, attributesOfStarBinding & ~PropertyAttribute::Accessor, jsUndefined());
                slot.disableCaching();
                return true;
            }
        }
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlot(extra, globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, false);
    finishSlotOfExtraProperty(object, slot, found);
    return found;
}

bool Object::getOwnPropertySlotByIndex(JSObject* cell, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    JSObject* extra = extraPropertiesIfCreated(*uncheckedDowncast<Object>(cell));
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlotByIndex(extra, globalObject, index, slot);
    slot.disableCaching();
    return found;
}

void Object::getOwnPropertyNames(JSObject* cell, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = object->layout();
    std::span<const FieldData> fields = layout.fields();
    JSObject* extra = object->extraProperties();
    ASSERT(!extra || extra->type() == FinalObjectType);
    if (extra)
        extra->getOwnIndexedPropertyNames(globalObject, names, mode);
    bool knownUnique = names.canAddKnownUniqueForStructure();
    auto add = [&](UniquedStringImpl* name) ALWAYS_INLINE_LAMBDA {
        if (knownUnique) [[likely]]
            names.addUnchecked(name);
        else
            names.add(name);
    };
    if (names.includeStringProperties() || names.includeSymbolProperties())
        forEachOwnKey(vm, state, *object, static_cast<KeysListed>(names.includeStringProperties() | names.includeSymbolProperties() << 1), mode, add);
    if (names.privateSymbolMode() != PrivateSymbolMode::Include) [[likely]]
        return;
    auto isIncluded = [&](unsigned attributes) {
        return mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum);
    };
    for (const FieldData& field : fields) {
        if (field.isPrivate() || layouts.hasSymbolKey(field) || layouts.hasStringKey(field))
            continue;
        unsigned attributes = field.attributes();
        if (!object->isInSlot(field)) {
            FieldProperty property = findFieldProperty(vm, *object, field);
            if (!property)
                continue;
            attributes = property.attributes;
        }
        if (isIncluded(attributes))
            add(layouts.nameOf(vm, field));
    }
    if (!extra)
        return;
    extra->structure()->forEachProperty(vm, [&](const auto& entry) {
        if (PropertyName(entry.key()).isPrivateName() && isIncluded(entry.attributes()))
            names.add(entry.key());
        return true;
    });
}

static bool getOwnPropertyDescriptorForStore(JSGlobalObject* globalObject, Object* object, PropertyName name, const PutPropertySlot& slot, PropertyDescriptor& descriptor)
{
    bool isReceiver = slot.thisValue() == JSValue(object);
    if (isReceiver || object->fieldNamed(object->state(), name))
        return object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    JSObject* extra = object->extraProperties();
    return extra && extra->getOwnPropertyDescriptor(globalObject, name, descriptor);
}

static bool ordinarySet(JSGlobalObject* globalObject, Object* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor ownDescriptor;
    bool isFound = getOwnPropertyDescriptorForStore(globalObject, object, name, slot, ownDescriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isFound) {
        JSValue prototype = object->getPrototypeDirect();
        std::optional<uint32_t> index = slot.thisValue() == JSValue(object) ? parseIndex(name) : std::nullopt;
        if (prototype.isObject() && !index)
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        if (prototype.isObject() && object->needsSlowPutIndexing()) {
            bool isStored = false;
            bool isIntercepted = asObject(prototype)->attemptToInterceptPutByIndexOnHoleForPrototype(globalObject, object, *index, value, slot.isStrictMode(), isStored);
            RETURN_IF_EXCEPTION(scope, false);
            if (isIntercepted)
                return isStored;
        }
        if (index && object->isNotExtensible())
            return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
        ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
    }
    bool isWritableAboveReceiver = slot.thisValue() != JSValue(object) && ownDescriptor.isDataDescriptor() && ownDescriptor.writable();
    if (isWritableAboveReceiver && !parseIndex(name))
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, name, value, slot));
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, name, value, slot.thisValue(), WTF::move(ownDescriptor), slot.isStrictMode()));
}

static bool putOutsideDataSlot(JSGlobalObject* globalObject, Object* object, PropertyName name, Object::LeavingProperty existing, JSValue value, bool isStrictMode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned attributes = existing.attributes;
    if (attributes & PropertyAttribute::Accessor)
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(existing.valueOrAccessor)->callSetter(globalObject, object, value, isStrictMode));
    if (attributes & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, isStrictMode, ReadonlyPropertyWriteError);
    if (attributes & PropertyAttribute::CustomAccessorOrValue) [[unlikely]] {
        if (PutValueFunc setter = uncheckedDowncast<CustomGetterSetter>(existing.valueOrAccessor)->setter()) {
            scope.release();
            bool isStored = setter(object->structure()->realm(), JSValue::encode(object), JSValue::encode(value), name);
            return isStored || attributes & PropertyAttribute::CustomAccessor;
        }
        if (attributes & PropertyAttribute::CustomAccessor)
            return false;
        attributes &= ~PropertyAttribute::CustomValue;
    }
    object->extraProperties()->putDirect(vm, name, value, attributes);
    if (object->structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Change, name.uid());
    if (attributes != existing.attributes)
        object->didChangeExtraProperties(vm);
    return true;
}

static bool hasNoTraps(VM& vm, State& state, JSObject* handler, std::initializer_list<const Identifier*> traps)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Object* compiledWithFieldsAlone = compiledObjectOrNull(handler);
    if (compiledWithFieldsAlone && !compiledWithFieldsAlone->hasFlagsInOrderWord()) {
        JSObject* above = handler->realm()->objectPrototype();
        Structure* structureAbove = above->structure();
        bool inheritsNoTrap = handler->getPrototypeDirect() == JSValue(above) && std::ranges::all_of(traps, [&](const Identifier* trap) {
            return structureAbove->isImmutablePropertyForReader(trap->impl());
        });
        if (inheritsNoTrap) {
            return std::ranges::all_of(traps, [&](const Identifier* trap) {
                const FieldData* field = compiledWithFieldsAlone->fieldNamed(state, trap->impl());
                return !field || !compiledWithFieldsAlone->isInSlot(*field);
            });
        }
    }
    for (const Identifier* trap : traps) {
        UniquedStringImpl* uid = trap->impl();
        if (const Slot* filed = state.propertyCaches->findByKey(handler->structure(), uid, SlotAccess::Read)) {
            if (filed->kind == Slot::Kind::Absent)
                continue;
            if (filed->kind != Slot::Kind::Field || !filed->has(Slot::absentIsUndefined) || handler->type() != CompiledObjectType)
                return false;
            const Object* compiledHandler = uncheckedDowncast<Object>(handler);
            const FieldData* field = compiledHandler->fieldNamed(state, uid);
            if (!field || findFieldProperty(vm, *compiledHandler, *field))
                return false;
            continue;
        }
        for (JSObject* object = handler; object; object = object->getPrototypeDirect().getObject()) {
            if (object->type() == ProxyObjectType || object->structure()->typeInfo().overridesGetPrototype())
                return false;
        }
        bool isFound = hasPropertyAndCache(&state.instance, handler, { uid, std::nullopt });
        RETURN_IF_EXCEPTION(scope, false);
        if (isFound)
            return false;
    }
    return true;
}

JSObject* RuntimeHooks::proxyDefinePropertyTarget(VM& vm, JSValue receiver)
{
    if (!receiver.isCell() || receiver.asCell()->type() != ProxyObjectType)
        return nullptr;
    ProxyObject* proxy = uncheckedDowncast<ProxyObject>(receiver.asCell());
    if (!proxy->handler().isObject())
        return nullptr;
    JSObject* handler = asObject(proxy->handler());
    Object* compiledHandler = compiledObjectOrNull(handler);
    Object* compiled = compiledHandler ? compiledHandler : compiledObjectOrNull(proxy->target());
    if (!compiled)
        return nullptr;
    return hasNoTraps(vm, compiled->state(), handler, { &vm.propertyNames->defineProperty, &vm.propertyNames->getOwnPropertyDescriptor }) ? proxy->target() : nullptr;
}

JSObject* RuntimeHooks::proxyOwnKeysTarget(VM& vm, JSObject* object, bool asksDescriptors)
{
    ProxyObject* proxy = uncheckedDowncast<ProxyObject>(object);
    Object* handler = proxy->handler().isObject() ? compiledObjectOrNull(asObject(proxy->handler())) : nullptr;
    JSObject* target = proxy->target();
    if (!handler || !(compiledObjectOrNull(target) || target->type() == FinalObjectType || target->type() == ArrayType))
        return nullptr;
    State& state = handler->state();
    bool listsTarget = asksDescriptors ? hasNoTraps(vm, state, handler, { &vm.propertyNames->ownKeys, &vm.propertyNames->getOwnPropertyDescriptor }) : hasNoTraps(vm, state, handler, { &vm.propertyNames->ownKeys });
    return listsTarget ? target : nullptr;
}

static bool isProxyThatDefinesOnTarget(VM& vm, JSValue receiver, Object* target)
{
    return RuntimeHooks::proxyDefinePropertyTarget(vm, receiver) == target;
}

bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    const FieldData* field = object->fieldNamed(object->state(), name);
    if (name.isSymbol()) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
    RELEASE_AND_RETURN(scope, putWithKnownField(cell, globalObject, name, value, slot, field));
}

static NEVER_INLINE bool trySetElementUnderIndexName(Object* object, PropertyName name, JSValue value)
{
    std::optional<uint32_t> index = parseIndex(name);
    if (!index)
        return false;
    Instance* instance = &object->state().instance;
    return object->hasExtraProperties() ? trySetElementOfCompiledObject(instance, object, *index, value) : !object->needsSlowPutIndexing() && tryDefineElementOfCompiledObject(instance, object, *index, value);
}

bool Object::putWithKnownField(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot, const FieldData* field, bool chainLetsStoresPass)
{
    ASSERT(!field || !field->isPrivate());
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (field && object->orderWord() & orderWordHasFieldOutsideSlot && slot.thisValue() == JSValue(object)) {
        const FieldReturn* back = object->state().propertyCaches->findFieldReturn(object->structureID(), field->name);
        if (back && object->tryRepeat(vm, *back, value))
            return true;
    }
    bool isWritableInSlot = field && !(field->flags & (fieldIsReadOnly | fieldIsAccessor)) && !(object->orderWord() & orderWordIntegrityBits);
    if (isWritableInSlot && slot.thisValue() == JSValue(object) && object->readInline(*field) && object->tryStore(vm, *field, value))
        return true;
    if (isWritableInSlot && slot.thisValue() != JSValue(object) && object->readInline(*field)) {
        bool definesHere = isProxyThatDefinesOnTarget(vm, slot.thisValue(), object);
        RETURN_IF_EXCEPTION(scope, false);
        if (definesHere && object->tryStore(vm, *field, value))
            return true;
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, name, value, slot));
    }
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]] {
        if (slot.thisValue() == JSValue(object)) {
            bool isStored = trySetElementUnderIndexName(object, name, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (isStored)
                return true;
        }
        RELEASE_AND_RETURN(scope, ordinarySet(globalObject, object, name, value, slot));
    }
    if (field) {
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (current.isInSlot && !field->isAccessor()) {
            if (current.attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (object->tryStore(vm, *field, value))
                return true;
            object->moveFieldToExtraProperties(vm, *field, { value, current.attributes }, SlotExitReason::StoreOfOtherType);
            RETURN_IF_EXCEPTION(scope, false);
            return true;
        }
        if (current) {
            bool isStored = putOutsideDataSlot(globalObject, object, name, { current.valueOrAccessor, current.attributes }, value, slot.isStrictMode());
            RETURN_IF_EXCEPTION(scope, false);
            if (isStored && !(current.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                object->tryMoveFieldBackToSlot(vm, *field);
            return isStored;
        }
    } else {
        bool isStored = object->tryStoreExtraProperty(vm, name, value, { ExtraPropertyStoreFlag::IsKnownNotToBeField }).kind != ExtraPropertyStore::NotHandled;
        RETURN_IF_EXCEPTION(scope, false);
        if (isStored)
            return true;
        if (JSObject* extra = object->extraProperties()) {
            unsigned attributes;
            if (JSValue existing = extra->getDirect(vm, name, attributes))
                RELEASE_AND_RETURN(scope, putOutsideDataSlot(globalObject, object, name, { existing, attributes }, value, slot.isStrictMode()));
        }
    }
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject() && !chainLetsStoresPass) {
        ChainWalk walk = walkChain({ globalObject, object->structure(), object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, name.uid());
        StoreThroughChain answer = storeThroughChain(walk);
        switch (answer) {
        case StoreThroughChain::CreatesOwnProperty:
            break;
        case StoreThroughChain::CallsSetter:
            slot.setCacheableSetter(walk.holder, walk.offset);
            RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(valueAtOffset(walk.holder, walk.offset))->callSetter(globalObject, object, value, slot.isStrictMode()));
        case StoreThroughChain::Unknown:
        case StoreThroughChain::CallsCustomSetter:
        case StoreThroughChain::IsRefused:
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        }
    }
    if (field && !(object->orderWord() & orderWordIntegrityBits)) {
        bool fitsSlot = !field->attributes();
        if (fitsSlot && tryCreateField(vm, object->state(), *object, *field, value))
            return true;
        object->moveFieldToExtraProperties(vm, *field, { value, static_cast<unsigned>(PropertyAttribute::None) }, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }
    RELEASE_AND_RETURN(scope, defineOwnProperty(object, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), slot.isStrictMode()));
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (index <= MAX_ARRAY_INDEX) {
        Instance* instance = &object->state().instance;
        if (object->hasExtraProperties() ? trySetElementOfCompiledObject(instance, object, index, value) : !object->needsSlowPutIndexing() && tryDefineElementOfCompiledObject(instance, object, index, value))
            return true;
    }
    PutPropertySlot slot(cell, shouldThrow);
    countEvent(globalObject->vm(), RuntimeCounterID::ElementsStoredByDescriptors);
    return ordinarySet(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
}

bool Object::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const FieldData* field = object->fieldNamed(state, name);
    if (name.isSymbol()) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (field) {
        if (field->isPrivate()) [[unlikely]]
            return true;
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (!current)
            return true;
        if (current.attributes & PropertyAttribute::DontDelete)
            return false;
        if (current.isInSlot) {
            if (!field->isAccessor()) {
                object->deleteField(vm, *field);
                RETURN_IF_EXCEPTION(scope, false);
                return true;
            }
            object->moveFieldToExtraProperties(vm, *field, { current.valueOrAccessor, current.attributes }, SlotExitReason::Delete);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    PlaceOfExtraKey place;
    if (!field)
        place = object->placeOfExtraKey(name);
    StructureID extraStructureBefore = extra->structureID();
    bool isDeleted = false;
    auto tells = makeScopeExit([&] {
        slot.disableCaching();
        if (!isDeleted)
            return;
        if (field)
            removeFieldFromKeyOrder(vm, state, *object, *field);
        if (extra->structureID() != extraStructureBefore)
            object->didChangeExtraProperties(vm);
        if (!field)
            object->didRemoveExtraKey(vm, state, place);
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, name.uid());
    });
    isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    scope.release();
    return isDeleted;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    if (index > MAX_ARRAY_INDEX) [[unlikely]]
        return JSCell::deleteProperty(cell, globalObject, Identifier::from(globalObject->vm(), index));
    Object* object = uncheckedDowncast<Object>(cell);
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    VM& vm = globalObject->vm();
    bool isDeleted = false;
    auto tells = makeScopeExit([&] {
        object->didChangeExtraProperties(vm);
        if (isDeleted)
            didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, nullptr);
    });
    isDeleted = extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
    return isDeleted;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
