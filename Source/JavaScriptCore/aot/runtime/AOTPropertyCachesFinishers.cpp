/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationScope.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeRecords.h"
#include "AOTSchemaRuntimeOperations.h"
#include "GetterSetter.h"
#include <array>
#include <bit>

namespace JSC { namespace AOT {

static void countReason(Instance& instance, uint32_t reason)
{
    RuntimeCounterID counter = static_cast<RuntimeCounterID>(reason);
    ASSERT(countsStubExit(counter));
    count(instance, counter);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallGetterOfSlot, EncodedJSValue, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCallGetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    countReason(*instance, reason);
    OPERATION_RETURN(scope, JSValue::encode(cellArgument<GetterSetter>(getterSetter, entry)->callGetter(globalObject, JSValue::decode(base))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallSetterOfSlot, void, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, EncodedJSValue value, uint32_t putMode, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCallSetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    countReason(*instance, reason);
    bool shouldThrow = static_cast<PutMode>(putMode) == PutMode::Strict;
    cellArgument<GetterSetter>(getterSetter, entry)->callSetter(globalObject, JSValue::decode(base), JSValue::decode(value), shouldThrow);
    OPERATION_RETURN(scope);
}

static void addPropertyByTransition(VM& vm, JSObject* target, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    ASSERT((newStructure->isTransitionFrom(target->structure()) || newStructure->transitionKind() == TransitionKind::PropertyRunAddition) && newStructure->isValidOffset(offset));
    // An array that was born with room for its first named properties has the storage that the transition asks for.
    bool hasRoomInOwnCell = !target->structure()->outOfLineCapacity() && newStructure->outOfLineCapacity() == initialOutOfLineCapacity
        && std::bit_cast<char*>(target->butterfly()) == std::bit_cast<char*>(target) + sizeOfArrayWithStorage(0, propertyRoomOfArray);
    if (!hasRoomInOwnCell)
        target->growOutOfLineStorageForTransition(vm, newStructure);
    target->putDirectOffset(vm, offset, value);
    target->setStructure(vm, newStructure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAddPropertyBySlot, void, (Instance* instance, JSCell* object, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t reason))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAddPropertyBySlot);
    countReason(*instance, reason);
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    RELEASE_ASSERT(object->structureID() == slot.structureID);
    if (slot.kind == Slot::Kind::Transition) {
        addPropertyByTransition(vm, asObject(object), slot.transition.newStructureID.decode(), slot.offsetInStorage(), JSValue::decode(value));
        return;
    }

    RELEASE_ASSERT(slot.kind == Slot::Kind::ExtraPropertyAddition);
    Object* receiver = uncheckedDowncast<Object>(object);
    Structure* newStructure = slot.extraPropertyAddition.newStructureID.decode();
    JSObject* extraProperties = receiver->ensureExtraProperties(vm);
    RELEASE_ASSERT(extraProperties);
    addPropertyByTransition(vm, extraProperties, compiledObjectStructureOrNull(newStructure)->extraPropertiesStructure(), Slot::offsetFor(slot.extraPropertyAddition.wordIndex), JSValue::decode(value));
    receiver->JSCell::setStructure(vm, newStructure);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
