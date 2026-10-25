/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "B3Policy.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT {

B3::ClientPhase checkASANShadowBeforeAccesses();

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
// Preserves every register, the flags and the red zone.
void emitASANShadowCheck(CCallHelpers&, CCallHelpers::Address, unsigned size, bool isStore);
void emitASANShadowCheck(CCallHelpers&, CCallHelpers::BaseIndex, unsigned size, bool isStore);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
