/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffects.h"
#include "AOTEffectsOfProtocols.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct EffectDescription;
struct Type;

// The effect rule of an operation covers what its own stores and deletes run. A built-in stores with [[Set]] and deletes with [[Delete]],
// and only the uses of its operands say so.
enum class OperandDeclarer : uint8_t { Operation, Builtin };

struct OperandBehavior {
    OperandUses uses { OperandUses::all() };
    Protocols protocols { Protocols::all() };
    std::span<const BuiltinName> namedMembers { };
    std::span<const BuiltinName> calledMembers { };
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t retainingOperand { 0 };
    OperandDeclarer declarer { OperandDeclarer::Builtin };
    NamedMembers namedMemberUse { NamedMembers::AreOnlyRead };
    bool resultOfCallEscapes { false };
    // Native code tests that a value is an object before it reads a member of it, by name or to iterate over it.
    bool membersAreOfObjectsOnly { false };
    // Native code tests that a value it means to call is callable and keeps it. It converts a function to a primitive only where it has read it under one of namedMembers.
    bool functionsAreConvertedUnderNamesOnly { false };
    std::span<const BuiltinName> convertedMembers { };
};

struct OperandSummary {
    OperandUses uses { OperandUses::all() };
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t retainingOperand { 0 };

    friend bool operator==(const OperandSummary&, const OperandSummary&) = default;
};

struct OperandMemory {
    LocationMasks reads { perObjectHeaps };
    LocationMasks writes { perObjectHeaps };

    static OperandMemory none() { return { LocationMasks(), LocationMasks() }; }

    bool merge(const OperandMemory& other)
    {
        bool changed = reads.merge(other.reads);
        changed |= writes.merge(other.writes);
        return changed;
    }

    friend bool operator==(const OperandMemory&, const OperandMemory&) = default;
};

inline constexpr OperandUses callingUses { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };

inline constexpr OperandUses operandWritingUses {
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::ChangesIntegrity,
};

inline constexpr OperandUses unconfinedUses {
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

inline constexpr Effects listReadEffects = [] {
    Effects effects = Effects::none();
    effects.reads = AbstractHeapSet { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
    return effects;
}();

OperandBehavior declaredBehaviorOf(Opcode, uint32_t operandIndex);
OperandBehavior declaredBehaviorOf(TerminatorKind, uint32_t operandIndex);
OperandBehavior declaredBehaviorOf(const BuiltinOverload&, BuiltinOperand);

void addOperandCode(const EffectContext&, const Type& operand, const OperandBehavior&, EffectsOnEdges&);
void describeOverload(const Program&, BuiltinOverloadID, BuiltinCallForm, const Type* receiver, std::span<const TypeID> arguments, EffectDescription&);

OperandSummary summarizeOperand(const EffectContext&, const Function&, const Value& user, uint32_t operandIndex);
OperandSummary summarizeOperand(const EffectContext&, const Function&, const Terminator& user, uint32_t operandIndex);
OperandMemory memoryAccessedThrough(const EffectContext&, const Function&, const Value& user, uint32_t operandIndex, const EffectsOnEdges& effectsOfUser);

enum class OperandConfinement : uint8_t { Unknown, WritesOnlyNamedOperands };

OperandConfinement confinementOf(Opcode);
bool isWrittenOperand(Opcode, uint32_t operandIndex);

OperandSummary joinOf(const OperandSummary&, const OperandSummary&);

BuiltinOperand builtinOperandAt(const Value& builtinCall, uint32_t operandIndex);
std::optional<uint32_t> entryParameterOf(const Function& callee, const Value& call, uint32_t operandIndex);
std::optional<uint32_t> findOperandPassedFor(const Function& callee, const Value& call, uint32_t entryParameter);
// The callee parameter, and `new.target` where a construction names none.
Vector<uint32_t, 2> entryParametersOfCalledValue(const Function& callee, const Value& call);

template<typename Entry>
bool mayRunOperandCode(Entry entry)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(entry) + (hasVariadicOperand(entry) ? 1 : 0);
    for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
        if (!protocolsOf(entry, index).isEmpty() || operandUsesOf(entry, index).containsAny(callingUses))
            return true;
    }
    return false;
}

bool mayBeReference(const Type&);

// An unaliased result is referenced from nowhere else, yet has properties or a structure that nothing declares.
enum class ResultFreshness : uint8_t { Unknown, Unaliased, Fresh };

ResultFreshness freshnessOf(const EffectContext&, const Function&, const Value&);
// Of the result that the terminator itself passes along an edge.
ResultFreshness freshnessOf(const EffectContext&, const Function&, const Terminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
