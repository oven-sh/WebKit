/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkLinks.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildOwnKeyListTakes { Opcode::Call };
AOT_LINK_OF_WALK(buildOwnKeyList, opcodesThatBuildOwnKeyListTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
