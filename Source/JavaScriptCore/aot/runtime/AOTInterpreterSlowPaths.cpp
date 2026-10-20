/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterpreterSlowPaths.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTPropertyCachesNamedAccess.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static Instance* runningInstanceOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance && instance->state ? instance : nullptr;
}

static NamedAccess namedAccess(Instance& instance, const Identifier& ident)
{
    return { &instance, instance.globalObject, *instance.vm, *instance.state->propertyCaches, ident, std::nullopt, nullptr };
}

static void* fieldInCell(const Slot& slot, Object& object)
{
    ASSERT(Slot::payloadOf(slot.kind) == SlotPayload::Field);
    return slot.has(Slot::isOutOfLine) ? nullptr : &object.at<uint8_t>(slot.field.slot.offset);
}

static JSValue readField(Instance& instance, const Slot& slot, Object& object)
{
    SlotRepresentation representation = slot.field.slot.representation();
    const void* field = fieldInCell(slot, object);
    if (!field || representation == SlotRepresentation::DeferredClosure)
        return { };
    if (representation == SlotRepresentation::Int32OrDouble && *static_cast<const int32_t*>(field) == doubleInExtensionInt32)
        return { };
    JSValue value = readSlot(representation, field, *instance.state->strings);
    if (!value && slot.has(Slot::absentIsUndefined))
        return jsUndefined();
    return value;
}

static JSValue readExtraProperty(const Slot& slot, Object& object)
{
    JSObject* extraProperties = object.extraProperties();
    return extraProperties ? extraProperties->getDirect(Slot::offsetFor(slot.extraProperty.wordIndex)) : JSValue();
}

static JSValue valueInStorage(const Slot& slot, JSCell* base)
{
    return asObject(slot.has(Slot::isOwn) ? base : slot.holder)->getDirect(slot.offsetInStorage());
}

// Empty where the slot does not say what the value is, and after an exception.
static JSValue readBySlot(Instance& instance, const Slot& slot, JSCell* base)
{
    switch (slot.kind) {
    case Slot::Kind::Value:
        return valueInStorage(slot, base);
    case Slot::Kind::Getter:
        return uncheckedDowncast<GetterSetter>(valueInStorage(slot, base))->callGetter(instance.globalObject, base);
    case Slot::Kind::FarValue:
        return asObject(base)->getDirect(slot.offsetInStorage());
    case Slot::Kind::Absent:
        return jsUndefined();
    case Slot::Kind::Field:
        return readField(instance, slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::ExtraProperty:
        return readExtraProperty(slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::Constant:
        return JSValue::decode(slot.constant);
    case Slot::Kind::Empty:
    case Slot::Kind::Setter:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::Transition:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Uncacheable:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue getById(Instance& instance, JSCell* base, const Identifier& ident)
{
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        RELEASE_AND_RETURN(scope, getAndCache(namedAccess(instance, ident), base));
    JSValue value = readBySlot(instance, *slot, base);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    RELEASE_AND_RETURN(scope, JSValue(base).get(instance.globalObject, ident));
}

static std::optional<bool> isFieldPresent(const Slot& slot, Object& object)
{
    const void* field = fieldInCell(slot, object);
    if (!field)
        return std::nullopt;
    if (!slotHoldsAbsent(slot.field.slot.representation(), field))
        return true;
    if (slot.has(Slot::absentIsUndefined))
        return false;
    return std::nullopt;
}

static std::optional<bool> isPresentBySlot(const Slot& slot, JSObject* base)
{
    switch (Slot::presenceOf(slot.kind)) {
    case SlotPresence::Unknown:
        return std::nullopt;
    case SlotPresence::Present:
        return true;
    case SlotPresence::Absent:
        return false;
    case SlotPresence::ReadsSlot:
        return isFieldPresent(slot, *uncheckedDowncast<Object>(base));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool inById(Instance& instance, JSObject* base, const Identifier& ident)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        return hasPropertyAndCache(&instance, base, { ident.impl(), std::nullopt });
    if (std::optional<bool> isPresent = isPresentBySlot(*slot, base))
        return *isPresent;
    return base->hasProperty(instance.globalObject, ident);
}

static bool tryStoreField(Instance& instance, const Slot& slot, Object& object, JSValue value)
{
    SlotRepresentation representation = slot.field.slot.representation();
    void* field = fieldInCell(slot, object);
    if (!field || object.isFrozen() || slotHoldsAbsent(representation, field))
        return false;
    State& state = *instance.state;
    const RuntimeLayoutTable& layouts = state.objects->layouts;
    if (!layouts.valueHasType(layouts.typeAt(slot.field.type), value) || !tryWriteSlot(*state.strings, representation, field, value))
        return false;
    if (traitsOf(representation).holdsCell)
        instance.vm->writeBarrier(&object, value);
    return true;
}

static void putById(Instance& instance, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Store);
    if (!slot) {
        putAndCache(namedAccess(instance, ident), base, value, isStrict ? PutMode::Strict : PutMode::Sloppy);
        return;
    }
    bool isField = slot->kind == Slot::Kind::Field || slot->kind == Slot::Kind::FieldCreation;
    if (isField && tryStoreField(instance, *slot, *uncheckedDowncast<Object>(base), value))
        return;
    if (slot->kind == Slot::Kind::Setter) {
        uncheckedDowncast<GetterSetter>(valueInStorage(*slot, base))->callSetter(instance.globalObject, base, value, isStrict);
        return;
    }
    PutPropertySlot putSlot(base, isStrict, PutPropertySlot::PutById);
    JSValue(base).putInline(instance.globalObject, ident, value, putSlot);
}

static UniquedStringImpl* nameOfKey(JSValue key)
{
    UniquedStringImpl* uid = key.isCell() ? uidOfKey(key.asCell()) : nullptr;
    return uid && !parseIndex(*uid) ? uid : nullptr;
}

namespace Interpreter {

std::optional<JSValue> tryGetById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident)
{
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterReadSlowPaths);
    return getById(*instance, base, ident);
}

std::optional<JSValue> tryGetByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key)
{
    UniquedStringImpl* uid = nameOfKey(key);
    if (!uid)
        return std::nullopt;
    return tryGetById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid));
}

std::optional<bool> tryInById(JSGlobalObject* globalObject, JSObject* base, const Identifier& ident)
{
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterPresenceSlowPaths);
    return inById(*instance, base, ident);
}

std::optional<bool> tryInByVal(JSGlobalObject* globalObject, JSObject* base, JSValue key)
{
    UniquedStringImpl* uid = nameOfKey(key);
    if (!uid)
        return std::nullopt;
    return tryInById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid));
}

bool tryPutById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return false;
    count(*instance, RuntimeCounterID::InterpreterStoreSlowPaths);
    putById(*instance, base, ident, value, isStrict);
    return true;
}

bool tryPutByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key, JSValue value, bool isStrict)
{
    UniquedStringImpl* uid = nameOfKey(key);
    return uid && tryPutById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid), value, isStrict);
}

void didLookUpCompiledCallee(VM& vm)
{
    count(vm, RuntimeCounterID::InterpreterCallSlowPaths);
}

void didLeaveInlineAllocation(VM& vm)
{
    count(vm, RuntimeCounterID::InterpreterAllocationSlowPaths);
}

void didLeaveInlineBranch(VM& vm)
{
    count(vm, RuntimeCounterID::InterpreterBranchSlowPaths);
}

} // namespace Interpreter

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
