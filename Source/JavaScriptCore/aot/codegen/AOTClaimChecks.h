/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTRuntimeTable.h"
#include "AOTStackSlotPoisoning.h"
#include "AOTVector.h"
#include "B3Policy.h"
#include "GPRInfo.h"
#include "JSExportMacros.h"
#include <memory>

namespace JSC {

class CCallHelpers;

namespace AOT {

// A read of a byte of member guards, and the bit of it that the operation asks for.
struct GuardTest {
    Typed::ValueID operation;
    int32_t offset;
    uint8_t mask;
    bool holdsWhileClear;
};
using GuardTests = Vector<GuardTest>;

// One that comes back keeps every register, prints a place once and lets the run go on.
enum class ReportOfBrokenClaim : uint8_t { EndsRun, ComesBack };
void emitReportOfBrokenClaim(CCallHelpers&, BrokenClaim, Typed::FunctionID, Typed::ValueID, int32_t offset = 0, GPRReg found = InvalidGPRReg, GPRReg given = InvalidGPRReg, ReportOfBrokenClaim = ReportOfBrokenClaim::EndsRun);

// A validating build has the back end test what it is told about a value right behind the value. A test that fails names the value, where it would only trap.
void reportBrokenKnownProperties(B3::Policy&, Typed::FunctionID);

// The back end is told what an ordinary build tells it. All the checks go in behind the last phase that moves a value, so up to there the code is that of an ordinary build.
JS_EXPORT_PRIVATE void checkClaimsAtRunTime(B3::Policy&, Typed::FunctionID, const SlotsOfCells&);

// Each phase that moves a load or puts another value in its place writes into a log where the load stood. The procedure holds nothing of the log, so the code is that of an ordinary build.
// Behind the last phase that moves a value a second read goes in at each place that is still there.
// --aot-check=DecidedComparisons: a phase that puts an answer in the place of a comparison of two integers writes the two into the same log, and they are compared at that place. They stay alive for it.
struct WhatIsLogged {
    bool loads { true };
    bool decidedComparisons { false };
    // --aot-check=KeepGoing beside them.
    bool reportsComeBack { false };
};
JS_EXPORT_PRIVATE void checkMovedLoadsAtRunTime(B3::Policy&, Typed::FunctionID, const std::unique_ptr<B3::Procedure>&, const GuardTests&, WhatIsLogged = { });

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
