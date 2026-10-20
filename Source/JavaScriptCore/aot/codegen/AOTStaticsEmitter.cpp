/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStaticsEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTMemoryLocation.h"
#include "AOTPlacement.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned bitsPerByte = 8;

static_assert(!(MemberGuardData::bitsPerWord % bitsPerByte));

StaticsEmitter::StaticsEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

TypedPointer StaticsEmitter::inStatics(const B3::AbstractHeap& heap, uint32_t offsetInStatics)
{
    return m_out.address(heap, m_statics, offsetInStatics);
}

TypedPointer StaticsEmitter::inStatics(const B3::AbstractHeap& heap, StaticsOffset offset)
{
    return inStatics(heap, offset.value());
}

TypedPointer StaticsEmitter::addressOf(VariableID id)
{
    const PlacedVariable& variable = m_placement.variable(id);
    RELEASE_ASSERT(variable.hasWordInStatics());
    return inStatics(m_heaps.at(m_program, MemoryLocation::moduleVariable(id)), variable.offset);
}

Operand StaticsEmitter::loadEncodedVariable(VariableID id)
{
    return emitter<SlotEmitter>().load(addressOf(id), m_placement.variable(id).representation);
}

Operand StaticsEmitter::loadVariable(VariableID id)
{
    const PlacedVariable& variable = m_placement.variable(id);
    SlotEmitter& slots = emitter<SlotEmitter>();
    Operand loaded = loadEncodedVariable(id);
    switch (variable.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::FunctionAsZero:
        return loaded;
    case VariableEncoding::UndefinedAsZero:
        RELEASE_ASSERT(representsAbsentAsZero(variable.representation));
        return slots.absentAsUndefined(loaded, variable.representation);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StaticsEmitter::storeVariable(VariableID id, Operand stored, const Type& storedType)
{
    const PlacedVariable& variable = m_placement.variable(id);
    switch (variable.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::FunctionAsZero:
        break;
    case VariableEncoding::UndefinedAsZero: {
        if (!(storedType.primitives & Primitive::Undefined))
            break;
        Boxed boxed = stored.as<Boxed>();
        stored = select(equal(boxed, undefined()), boxedConstant(JSValue()), boxed);
        break;
    }
    }
    emitter<SlotEmitter>().store(addressOf(id), variable.representation, stored);
    if (traitsOf(variable.representation).holdsCell)
        emitter<BarrierEmitter>().markCardOfStatics(variable.offset);
}

TypedPointer StaticsEmitter::guardByteOf(GuardBitID id)
{
    const PlacedGuardBit& guard = m_placement.guardBit(id);
    uint32_t bitInWord = guard.bit % MemberGuardData::bitsPerWord;
    return inStatics(m_heaps.at(m_program, MemoryLocation::memberGuards(guard.classLayout)), guard.wordOffset + bitInWord / bitsPerByte);
}

uint8_t StaticsEmitter::guardMaskOf(GuardBitID id) const
{
    return static_cast<uint8_t>(1u << m_placement.guardBit(id).bit % bitsPerByte);
}

TypedPointer StaticsEmitter::runtimeCounter(RuntimeCounterID id)
{
    RELEASE_ASSERT(id != RuntimeCounterID::None);
    return instanceField(m_heaps.counters, AOT_INSTANCE_OFFSET(runtime.counters) + CounterWords::offsetOfValue(id));
}

TypedPointer StaticsEmitter::branchCounter(uint32_t number)
{
    Pointer branchCounts = loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.counters.branchCounts));
    return m_out.address(m_heaps.counters, branchCounts.value(), static_cast<ptrdiff_t>(number) * static_cast<ptrdiff_t>(sizeof(uint64_t)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
