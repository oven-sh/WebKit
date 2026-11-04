/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkLinks.h"

namespace JSC { namespace AOT { namespace Typed {

// `Object.keys(o)`, `Object.values(o)`, `Object.entries(o)`: of an object in sight, or of a layout that is proven or expected behind the tests of an arm, the list is a literal. In front of callBuiltin(), which takes the call by its row.
inline constexpr OpcodeSet opcodesThatBuildOwnKeyListTakes { Opcode::Call };
AOT_LINK_OF_WALK(buildOwnKeyList, opcodesThatBuildOwnKeyListTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
