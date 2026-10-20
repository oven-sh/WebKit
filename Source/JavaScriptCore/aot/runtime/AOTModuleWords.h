/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <stdint.h>
#include <type_traits>

namespace JSC { namespace AOT {

enum class StoredModuleStatus : uint8_t {
    Unlinked,
    Linked,
    Evaluating,
    Evaluated,
    InRecord,
};

enum class RequireEdgeState : uint8_t {
    NeedsLoader,
    Ready,
};

struct ModuleWords {
    uint8_t* status;
    uint8_t* requireEdges;
};

static_assert(std::is_trivially_default_constructible_v<ModuleWords>);
static_assert(!static_cast<uint8_t>(StoredModuleStatus::Unlinked), "the status table starts as zero-filled pages");
static_assert(!static_cast<uint8_t>(RequireEdgeState::NeedsLoader), "the table of require edges starts as zero-filled pages");

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
