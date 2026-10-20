/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccess.h"

namespace JSC { namespace AOT { namespace Typed {

using FunctionList = SortedSet<FunctionID, 2>;

// Test: nothing proves the callees, and the operation has to tell them from whatever else the value is.
enum class CalleeProof : uint8_t { Unknown, Test, Type };

struct CalleeCandidates {
    CalleeProof proof { CalleeProof::Unknown };
    FunctionList proven;
    FunctionList predicted;
    ValueID value;
    bool hasOtherCallees { false };
    bool mayBeNullish { false };

    bool isProven() const { return proof != CalleeProof::Unknown; }

    void dump(PrintStream&) const;
};

CalleeCandidates calleeCandidates(const Program&, const FlowResults&, const Function&, ValueID callee, CalleeSetID stamp, AccessSiteID);
// Which code a closure of the function enters here decides which layouts are allocated, and the types behind the operation rest on that.
void requireArmFor(CalleeCandidates&, FunctionID closureFunction);
void requireArmsForCallSiteVersions(const Program&, CalleeCandidates&, CalleeSetID stamp);

FunctionID closureFunctionOf(const Program&, const FlowResults&, FunctionID version);
FunctionID versionToEnter(const Program&, const FlowResults&, FunctionID closureFunction, CalleeSetID stamp);
ResultCheck resultCheckOf(const FlowResults&, FunctionID);

ValueID findClosureCreation(const Function&, ValueID closure, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
