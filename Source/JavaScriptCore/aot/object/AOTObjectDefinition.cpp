/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTObjectInternal.h"
#include "AOTCompiledCallee.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "TypeError.h"

namespace JSC { namespace AOT {

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
    // A property that is not there is created with what the descriptor says, and with false or undefined for what it does not say.
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

// A property is in its slot only while it has the form that the layout gives the field, with what sealing or freezing the object adds. The slot of an accessor holds its GetterSetter.
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
    // `{ get name() { .. }, set name(value) { .. } }`: the second half joins the first. A getter that the slot does not take leaves with the property.
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
    const FieldData* field = object->fieldNamed(object->state(), name);
    ASSERT(!field || !field->isPrivate());
    if (field)
        return defineFieldProperty(object, globalObject, *field, name, descriptor, shouldThrow);
    return defineExtraProperty(object, globalObject, name, descriptor, shouldThrow);
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

// defineFieldProperty() for `get name() { .. }` where the object has no property of the name yet and the slot takes the getter.
bool Object::tryDefineAbsentGetterInSlot(VM& vm, JSGlobalObject* globalObject, UniquedStringImpl* name, JSObject* getter, unsigned attributes)
{
    State& state = this->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const FieldData* field = layouts.field(vm, layout(), name);
    if (!field || !field->isAccessor() || isNotExtensible() || attributes != attributesOf(*field))
        return false;
    if (isInSlot(*field) || livesInExtraProperties(vm, *this, *field) || !layouts.objectHasType(layouts.typeOf(*field), getter))
        return false;
    if (!store(vm, *field, GetterSetter::create(vm, globalObject, getter, nullptr)))
        return false;
    didCreateField(state, *field, true);
    if (field->isFilledAtBirth)
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

// Several literals may make objects of one layout, each with getters of its own.
static GetterDefinitionPlan& getterDefinitionPlanFor(State& state, const Object& object, JSValue firstKey, JSValue firstGetter, size_t count)
{
    uintptr_t hash = object.structureID().bits() * 0x9e3779b1u ^ (std::bit_cast<uintptr_t>(firstKey.asCell()) >> 4) ^ count ^ functionOf(firstGetter.asCell()).value_or(FunctionID()).index() * 0x85ebca6bu;
    return state.objects->caches->getterDefinitionPlans[hash % ObjectModelCaches::numberOfGetterDefinitionPlans];
}

static bool applies(const GetterDefinitionPlan& plan, const Object& object, std::span<const EncodedJSValue> keysAndGetters, uint32_t enumerable)
{
    if (plan.layout != &object.layout() || plan.structureBits != object.structureID().bits() || plan.orderWordBefore != object.orderWord() || plan.enumerable != enumerable || plan.entries.size() * 2 != keysAndGetters.size())
        return false;
    for (size_t index = 0; index < plan.entries.size(); ++index) {
        const GetterDefinitionPlan::Entry& entry = plan.entries[index];
        if (atomOf(JSValue::decode(keysAndGetters[index * 2])) != entry.name || functionOf(JSValue::decode(keysAndGetters[index * 2 + 1]).asCell()) != entry.getter)
            return false;
        if (!slotHoldsAbsent(entry.field->slot.representation(), object.slotOf(*entry.field)))
            return false;
    }
    return true;
}

bool Object::tryDefineGettersOfLiteral(VM& vm, JSGlobalObject* globalObject, std::span<const EncodedJSValue> keysAndGetters, uint32_t enumerable)
{
    State& state = this->state();
    size_t count = keysAndGetters.size() / 2;
    if (!count || !JSValue::decode(keysAndGetters[0]).isCell())
        return false;
    GetterDefinitionPlan& plan = getterDefinitionPlanFor(state, *this, JSValue::decode(keysAndGetters[0]), JSValue::decode(keysAndGetters[1]), count);
    if (applies(plan, *this, keysAndGetters, enumerable)) {
        for (size_t index = 0; index < count; ++index) {
            const FieldData& field = *plan.entries[index].field;
            auto* accessor = GetterSetter::create(vm, globalObject, asObject(JSValue::decode(keysAndGetters[index * 2 + 1])), nullptr);
            bool isWritten = tryWriteSlot(*state.strings, field.slot.representation(), slotOf(field), accessor);
            RELEASE_ASSERT(isWritten);
            vm.writeBarrier(this, accessor);
        }
        at<uint32_t>(offsetOfOrderWord) = plan.orderWordAfter;
        return true;
    }

    // Nothing is written before every getter is known to find its slot, so a literal that the plan does not serve is left to the general way whole.
    if (hasFlagsInOrderWord() || hasOutOfLineKeyOrder() || structure()->mayBePrototype())
        return false;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    GetterDefinitionPlan made { &layout(), structureID().bits(), orderWord(), 0, enumerable, { } };
    for (size_t index = 0; index < count; ++index) {
        UniquedStringImpl* name = atomOf(JSValue::decode(keysAndGetters[index * 2]));
        JSCell* getter = JSValue::decode(keysAndGetters[index * 2 + 1]).asCell();
        const FieldData* field = name ? layouts.field(vm, layout(), name) : nullptr;
        unsigned attributes = PropertyAttribute::Accessor | (enumerable >> index & 1 ? PropertyAttribute::None : PropertyAttribute::DontEnum);
        if (!field || !field->isAccessor() || field->isOutOfLine() || field->isFilledAtBirth || attributes != attributesOf(*field) || isInSlot(*field))
            return false;
        std::optional<FunctionID> function = functionOf(getter);
        if (!function || !layouts.objectHasType(layouts.typeOf(*field), getter))
            return false;
        bool isRepeated = made.entries.containsIf([&](const GetterDefinitionPlan::Entry& entry) {
            return entry.field == field;
        });
        if (isRepeated)
            return false;
        made.entries.append({ name, field, *function });
    }
    for (size_t index = 0; index < count; ++index) {
        const FieldData& field = *made.entries[index].field;
        bool isStored = store(vm, field, GetterSetter::create(vm, globalObject, asObject(JSValue::decode(keysAndGetters[index * 2 + 1])), nullptr));
        RELEASE_ASSERT(isStored);
        didCreateField(state, field, true);
    }
    if (!hasFlagsInOrderWord() && !hasOutOfLineKeyOrder() && structureID().bits() == made.structureBits) {
        made.orderWordAfter = orderWord();
        plan = WTF::move(made);
    }
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
