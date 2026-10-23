/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCaches.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static void validateSlotLiveness(VM& vm, const CustomAccessorTable& customAccessors, const Slot& slot)
{
    bool namesOnlyLiveCells = slot.namesOnlyLiveCells([&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    }, vm.heap.cage().base());
    bool namesFreedRecord = Slot::payloadOf(slot.kind) == SlotPayload::CustomAccessor && !customAccessors.isInUse(slot.customAccessor.index);
    if (!namesOnlyLiveCells || namesFreedRecord)
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, JSValue() });
}

static bool requiresFinishedWalk(Slot::Kind kind)
{
    switch (kind) {
    case Slot::Kind::Value:
    case Slot::Kind::Absent:
    case Slot::Kind::Getter:
    case Slot::Kind::Setter:
    case Slot::Kind::Transition:
    case Slot::Kind::TypedArrayLength:
        return true;
    case Slot::Kind::Empty:
    case Slot::Kind::FarValue:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::Field:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::InheritedField:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Constant:
    case Slot::Kind::Uncacheable:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* findUnregisteredChainObject(const ChainDependents& dependents, Structure* receiverStructure, const StructureLookupResult& found, UniquedStringImpl* uid)
{
    switch (found.outcome) {
    case StructureLookup::Unknown:
    case StructureLookup::Own:
        return nullptr;
    case StructureLookup::Absent:
    case StructureLookup::Inherited:
        break;
    }
    // An outcome other than Unknown proves that every Structure up to the holder has a mono proto.
    for (JSObject* object = receiverStructure->storedPrototypeObject(); object; object = object->structure()->storedPrototypeObject()) {
        Structure* structure = object->structure();
        bool isHolder = object == found.holder;
        bool canGainProperty = structure->isStructureExtensible() && !structure->isImmutablePropertyForReader(uid);
        bool canChange = isHolder ? !structure->isImmutablePropertyForReader(uid) : canGainProperty || !structure->hasImmutablePrototype();
        if (canChange && !dependents.hasDependents(object))
            return object;
        if (isHolder)
            break;
    }
    return nullptr;
}

static void validateSlotAnswer(VM& vm, const Slot& slot, SlotAccess access, UniquedStringImpl* uid, const ChainDependents* dependents)
{
    // The prototype of a string, symbol or BigInt comes from a realm, so its Structure alone says nothing.
    Structure* structure = slot.structureID.decode();
    if (!structure->isObject())
        return;
    if (!slotAgreesWithStructures(vm, slot, access, uid)) {
        dataLogLn("AOT validation: a row of the property caches does not agree with the Structure it is filed under: name ", uid ? String(uid) : "(none)"_str, ", kind of row ", static_cast<unsigned>(slot.kind), ", access ", static_cast<unsigned>(access), ", kind of dictionary ", static_cast<unsigned>(structure->dictionaryKind()), ", flattened before ", structure->hasBeenFlattenedBefore(), ", may be a prototype ", structure->mayBePrototype(), ", properties up to offset ", structure->maxOffset());
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheHitDiffers, { }, { }, structure });
    }
    if (!dependents || !slot.has(Slot::dependsOnChain) || !requiresFinishedWalk(slot.kind))
        return;
    if (JSObject* object = findUnregisteredChainObject(*dependents, structure, lookUpByStructure(vm, structure, uid), uid))
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, object });
}

static const ChainDependents* chainDependentsFor(Structure* structure)
{
    JSGlobalObject* realm = structure->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance && instance->state ? instance->state->chainDependents.get() : nullptr;
}

void VMPropertyCaches::validate(VM& vm) const
{
    auto validateSlot = [&](const Slot& slot, SlotAccess access, UniquedStringImpl* uidOrNull) {
        validateSlotLiveness(vm, m_customAccessors, slot);
        if (uidOrNull)
            validateSlotAnswer(vm, slot, access, uidOrNull, chainDependentsFor(slot.structureID.decode()));
    };

    // A collection may end off the mutator's thread, where no identifier can be created.
    const ProgramIdentifiers* identifiers = vm.aotState().identifiers.get();
    m_front.forEachSlot([&](const Slot& slot, uint32_t column, SlotAccess access) {
        validateSlot(slot, access, identifiers && column < identifiers->count() ? identifiers->tryGetIdentifierConcurrently(column) : nullptr);
    });
    m_hashedTable.forEachEntry([&](const HashedSlot& entry) {
        validateSlot(entry.slot, entry.access(), entry.uid());
    });
}

void PropertyCaches::didFinishCollection(VM& vm, CollectionScope scope)
{
    {
        Locker locker { m_state.caches->lock() };
        m_additionsSinceCollection.shrink(0);
    }
    m_shouldForgetMisses.store(true, std::memory_order_relaxed);
    if (!validatesRuntime(vm))
        return;
    if (vm.aotState().instances.first() == &m_state.instance)
        m_tables.validate(vm);
    m_siteCaches.forEachValidatedSite([&](const Slot& slot, UniquedStringImpl* uid, SlotAccess access) {
        validateSlotLiveness(vm, m_tables.customAccessors(), slot);
        if (slot.structureID)
            validateSlotAnswer(vm, slot, access, uid, m_state.chainDependents.get());
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
