/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccess.h"

namespace JSC { namespace AOT { namespace Typed {

using FunctionList = SortedSet<FunctionID, 2>;

// Type: the type of the value lists its callees. Test: only its expected type lists them, and the operation has to tell them from whatever else the value is.
enum class CalleeProof : uint8_t { Unknown, Test, Type };

class CalleeCandidates {
public:
    ValueID value;
    bool mayBeNullish { false };
    // The function itself makes the closures that are expected, each on a way of its own to the operation.
    bool arriveOnWaysOfTheirOwn { false };

    CalleeProof proof() const { return m_proof; }
    bool isProven() const { return m_proof == CalleeProof::Type; }

    // Every function that the value may be. Beside them it is at most undefined or null (mayBeNullish).
    std::optional<ProvenValue<std::span<const FunctionID>>> all() const LIFETIME_BOUND
    {
        return isProven() && !m_hasOtherCallees ? std::optional(Proven::byClosedSet(m_listedByType.span())) : std::nullopt;
    }
    // The function that the value is.
    std::optional<ProvenValue<FunctionID>> only() const
    {
        return all() && m_listedByType.size() == 1 && !mayBeNullish ? std::optional(Proven::byClosedSet(m_listedByType[0])) : std::nullopt;
    }
    // What arms or a test are made for. Each arm tests for its function, and what the arms leave is decided by the type of the value.
    Expected<std::span<const FunctionID>> listed() const LIFETIME_BOUND { return isProven() ? m_listedByType.span() : m_expected.span(); }
    Expected<std::span<const FunctionID>> expected() const LIFETIME_BOUND { return m_expected.span(); }

    // PlaceTypeTests puts the test for one expected callee. Several that are only expected get neither a test nor arms: chooseDispatchedCallees() says why.
    bool hasOneExpectedCallee() const { return m_proof == CalleeProof::Test && m_expected.size() == 1 && !arriveOnWaysOfTheirOwn; }
    bool isDispatchedAtOperation() const { return m_proof != CalleeProof::Unknown && !hasOneExpectedCallee(); }

    void listByType(std::span<const FunctionID> functions, bool hasOtherCallees)
    {
        m_proof = CalleeProof::Type;
        m_listedByType = FunctionList(functions);
        m_hasOtherCallees = hasOtherCallees;
    }
    void expect(FunctionList&& functions)
    {
        m_expected = WTF::move(functions);
        if (m_expected.isEmpty())
            return;
        m_proof = CalleeProof::Test;
        m_hasOtherCallees = true;
    }

private:
    CalleeProof m_proof { CalleeProof::Unknown };
    FunctionList m_listedByType;
    FunctionList m_expected;
    bool m_hasOtherCallees { false };
};

CalleeCandidates calleeCandidates(const Program&, const Function&, ValueID callee);
// A value of the type may be a function that the type does not list.
bool admitsUnlistedFunctions(const Program&, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
