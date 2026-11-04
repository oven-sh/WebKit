/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <stdint.h>
#include <type_traits>

namespace JSC { namespace AOT {

// Compiled code tells by one comparison whether a module is still to be evaluated.
enum class StoredModuleStatus : uint8_t {
    Unlinked,
    Linked,
    InRecord,
    Evaluated,
    // And every higher value: see ModuleWords::evaluatingMark.
    Evaluating,
};

struct ModuleWords {
    uint8_t* status;
    // The status that a module which evaluates itself stores when it begins. Each evaluation that the run time starts has a value of its own, by which it finds the modules that it left unfinished.
    uint32_t evaluatingMark;
};

static_assert(std::is_trivially_default_constructible_v<ModuleWords>);
static_assert(!static_cast<uint8_t>(StoredModuleStatus::Unlinked), "the status table starts as zero-filled pages");

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
