/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBrokenClaims.h"
#include "AOTID.h"
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

void emitReportOfBrokenClaim(CCallHelpers&, BrokenClaim, Typed::FunctionID, Typed::ValueID, int32_t offset = 0, GPRReg found = InvalidGPRReg, GPRReg given = InvalidGPRReg);

// The back end is told what an ordinary build tells it. All the checks go in behind the last phase that moves a value, so up to there the code is that of an ordinary build.
JS_EXPORT_PRIVATE void checkClaimsAtRunTime(B3::Policy&, Typed::FunctionID, const StackSlots& slotsOfCells, bool entersOnlyAtRoot);

// Each phase that moves a load or puts another value in its place writes into a log where the load stood. The procedure holds nothing of the log, so the code is that of an ordinary build.
// Behind the last phase that moves a value a second read goes in at each place that is still there.
JS_EXPORT_PRIVATE void checkMovedLoadsAtRunTime(B3::Policy&, Typed::FunctionID, const std::unique_ptr<B3::Procedure>&, const GuardTests&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
