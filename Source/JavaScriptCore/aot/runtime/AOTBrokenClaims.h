/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC { namespace AOT {

// What the code generator told the back end, and a run has found untrue. --aot-check=ClaimsToBackEnd, --aot-check=MovedLoads
enum class BrokenClaim : uint32_t {
    StackSlotIsWrittenBeforeRead,
    WordNeverChanges,
    LoadMayMove,
    ValueIsInRange,
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
