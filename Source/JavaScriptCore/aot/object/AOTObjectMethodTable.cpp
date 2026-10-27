/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTErrorMessages.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "CustomGetterSetter.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "ProxyObject.h"
#include "TopExceptionScope.h"
#include "TypeError.h"

namespace JSC { namespace AOT {

static ASCIILiteral describeExit(SlotExitReason reason, JSValue value)
{
    if (reason != SlotExitReason::StoreOfOtherType)
        return "field left its slot by redefinition"_s;
    if (!value.isCell())
        return value.isUndefinedOrNull() ? "field left its slot for undefined or null"_s : value.isNumber() ? "field left its slot for a number"_s : "field left its slot for a boolean"_s;
    JSCell* cell = value.asCell();
    if (isCompiledClosure(cell))
        return "field left its slot for a compiled closure"_s;
    if (cell->inherits<JSBoundFunction>())
        return "field left its slot for a bound function"_s;
    if (cell->isCallable())
        return "field left its slot for another function"_s;
    if (isCompiledObject(cell))
        return "field left its slot for a compiled object"_s;
    if (cell->isObject())
        return "field left its slot for another object"_s;
    return cell->isString() ? "field left its slot for a string"_s : "field left its slot for another primitive"_s;
}

void Object::moveFieldToExtraProperties(VM& vm, const FieldData& field, LeavingProperty leaving, SlotExitReason reason)
{
    willLeaveStateOfBirth(vm, *this, field, mutationOf(reason));
    validateMutation(vm, *this, field, mutationOf(reason));
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    JSObject* extra = ensureExtraProperties(vm);
    RELEASE_ASSERT(extra);
    State& state = this->state();
    if (!isInKeyOrder(state, field))
        didCreateField(state, field, false);
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasFieldOutsideSlot;
    setPerCellBit(true);
    putProperty(vm, extra, name, leaving);
    clear(field);
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    count(instance(), counterFor(reason));
    state.propertyCaches->countNamed(describeExit(reason, leaving.valueOrAccessor), name);
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
    {
        VM::DeletePropertyModeScope ignoreConfigurable(vm, VM::DeletePropertyMode::IgnoreConfigurable);
        DeletePropertySlot slot;
        JSObject::deleteProperty(extra, structure()->realm(), name, slot);
    }
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    return true;
}

bool Object::hasEveryFieldOfBirth() const
{
    State& state = this->state();
    return std::ranges::all_of(layout().fields(), [&](const FieldData& field) {
        if (field.holdsRawValueFromBirth) {
            const void* slot = slotOf(field);
            return slot && !slotHoldsMarker(field.slot.representation(), slot);
        }
        if (!field.isFilledAtBirth || isInSlot(field))
            return true;
        return field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field) && !livesInExtraProperties(vm(), *this, field);
    });
}

void Object::tryFinishBirth()
{
    if (perCellBit() && !(orderWord() & orderWordHasFieldOutsideSlot) && hasEveryFieldOfBirth())
        setPerCellBit(false);
}

void Object::deleteField(VM& vm, const FieldData& field)
{
    willLeaveStateOfBirth(vm, *this, field, ObjectMutation::DeleteField);
    validateMutation(vm, *this, field, ObjectMutation::DeleteField);
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    State& state = this->state();
    clear(field);
    if (field.isFilledAtBirth)
        setPerCellBit(true);
    removeFieldFromKeyOrder(vm, state, *this, field);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Remove, name);
    count(instance(), RuntimeCounterID::FieldsLeftSlotByDelete);
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

static bool defineExtraProperty(Object* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* extra = object->extraProperties();
    if (!parseIndex(name) && !object->isNotExtensible() && (!extra || !isValidOffset(extra->structure()->get(vm, name)))) {
        Object::LeavingProperty created = createPropertyFromDescriptor(globalObject, descriptor);
        if (object->tryDefineAbsentExtraProperty(vm, name, created.valueOrAccessor, created.attributes))
            return true;
        extra = object->extraProperties();
    }
    bool exists = extra && extra->hasOwnProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!exists && object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
    if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)))
        validateMutation(vm, *object, ObjectMutation::RedefineProperty);
    if (!exists) {
        extra = object->ensureExtraProperties(vm);
        RELEASE_ASSERT(extra);
        object->willAddExtraProperty(vm, name);
    }
    bool interceptedIndexedAccesses = interceptsIndexedAccesses(*object);
    scope.release();
    bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
    object->didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *object, exists ? VM::StructureChainIntegrityEvent::Change : VM::StructureChainIntegrityEvent::Add, name.uid());
    didChangeIndexedProperties(vm, *object, interceptedIndexedAccesses);
    return isDefined;
}

static bool defineFieldProperty(Object* object, JSGlobalObject* globalObject, const FieldData& field, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current && !current.isInSlot) {
        JSObject* extra = object->extraProperties();
        bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        object->tryMoveFieldBackToSlot(vm, field);
        object->didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Change, name.uid());
        return isDefined;
    }

    PropertyDescriptor currentDescriptor;
    if (current)
        currentDescriptor.setDescriptor(current.valueOrAccessor, current.attributes);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, !object->isNotExtensible(), descriptor, !!current, currentDescriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid)
        return false;

    if (!current) {
        Object::LeavingProperty created = createPropertyFromDescriptor(globalObject, descriptor);
        bool fitsSlot = created.attributes == object->attributesOf(field);
        if (fitsSlot && tryCreateField(vm, object->state(), *object, field, created.valueOrAccessor))
            return true;
        object->moveFieldToExtraProperties(vm, field, created, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
        return true;
    }

    unsigned attributes = descriptor.attributesOverridingCurrent(currentDescriptor);
    if (field.isAccessor() && descriptor.isAccessorDescriptor() && attributes == object->attributesOf(field)) {
        auto* held = uncheckedDowncast<GetterSetter>(current.valueOrAccessor);
        JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : held->isGetterNull() ? nullptr : held->getter();
        JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : held->isSetterNull() ? nullptr : held->setter();
        if (object->tryStore(vm, field, GetterSetter::create(vm, globalObject, getter, setter))) {
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
        return true;
    }

    object->moveFieldToExtraProperties(vm, field, { current.valueOrAccessor, current.attributes }, SlotExitReason::Redefinition);
    JSObject* extra = object->extraProperties();
    scope.release();
    bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
    object->didChangeExtraProperties(vm);
    return isDefined;
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    const FieldData* field = object->fieldNamed(object->state(), name);
    ASSERT(!field || !field->isPrivate());
    if (field && name.isSymbol()) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
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

static bool putFieldDirect(VM& vm, Object& object, const FieldData& field, PropertyName name, Object::LeavingProperty property)
{
    FieldProperty current = findFieldProperty(vm, object, field);
    bool fitsSlot = property.attributes == object.attributesOf(field);
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

bool Object::putDirectFromNativeCode(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    if (const FieldData* field = fieldNamed(state(), name))
        return putFieldDirect(vm, *this, *field, name, { value, attributes });
    JSObject* extra = ensureExtraProperties(vm);
    RELEASE_ASSERT(extra);
    if (!extra->getDirect(vm, name))
        willAddExtraProperty(vm, name);
    putProperty(vm, extra, name, { value, attributes });
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name.uid());
    return true;
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
    if (isInSlot(field) || livesInExtraProperties(vm, *this, field) || !layouts.objectHasType(layouts.typeOf(field), getter))
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

static AccessorDefinitionPlan& accessorDefinitionPlanFor(State& state, const Object& object, JSValue firstKey, JSValue firstFunction, size_t count)
{
    uintptr_t hash = object.structureID().bits() * 0x9e3779b1u ^ (std::bit_cast<uintptr_t>(firstKey.asCell()) >> 4) ^ count ^ functionOf(firstFunction.asCell()).value_or(FunctionID()).index() * 0x85ebca6bu;
    return state.objects->caches->accessorDefinitionPlans[hash % ObjectModelCaches::numberOfAccessorDefinitionPlans];
}

static bool applies(const AccessorDefinitionPlan& plan, const Object& object, std::span<const EncodedJSValue> keysAndFunctions, uint32_t getters, uint32_t enumerable)
{
    if (plan.layout != &object.layout() || plan.structureBits != object.structureID().bits() || plan.orderWordBefore != object.orderWord() || plan.getters != getters || plan.enumerable != enumerable || plan.definitions.size() * 2 != keysAndFunctions.size())
        return false;
    std::span definitions = plan.definitions.span();
    std::span slots = plan.slots.span();
    bool definesGettersOfEmptySlots = plan.definesGettersOfEmptySlots;
    for (size_t index = 0; index < definitions.size(); ++index) {
        const AccessorDefinitionPlan::Definition& definition = definitions[index];
        if (atomOf(JSValue::decode(keysAndFunctions[index * 2])) != definition.name || functionOf(JSValue::decode(keysAndFunctions[index * 2 + 1]).asCell()) != definition.function)
            return false;
        if (definesGettersOfEmptySlots && !slotHoldsAbsent(slots[index].field->slot.representation(), object.slotOf(*slots[index].field)))
            return false;
    }
    if (definesGettersOfEmptySlots)
        return true;
    for (const AccessorDefinitionPlan::FilledSlot& slot : slots) {
        if (slotHoldsAbsent(slot.field->slot.representation(), object.slotOf(*slot.field)) == slot.isFilledBefore)
            return false;
    }
    return true;
}

bool Object::tryDefineAccessorsOfLiteral(VM& vm, JSGlobalObject* globalObject, std::span<const EncodedJSValue> keysAndFunctions, uint32_t getters, uint32_t enumerable)
{
    State& state = this->state();
    size_t count = keysAndFunctions.size() / 2;
    if (!count || count >= AccessorDefinitionPlan::noDefinition || !JSValue::decode(keysAndFunctions[0]).isCell())
        return false;
    AccessorDefinitionPlan& plan = accessorDefinitionPlanFor(state, *this, JSValue::decode(keysAndFunctions[0]), JSValue::decode(keysAndFunctions[1]), count);
    bool isPlanned = applies(plan, *this, keysAndFunctions, getters, enumerable);
    if (!isPlanned) {
        // Nothing is written before every function is known to find its slot, so a literal that the plan does not serve is left to the general way whole.
        if (hasFlagsInOrderWord() || hasOutOfLineKeyOrder() || structure()->mayBePrototype())
            return false;
        RuntimeLayoutTable& layouts = state.objects->layouts;
        AccessorDefinitionPlan made { &layout(), structureID().bits(), orderWord(), 0, getters, enumerable, { }, { } };
        for (size_t index = 0; index < count; ++index) {
            UniquedStringImpl* name = atomOf(JSValue::decode(keysAndFunctions[index * 2]));
            JSCell* function = JSValue::decode(keysAndFunctions[index * 2 + 1]).asCell();
            const FieldData* field = name ? layouts.field(vm, layout(), name) : nullptr;
            unsigned attributes = PropertyAttribute::Accessor | (enumerable >> index & 1 ? PropertyAttribute::None : PropertyAttribute::DontEnum);
            if (!field || !field->isAccessor() || field->isOutOfLine() || field->isFilledAtBirth || attributes != attributesOf(*field))
                return false;
            bool isGetter = getters >> index & 1;
            std::optional<FunctionID> id = functionOf(function);
            if (!id || (isGetter && !layouts.objectHasType(layouts.typeOf(*field), function)))
                return false;
            made.definitions.append({ name, *id });
            size_t place = made.slots.findIf([&](const AccessorDefinitionPlan::FilledSlot& slot) {
                return slot.field == field;
            });
            if (place == notFound) {
                place = made.slots.size();
                made.slots.append({ field, AccessorDefinitionPlan::noDefinition, AccessorDefinitionPlan::noDefinition, isInSlot(*field) });
            }
            (isGetter ? made.slots[place].getter : made.slots[place].setter) = static_cast<uint8_t>(index);
        }
        // Compiled code calls the getter that it finds in a filled slot as one of those that the type of the field lists.
        for (const AccessorDefinitionPlan::FilledSlot& slot : made.slots) {
            if (!slot.isFilledBefore && slot.getter == AccessorDefinitionPlan::noDefinition && !layouts.objectHasType(layouts.typeOf(*slot.field), globalObject->nullGetterFunction()))
                return false;
        }
        made.definesGettersOfEmptySlots = made.slots.size() == count && std::ranges::all_of(made.slots, [](const AccessorDefinitionPlan::FilledSlot& slot) {
            return !slot.isFilledBefore && slot.setter == AccessorDefinitionPlan::noDefinition;
        });
        plan = WTF::move(made);
    }
    ProgramStrings& strings = *state.strings;
    bool definesGettersOfEmptySlots = plan.definesGettersOfEmptySlots;
    const EncodedJSValue* nextGetter = keysAndFunctions.data() + 1;
    for (const AccessorDefinitionPlan::FilledSlot& slot : plan.slots.span()) {
        JSObject* getter = nullptr;
        JSObject* setter = nullptr;
        if (definesGettersOfEmptySlots) [[likely]] {
            getter = asObject(JSValue::decode(*nextGetter));
            nextGetter += 2;
        } else {
            if (slot.isFilledBefore) {
                auto* held = uncheckedDowncast<GetterSetter>(read(*slot.field).asCell());
                getter = held->isGetterNull() ? nullptr : held->getter();
                setter = held->isSetterNull() ? nullptr : held->setter();
            }
            if (slot.getter != AccessorDefinitionPlan::noDefinition)
                getter = asObject(JSValue::decode(keysAndFunctions[slot.getter * 2 + 1]));
            if (slot.setter != AccessorDefinitionPlan::noDefinition)
                setter = asObject(JSValue::decode(keysAndFunctions[slot.setter * 2 + 1]));
        }
        auto* accessor = GetterSetter::create(vm, globalObject, getter, setter);
        bool isWritten = tryWriteSlot(strings, slot.field->slot.representation(), slotOf(*slot.field), accessor);
        RELEASE_ASSERT(isWritten);
        vm.writeBarrier(this, accessor);
        if (!isPlanned && !slot.isFilledBefore)
            didCreateField(state, *slot.field, true);
    }
    if (isPlanned) {
        at<uint32_t>(offsetOfOrderWord) = plan.orderWordAfter;
        return true;
    }
    if (!hasFlagsInOrderWord() && !hasOutOfLineKeyOrder() && structureID().bits() == plan.structureBits)
        plan.orderWordAfter = orderWord();
    else
        plan = { };
    return true;
}

bool Object::tryDefineAbsentExtraProperty(VM& vm, PropertyName name, JSValue valueOrAccessor, unsigned attributes)
{
    if (parseIndex(name) || name.isPrivateName() || isNotExtensible() || fieldNamed(state(), name))
        return false;
    JSObject* extra = extraProperties();
    if (extra && isValidOffset(extra->structure()->get(vm, name)))
        return false;
    extra = ensureExtraProperties(vm);
    if (!extra)
        return false;
    willAddExtraProperty(vm, name);
    if (!extra->tryPutDirectByExistingTransition(vm, name, valueOrAccessor, attributes))
        putProperty(vm, extra, name, { valueOrAccessor, attributes });
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, name.uid());
    return true;
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
    validateMutation(vm, *this, ObjectMutation::ChangeIntegrity);
    bool interceptedIndexedAccesses = interceptsIndexedAccesses(*this);
    if (JSObject* extra = extraProperties()) {
        setOrdinaryObjectIntegrity(globalObject, extra, integrity);
        didChangeExtraProperties(vm);
    }
    at<uint32_t>(offsetOfOrderWord) |= bits;
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, nullptr);
    didChangeIndexedProperties(vm, *this, interceptedIndexedAccesses);
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
            if (!isListed || !read(field))
                continue;
            unsigned attributes = attributesOf(field);
            if (!(attributes & PropertyAttribute::DontDelete))
                return false;
            if (testsFrozen && !field.isAccessor() && !(attributes & PropertyAttribute::ReadOnly))
                return false;
        }
        return true;
    };
    // The bits speak for the fields alone: native code may define an extra property on an object that is closed.
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
    validateMutation(vm, *this, ObjectMutation::ChangePrototype);
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    bool chainAlreadyMayInterceptIndexedAccesses = mayBePrototype() && anyObjectInChainMayInterceptIndexedAccesses();
    JSCell::setStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Prototype, nullptr);
    if (mayBePrototype() && !chainAlreadyMayInterceptIndexedAccesses && anyObjectInChainMayInterceptIndexedAccesses())
        structure()->realm()->haveABadTime(vm, this);
}

void Object::setPrototypeAtCreation(VM& vm, JSValue prototype)
{
    ASSERT(!mayBePrototype());
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    JSCell::setStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
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
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        if (field->isPrivate()) [[unlikely]]
            return false;
        if (name.isSymbol() && slot.internalMethodType() != PropertySlot::InternalMethodType::VMInquiry) [[unlikely]] {
            prepareForLookup(globalObject, object, name.uid());
            RETURN_IF_EXCEPTION(scope, false);
        }
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
    } else
        object->state().objects->layouts.didReadAbsentField(vm, object->layout(), name.uid());
    if (object->hasExtraProperties() && slot.internalMethodType() == PropertySlot::InternalMethodType::Get) {
        if (std::optional<Object*> source = sourceOfPendingBinding(vm, *object, name.uid())) {
            if (!*source)
                return false;
            JSValue value = (*source)->get(globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setValue(object, 0, value);
            slot.disableCaching();
            return true;
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
    JSObject* extra = uncheckedDowncast<Object>(cell)->extraProperties();
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
    validateKeyListing(vm, *object);
    std::span<const FieldData> fields = layout.fields();
    JSObject* extra = object->extraProperties();
    if (extra)
        extra->getOwnIndexedPropertyNames(globalObject, names, mode);
    auto extraKeyNames = object->extraKeyNames();
    KeyOrderStorage storage;
    std::span<const uint16_t> order = object->keyOrder(state, storage);
    auto isIncluded = [&](unsigned attributes) {
        return mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum);
    };
    auto addKeysInOrder = [&](bool symbols) {
        forEachPositionedKey(order, layout.numberOfFields, extraKeyNames.size(), [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (field.isPrivate() || !(symbols ? layouts.hasSymbolKey(field) : layouts.hasStringKey(field)))
                return;
            unsigned attributes = field.attributes();
            if (!object->isInSlot(field)) {
                FieldProperty property = findFieldProperty(vm, *object, field);
                if (!property)
                    return;
                attributes = property.attributes;
            }
            if (isIncluded(attributes))
                names.add(Identifier::fromUid(vm, layouts.nameOf(vm, field)));
        }, [&](size_t ordinal) {
            UniquedStringImpl* name = extraKeyNames[ordinal];
            unsigned attributes;
            if (name->isSymbol() == symbols && extra->getDirect(vm, name, attributes) && isIncluded(attributes))
                names.add(Identifier::fromUid(vm, name));
        });
    };
    auto addFieldsUnderPrivateNames = [&] {
        if (names.privateSymbolMode() != PrivateSymbolMode::Include) [[likely]]
            return;
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
                names.add(Identifier::fromUid(vm, layouts.nameOf(vm, field)));
        }
    };
    if (names.includeStringProperties())
        addKeysInOrder(false);
    if (!layout.hasFieldsWithSymbolKeys) {
        addFieldsUnderPrivateNames();
        if (extra)
            extra->getOwnNonIndexPropertyNames(globalObject, names, mode);
        return;
    }
    auto addExtraKeys = [&](bool symbols) {
        if (!extra)
            return;
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            if (entry.key()->isSymbol() == symbols && isIncluded(entry.attributes()))
                names.add(Identifier::fromUid(vm, entry.key()));
            return true;
        });
    };
    if (names.includeStringProperties())
        addExtraKeys(false);
    if (names.includeSymbolProperties()) {
        addKeysInOrder(true);
        addFieldsUnderPrivateNames();
        addExtraKeys(true);
    }
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
        // The words of JSObject::putByIndex().
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
    if (attributes != existing.attributes)
        object->didChangeExtraProperties(vm);
    return true;
}

static bool hasNeitherTrapOfDefinitions(VM& vm, State& state, JSObject* handler)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const Identifier* trap : { &vm.propertyNames->defineProperty, &vm.propertyNames->getOwnPropertyDescriptor }) {
        UniquedStringImpl* uid = trap->impl();
        if (const Slot* filed = state.propertyCaches->findByKey(handler->structure(), uid, SlotAccess::Read)) {
            if (filed->kind == Slot::Kind::Absent)
                continue;
            // The layout has a field of the name, and this object may lack it.
            if (filed->kind != Slot::Kind::Field || !filed->has(Slot::absentIsUndefined) || handler->type() != CompiledObjectType)
                return false;
            const Object* compiledHandler = uncheckedDowncast<Object>(handler);
            const FieldData* field = compiledHandler->fieldNamed(state, uid);
            if (!field || findFieldProperty(vm, *compiledHandler, *field))
                return false;
            continue;
        }
        // The question files its answer for the next time. It runs no code unless a proxy is asked.
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

JSObject* RuntimeHooks::targetThatProxyDefinesOn(VM& vm, JSValue receiver)
{
    if (!receiver.isCell() || receiver.asCell()->type() != ProxyObjectType)
        return nullptr;
    ProxyObject* proxy = uncheckedDowncast<ProxyObject>(receiver.asCell());
    // A proxy that has been revoked holds null for both.
    if (!proxy->handler().isObject())
        return nullptr;
    JSObject* handler = asObject(proxy->handler());
    Object* compiledHandler = compiledObjectOrNull(handler);
    Object* compiled = compiledHandler ? compiledHandler : compiledObjectOrNull(proxy->target());
    if (!compiled)
        return nullptr;
    State& state = compiled->state();
    if (!compiledHandler || compiledHandler->hasFlagsInOrderWord())
        return hasNeitherTrapOfDefinitions(vm, state, handler) ? proxy->target() : nullptr;

    StructureID structure = handler->structureID();
    HandlerKnownByItsSlots& known = state.handlersKnownByTheirSlots[(structure.bits() >> 4) % State::numberOfHandlersKnownByTheirSlots];
    auto isEmpty = [&](const FieldData* field) {
        return !field || !compiledHandler->isInSlot(*field);
    };
    if (known.structure != structure || known.prototype != handler->getPrototypeDirect().getObject()) {
        JSGlobalObject* realm = handler->realm();
        if (handler->getPrototypeDirect() != JSValue(realm->objectPrototype()) || !locksBuiltins(realm))
            return hasNeitherTrapOfDefinitions(vm, state, handler) ? proxy->target() : nullptr;
        known = { structure, realm->objectPrototype(), compiledHandler->fieldNamed(state, vm.propertyNames->defineProperty.impl()), compiledHandler->fieldNamed(state, vm.propertyNames->getOwnPropertyDescriptor.impl()) };
    }
    return isEmpty(known.fieldOfDefineProperty) && isEmpty(known.fieldOfGetOwnPropertyDescriptor) ? proxy->target() : nullptr;
}

static bool isProxyThatDefinesOnTarget(VM& vm, JSValue receiver, Object* target)
{
    return RuntimeHooks::targetThatProxyDefinesOn(vm, receiver) == target;
}

bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    const FieldData* field = object->fieldNamed(object->state(), name);
    if (field && name.isSymbol()) [[unlikely]] {
        prepareForLookup(globalObject, object, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
    }
    RELEASE_AND_RETURN(scope, putWithKnownField(cell, globalObject, name, value, slot, field));
}

bool Object::putWithKnownField(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot, const FieldData* field)
{
    ASSERT(!field || !field->isPrivate());
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isWritableInSlot = field && !(field->flags & (fieldIsReadOnly | fieldIsAccessor)) && !(object->orderWord() & orderWordIntegrityBits);
    if (isWritableInSlot && slot.thisValue() == JSValue(object) && object->readInline(*field) && object->tryStore(vm, *field, value))
        return true;
    if (isWritableInSlot && slot.thisValue() != JSValue(object) && object->readInline(*field)) {
        bool definesHere = isProxyThatDefinesOnTarget(vm, slot.thisValue(), object);
        RETURN_IF_EXCEPTION(scope, false);
        if (definesHere && object->tryStore(vm, *field, value))
            return true;
        // The field is a writable data property, and no field is named by an index.
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, name, value, slot));
    }
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]]
        RELEASE_AND_RETURN(scope, ordinarySet(globalObject, object, name, value, slot));
    if (field) {
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (current.isInSlot && !field->isAccessor()) {
            if (current.attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (object->tryStore(vm, *field, value))
                return true;
            object->moveFieldToExtraProperties(vm, *field, { value, current.attributes }, SlotExitReason::StoreOfOtherType);
            return true;
        }
        if (current) {
            bool isStored = putOutsideDataSlot(globalObject, object, name, { current.valueOrAccessor, current.attributes }, value, slot.isStrictMode());
            RETURN_IF_EXCEPTION(scope, false);
            if (isStored && !(current.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                object->tryMoveFieldBackToSlot(vm, *field);
            return isStored;
        }
    } else if (object->tryStoreExtraProperty(vm, name, value, { ExtraPropertyStoreFlag::IsKnownNotToBeField }).kind != ExtraPropertyStore::NotHandled)
        return true;
    else if (JSObject* extra = object->extraProperties()) {
        unsigned attributes;
        if (JSValue existing = extra->getDirect(vm, name, attributes))
            RELEASE_AND_RETURN(scope, putOutsideDataSlot(globalObject, object, name, { existing, attributes }, value, slot.isStrictMode()));
    }
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject()) {
        ChainWalk walk = walkChain({ globalObject, object->structure(), object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, name.uid());
        switch (storeThroughChain(walk)) {
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
        return true;
    }
    RELEASE_AND_RETURN(scope, defineOwnProperty(object, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), slot.isStrictMode()));
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    PutPropertySlot slot(cell, shouldThrow);
    return ordinarySet(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
}

bool Object::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const FieldData* field = object->fieldNamed(state, name);
    if (field) {
        if (field->isPrivate()) [[unlikely]]
            return true;
        if (name.isSymbol()) [[unlikely]] {
            prepareForLookup(globalObject, object, name.uid());
            RETURN_IF_EXCEPTION(scope, false);
        }
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (!current)
            return true;
        if (current.attributes & PropertyAttribute::DontDelete)
            return false;
        if (current.isInSlot) {
            if (!field->isAccessor()) {
                object->deleteField(vm, *field);
                return true;
            }
            // The Structure of the extra properties goes on saying that there has been an accessor, as that of an ordinary object does.
            object->moveFieldToExtraProperties(vm, *field, { current.valueOrAccessor, current.attributes }, SlotExitReason::Delete);
        }
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    std::optional<uint32_t> ordinal = field ? std::nullopt : object->extraKeyOrdinal(name);
    bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, false);
    slot.disableCaching();
    if (!isDeleted)
        return false;
    if (field)
        removeFieldFromKeyOrder(vm, state, *object, *field);
    else if (ordinal)
        object->removeExtraKeyFromOrder(vm, state, *ordinal);
    object->didChangeExtraProperties(vm);
    if (!field)
        object->didRemoveExtraKey(state);
    didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, name.uid());
    return true;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    VM& vm = globalObject->vm();
    bool isDeleted = extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
    object->didChangeExtraProperties(vm);
    if (isDeleted)
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, nullptr);
    return isDeleted;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
