/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "CCallHelpers.h"
#include <optional>

namespace JSC {

class LinkBuffer;

namespace B3 {
class Procedure;
}

namespace AOT { namespace Typed {

enum class StackCheck : uint8_t;

enum class StackLimit : uint8_t { TrapAwareSoft, Soft };

struct PendingStackCheck {
    CCallHelpers::JumpList doesNotFit;
    CCallHelpers::Label resume;
    unsigned bytesBelowFrameRecord { 0 };
    // Of a frame without a record.
    std::optional<unsigned> bytesAllocatedBelowReturnAddress;
    StackLimit limit { StackLimit::TrapAwareSoft };
};

struct StackCheckState {
    Vector<PendingStackCheck, 1> pending;
    bool isProvenToFitOnStack { false };
    bool isEnteredWithoutStackCheck { true };
    bool functionMayCollect { true };
};

unsigned smallestFrameTestedAtSetup(StackCheck);
void setStackCheckGenerator(B3::Procedure&, StackCheck, const FrameRequirements&, StackCheckState&);
void emitStackOverflowPaths(CCallHelpers&, CodeRecords&, StackCheckState&);
Vector<PrologueCheck, 1> linkStackChecks(LinkBuffer&, FunctionID, const CodeRecords&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
