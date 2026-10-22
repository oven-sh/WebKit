/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// An object with a layout stays where it is: the forms of its constructor and the end of its birth go by that place.
inline constexpr OpcodeSet allocationsPlacedAtTheirUse { Opcode::NewArray, Opcode::CreateUnmappedArguments };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
