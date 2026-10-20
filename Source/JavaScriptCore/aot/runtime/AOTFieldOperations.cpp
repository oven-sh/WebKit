/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeRecords.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include <array>
#include <bit>

namespace JSC { namespace AOT {

static RuntimeCounterID stubExitCounter(uint32_t reason, Entry entry)
{
    auto counter = static_cast<RuntimeCounterID>(reason);
    if (!countsStubExit(counter)) [[unlikely]]
        crashOnWrongOperand(entry);
    return counter;
}

static const FieldData& fieldOf(Object* object, uint64_t header, uint64_t payload, Entry entry)
{
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    if (Slot::payloadOf(slot.kind) != SlotPayload::Field && Slot::payloadOf(slot.kind) != SlotPayload::FieldAndHolder) [[unlikely]]
        crashOnWrongOperand(entry);
    // An inline field and a field in the extension may have the same offset and representation.
    for (const FieldData& field : object->layout().fields()) {
        if (field.slot == slot.fieldSlot() && field.isOutOfLine() == slot.has(Slot::isOutOfLine))
            return field;
    }
    crashOnWrongOperand(entry);
}

static void storeInPresentField(VM& vm, Object* object, const FieldData& field, JSValue value)
{
    if (!object->tryStore(vm, field, value))
        (void)object->moveFieldToExtraProperties(vm, field, { value, object->attributesOf(field) }, SlotExitReason::StoreOfOtherType);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTReadField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    if (field.slot.representation() == SlotRepresentation::DeferredClosure)
        return JSValue::encode(object->materializeDeferredClosure(vm, field));
    JSValue value = object->read(field);
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStoreField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTStoreField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    if (!findFieldProperty(vm, *object, field).isInSlot) [[unlikely]]
        crashOnWrongOperand(entry);
    storeInPresentField(vm, object, field, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCreateField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue encodedValue, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCreateField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    JSValue value = JSValue::decode(encodedValue);
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current.isInSlot) {
        storeInPresentField(vm, object, field, value);
        return;
    }
    if (current || object->isNotExtensible()) [[unlikely]]
        crashOnWrongOperand(entry);
    if (!tryCreateField(vm, *instance->state, *object, field, value))
        object->putDirectFromNativeCode(vm, identifierAt(instance, field.name), value, 0);
}

static const FieldData& namedField(State& state, Object* object, const Identifier& identifier)
{
    const FieldData* field = object->fieldNamed(state, identifier);
    RELEASE_ASSERT(field);
    return *field;
}

static const FieldData& fieldAtSlotKey(State& state, Object* object, uint32_t slotKey)
{
    const FieldData* field = state.objects->keyOrders.fieldAt(object->layout(), slotKey);
    RELEASE_ASSERT(field);
    return *field;
}

static void listCreatedField(State& state, Object* object, const FieldData& field)
{
    if (object->structure()->mayBePrototype()) [[unlikely]] {
        VM& vm = *state.instance.vm;
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    }
    if (!object->isInKeyOrder(state, field)) {
        object->didCreateField(state, field);
        return;
    }
    if (object->hasOutOfLineKeyOrder())
        return;
    uint32_t orderAndCount = object->orderWord() & orderAndCountMask;
    if (std::optional<uint32_t> numberOfExtraKeys = object->numberOfExtraKeysByStructure())
        state.objects->keyOrders.publishTransition(object->layout(), orderAndCount, field, orderAndCount, *numberOfExtraKeys);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateField);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateField);
    const FieldData& field = namedField(state, object, identifierAt(instance, key));
    didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, identifierAt(instance, key).impl());
    object->didCreateField(state, field, !(field.flags & fieldMayExistWithoutStorage));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldAt);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldAt);
    vm.writeBarrier(object);
    listCreatedField(state, object, fieldAtSlotKey(state, object, offset));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldOutOfLineAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldOutOfLineAt);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldOutOfLineAt);
    vm.writeBarrier(object);
    const FieldData& field = fieldAtSlotKey(state, object, offset | outOfLineSlotKey);
    listCreatedField(state, object, field);
    object->ensureStorageFor(vm, field);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldsAt, void, (Instance* instance, JSCell* cell, uint64_t offsets))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldsAt);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldsAt);
    vm.writeBarrier(object);
    for (uint64_t rest = offsets; static_cast<uint32_t>(rest) & slotKeyMask; rest >>= bitsPerSlotKey)
        listCreatedField(state, object, fieldAtSlotKey(state, object, static_cast<uint32_t>(rest) & slotKeyMask));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranules, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnsureExtensionGranules);
    cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranules)->ensureExtensionGranules(vm, granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStoreDoubleInExtension, void, (Instance* instance, JSCell* cell, uint32_t overflowOffset, double value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStoreDoubleInExtension);
    cellArgument<Object>(cell, Entry::operationAOTStoreDoubleInExtension)->storeDoubleInExtension(vm, overflowOffset, purifyNaN(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMaterializeDeferredClosure, JSCell*, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMaterializeDeferredClosure);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTMaterializeDeferredClosure);
    const FieldData& field = namedField(*instance->state, object, identifierAt(instance, key));
    OPERATION_RETURN(scope, object->materializeDeferredClosure(vm, field).asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetExtraProperty, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetExtraProperty);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTGetExtraProperty);
    const Identifier& identifier = identifierAt(instance, key);

    Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
    Structure* structure = object->structure();
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool found = object->getPropertySlot(globalObject, identifier, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue value = found ? slot.getValue(globalObject, identifier) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    PropertyCaches& caches = *instance->state->propertyCaches;
    caches.cache({ structure, caches.nameOf(key), SlotAccess::Read }, classifyRead({ globalObject, object, structure, identifier.impl(), slot }));
    ensureStillAliveHere(structure);
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasExtraProperty, size_t, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasExtraProperty);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    const Identifier& identifier = identifierAt(instance, key);
    if (const FieldData* field = object->fieldNamed(*instance->state, identifier)) {
        if (field->slot.representation() == SlotRepresentation::DeferredClosure || object->read(*field))
            OPERATION_RETURN(scope, true);
    }
    JSObject* extra = object->extraProperties();
    OPERATION_RETURN(scope, extra && extra->getDirectOffset(vm, identifier) != invalidOffset);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutExtraProperty, void, (Instance* instance, JSCell* cell, uint32_t key, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutExtraProperty);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTPutExtraProperty);
    const Identifier& identifier = identifierAt(instance, key);

    Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
    Structure* structure = object->structure();
    PutPropertySlot slot(object, true);
    object->methodTable()->put(object, globalObject, identifier, JSValue::decode(encodedValue), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);

    PropertyCaches& caches = *instance->state->propertyCaches;
    caches.cache({ structure, caches.nameOf(key), SlotAccess::Store }, classifyStore({ globalObject, object, structure, identifier.impl(), slot, PutMode::Strict }));
    ensureStillAliveHere(structure);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
