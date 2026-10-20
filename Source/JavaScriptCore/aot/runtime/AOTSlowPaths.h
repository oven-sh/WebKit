/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "GPRInfo.h"

namespace JSC { namespace AOT {

enum class LeafAnswer : size_t { TakeSlowPath, No, Yes };
enum class LeafPresence : size_t { TakeSlowPath, Absent, Inherited, Own };

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
