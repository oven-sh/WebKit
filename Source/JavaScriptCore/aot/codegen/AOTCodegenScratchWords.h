/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <bit>
#include <cstddef>
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
    const void* returnAddress;
    const void* landing;
};
static_assert(sizeof(LandingOfMisfit) == 16);

// An address has a place, and is listed there or in the first entry behind it that was empty. As many entries as there are places lie behind the places, and the last of them stays empty,
// so a search ends without turning round. The table grows, so two callers never take an entry from each other.
struct LandingsOfMisfits {
    static constexpr unsigned shiftOfAddress = 2;
    static constexpr size_t leastNumberOfPlaces = 64;
    size_t numberOfPlaces() const { return (maskOfAddress >> shiftOfAddress) + 1; }
    LandingOfMisfit* placeOf(const void* returnAddress) const { return entries + ((std::bit_cast<uintptr_t>(returnAddress) & maskOfAddress) >> shiftOfAddress); }

    LandingOfMisfit* entries;
    uintptr_t maskOfAddress;
    size_t numberOfEntriesFilled;
    // The table of a program that has returned no such value.
    LandingOfMisfit empty;
};

struct CodegenScratchWords {
    // Not a root: compiled code compares it and never dereferences it.
    JSCell* pendingBarrierOwner;
    ExpectedCallWords expectedCall;
    // Under validation: set whenever the run time enters compiled code.
    uint64_t didEnterCompiledCode;
    // Not a root: the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs.
    uint64_t misfit;
    LandingsOfMisfits landingsOfMisfits;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
