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

struct CalleeCandidates {
    CalleeProof proof { CalleeProof::Unknown };
    FunctionList proven;
    FunctionList expected;
    ValueID value;
    bool hasOtherCallees { false };
    bool mayBeNullish { false };

    bool isProven() const { return proof == CalleeProof::Type; }
    const FunctionList& listed() const LIFETIME_BOUND { return isProven() ? proven : expected; }
    // PlaceTypeTests puts the test for one expected callee. Several that are only expected get neither a test nor arms: chooseDispatchedCallees() says why.
    bool hasOneExpectedCallee() const { return proof == CalleeProof::Test && expected.size() == 1; }
    bool isDispatchedAtOperation() const { return proof != CalleeProof::Unknown && !hasOneExpectedCallee(); }

    void dump(PrintStream&) const;
};

CalleeCandidates calleeCandidates(const Program&, const Function&, ValueID callee);
// A value of the type may be a function that the type does not list.
bool admitsUnlistedFunctions(const Program&, const Type&);
// Where neither the type of the callee nor its expected type lists a function, the callee is expected to be one of the functions that flow saw arrive at the operation.
CalleeCandidates calleeCandidatesOf(const Program&, const Function&, const Value& operation, ValueID callee);

struct OnlyCallee {
    ValueID value;
    FunctionID function;
};

// Where calleeCandidates() proves one function and admits nothing else, not even undefined: that function, and the value whose type says it.
std::optional<OnlyCallee> findOnlyCallee(const Program&, const Function&, ValueID callee);

ValueID findClosureCreation(const Function&, ValueID closure, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
