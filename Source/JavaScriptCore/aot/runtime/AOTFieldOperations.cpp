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

static void countStubExitByName(Instance* instance, ASCIILiteral what, const FieldData& field)
{
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (caches.countsNames()) [[unlikely]]
        caches.countNamed(what, instance->state->objects->layouts.nameOf(*instance->vm, field));
}

static void storeInPresentField(VM& vm, Object* object, const FieldData& field, JSValue value)
{
    if (!object->tryStore(vm, field, value))
        object->moveFieldToExtraProperties(vm, field, { value, object->attributesOf(field) }, SlotExitReason::StoreOfOtherType);
}

// A constructor that stores by name asks for one granule at a time, in the sequence of its stores, and an Extension is copied for each granule that has no room behind the others.
static void ensureGranules(VM& vm, Object* object, uint64_t granuleBits)
{
    object->ensureExtensionGranules(vm, granuleBits | object->layout().granulesFilledByConstructor());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTReadField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    countStubExitByName(instance, "stub could not read field"_s, field);
    if (field.slot.representation() == SlotRepresentation::DeferredClosure) {
        if (!object->isInSlot(field)) [[unlikely]]
            crashOnWrongOperand(entry);
        return JSValue::encode(object->materializeDeferredClosure(vm, field));
    }
    JSValue value = object->read(field);
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStoreField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTStoreField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    countStubExitByName(instance, "stub could not store field"_s, field);
    if (!findFieldProperty(vm, *object, field).isInSlot) [[unlikely]]
        crashOnWrongOperand(entry);
    storeInPresentField(vm, object, field, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue encodedValue, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCreateField;
    AOT_OPERATION_BEGIN(instance, entry);
    count(*instance, stubExitCounter(reason, entry));
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = fieldOf(object, header, payload, entry);
    JSValue value = JSValue::decode(encodedValue);
    if (instance->state->propertyCaches->countsNames()) [[unlikely]] {
        if (field.slot.representation() == SlotRepresentation::DeferredClosure)
            countStubExitByName(instance, "stub could not create field that defers its closure"_s, field);
        else if (!instance->state->objects->layouts.slotAdmits(field, value))
            countStubExitByName(instance, "stub could not create field whose slot does not admit the value"_s, field);
        else
            countStubExitByName(instance, "stub could not create field though its slot admits the value"_s, field);
    }
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current.isInSlot) {
        storeInPresentField(vm, object, field, value);
        OPERATION_RETURN(scope);
    }
    if (current || object->isNotExtensible()) [[unlikely]]
        crashOnWrongOperand(entry);
    // A store creates a plain property. A field that is created in its slot has the form that the layout gives it.
    if (field.attributes() || !tryCreateField(vm, *instance->state, *object, field, value))
        object->putDirectFromNativeCode(vm, identifierAt(instance, field.name), value, 0);
    OPERATION_RETURN(scope);
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

// Whoever needs storage for the field sees to it. A field that got undefined may need none.
static void listCreatedField(State& state, Object* object, const FieldData& field)
{
    if (object->structure()->mayBePrototype()) [[unlikely]] {
        VM& vm = *state.instance.vm;
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    }
    if (!object->isInKeyOrder(state, field)) {
        object->didCreateField(state, field, false);
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
    DeferGCForAWhile deferGC(vm);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateField);
    const FieldData& field = namedField(state, object, identifierAt(instance, key));
    didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, identifierAt(instance, key).impl());
    object->didCreateField(state, field, !(field.flags & fieldMayExistWithoutStorage));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryFinishBirth, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryFinishBirth);
    if (isCompiledObject(cell))
        uncheckedDowncast<Object>(cell)->tryFinishBirth();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldAt);
    DeferGCForAWhile deferGC(vm);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldAt);
    vm.writeBarrier(object);
    listCreatedField(state, object, fieldAtSlotKey(state, object, offset));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldOutOfLineAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldOutOfLineAt);
    DeferGCForAWhile deferGC(vm);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldOutOfLineAt);
    vm.writeBarrier(object);
    const FieldData& field = fieldAtSlotKey(state, object, offset | outOfLineSlotKey);
    listCreatedField(state, object, field);
    ensureGranules(vm, object, granuleBitOf(field.slot.offset));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldsAt, void, (Instance* instance, JSCell* cell, uint64_t offsets))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldsAt);
    DeferGCForAWhile deferGC(vm);
    State& state = *instance->state;
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldsAt);
    vm.writeBarrier(object);
    for (uint64_t rest = offsets; static_cast<uint32_t>(rest) & slotKeyMask; rest >>= bitsPerSlotKey)
        listCreatedField(state, object, fieldAtSlotKey(state, object, static_cast<uint32_t>(rest) & slotKeyMask));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranules, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnsureExtensionGranules);
    DeferGCForAWhile deferGC(vm);
    ensureGranules(vm, cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranules), granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranulesWithoutCollecting, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnsureExtensionGranulesWithoutCollecting);
    DeferGCForAWhile deferGC(vm);
    ensureGranules(vm, cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranulesWithoutCollecting), granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMaterializeDeferredClosure, JSCell*, (Instance* instance, JSCell* cell, uint32_t key))
{
    constexpr Entry entry = Entry::operationAOTMaterializeDeferredClosure;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = namedField(*instance->state, object, identifierAt(instance, key));
    if (!object->isInSlot(field)) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, object->materializeDeferredClosure(vm, field).asCell());
}

// A store has given the field a value that its slot does not hold, so the property lies among the extra properties. Nobody has redefined it: it is a plain value.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetFieldOutsideSlot, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t key))
{
    constexpr Entry entry = Entry::operationAOTGetFieldOutsideSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* object = cellArgument<Object>(cell, entry);
    JSObject* extra = object->extraProperties();
    JSValue value = extra ? extra->getDirect(vm, identifierAt(instance, key)) : JSValue();
    if (value && (value.isGetterSetter() || value.isCustomGetterSetter())) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, JSValue::encode(value ? value : jsUndefined()));
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

static NEVER_INLINE size_t JIT_OPERATION_ATTRIBUTES hasExtraPropertyByLookup(Instance*, JSCell*, uint32_t key);

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasExtraProperty, size_t, (Instance* instance, JSCell* cell, uint32_t key))
{
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    uint32_t orderWord = object->orderWord();
    // What is remembered is about a field. No field lies among the extra properties while the bit is clear, whatever else they hold.
    if (!(orderWord & orderWordHasFieldOutsideSlot)) [[likely]] {
        if (std::optional<bool> lists = instance->state->objects->keyOrders.rememberedWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key)) [[likely]]
            return *lists;
    }
    [[clang::musttail]] return hasExtraPropertyByLookup(instance, cell, key);
}

static size_t JIT_OPERATION_ATTRIBUTES hasExtraPropertyByLookup(Instance* instance, JSCell* cell, uint32_t key)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasExtraProperty);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    const Identifier& identifier = identifierAt(instance, key);
    // Of a field the question is whether its slot reads as a value. A field that lives among the extra properties has left its slot.
    if (const FieldData* field = object->fieldNamed(*instance->state, identifier)) {
        if (field->slot.representation() == SlotRepresentation::DeferredClosure)
            OPERATION_RETURN(scope, object->isInSlot(*field));
        bool readsAsValue = !!object->read(*field);
        uint32_t orderWord = object->orderWord();
        bool dependsOnOrderAlone = !(orderWord & orderWordHasFieldOutsideSlot) && !isOrderStoredOutOfLine(orderWord) && !object->layout().keysAreNeverEnumerated;
        if (dependsOnOrderAlone && !validatesRuntime(vm) && object->isInKeyOrder(*instance->state, *field) == readsAsValue)
            instance->state->objects->keyOrders.rememberWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key, readsAsValue);
        OPERATION_RETURN(scope, readsAsValue);
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
