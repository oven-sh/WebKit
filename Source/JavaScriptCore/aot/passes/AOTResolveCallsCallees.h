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
        return isProven() && !m_hasOtherCallees ? std::optional(Proven::byClosedSet(m_listedByType)) : std::nullopt;
    }
    // The function that the value is.
    std::optional<ProvenValue<FunctionID>> only() const
    {
        return all() && m_listedByType.size() == 1 && !mayBeNullish ? std::optional(Proven::byClosedSet(m_listedByType[0])) : std::nullopt;
    }
    // What arms or a test are made for. Each arm tests for its function, and what the arms leave is decided by the type of the value.
    Expected<std::span<const FunctionID>> listed() const LIFETIME_BOUND { return isProven() ? m_listedByType : expectedList(); }
    Expected<std::span<const FunctionID>> expected() const LIFETIME_BOUND { return expectedList(); }
    // Whether expected() outlives the candidates, as the list of a type does.
    bool expectsListOfProgram() const { return m_ownExpected.isEmpty(); }
    bool listsListOfProgram() const { return isProven() || expectsListOfProgram(); }

    // PlaceTypeTests puts the test for one expected callee. Several expected callees of a call get neither a test nor arms: chooseDispatchedCallees() says why. A construct gets an arm for each.
    bool hasOneExpectedCallee() const { return m_proof == CalleeProof::Test && expectedList().size() == 1 && !arriveOnWaysOfTheirOwn; }
    bool isDispatchedAtOperation() const { return m_proof != CalleeProof::Unknown && !hasOneExpectedCallee(); }

    // The sorted list of a type or a callee set of the program. It is not copied: many call sites may name one long list.
    void listByType(std::span<const FunctionID> listOfProgram, bool hasOtherCallees)
    {
        m_proof = CalleeProof::Type;
        m_listedByType = listOfProgram;
        m_hasOtherCallees = hasOtherCallees;
    }
    void expectListOfProgram(std::span<const FunctionID> listOfProgram)
    {
        m_ownExpected.clear();
        m_expectedOfProgram = listOfProgram;
        didExpect();
    }
    void expect(FunctionList&& functions)
    {
        m_ownExpected = WTF::move(functions);
        m_expectedOfProgram = { };
        didExpect();
    }

private:
    std::span<const FunctionID> expectedList() const LIFETIME_BOUND { return m_expectedOfProgram.empty() ? m_ownExpected.span() : m_expectedOfProgram; }
    void didExpect()
    {
        if (expectedList().empty())
            return;
        m_proof = CalleeProof::Test;
        m_hasOtherCallees = true;
    }

    CalleeProof m_proof { CalleeProof::Unknown };
    std::span<const FunctionID> m_listedByType;
    std::span<const FunctionID> m_expectedOfProgram;
    FunctionList m_ownExpected;
    bool m_hasOtherCallees { false };
};

CalleeCandidates calleeCandidates(const Program&, const Function&, ValueID callee);
// A value of the type may be a function that the type does not list.
bool admitsUnlistedFunctions(const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
