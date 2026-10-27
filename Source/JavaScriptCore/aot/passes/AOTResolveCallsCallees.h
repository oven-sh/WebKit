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
    // The function itself makes the closures that are expected, each on a way of its own to the operation.
    bool arriveOnWaysOfTheirOwn { false };

    bool isProven() const { return proof == CalleeProof::Type; }
    const FunctionList& listed() const LIFETIME_BOUND { return isProven() ? proven : expected; }
    // PlaceTypeTests puts the test for one expected callee. Several that are only expected get neither a test nor arms: chooseDispatchedCallees() says why.
    bool hasOneExpectedCallee() const { return proof == CalleeProof::Test && expected.size() == 1 && !arriveOnWaysOfTheirOwn; }
    bool isDispatchedAtOperation() const { return proof != CalleeProof::Unknown && !hasOneExpectedCallee(); }
};

CalleeCandidates calleeCandidates(const Program&, const Function&, ValueID callee);
// A value of the type may be a function that the type does not list.
bool admitsUnlistedFunctions(const Program&, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
