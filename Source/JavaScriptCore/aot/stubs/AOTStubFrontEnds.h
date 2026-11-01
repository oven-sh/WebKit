/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubKey.h"
#include <span>

namespace JSC { namespace AOT {

class StubAssembler;
struct StubSignature;

struct OperationFrontEnd {
    Entry operation;
    Entry raw;
    StubKey stub;
};

inline constexpr OperationFrontEnd operationFrontEndTable[] = {
#define AOT_STUB_FRONT_END(Operation, Family) { Entry::operationAOT##Operation, Entry::Raw##Operation, StubKey(StubFamily::Family) },
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
};

constexpr std::span<const OperationFrontEnd> operationFrontEnds()
{
    return operationFrontEndTable;
}

consteval bool operationFrontEndsAreWellFormed()
{
    for (auto& frontEnd : operationFrontEndTable) {
        for (auto& other : operationFrontEndTable) {
            if (frontEnd.raw == other.operation)
                return false;
            if (&frontEnd != &other && (frontEnd.operation == other.operation || frontEnd.raw == other.raw || frontEnd.stub == other.stub))
                return false;
        }
    }
    return true;
}
static_assert(operationFrontEndsAreWellFormed());

// Argument 0, the Instance*, is not an operand: a front end reads instanceGPR, and tailCallOperation sets the argument again.
StubSignature operationFrontEndSignature(unsigned numberOfOperands);

enum class PreludeExit : bool { CallsOperation, Returns };

struct OperationPrelude {
    Entry operation;
    PreludeExit (*emit)(StubAssembler&);
};

std::span<const OperationPrelude> operationPreludes();
bool hasPrelude(Entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
