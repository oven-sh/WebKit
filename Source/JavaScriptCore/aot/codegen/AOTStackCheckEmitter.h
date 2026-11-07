/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTCodeRecords.h"
#include "AOTVector.h"
#include "CCallHelpers.h"

namespace JSC {

class LinkBuffer;

namespace AOT { namespace Typed {

enum class StackLimit : uint8_t { TrapAwareSoft, Soft };

struct PendingStackCheck {
    CCallHelpers::JumpList doesNotFit;
    CCallHelpers::Label resume;
    StackLimit limit { StackLimit::TrapAwareSoft };
    bool isAtEntry { true };
};

struct StackCheckState {
    StackCheck stackCheck { StackCheck::Required };
    const FrameRequirements* frame { nullptr };
    Vector<PendingStackCheck, 1> pending;
    bool isProvenToFitOnStack { false };
    bool isEnteredWithoutStackCheck { true };
    bool functionMayCollect { true };
};

void setStackCheckGenerator(Machine::Procedure&, StackCheck, const FrameRequirements&, StackCheckState&);
void emitStackTestAheadOfFramedPart(CCallHelpers&, const Machine::PatchParameters&);
void emitStackOverflowPaths(CCallHelpers&, CodeRecords&, StackCheckState&);
Vector<PrologueCheck, 1> linkStackChecks(LinkBuffer&, const CodeRecords&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
