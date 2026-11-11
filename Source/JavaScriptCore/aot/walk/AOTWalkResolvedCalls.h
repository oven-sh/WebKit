/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatCallThroughTargetResolvedOnceTakes { Opcode::Call };
AOT_LINK_OF_WALK(callThroughTargetResolvedOnce, opcodesThatCallThroughTargetResolvedOnceTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
