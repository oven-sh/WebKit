/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class Function;
class PredictedValues;
struct DeclaredType;
struct Value;

// What an annotation or a guess says of a value. Only a test makes it true.
class ExpectedType {
public:
    constexpr ExpectedType() = default;
    constexpr explicit ExpectedType(TypeID type)
        : m_type(type)
    {
    }

    constexpr TypeID predicted() const { return m_type; }
    constexpr TypeID payload() const { return m_type; }
    constexpr explicit operator bool() const { return !!m_type; }
    friend constexpr bool operator==(ExpectedType, ExpectedType) = default;

private:
    TypeID m_type;
};

// EveryVersion: true whatever arrives. BehindChecks: true where every check in front of the value has passed.
enum class ProofScope : uint8_t { EveryVersion, BehindChecks };

class ProvenInEveryVersion;

// A type that no rule derives and no test backs where it is written down. Only Proven makes one.
class ProvenType {
public:
    constexpr operator TypeID() const { return m_type; }
    constexpr TypeID payload() const { return m_type; }
    constexpr ProofScope scope() const { return m_scope; }
    constexpr std::optional<ProvenInEveryVersion> inEveryVersion() const;

private:
    friend struct Proven;
    friend class ProvenInEveryVersion;
    constexpr ProvenType(TypeID type, ProofScope scope)
        : m_type(type)
        , m_scope(scope)
    {
    }

    TypeID m_type;
    ProofScope m_scope;
};

// What outlives the checks of a body takes no other proof: the type of a position.
class ProvenInEveryVersion : public ProvenType {
private:
    friend struct Proven;
    friend class ProvenType;
    constexpr explicit ProvenInEveryVersion(TypeID type)
        : ProvenType(type, ProofScope::EveryVersion)
    {
    }
};

constexpr std::optional<ProvenInEveryVersion> ProvenType::inEveryVersion() const
{
    return m_scope == ProofScope::EveryVersion ? std::optional(ProvenInEveryVersion(m_type)) : std::nullopt;
}

// Every reason for which a type may be written down as true.
struct Proven {
    // The type that the rules give the value, or a type that it lies inside.
    static ProvenType byRules(const PredictedValues&, ValueID, TypeID);
    // A join that has grown past a limit says less than its sources do.
    static ProvenType byRulesBeforeWidening(TypeID type, ProofScope scope) { return { type, scope }; }
    // In a body without checks.
    static ProvenInEveryVersion byRulesWithoutChecks(TypeID type) { return ProvenInEveryVersion(type); }
    // The pass has made the value that way, or the operation that takes it has no other form.
    static ProvenInEveryVersion byConstruction(TypeID type) { return ProvenInEveryVersion(type); }
    // Every store into the position is held to its type.
    static ProvenInEveryVersion byPosition(TypeID type) { return ProvenInEveryVersion(type); }
    // What the whole program lets arrive.
    static ProvenType byFlow(TypeID type, ProofScope scope) { return { type, scope }; }
    static ProvenInEveryVersion byFlowOfPosition(TypeID type) { return ProvenInEveryVersion(type); }
    // The claim is moved or copied as it stands.
    static ProvenType asClaimedBy(const Value& assumeType);
    // The type that the rules give a value, for whoever has not looked at what stands in front of the value.
    static ProvenType byRulesBehindChecks(TypeID type) { return { type, ProofScope::BehindChecks }; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
