/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// `for (..) callback(i)`, the loop of `map` in the library: a Call that every link and every guess has left closed, of a value from in front of the loop. The last link of the chain. False: nothing that stays was built.
inline constexpr OpcodeSet opcodesThatCallThroughTargetResolvedOnceTakes { Opcode::Call };
AOT_LINK_OF_WALK(callThroughTargetResolvedOnce, opcodesThatCallThroughTargetResolvedOnceTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
