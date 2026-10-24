/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// SelectTypedResults has nothing to do in a function without any of these.
inline constexpr OpcodeSet typedResultCallOpcodes { Opcode::Call, Opcode::CallBuiltin };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
