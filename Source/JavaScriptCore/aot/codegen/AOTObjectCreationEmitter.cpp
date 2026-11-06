/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectCreationEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "GetterSetter.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maskWidth = 32;

static uint32_t descriptorBitsOf(OptionSet<PropertyDescriptorFlags> flags)
{
    PropertyDescriptorBits bits { };
    bits.hasValue = flags.contains(PropertyDescriptorFlags::HasValue);
    bits.hasWritable = flags.contains(PropertyDescriptorFlags::HasWritable);
    bits.isWritable = flags.contains(PropertyDescriptorFlags::IsWritable);
    bits.hasEnumerable = flags.contains(PropertyDescriptorFlags::HasEnumerable);
    bits.isEnumerable = flags.contains(PropertyDescriptorFlags::IsEnumerable);
    bits.hasConfigurable = flags.contains(PropertyDescriptorFlags::HasConfigurable);
    bits.isConfigurable = flags.contains(PropertyDescriptorFlags::IsConfigurable);
    bits.hasGet = flags.contains(PropertyDescriptorFlags::HasGet);
    bits.hasSet = flags.contains(PropertyDescriptorFlags::HasSet);
    return std::bit_cast<uint32_t>(bits);
}

void ObjectCreationEmitter::generateDefineOwnProperty(const ValueContext& value)
{
    ValueID objectID = value->objectOperand();
    ValueID definedValueID = value->valueOperand();
    ValueID getterID = value->getterOperand();
    ValueID setterID = value->setterOperand();
    OptionSet<PropertyDescriptorFlags> flags = value->descriptorFlags();
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasValue) == !!definedValueID);
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasGet) == !!getterID);
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasSet) == !!setterID);
    RELEASE_ASSERT(!definedValueID || !getterID);

    Boxed object = in<Boxed>(objectID);
    auto defineByRunTime = [&] {
        Boxed key = in<Boxed>(value->computedKeyOperand());
        Boxed valueOrGetter = inOptional<Boxed>(definedValueID ? definedValueID : getterID).value_or(undefined());
        Boxed setter = inOptional<Boxed>(setterID).value_or(undefined());
        callVoidOperation(value, RuntimeOperation::DefineOwnProperty, { object, key, valueOrGetter, setter, word32(static_cast<int32_t>(descriptorBitsOf(flags))) });
    };
    SlotAccessEmitter::CreationByDefinition creation;
    if (typeOf(objectID).isObject())
        creation = emitter<SlotAccessEmitter>().tryCreateFieldByDefinition(value, unboxCell(object));
    if (!creation.isEmitted)
        defineByRunTime();
    else if (creation.leftToRunTime) {
        LBasicBlock done = newBlock();
        m_out.jump(done);
        m_out.appendTo(creation.leftToRunTime);
        defineByRunTime();
        m_out.jump(done);
        m_out.appendTo(done);
    }
    // A value of type never has no representation of an object.
    if (!typeOf(value.id()).isNever())
        define(value, unboxCell(object));
}

namespace {

struct AccessorInSlot {
    FieldIndex field;
    ValueID getter;
    ValueID setter;
};

} // anonymous namespace

// Object::tryDefineAccessorsOfLiteral(), for accessors that no slot holds yet.
bool ObjectCreationEmitter::tryDefineAccessorsInSlots(const ValueContext& value, CellPointer object, std::span<const PropertyKeyID> keys, std::span<const ValueID> functions, uint32_t getterMask, uint32_t enumerableMask, LBasicBlock done, LBasicBlock leftToRunTime)
{
    AOT_THRESHOLD(size_t, maxAccessorsMadeInLine, 24);
    // About 170 bytes an accessor.
    if (value.shape() != CodeShape::Fast)
        return false;
    LayoutID layoutID = typeOf(value->objectOperand()).singleLayout(m_program);
    if (!layoutID || !m_placement.layout(layoutID).holdsCompiledObjects())
        return false;
    const Layout& layout = m_program.at(layoutID);
    auto listsOnlyFunctionsOfProgram = [](const Type& type) {
        return !type.functions.isEmpty() && !type.primitives && !type.anyFunction && !type.anyObject && !type.layouts && type.classes.isEmpty() && type.builtins.isEmpty();
    };

    Vector<AccessorInSlot, 8> accessors;
    for (size_t index = 0; index < keys.size(); ++index) {
        std::optional<uint32_t> field = keys[index] ? layout.findField(keys[index]) : std::nullopt;
        if (!field)
            return false;
        const Field& declaration = layout.fields[*field];
        bool isEnumerable = enumerableMask >> index & 1;
        OptionSet<FieldForm> form { FieldForm::IsAccessor };
        if (!isEnumerable)
            form.add(FieldForm::IsNotEnumerable);
        if (declaration.form != form || isFilledAtBirth(m_program, { layoutID, *field }))
            return false;
        const PlacedSlot& slot = m_placement.slot(layoutID, *field);
        if (slot.storage != SlotStorage::Inline || slot.mayExistWithoutStorage || slotKeyFor(slot.offset, false) == noSlotKey || !traitsOf(slot.representation).hasAbsent)
            return false;
        Representation held = Typed::representationOf(slot.representation);
        if (held != Representation::Cell && held != Representation::Cell32 && held != Representation::JSValue)
            return false;
        const Type& function = typeOf(functions[index]);
        if (!listsOnlyFunctionsOfProgram(function))
            return false;
        bool isGetter = getterMask >> index & 1;
        // Compiled code calls the getter that it finds in a filled slot as one of those that the type of the field lists.
        if (isGetter && !function.isSubsetOf(m_program.at(slotTypeOf(m_program, declaration)), m_program))
            return false;
        size_t place = accessors.findIf([&](const AccessorInSlot& accessor) {
            return accessor.field == *field;
        });
        if (place == notFound) {
            place = accessors.size();
            accessors.append({ *field, { }, { } });
        }
        (isGetter ? accessors[place].getter : accessors[place].setter) = functions[index];
    }
    if (accessors.size() > maxAccessorsMadeInLine)
        return false;
    for (const AccessorInSlot& accessor : accessors) {
        if (!accessor.getter && !m_program.at(slotTypeOf(m_program, layout.fields[accessor.field])).anyFunction)
            return false;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    auto goOnIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        branch(condition, { normalEdge(next), unlikelyEdge(leftToRunTime) });
        m_out.appendTo(next);
    };
    auto locationOf = [&](const AccessorInSlot& accessor) {
        return MemoryLocation::objectField(layout.fields[accessor.field].key);
    };
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(object);
    goOnIf(hasNoBits(orderWord, ~orderAndCountMask));
    Word32 order = bitAnd(orderWord, orderWordOrderMask);
    goOnIf(notEqual(order, orderStoredOutOfLine));
    goOnIf(hasNoBits(loadField<Word32>(loadStructure(object), m_heaps.Structure_bitField), Structure::s_mayBePrototypeBits));
    for (const AccessorInSlot& accessor : accessors) {
        const PlacedSlot& slot = m_placement.slot(layoutID, accessor.field);
        goOnIf(slots.holdsAbsent(slots.addressOf(object, slot, locationOf(accessor)), slot.representation));
    }

    // Every pair is made before the object is written to: where the allocator has no cell at hand the run time finds the object as it was.
    const B3::AbstractHeap& allocatorState = m_heaps.entire(AbstractHeap::AllocatorState);
    Vector<CellPointer, 8> pairs;
    for (const AccessorInSlot& accessor : accessors) {
        MemoryLocation location = locationOf(accessor);
        CellPointer getter = accessor.getter ? in<CellPointer>(accessor.getter) : CellPointer(m_out.loadPtr(instanceField(allocatorState, AOT_INSTANCE_OFFSET(allocation.getterOfAccessorWithoutGetter))));
        CellPointer setter = accessor.setter ? in<CellPointer>(accessor.setter) : CellPointer(m_out.loadPtr(instanceField(allocatorState, AOT_INSTANCE_OFFSET(allocation.setterOfAccessorWithoutSetter))));
        CodeGenerator::InitializationScope scope(generator());
        CellPointer pair = emitter<CellAllocationEmitter>().allocateCellWithHeader(InlineAllocation::GetterSetter, leftToRunTime);
        // SlotAccessEmitter::loadHalfOfAccessorInSlot() reads them as part of the field.
        m_out.storePtr(getter.value(), m_out.address(m_heaps.at(m_program, location), pair.value(), GetterSetter::offsetOfGetter()));
        m_out.storePtr(setter.value(), m_out.address(m_heaps.at(m_program, location), pair.value(), GetterSetter::offsetOfSetter()));
        pairs.append(pair);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    // The keys are listed first, while the slots still say that the fields are missing. Nothing stands between the stores and their barrier.
    for (const AccessorInSlot& accessor : accessors)
        callStub(value, AOT::didCreateField(CreatedFields::One), { object.value(), m_out.constInt64(slotKeyFor(m_placement.slot(layoutID, accessor.field).offset, false)) });
    for (size_t index = 0; index < accessors.size(); ++index) {
        const PlacedSlot& slot = m_placement.slot(layoutID, accessors[index].field);
        CellPointer pair = pairs[index];
        Representation held = Typed::representationOf(slot.representation);
        slots.storeSlot(object, slot, locationOf(accessors[index]), held == Representation::Cell ? Operand(pair) : held == Representation::Cell32 ? Operand(compress(pair)) : Operand(boxCell(pair)));
    }
    emitter<BarrierEmitter>().barrier(value, object);
    m_out.jump(done);
    return true;
}

void ObjectCreationEmitter::generateDefineAccessors(const ValueContext& value)
{
    static_assert(!OBJECT_OFFSETOF(DefinedAccessor, key) && OBJECT_OFFSETOF(DefinedAccessor, function) == sizeof(EncodedJSValue) && sizeof(DefinedAccessor) == 2 * sizeof(EncodedJSValue));
    std::span<const PropertyKeyID> keys = value->keys();
    std::span<const ValueID> functions = value->functionsOperands();
    size_t count = keys.size();
    RELEASE_ASSERT(functions.size() == count && count <= maskWidth);

    CellPointer object = in<CellPointer>(value->objectOperand());
    LBasicBlock done = newBlock();
    LBasicBlock leftToRunTime = newBlock();
    if (tryDefineAccessorsInSlots(value, object, keys, functions, value->getterMask(), value->enumerableMask(), done, leftToRunTime))
        m_out.appendTo(leftToRunTime);
    else
        m_out.jump(leftToRunTime), m_out.appendTo(leftToRunTime);
    ConstantEmitter& constants = emitter<ConstantEmitter>();
    Vector<Boxed, 16> keysAndFunctions;
    for (size_t i = 0; i < count; ++i) {
        keysAndFunctions.append(boxCell(constants.keyConstant(value, keys[i])));
        keysAndFunctions.append(boxCell(in<CellPointer>(functions[i])));
    }
    Pointer buffer = argumentBuffer(keysAndFunctions.span());
    Word32 getters = word32(static_cast<int32_t>(value->getterMask()));
    Word32 enumerable = word32(static_cast<int32_t>(value->enumerableMask()));
    callVoidOperation(value, RuntimeOperation::DefineAccessors, { object, word32(static_cast<int32_t>(count)), getters, enumerable, buffer });
    m_out.jump(done);
    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
