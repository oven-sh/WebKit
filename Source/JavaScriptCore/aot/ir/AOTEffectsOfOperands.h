/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffects.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTOpcode.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct EffectDescription;
struct Type;

Effects effectsOfCreatingPrototypeProperty(const Program&, const Type& functions);

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
    bool membersAreOfObjectsOnly { false };
    bool functionsAreConvertedUnderNamesOnly { false };
    std::span<const BuiltinName> convertedMembers { };
    bool isDescribedWhenRefused { false };
    BuiltinTypeID accepted { };
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
// How the host describes a value in the message of an error. Null where the table has no host.
const BuiltinParameter* findParameterDescribedInMessages(const BuiltinTable&);
bool mayFailOnOperand(const Program&, const BuiltinParameter&, const Type& operand);
bool isDescribedByModel(const BuiltinOverload&);
Vector<BuiltinName> namesReadFromOperands(const BuiltinTable&);

void addOperandCode(const EffectContext&, const Type& operand, const OperandBehavior&, EffectsOnEdges&);
void describeOverload(const Program&, BuiltinOverloadID, BuiltinCallForm, const Type* receiver, std::span<const TypeID> arguments, EffectDescription&);

enum class UsesAskedFor : uint8_t { All, AllButPassesToUserCode };

OperandSummary summarizeOperand(const EffectContext&, const Function&, const Value& user, uint32_t operandIndex, UsesAskedFor = UsesAskedFor::All);
OperandSummary summarizeOperand(const EffectContext&, const Function&, const Terminator& user, uint32_t operandIndex, UsesAskedFor = UsesAskedFor::All);
OperandMemory memoryAccessedThrough(const EffectContext&, const Function&, const Value& user, uint32_t operandIndex, const EffectsOnEdges& effectsOfUser);

enum class OperandConfinement : uint8_t { Unknown, WritesOnlyNamedOperands };

OperandConfinement confinementOf(Opcode);
bool isWrittenOperand(Opcode, uint32_t operandIndex);

OperandSummary joinOf(const OperandSummary&, const OperandSummary&);

BuiltinOperand builtinOperandAt(const Value& builtinCall, uint32_t operandIndex);
std::optional<uint32_t> entryParameterOf(const Function& callee, const Value& call, uint32_t operandIndex);
std::optional<uint32_t> findOperandPassedFor(const Function& callee, const Value& call, uint32_t entryParameter);
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

enum class ResultFreshness : uint8_t { Unknown, Unaliased, Fresh };

ResultFreshness freshnessOf(const EffectContext&, const Function&, const Value&);
ResultFreshness freshnessOf(const EffectContext&, const Function&, const Value&, NOESCAPE const ScopedLambda<EffectsOnEdges()>& effectsOfValue);
ResultFreshness freshnessOf(const EffectContext&, const Function&, const Terminator&);
bool resultMayBeFresh(Opcode);
constexpr bool resultMayBeFresh(TerminatorKind kind) { return effectRuleOf(kind) == EffectRule::Inline; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
