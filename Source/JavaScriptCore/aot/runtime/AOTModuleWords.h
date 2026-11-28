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
    InRecord,
    Evaluated,
    Evaluating,
};

struct ModuleWords {
    uint8_t* status;
    uint32_t evaluatingMark;
    uint32_t runsImportedFilesAlone;
    uint32_t moduleThatRunTimeStartsPlusOne;
};

static_assert(std::is_trivially_default_constructible_v<ModuleWords>);
static_assert(!static_cast<uint8_t>(StoredModuleStatus::Unlinked), "the status table starts as zero-filled pages");

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
