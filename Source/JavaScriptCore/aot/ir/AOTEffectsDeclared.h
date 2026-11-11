/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTSchemaEffects.h"
#include <array>
#include <span>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

class EffectsBuilder {
public:
    EffectsBuilder(const Program& program, Effects& effects)
        : m_program(program)
        , m_effects(effects)
    {
    }

    ALWAYS_INLINE void reads(const MemoryLocation& location) { m_effects.reads.addRead(m_program, location); }
    ALWAYS_INLINE void writes(const MemoryLocation& location) { m_effects.writes.add(m_program, location); }
    void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    void throws() { m_effects.mayThrow = true; }
    void collects() { m_effects.mayCollect = true; }
    void suspends() { m_effects.maySuspend = true; }
    void terminates() { m_effects.mayBeTerminated = true; }
    void unknownCode() { m_effects.merge(Effects::unknownCode()); }
    void asOverload() { unknownCode(); }

private:
    const Program& m_program;
    Effects& m_effects;
};

class EffectBoundBuilder {
public:
    constexpr void reads(const MemoryLocation& location) { m_effects.reads.add(location.heap()); }
    constexpr void writes(const MemoryLocation& location) { m_effects.writes.add(location.heap()); }
    constexpr void readsAtPayload(AbstractHeap heap) { m_effects.reads.add(heap); }
    constexpr void writesAtPayload(AbstractHeap heap) { m_effects.writes.add(heap); }
    constexpr void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    constexpr void throws() { m_effects.mayThrow = true; }
    constexpr void collects() { m_effects.mayCollect = true; }
    constexpr void suspends() { m_effects.maySuspend = true; }
    constexpr void terminates() { m_effects.mayBeTerminated = true; }
    constexpr void unknownCode() { m_effects.merge(Effects::unknownCode()); }
    constexpr void asOverload() { unknownCode(); }

    constexpr const Effects& effects() const LIFETIME_BOUND { return m_effects; }

private:
    Effects m_effects { Effects::none() };
};

inline constexpr OpcodeSet opcodesThrowingOnlyForLackOfMemory {
    Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData, Opcode::LoopHint,
    Opcode::GetStringCodeUnit, Opcode::Substring, Opcode::FlattenString, Opcode::StringOrder, Opcode::StringStartsWithLiteral, Opcode::StringEndsWithLiteral,
    Opcode::StringOrderToCharacter, Opcode::StringIndexOfLiteral, Opcode::StringIndexOfCharacter, Opcode::StringLastIndexOfLiteral,
    Opcode::StringRangeEqualsLiteral, Opcode::StringRangeToNumber, Opcode::StringRangeParseInt, Opcode::FindStringRangeInMap,
    Opcode::SubstringOrUndefined, Opcode::NewRegExpMatchArray, Opcode::NewArrayOfRegExpMatch, Opcode::BoundsOfStringSplit, Opcode::NewStringSplitArray
};

constexpr Effects withDeclaredKindOfThrow(Opcode opcode, Effects effects)
{
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

extern const std::array<uint8_t, numberOfOpcodes>& rowsOfDeclaredBoundsByOpcode;
extern const std::span<const Effects> declaredBoundsInRows;
extern const OpcodeSet opcodesWithExactBoundsInTable;
ALWAYS_INLINE const Effects& declaredBoundInTable(Opcode opcode) { return declaredBoundsInRows[rowsOfDeclaredBoundsByOpcode[static_cast<unsigned>(opcode)]]; }
ALWAYS_INLINE const Effects* findDeclaredEffectsOfEveryValue(Opcode opcode) { return opcodesWithExactBoundsInTable.contains(opcode) ? &declaredBoundInTable(opcode) : nullptr; }

inline constexpr AbstractHeapSet heapsThatOnlyFlagsOfLooksAtCellRead { AbstractHeap::NamedProperties };

template<typename Operation>
ALWAYS_INLINE bool lookAtCellHasNoFlagThatReads(Opcode opcode, const Operation& operation)
{
    if (opcode == Opcode::ShapeTest)
        return !operation.rejectsMarkedObjects();
    if (opcode == Opcode::IsNotFrozen)
        return !operation.hasNoExtraProperties() && !operation.hasNoFieldOutsideSlot();
    return opcode == Opcode::HasLayoutPrototype && !operation.hasNoFieldOutsideSlot();
}

template<typename Operation>
Effects declaredEffectsOfOperation(const Program& program, Opcode opcode, const Operation& operation)
{
    if (const Effects* effects = findDeclaredEffectsOfEveryValue(opcode)) {
        if (!lookAtCellHasNoFlagThatReads(opcode, operation))
            return *effects;
        Effects withoutFlags = *effects;
        withoutFlags.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
        return withoutFlags;
    }
    Effects effects = Effects::none();
    EffectsBuilder builder(program, effects);
    forEachDeclaredEffect(operation, builder);
    if (lookAtCellHasNoFlagThatReads(opcode, operation))
        effects.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

template<typename Operation>
const Effects& declaredEffectsOfOperation(const Program& program, Opcode opcode, const Operation& operation, Effects& storage)
{
    if (const Effects* effects = findDeclaredEffectsOfEveryValue(opcode)) {
        if (!lookAtCellHasNoFlagThatReads(opcode, operation))
            return *effects;
        storage = *effects;
        storage.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
        return storage;
    }
    storage = Effects::none();
    EffectsBuilder builder(program, storage);
    forEachDeclaredEffect(operation, builder);
    if (lookAtCellHasNoFlagThatReads(opcode, operation))
        storage.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
    if (storage.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        storage.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return storage;
}

template<typename Terminator>
Effects declaredEffectsOfTerminator(const Program& program, const Terminator& terminator)
{
    Effects effects = Effects::none();
    EffectsBuilder builder(program, effects);
    forEachDeclaredEffect(terminator, builder);
    return effects;
}


extern const OpcodeSet opcodesWithPureBoundInTable;
inline const OpcodeSet& opcodesWithPureDeclaredBound() { return opcodesWithPureBoundInTable; }
extern const OpcodeSet opcodesWithSuspendingBoundInTable;

template<typename Entry>
constexpr Effects declaredBoundOf(Entry entry)
{
    if constexpr (std::is_same_v<Entry, Opcode>) {
        if (!std::is_constant_evaluated())
            return declaredBoundInTable(entry);
    }
    EffectBoundBuilder builder;
    forEachDeclaredEffect(entry, builder);
    if constexpr (std::is_same_v<Entry, Opcode>)
        return withDeclaredKindOfThrow(entry, builder.effects());
    else
        return builder.effects();
}

constexpr bool maySuspend(Opcode opcode)
{
    if (!std::is_constant_evaluated())
        return opcodesWithSuspendingBoundInTable.contains(opcode);
    return declaredBoundOf(opcode).maySuspend;
}
constexpr bool mayThrow(Opcode opcode) { return declaredBoundOf(opcode).mayThrow; }
constexpr bool mayCollect(Opcode opcode) { return declaredBoundOf(opcode).mayCollect; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
