/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3HeapRange.h"
#include "B3Opcode.h"
#include "B3Origin.h"
#include "B3Type.h"
#include <optional>
#include <wtf/SharedTask.h>

namespace JSC {

class CCallHelpers;

namespace B3 {

class PatchpointValue;
class Procedure;
class Value;

// What is left of a load once a phase has moved it or put another value in its place.
struct MovedLoad {
    // Only where the policy asks for the check.
    static std::optional<MovedLoad> of(const Procedure&, const Value*);
    JS_EXPORT_PRIVATE static std::optional<MovedLoad> describe(const Value*);

    Opcode opcode;
    Type type;
    int32_t offset;
    Origin origin;
};

// Stands where the load stood: it reads the location again and reports unless it holds what the users of the load are given. No load or store waits for it.
using DifferenceReport = SharedTask<void(CCallHelpers&, Origin)>;
JS_EXPORT_PRIVATE PatchpointValue* createMovedLoadCheck(Procedure&, const MovedLoad&, Value* address, Value* givenToUsers, RefPtr<DifferenceReport>);
PatchpointValue* createMovedLoadCheck(Procedure&, const MovedLoad&, Value* address, Value* givenToUsers);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
