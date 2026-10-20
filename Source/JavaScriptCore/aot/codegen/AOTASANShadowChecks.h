/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "B3Policy.h"

namespace JSC { namespace AOT {

B3::ClientPhase checkASANShadowBeforeAccesses();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
