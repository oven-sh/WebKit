/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObject.h"

#if ENABLE(AOT)

#include "AOTNarrowFieldTypes.h"
#include "AOTTypeArena.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static ValueID findPart(const AllocationRequest& request, VirtualFieldKind kind)
{
    for (const FieldValue& part : request.parts) {
        if (part.field.kind == kind)
            return part.value;
    }
    return { };
}

static ValueID requiredPart(const AllocationRequest& request, VirtualFieldKind kind)
{
    ValueID value = findPart(request, kind);
    RELEASE_ASSERT(value);
    return value;
}

static Vector<ValueID, 8> valuesByIndex(std::span<const FieldValue> fields, VirtualFieldKind kind, size_t count)
{
    Vector<ValueID, 8> values(count);
    for (const FieldValue& entry : fields) {
        if (entry.field.kind == kind)
            values[entry.field.index] = entry.value;
    }
    return values;
}

static uint32_t requiredLayoutField(const Program& program, const VirtualObjectDescription& description, const VirtualField& field)
{
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
    RELEASE_ASSERT(layoutField);
    return *layoutField;
}

static void setObjectContents(const Program& program, const AllocationRequest& request, Value& object)
{
    RELEASE_ASSERT(object.opcode == Opcode::NewObject);
    Vector<uint32_t, 8> fields;
    Vector<ValueID, 8> values;
    for (const FieldValue& property : request.properties) {
        fields.append(requiredLayoutField(program, request.description, property.field));
        values.append(property.value);
    }
    object.setFields(fields.span());
    object.setValuesOperands(values.span());
    object.setPrototypeOperand(findPart(request, VirtualFieldKind::Prototype));
    object.setStorage(AllocationStorage::Heap);
}

static Value makeObject(const Program& program, const AllocationRequest& request)
{
    Value object = request.prototype;
    setObjectContents(program, request, object);
    return object;
}

static Value makeClosure(const AllocationRequest& request)
{
    Value closure = request.prototype;
    Vector<ValueID, 8> captures = valuesByIndex(request.parts, VirtualFieldKind::Capture, closure.capturesOperands().size());
    RELEASE_ASSERT(!captures.contains(ValueID()));
    closure.setCapturesOperands(captures.span());
    return closure;
}

static Value makeArray(const AllocationRequest& request)
{
    Vector<ValueID, 8> valueAtPosition = valuesByIndex(request.properties, VirtualFieldKind::Element, request.length);
    Vector<uint32_t, 8> holes;
    Vector<ValueID, 8> elements;
    for (uint32_t position = 0; position < request.length; ++position) {
        if (ValueID element = valueAtPosition[position])
            elements.append(element);
        else
            holes.append(position);
    }

    Value array = request.prototype;
    array.setHoles(holes.span());
    array.setElementsOperands(elements.span());
    array.setCapacityOperand(findPart(request, VirtualFieldKind::Capacity));
    return array;
}

static Value makeArguments(const AllocationRequest& request)
{
    Vector<ValueID, 8> parameters = valuesByIndex(request.properties, VirtualFieldKind::Element, request.description.numberOfParameters);
    RELEASE_ASSERT(!parameters.contains(ValueID()));

    Value arguments = request.prototype;
    arguments.setParametersOperands(parameters.span());
    arguments.setCountOperand(requiredPart(request, VirtualFieldKind::ArgumentCount));
    arguments.setRestOperand(requiredPart(request, VirtualFieldKind::RestList));
    if (request.description.isMapped)
        arguments.setCalleeOperand(requiredPart(request, VirtualFieldKind::Callee));
    return arguments;
}

static Value makeStringRange(const AllocationRequest& request)
{
    Value range = request.prototype;
    if (!request.mayBeAbsent)
        range.become(Opcode::Substring);
    range.setStringOperand(requiredPart(request, VirtualFieldKind::StringBase));
    range.setStartOperand(requiredPart(request, VirtualFieldKind::RangeStart));
    range.setEndOperand(requiredPart(request, VirtualFieldKind::RangeEnd));
    return range;
}

static Value makeCopyOfAllocation(const AllocationRequest& request)
{
    Value copy = request.prototype;
    for (const FieldValue& part : request.parts) {
        RELEASE_ASSERT(part.field.kind == VirtualFieldKind::Operand);
        copy.setOperand(part.field.index, part.value);
    }
    return copy;
}

Value makeAllocation(const Program& program, const AllocationRequest& request)
{
    switch (request.description.kind) {
    case VirtualObjectKind::Object:
    case VirtualObjectKind::Environment:
        return makeObject(program, request);
    case VirtualObjectKind::Closure:
        return makeClosure(request);
    case VirtualObjectKind::Array:
        return makeArray(request);
    case VirtualObjectKind::Opaque:
        return makeCopyOfAllocation(request);
    case VirtualObjectKind::ArgumentList:
        return request.prototype;
    case VirtualObjectKind::Arguments:
        return makeArguments(request);
    case VirtualObjectKind::StringRange:
        return makeStringRange(request);
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canReadSlotWithoutGuard(const Program& program, const VirtualObjectDescription& description, uint32_t layoutField)
{
    bool typeProvesShape = program.at(description.type).singleLayout(program) == description.layout;
    return typeProvesShape && flowOf(program, { description.layout, layoutField }).occupancy == Occupancy::AlwaysFilled;
}

static void setSlotStamp(Value& slotOperation, PropertyKeyID key)
{
    slotOperation.setKey(key);
    slotOperation.setLevel(KnowledgeLevel::Proven);
}

Value makeFieldRead(const Program& program, const VirtualObjectDescription& description, const VirtualField& field, ValueID cell, ValueID guard)
{
    switch (description.kind) {
    case VirtualObjectKind::Environment: {
        Value read(Opcode::GetEnvironmentField);
        read.setEnvironmentOperand(cell);
        read.setLayout(description.layout);
        read.setField(requiredLayoutField(program, description, field));
        return read;
    }
    case VirtualObjectKind::Object: {
        RELEASE_ASSERT(field.kind == VirtualFieldKind::Named);
        Value read(Opcode::GetSlot);
        read.setObjectOperand(cell);
        // The guard of a cell that other code has seen may have ended, and an operation may only name a guard that is available.
        if (!canReadSlotWithoutGuard(program, description, requiredLayoutField(program, description, field)))
            read.setGuardOperand(guard);
        setSlotStamp(read, field.key);
        return read;
    }
    case VirtualObjectKind::Closure: {
        RELEASE_ASSERT(field.kind == VirtualFieldKind::Capture);
        Value read(Opcode::GetCapture);
        read.setClosureOperand(cell);
        read.setFunction(description.function);
        read.setIndex(field.index);
        return read;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value makeFieldStore(const Program& program, const VirtualObjectDescription& description, const FieldValue& stored, ValueID cell, ValueID guard)
{
    switch (description.kind) {
    case VirtualObjectKind::Environment: {
        Value store(Opcode::PutEnvironmentField);
        store.setEnvironmentOperand(cell);
        store.setValueOperand(stored.value);
        store.setLayout(description.layout);
        store.setField(requiredLayoutField(program, description, stored.field));
        return store;
    }
    case VirtualObjectKind::Object: {
        RELEASE_ASSERT(stored.field.kind == VirtualFieldKind::Named);
        Value store(Opcode::PutSlotOrAdd);
        store.setObjectOperand(cell);
        store.setGuardOperand(guard);
        store.setValueOperand(stored.value);
        setSlotStamp(store, stored.field.key);
        return store;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::Closure:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canHoldUnprovenValue(const Program& program, const VirtualObjectDescription& description, const VirtualField& named)
{
    if (description.kind != VirtualObjectKind::Object || named.kind != VirtualFieldKind::Named)
        return false;
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, named);
    if (!layoutField)
        return false;
    const Field& field = program.at(description.layout).fields[*layoutField];
    return canBeStoredBehindAllocation(field) && tryCreateFiller(program, program.at(slotTypeOf(program, field)));
}

Value makeFiller(const Program& program, const VirtualObjectDescription& description, const VirtualField& unprovenNamed)
{
    RELEASE_ASSERT(unprovenNamed.kind == VirtualFieldKind::UnprovenNamed);
    const Field& field = program.at(description.layout).fields[requiredLayoutField(program, description, unprovenNamed)];
    std::optional<Value> filler = tryCreateFiller(program, program.at(slotTypeOf(program, field)));
    RELEASE_ASSERT(filler);
    return WTF::move(*filler);
}

Value makeStoreByName(const FieldValue& unprovenNamed, ValueID cell)
{
    RELEASE_ASSERT(unprovenNamed.field.kind == VirtualFieldKind::UnprovenNamed);
    Value store(Opcode::PropSet);
    store.setObjectOperand(cell);
    store.setValueOperand(unprovenNamed.value);
    store.setKey(unprovenNamed.field.key);
    store.setMode(ECMAMode::Strict);
    return store;
}

void setExactReceivers(Value& slotOperation, LayoutID layout, TypeArena& types)
{
    RELEASE_ASSERT(Value::opcodesWithReceivers.contains(slotOperation.opcode));
    slotOperation.setReceivers(types.completeLayoutSet(singleElementSpan(layout)));
}

SpeculativeLoad canLoadSpeculatively(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    const Program& program = context.program;
    bool isSafe = false;
    switch (description.kind) {
    case VirtualObjectKind::Environment:
        isSafe = layoutFieldOf(program, description, field).has_value();
        break;
    case VirtualObjectKind::Closure:
        isSafe = field.kind == VirtualFieldKind::Capture && field.index < program.at(description.function).captures.size();
        break;
    case VirtualObjectKind::Object: {
        std::optional<uint32_t> layoutField = field.kind == VirtualFieldKind::Named ? layoutFieldOf(program, description, field) : std::nullopt;
        // The read of a slot that defers its closure creates the closure and stores it.
        isSafe = layoutField && canReadSlotWithoutGuard(program, description, *layoutField)
            && flowOf(program, { description.layout, *layoutField }).location.deferral == ClosureDeferral::Unknown;
        break;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    return isSafe ? SpeculativeLoad::Safe : SpeculativeLoad::Unknown;
}

static bool isListedByAllocation(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    bool isListed = false;
    forEachInitialField(context, description, [&](const VirtualField& initialField, ValueID) {
        isListed |= initialField == field;
    });
    return isListed;
}

LateStore canStoreLate(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    const Program& program = context.program;
    if (!layoutFieldOf(program, description, field))
        return LateStore::Unknown;

    switch (description.kind) {
    case VirtualObjectKind::Environment:
        return LateStore::NeedsNoPlaceholder;
    case VirtualObjectKind::Object: {
        if (field.kind != VirtualFieldKind::Named)
            return LateStore::Unknown;
        const Type& fieldType = program.at(typeOfField(context, description, field));
        if (program.at(TypeTable::undefined).isSubsetOf(fieldType, program))
            return LateStore::NeedsUndefinedPlaceholder;
        // Flow proves a field filled from the start where every allocation lists it, and placement then gives its slot no absent state for a store to find.
        return isListedByAllocation(context, description, field) ? LateStore::Unknown : LateStore::CreatesProperty;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::Closure:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        return LateStore::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
