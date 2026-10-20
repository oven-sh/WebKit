/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesAnswers.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include <limits>

namespace JSC { namespace AOT {

static Slot uncacheableSlot(StructureID id, uint8_t flags)
{
    return Slot::createWithoutPayload(id, Slot::Kind::Uncacheable, flags & Slot::dependsOnChain);
}

static const CompiledStructure& compiledStructureOf(Structure* structure)
{
    RELEASE_ASSERT(structure);
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    RELEASE_ASSERT(compiled);
    return *compiled;
}

static std::optional<uint16_t> locationOfHiddenWord(std::optional<SlotData> hiddenWord)
{
    uint32_t offset = hiddenWord ? hiddenWord->offset : 0;
    if (offset > std::numeric_limits<uint16_t>::max())
        return std::nullopt;
    return static_cast<uint16_t>(offset);
}

static Slot slotForField(const PropertyAnswer& answer, Structure* receiverStructure, Slot::Kind kind, uint8_t flags)
{
    RELEASE_ASSERT(answer.field);
    const FieldData& field = *answer.field;
    std::optional<SlotData> extensionWord = compiledStructureOf(receiverStructure).extensionSlot();
    RELEASE_ASSERT(extensionWord || !field.isOutOfLine());
    StructureID id = receiverStructure->id();
    std::optional<uint16_t> extensionWordOffset = locationOfHiddenWord(extensionWord);
    if (!extensionWordOffset)
        return uncacheableSlot(id, flags);
    if (field.isOutOfLine())
        flags |= Slot::isOutOfLine;
    if (answer.absentIsUndefined)
        flags |= Slot::absentIsUndefined;
    return Slot::createField(id, kind, flags, field.slot, answer.access == SlotAccess::Read ? 0 : field.type, *extensionWordOffset);
}

// A record that has no room for the holder says what it says of the field alone.
static Slot slotForFieldOrInherited(const PropertyAnswer& answer, Structure* receiverStructure, uint8_t flags)
{
    RELEASE_ASSERT(answer.field && answer.holder);
    bool isRead = answer.access == SlotAccess::Read;
    uint8_t flagsOfKind = flags;
    if (isRead && answer.kind == PropertyAnswerKind::FieldOrInheritedAccessor)
        flagsOfKind |= Slot::inheritsGetter;
    if (answer.field->isOutOfLine())
        flagsOfKind |= Slot::isOutOfLine;
    Slot::Kind kind = isRead ? Slot::Kind::FieldOrInherited : Slot::Kind::FieldOrInheritedSetter;
    if (auto slot = Slot::tryCreateFieldAndHolder(receiverStructure->id(), kind, flagsOfKind, answer.field->slot, answer.holder, answer.offset, receiverStructure->vm().heap.cage().base()))
        return *slot;
    return slotForField(answer, receiverStructure, Slot::Kind::Field, flags);
}

static Slot slotForVariable(const PropertyAnswer& answer, StructureID id, uint8_t flags)
{
    if (answer.access == SlotAccess::Read)
        return Slot::createField(id, Slot::Kind::Variable, flags, answer.variable, 0, 0);
    RELEASE_ASSERT(answer.field);
    return Slot::createField(id, Slot::Kind::Variable, flags, answer.variable, answer.field->type, 0);
}

static Slot slotForExtraProperty(const PropertyAnswer& answer, Structure* receiverStructure, Slot::Kind kind, uint8_t flags)
{
    const CompiledStructure& structure = compiledStructureOf(receiverStructure);
    RELEASE_ASSERT(structure.extraPropertiesSlot() && structure.extraPropertiesStructure());
    StructureID id = receiverStructure->id();
    std::optional<uint16_t> extraPropertiesWordOffset = locationOfHiddenWord(structure.extraPropertiesSlot());
    if (!extraPropertiesWordOffset)
        return uncacheableSlot(id, flags);
    return Slot::createExtraProperty(id, kind, flags, StructureID(), answer.offset, *extraPropertiesWordOffset);
}

static Slot slotForExtraPropertyAddition(const PropertyAnswer& answer, StructureID id, uint8_t flags)
{
    const CompiledStructure& newStructure = compiledStructureOf(answer.newStructure);
    RELEASE_ASSERT(newStructure.extraPropertiesSlot() && newStructure.extraPropertiesStructure());
    std::optional<uint16_t> extraPropertiesWordOffset = locationOfHiddenWord(newStructure.extraPropertiesSlot());
    if (!extraPropertiesWordOffset)
        return uncacheableSlot(id, flags);
    if (answer.newStorageCapacity)
        flags |= Slot::growsStorage;
    if (answer.storageHasIndexingHeader)
        flags |= Slot::storageHasIndexingHeader;
    return Slot::createExtraPropertyAddition(id, flags, answer.newStructure->id(), answer.offset, *extraPropertiesWordOffset);
}

static Slot slotForAddition(const PropertyAnswer& answer, StructureID id, uint8_t flags)
{
    if (answer.storageHasIndexingHeader)
        flags |= Slot::storageHasIndexingHeader;
    return Slot::tryCreateTransition(id, flags, answer.newStructure->id(), answer.offset, answer.newStorageCapacity).value_or(uncacheableSlot(id, flags));
}

Slot slotFor(const PropertyAnswer& answer, Structure* receiverStructure)
{
    using Kind = Slot::Kind;
    StructureID id = receiverStructure ? receiverStructure->id() : StructureID();
    bool isRead = answer.access == SlotAccess::Read;
    uint8_t flags = 0;
    if (answer.isOwn)
        flags |= Slot::isOwn;
    if (answer.dependsOnChain())
        flags |= Slot::dependsOnChain;

    switch (answer.kind) {
    case PropertyAnswerKind::Uncacheable:
        return uncacheableSlot(id, flags);
    case PropertyAnswerKind::Value:
        return Slot::createInStorage(id, Kind::Value, flags, answer.holder, answer.offset);
    case PropertyAnswerKind::Absent:
        return Slot::createWithoutPayload(id, Kind::Absent, flags);
    case PropertyAnswerKind::Accessor:
        return Slot::createInStorage(id, isRead ? Kind::Getter : Kind::Setter, flags, answer.holder, answer.offset);
    case PropertyAnswerKind::CustomAccessor:
        return Slot::createCustomAccessor(id, isRead ? Kind::CustomGetter : Kind::CustomSetter, flags, answer.customAccessor);
    case PropertyAnswerKind::Field:
        return slotForField(answer, receiverStructure, Kind::Field, flags);
    case PropertyAnswerKind::FieldCreation:
        return slotForField(answer, receiverStructure, Kind::FieldCreation, flags);
    case PropertyAnswerKind::FieldOrInheritedValue:
    case PropertyAnswerKind::FieldOrInheritedAccessor:
        return slotForFieldOrInherited(answer, receiverStructure, flags);
    case PropertyAnswerKind::Variable:
        return slotForVariable(answer, id, flags);
    case PropertyAnswerKind::ExtraProperty:
        return slotForExtraProperty(answer, receiverStructure, Kind::ExtraProperty, flags);
    case PropertyAnswerKind::ExtraAccessor:
        return slotForExtraProperty(answer, receiverStructure, isRead ? Kind::ExtraGetter : Kind::ExtraSetter, flags);
    case PropertyAnswerKind::ExtraPropertyAddition:
        return slotForExtraPropertyAddition(answer, id, flags);
    case PropertyAnswerKind::Addition:
        return slotForAddition(answer, id, flags);
    case PropertyAnswerKind::ArrayLength:
        return Slot::createWithoutPayload(id, Kind::ArrayLength, flags);
    case PropertyAnswerKind::TypedArrayLength:
        return Slot::createWithoutPayload(id, Kind::TypedArrayLength, flags);
    case PropertyAnswerKind::StringLength:
        return Slot::createWithoutPayload(id, Kind::StringLength, flags);
    case PropertyAnswerKind::ClosureLength:
        return Slot::createWithoutPayload(id, Kind::ClosureLength, flags);
    case PropertyAnswerKind::ClosureName:
        return Slot::createWithoutPayload(id, Kind::ClosureName, flags);
    case PropertyAnswerKind::Constant:
        return Slot::createConstant(id, flags, answer.constant);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue readThroughAnswer(JSGlobalObject* globalObject, JSValue base, const PropertyAnswer& answer)
{
    switch (answer.kind) {
    case PropertyAnswerKind::Value:
        return answer.holder->getDirect(answer.offset);
    case PropertyAnswerKind::Accessor:
        return uncheckedDowncast<GetterSetter>(answer.holder->getDirect(answer.offset).asCell())->callGetter(globalObject, base);
    case PropertyAnswerKind::Absent:
        return jsUndefined();
    case PropertyAnswerKind::Uncacheable:
    case PropertyAnswerKind::CustomAccessor:
    case PropertyAnswerKind::Field:
    case PropertyAnswerKind::FieldCreation:
    case PropertyAnswerKind::FieldOrInheritedValue:
    case PropertyAnswerKind::FieldOrInheritedAccessor:
    case PropertyAnswerKind::Variable:
    case PropertyAnswerKind::ExtraProperty:
    case PropertyAnswerKind::ExtraAccessor:
    case PropertyAnswerKind::ExtraPropertyAddition:
    case PropertyAnswerKind::Addition:
    case PropertyAnswerKind::ArrayLength:
    case PropertyAnswerKind::TypedArrayLength:
    case PropertyAnswerKind::StringLength:
    case PropertyAnswerKind::ClosureLength:
    case PropertyAnswerKind::ClosureName:
    case PropertyAnswerKind::Constant:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool flattenOnce(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (!structure->isDictionary())
        return true;
    if (structure->hasBeenFlattenedBefore() || structure->typeInfo().prohibitsPropertyCaching() || structure->isCompiledStructure())
        return false;
    structure->flattenDictionaryStructure(vm, object);
    return true;
}

static bool isReadableByStructure(Structure* structure)
{
    TypeInfo info = structure->typeInfo();
    return info.isObject() && structure->hasMonoProto() && !info.overridesGetOwnPropertySlot() && !info.overridesGetPrototype()
        && !TypeInfo::hasStaticPropertyTable(info.inlineTypeFlags()) && !structure->isCompiledStructure() && !structure->isUncacheableDictionary();
}

StructureLookupResult lookUpByStructure(VM&, Structure* structure, UniquedStringImpl* uid)
{
    JSObject* holder = nullptr;
    while (true) {
        if (!isReadableByStructure(structure))
            return { };
        unsigned attributes = 0;
        PropertyOffset offset = structure->getConcurrently(uid, attributes);
        if (isValidOffset(offset))
            return { holder ? StructureLookup::Inherited : StructureLookup::Own, holder, offset, attributes };
        holder = structure->storedPrototypeObject();
        if (!holder)
            return { StructureLookup::Absent, nullptr, invalidOffset, 0 };
        structure = holder->structure();
    }
}

static bool isInterceptedByHolder(unsigned attributes)
{
    return attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue);
}

static bool slotInStorageAgrees(const Slot& slot, SlotAccess access, const StructureLookupResult& found)
{
    bool isOwn = slot.has(Slot::isOwn);
    if (found.outcome != (isOwn ? StructureLookup::Own : StructureLookup::Inherited))
        return false;
    if ((!isOwn && found.holder != slot.holder) || found.offset != slot.offsetInStorage())
        return false;
    if (slot.kind == Slot::Kind::Getter || slot.kind == Slot::Kind::Setter)
        return found.attributes & PropertyAttribute::Accessor;
    switch (access) {
    case SlotAccess::Read:
        return !(found.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue);
    case SlotAccess::Store:
        return !isInterceptedByHolder(found.attributes);
    case SlotAccess::Define:
        return !found.attributes;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool transitionSlotAgrees(const Slot& slot, SlotAccess access, const StructureLookupResult& found, UniquedStringImpl* uid)
{
    if (found.outcome == StructureLookup::Own)
        return false;
    if (access == SlotAccess::Store && found.outcome == StructureLookup::Inherited && isInterceptedByHolder(found.attributes))
        return false;
    Structure* newStructure = slot.transition.newStructureID.decode();
    return newStructure->previousID() == slot.structureID.decode() && newStructure->getConcurrently(uid) == slot.offsetInStorage();
}

bool slotAgreesWithStructures(VM& vm, const Slot& slot, SlotAccess access, UniquedStringImpl* uid)
{
    if (!slot.structureID)
        return true;
    StructureLookupResult found = lookUpByStructure(vm, slot.structureID.decode(), uid);
    if (found.outcome == StructureLookup::Unknown)
        return true;

    switch (slot.kind) {
    case Slot::Kind::Value:
    case Slot::Kind::FarValue:
    case Slot::Kind::Getter:
    case Slot::Kind::Setter:
        return slotInStorageAgrees(slot, access, found);
    case Slot::Kind::Absent:
        return found.outcome == StructureLookup::Absent;
    case Slot::Kind::Transition:
        return transitionSlotAgrees(slot, access, found, uid);
    case Slot::Kind::Empty:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::Field:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Constant:
    case Slot::Kind::Uncacheable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
