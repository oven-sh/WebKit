/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC { namespace AOT {

static void writeExtraPropertiesWord(Object& object, JSObject* extra)
{
    SlotData slot = *object.compiledStructure().extraPropertiesSlot();
    if (slot.representation() == SlotRepresentation::Cell) {
        object.at<JSObject*>(slot.offset) = extra;
        return;
    }
    RELEASE_ASSERT(slot.representation() == SlotRepresentation::Cell32);
    object.at<uint32_t>(slot.offset) = encodeCell32(&object, extra);
}

static CompiledStructure* structureForNewExtraProperties(VM& vm, Object& object, JSObject* extra, bool namesStructure)
{
    StructureTable& structures = object.state().objects->structures;
    const CompiledStructure& current = object.compiledStructure();
    CompiledStructure* structure = namesStructure ? structures.withExtraProperties(vm, current, extra->structure()) : structures.existingWithExtraProperties(current, *extra->structure());
    return structure ? structure : structures.withExtraProperties(vm, current, nullptr);
}

JSObject* Object::extraPropertiesSlow() const
{
    SlotData slot = *compiledStructure().extraPropertiesSlot();
    if (slot.representation() == SlotRepresentation::Cell)
        return at<JSObject*>(slot.offset);
    ASSERT(slot.representation() == SlotRepresentation::Cell32);
    return asObject(decodeCell32(this, at<uint32_t>(slot.offset)));
}

JSObject* Object::ensureExtraProperties(VM& vm)
{
    if (JSObject* existing = extraProperties())
        return existing;
    if (!layout().extraPropertiesSlot())
        return nullptr;
    JSGlobalObject* realm = structure()->realm();
    JSObject* extra = constructEmptyObject(vm, Instance::of(realm)->realm.emptyExtraPropertiesStructure);
    if (isNotExtensible())
        extra->methodTable()->preventExtensions(extra, realm);
    setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, false));
    return extra;
}

void Object::setExtraProperties(VM& vm, JSObject* extra, CompiledStructure* withExtraProperties)
{
    ASSERT(!extraProperties() && layout().extraPropertiesSlot() && withExtraProperties->hasExtraProperties());
    writeExtraPropertiesWord(*this, extra);
    vm.mutatorFence();
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    vm.mutatorFence();
    JSCell::setStructure(vm, withExtraProperties);
    vm.writeBarrier(this, extra);
}

JSObject* Object::createExtraProperties(JSGlobalObject* realm, std::span<const ExtraProperty> inOrder)
{
    VM& vm = realm->vm();
    Structure* structure = Instance::of(realm)->realm.emptyExtraPropertiesStructure;
    Vector<PropertyOffset, 16> offsets;
    for (auto& property : inOrder) {
        PropertyOffset offset = invalidOffset;
        structure = parseIndex(*property.name) ? nullptr : Structure::addPropertyTransitionToExistingStructure(structure, property.name, 0, offset);
        if (!structure)
            break;
        offsets.append(offset);
    }
    if (!structure) {
        JSObject* extra = constructEmptyObject(vm, Instance::of(realm)->realm.emptyExtraPropertiesStructure);
        for (auto& property : inOrder) {
            if (auto index = parseIndex(*property.name))
                extra->putDirectIndex(realm, *index, property.value);
            else
                extra->putDirect(vm, property.name, property.value);
        }
        return extra;
    }
    Butterfly* butterfly = structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (!isInlineOffset(offsets[i]))
            butterfly->propertyStorage()[offsetInOutOfLineStorage(offsets[i])].setWithoutWriteBarrier(inOrder[i].value);
    }
    JSFinalObject* extra = JSFinalObject::createWithButterfly(vm, structure, butterfly);
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (isInlineOffset(offsets[i]))
            extra->putDirectOffset(vm, offsets[i], inOrder[i].value);
    }
    return extra;
}

// An extra-properties object that became empty stays, and bit 28 stays set: compiled code tests the bit at a member call on behalf of the member read before it.
void Object::didChangeExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    const CompiledStructure& current = compiledStructure();
    StructureID namedStructureID = current.extraPropertiesStructureID();
    if (namedStructureID == extra->structureID())
        return;
    StructureTable& structures = state().objects->structures;
    CompiledStructure* updatedStructure = structures.existingWithExtraProperties(current, *extra->structure());
    if (!updatedStructure && !namedStructureID)
        return;
    JSCell::setStructure(vm, updatedStructure ? updatedStructure : structures.withExtraProperties(vm, current, nullptr));
}

void Object::ensureStructureForExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    flattenOnce(vm, extra);
    const CompiledStructure& current = compiledStructure();
    if (current.extraPropertiesStructureID() == extra->structureID())
        return;
    CompiledStructure* updatedStructure = state().objects->structures.withExtraProperties(vm, current, extra->structure());
    if (updatedStructure != &current)
        JSCell::setStructure(vm, updatedStructure);
}

bool Object::ensureStructureForExtraPropertiesIfNeeded(VM& vm, JSCell* cell)
{
    const CompiledStructure* structure = compiledObjectStructureOrNull(cell->structure());
    if (!structure || !structure->hasExtraProperties() || structure->extraPropertiesStructureID()) [[likely]]
        return false;
    uncheckedDowncast<Object>(cell)->ensureStructureForExtraProperties(vm);
    return cell->structure() != structure;
}

std::optional<PropertyOffset> Object::offsetOfExtraProperty(VM& vm, JSCell* cell, PropertyName name, unsigned& attributes)
{
    const CompiledStructure* structure = compiledObjectStructureOrNull(cell->structure());
    if (!structure)
        return std::nullopt;
    Structure* extraPropertiesStructure = structure->extraPropertiesStructure();
    if (!extraPropertiesStructure || parseIndex(name) || name.isPrivateName())
        return std::nullopt;
    PropertyOffset offset = extraPropertiesStructure->get(vm, name, attributes);
    return isValidOffset(offset) ? std::optional(offset) : std::nullopt;
}

Object::ExtraPropertyStoreResult Object::tryStoreExtraProperty(VM& vm, PropertyName name, JSValue value, OptionSet<ExtraPropertyStoreFlag> flags)
{
    bool isDefinition = flags.contains(ExtraPropertyStoreFlag::IsDefinition);
    bool namesStructure = flags.contains(ExtraPropertyStoreFlag::NamesExtraPropertiesStructure);
    if (!flags.contains(ExtraPropertyStoreFlag::IsKnownNotToBeField) && fieldNamed(state(), name))
        return { };
    unsigned attributes = 0;
    JSObject* extra = extraProperties();
    PropertyOffset offset = extra ? extra->structure()->get(vm, name, attributes) : invalidOffset;
    if (!isValidOffset(offset) && (parseIndex(name) || name.isPrivateName() || name == vm.propertyNames->underscoreProto))
        return { };
    if (!extra && !layout().extraPropertiesSlot())
        return { };
    if (isValidOffset(offset)) {
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue) || (isDefinition && attributes))
            return { };
        extra->putDirectOffset(vm, offset, value);
        return { ExtraPropertyStore::Replaced, offset };
    }
    if (isNotExtensible())
        return { };
    JSGlobalObject* realm = structure()->realm();
    if (!isDefinition && !storeCreatesOwnProperty(realm, structure(), this, name.uid()))
        return { };
    willAddExtraProperty(vm, name);
    bool hadNone = !extra;
    if (hadNone)
        extra = constructEmptyObject(vm, Instance::of(realm)->realm.emptyExtraPropertiesStructure);
    extra->putDirect(vm, name, value);
    if (hadNone)
        setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, namesStructure));
    else if (namesStructure)
        ensureStructureForExtraProperties(vm);
    else
        didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, name.uid());
    return { ExtraPropertyStore::Added, extra->structure()->get(vm, name) };
}

bool Object::repeatsExtraPropertyStore(VM& vm, PropertyName name) const
{
    JSObject* extra = extraProperties();
    Structure* structure = extra ? extra->structure() : Instance::of(this->structure()->realm())->realm.emptyExtraPropertiesStructure;
    if (structure->isDictionary() || isValidOffset(structure->get(vm, name)))
        return true;
    PropertyOffset offset = invalidOffset;
    return Structure::addPropertyTransitionToExistingStructure(structure, name, 0, offset);
}

void Object::willAddExtraProperty(VM& vm, PropertyName name)
{
    validateMutation(vm, *this, ObjectMutation::AddProperty);
    state().objects->layouts.willAddProperty(vm, layout(), name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
