/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTSchemaEffects.h"
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

// It writes into the Effects of whoever asks, which come in as none().
class EffectsBuilder {
public:
    EffectsBuilder(const Program& program, Effects& effects)
        : m_program(program)
        , m_effects(effects)
    {
    }

    void reads(const MemoryLocation& location) { m_effects.reads.add(m_program, location); }
    void writes(const MemoryLocation& location) { m_effects.writes.add(m_program, location); }
    void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    void throws() { m_effects.mayThrow = true; }
    void collects() { m_effects.mayCollect = true; }
    void suspends() { m_effects.maySuspend = true; }
    void terminates() { m_effects.mayBeTerminated = true; }
    void unknownCode() { m_effects.merge(Effects::unknownCode()); }
    // An inline built-in whose row says asOverload has the effects of the overload it stands for, which no table of opcodes knows: walk/AOTWalkBuiltins.cpp works them out from the row.
    // For everybody else it may do anything.
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
    Opcode::GetStringCodeUnit, Opcode::Substring, Opcode::FlattenString, Opcode::TryStrcat, Opcode::StringOrder, Opcode::StringStartsWithLiteral, Opcode::StringEndsWithLiteral,
    Opcode::StringOrderToCharacter, Opcode::StringIndexOfLiteral, Opcode::StringIndexOfCharacter, Opcode::StringLastIndexOfLiteral,
    Opcode::StringRangeEqualsLiteral, Opcode::StringRangeToNumber, Opcode::StringRangeParseInt
};

constexpr Effects withDeclaredKindOfThrow(Opcode opcode, Effects effects)
{
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

const Effects* findDeclaredEffectsOfEveryValue(Opcode);

// Whoever gives an object an extra property, or moves a field out of its slot, writes a property by name. Neither changes whether the object is frozen.
inline constexpr AbstractHeapSet heapsThatOnlyFlagsOfIsNotFrozenRead { AbstractHeap::NamedProperties };

// `Operation` and `Terminator` are anything with the schema's getters under their names: Draft, Operation, TerminatorDraft and Terminator of walk/AOTCode.h, BodyValue and BodyTerminator of AOTBody.h.
// The generated forEachDeclaredEffect() is a template over them as well.
template<typename Operation>
Effects declaredEffectsOfOperation(const Program& program, Opcode opcode, const Operation& operation)
{
    if (const Effects* effects = findDeclaredEffectsOfEveryValue(opcode)) {
        if (opcode != Opcode::IsNotFrozen || operation.hasNoExtraProperties() || operation.hasNoFieldOutsideSlot())
            return *effects;
        Effects withoutFlags = *effects;
        withoutFlags.reads.remove(heapsThatOnlyFlagsOfIsNotFrozenRead);
        return withoutFlags;
    }
    Effects effects = Effects::none();
    EffectsBuilder builder(program, effects);
    forEachDeclaredEffect(operation, builder);
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

// Without a copy where the effects are those of every operation with the opcode. Otherwise the answer is `storage`.
template<typename Operation>
const Effects& declaredEffectsOfOperation(const Program& program, Opcode opcode, const Operation& operation, Effects& storage)
{
    if (const Effects* effects = findDeclaredEffectsOfEveryValue(opcode)) {
        if (opcode != Opcode::IsNotFrozen || operation.hasNoExtraProperties() || operation.hasNoFieldOutsideSlot())
            return *effects;
    }
    storage = declaredEffectsOfOperation(program, opcode, operation);
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

const Effects& declaredBoundInTable(Opcode);
// declaredBoundInTable() of each isPure().
extern const OpcodeSet opcodesWithPureBoundInTable;
inline const OpcodeSet& opcodesWithPureDeclaredBound() { return opcodesWithPureBoundInTable; }

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

constexpr bool maySuspend(Opcode opcode) { return declaredBoundOf(opcode).maySuspend; }
constexpr bool mayThrow(Opcode opcode) { return declaredBoundOf(opcode).mayThrow; }
constexpr bool mayCollect(Opcode opcode) { return declaredBoundOf(opcode).mayCollect; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
