/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTSchemaEffects.h"

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

inline Effects declaredEffectsOf(const Program& program, const Value& value)
{
    EffectsBuilder builder(program);
    forEachDeclaredEffect(value, builder);
    return builder.effects();
}

inline Effects declaredEffectsOf(const Program& program, const Terminator& terminator)
{
    EffectsBuilder builder(program);
    forEachDeclaredEffect(terminator, builder);
    return builder.effects();
}

template<typename Entry>
constexpr Effects declaredBoundOf(Entry entry)
{
    EffectBoundBuilder builder;
    forEachDeclaredEffect(entry, builder);
    return builder.effects();
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
