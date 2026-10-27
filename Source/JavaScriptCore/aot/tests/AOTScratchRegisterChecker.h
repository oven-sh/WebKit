/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

// The decoder is C that is compiled as C++ here. clang takes it; clang-cl was not tried.
#if CPU(ARM64) && !OS(WINDOWS)
#define ENABLE_AOT_SCRATCH_REGISTER_CHECKER 1
#endif

#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)

#include "GPRInfo.h"
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct ScratchRegisterConflict {
    enum class Kind : uint8_t {
        // The register is written twice and nothing in between reads it.
        ReplacedUnread,
        // One instruction names the register as two of its source operands.
        ReadAsTwoOperands,
    };

    friend bool operator==(const ScratchRegisterConflict&, const ScratchRegisterConflict&) = default;

    Kind kind;
    GPRReg reg;
    uint32_t offset;
    // Of the earlier write, for ReplacedUnread.
    uint32_t offsetOfWrite { 0 };
};

// Follows the macro assembler's two scratch registers through straight-line code: an entry, a branch target and whatever may leave the line start over.
// A test of a register against itself is no conflict. A pair loaded off the stack pointer restores what a call would have lost, and need not be read.
Vector<ScratchRegisterConflict> findScratchRegisterConflicts(std::span<const uint8_t> code, std::span<const uint32_t> entryOffsets);

void printInstructions(PrintStream&, std::span<const uint8_t> code, uint32_t firstOffset, uint32_t lastOffset);

} } // namespace JSC::AOT

#endif // ENABLE(AOT_SCRATCH_REGISTER_CHECKER)

#endif // ENABLE(AOT)
