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

class EffectsBuilder {
public:
    explicit EffectsBuilder(const Program& program)
        : m_program(program)
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

    const Effects& effects() const LIFETIME_BOUND { return m_effects; }

private:
    const Program& m_program;
    Effects m_effects { Effects::none() };
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
    constexpr void asOverload() { m_isAsOverload = true; }

    constexpr const Effects& effects() const LIFETIME_BOUND { return m_effects; }
    constexpr bool isAsOverload() const { return m_isAsOverload; }

private:
    Effects m_effects { Effects::none() };
    bool m_isAsOverload { false };
};

// Their rows say `throws` for the lack of memory alone.
inline constexpr OpcodeSet opcodesThrowingOnlyForLackOfMemory {
    Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData, Opcode::LoopHint,
    Opcode::GetStringCodeUnit, Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::FlattenString, Opcode::StringOrder, Opcode::StringOrderToCharacter,
    Opcode::StringStartsWithLiteral, Opcode::StringEndsWithLiteral, Opcode::StringIndexOfLiteral, Opcode::StringIndexOfCharacter, Opcode::StringLastIndexOfLiteral,
    Opcode::StringRangeEqualsLiteral, Opcode::StringRangeToNumber, Opcode::StringRangeParseInt, Opcode::StringRangeSubstringOrUndefined,
    Opcode::StringRangeCodeUnitOrNaN, Opcode::StringRangeCodeUnitOrMinusOne, Opcode::StringRangeCodePointOrUndefined, Opcode::StringRangeCharacterOrEmpty, Opcode::StringRangeCharacterOrUndefined
};

constexpr Effects withDeclaredKindOfThrow(Opcode opcode, Effects effects)
{
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

const Effects* findDeclaredEffectsOfEveryValue(Opcode);

inline Effects declaredEffectsOf(const Program& program, const Value& value)
{
    if (const Effects* effects = findDeclaredEffectsOfEveryValue(value.opcode))
        return *effects;
    EffectsBuilder builder(program);
    forEachDeclaredEffect(value, builder);
    return withDeclaredKindOfThrow(value.opcode, builder.effects());
}

inline Effects declaredEffectsOf(const Program& program, const Terminator& terminator)
{
    EffectsBuilder builder(program);
    forEachDeclaredEffect(terminator, builder);
    return builder.effects();
}

const Effects& declaredBoundInTable(Opcode);

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

// Whether a row says what the named set says and nothing else. The row may know more about the kind of error.
constexpr bool isBoundOf(NamedEffectSet set, const Effects& bound)
{
    Effects named = declaredBoundOf(set);
    if (named.mayThrow && bound.mayThrow)
        named.mayThrow = bound.mayThrow;
    return bound == named;
}

constexpr bool hasEffectsOfOverload(InlineBuiltin builtin)
{
    EffectBoundBuilder builder;
    forEachDeclaredEffect(builtin, builder);
    return builder.isAsOverload();
}

constexpr bool maySuspend(Opcode opcode) { return declaredBoundOf(opcode).maySuspend; }
constexpr bool mayThrow(Opcode opcode) { return declaredBoundOf(opcode).mayThrow; }
constexpr bool mayCollect(Opcode opcode) { return declaredBoundOf(opcode).mayCollect; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
