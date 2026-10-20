/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "PreventCollectionScope.h"
#include "PropertyNameArray.h"

namespace JSC { namespace AOT {

static bool canLiveOutsideSlot(const FieldData& field)
{
    return field.canLeaveSlot() && !(field.flags & fieldMayExistWithoutStorage);
}

ShapeStatus Object::moveFieldToExtraProperties(VM& vm, const FieldData& field, LeavingProperty leaving, SlotExitReason reason)
{
    validateMutation(vm, *this, field, mutationOf(reason));
    Instance& instance = this->instance();
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    JSObject* extra = canLiveOutsideSlot(field) ? ensureExtraProperties(vm) : nullptr;
    if (!extra) {
        JSObject* ordinary = convertToOrdinaryObject(vm, ShapeLossReason::RepresentationLimit);
        putProperty(vm, ordinary, name, leaving);
        return ShapeStatus::Lost;
    }
    State& state = this->state();
    if (!isInKeyOrder(state, field))
        didCreateField(state, field, false);
    putProperty(vm, extra, name, leaving);
    clear(field);
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    count(instance, counterFor(reason));
    return ShapeStatus::Kept;
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
    {
        VM::DeletePropertyModeScope ignoreConfigurable(vm, VM::DeletePropertyMode::IgnoreConfigurable);
        DeletePropertySlot slot;
        JSObject::deleteProperty(extra, structure()->realm(), name, slot);
    }
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    return true;
}

ShapeStatus Object::deleteField(VM& vm, const FieldData& field)
{
    validateMutation(vm, *this, field, ObjectMutation::DeleteField);
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    if (!field.canLeaveSlot()) {
        JSObject* ordinary = convertToOrdinaryObject(vm, ShapeLossReason::RepresentationLimit);
        DeletePropertySlot slot;
        JSObject::deleteProperty(ordinary, ordinary->structure()->realm(), name, slot);
        return ShapeStatus::Lost;
    }
    State& state = this->state();
    clear(field);
    removeFieldFromKeyOrder(vm, state, *this, field);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Remove, name);
    count(instance(), RuntimeCounterID::FieldsLeftSlotByDelete);
    return ShapeStatus::Kept;
}

JSFinalObject* Object::copyAsOrdinaryObject(VM& vm, unsigned inlineCapacity, JSValue prototype)
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    JSGlobalObject* realm = structure()->realm();
    if (!prototype)
        prototype = getPrototypeDirect();
    Structure* empty = prototype.isObject() ? realm->structureCache().emptyObjectStructureForPrototype(realm, asObject(prototype), inlineCapacity) : JSFinalObject::createStructure(vm, realm, jsNull(), inlineCapacity);
    JSFinalObject* ordinary = JSFinalObject::create(vm, empty);
    JSObject* extra = extraProperties();
    auto copyExtra = [&](PropertyName name) {
        unsigned attributes;
        JSValue value = extra->getDirect(vm, name, attributes);
        if (!value || ordinary->getDirect(vm, name))
            return;
        if (name.isPrivateName())
            attributes = 0;
        putProperty(vm, ordinary, name.uid(), { value, attributes });
    };
    std::span<const FieldData> fields = layout.fields();
    auto extraKeyNames = this->extraKeyNames();
    KeyOrderStorage storage;
    forEachPositionedKey(keyOrder(state, storage), layout.numberOfFields, extraKeyNames.size(), [&](uint16_t index) {
        if (FieldProperty property = findFieldProperty(vm, *this, fields[index]))
            putProperty(vm, ordinary, propertyNameOf(vm, *this, fields[index]), { property.valueOrAccessor, property.attributes });
    }, [&](size_t ordinal) {
        copyExtra(extraKeyNames[ordinal]);
    });
    if (extra) {
        Vector<RefPtr<UniquedStringImpl>, 8> names;
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            names.append(entry.key());
            return true;
        });
        for (auto& name : names)
            copyExtra(name.get());
        PropertyNameArrayBuilder indices(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        extra->getOwnIndexedPropertyNames(realm, indices, DontEnumPropertiesMode::Include);
        for (auto& name : indices) {
            uint32_t index = *parseIndex(name);
            PropertySlot slot(extra, PropertySlot::InternalMethodType::VMInquiry, &vm);
            if (!extra->methodTable()->getOwnPropertySlotByIndex(extra, realm, index, slot))
                continue;
            RELEASE_ASSERT(slot.isValue());
            ordinary->putDirectIndex(realm, index, slot.getValue(realm, index), slot.attributes(), PutDirectIndexLikePutDirect);
        }
    }
    if (isNotExtensible())
        ordinary->methodTable()->preventExtensions(ordinary, realm);
    return ordinary;
}

JSObject* Object::convertToOrdinaryObject(VM& vm, ShapeLossReason reason, JSValue prototype)
{
    validateMutation(vm, *this, ObjectMutation::LoseShape);
    validateMutation(vm, *this, mutationOf(reason));
    Instance& instance = this->instance();
    bool wasPrototype = structure()->mayBePrototype();
    materializeDeferredClosures(vm);
    static_assert(sizeof(JSObjectWithButterfly) == offsetOfFirstField && JSObjectWithButterfly::butterflyOffset() == offsetOfOrderWord);
    unsigned inlineCapacity = std::min<size_t>((cellSize() - offsetOfFirstField) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
    JSFinalObject* ordinary = copyAsOrdinaryObject(vm, inlineCapacity, prototype);
    if (wasPrototype)
        ordinary->didBecomePrototype(vm);
    if (ordinary->structure()->isDictionary())
        ordinary->flattenDictionaryObject(vm);
    RELEASE_ASSERT(ordinary->structure()->inlineCapacity() == inlineCapacity);
    {
        PreventCollectionScope preventCollection(vm.heap);
        for (unsigned i = 0; i < inlineCapacity; ++i)
            at<EncodedJSValue>(offsetOfFirstField + i * sizeof(EncodedJSValue)) = JSValue::encode(ordinary->inlineStorage()[i].get());
        at<Butterfly*>(offsetOfOrderWord) = ordinary->butterfly();
        JSCell::setStructure(vm, ordinary->structure());
    }
    vm.writeBarrier(this);
    ensureStillAliveHere(ordinary);
    instance.bumpShapeEpoch();
    count(instance, RuntimeCounterID::ShapeEpochBumps);
    count(instance, counterFor(reason));
    if (wasPrototype)
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
    return this;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
