/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC {

class JSCell;

namespace AOT {

// Under validation: what the compiler believes about the call by convention that follows. NativeCallExpectation encodes the words.
struct ExpectedCallWords {
    // Not a root: the run time compares it and never dereferences it.
    JSCell* callee;
    uint64_t effects;
    uint64_t site;
    uint64_t builtins;
};

// Where a value that is not of the type its function promises goes, for a caller that was asked before: its landing, or, where it has none, the address that it is returned to anyway.
// Code does not move, so an entry holds for good.
struct LandingOfMisfit {
    static constexpr unsigned numberOfEntriesLog2 = 6;
    static constexpr unsigned shiftOfAddress = 2;
    static constexpr unsigned indexOf(const void* returnAddress) { return static_cast<unsigned>(std::bit_cast<uintptr_t>(returnAddress) >> shiftOfAddress) & ((1u << numberOfEntriesLog2) - 1); }

    const void* returnAddress;
    const void* landing;
};
static_assert(sizeof(LandingOfMisfit) == 16);

struct CodegenScratchWords {
    // Not a root: compiled code compares it and never dereferences it.
    JSCell* pendingBarrierOwner;
    ExpectedCallWords expectedCall;
    // Under validation: set whenever the run time enters compiled code.
    uint64_t didEnterCompiledCode;
    // Not a root: the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs.
    uint64_t misfit;
    LandingOfMisfit landingsOfMisfits[1u << LandingOfMisfit::numberOfEntriesLog2];
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
