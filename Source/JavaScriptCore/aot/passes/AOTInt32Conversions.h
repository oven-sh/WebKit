/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// Each applies ToInt32 or ToUint32 to every operand that is a number.
inline constexpr OpcodeSet int32ConvertingOperations {
    Opcode::BitAnd,
    Opcode::BitOr,
    Opcode::BitXor,
    Opcode::LShift,
    Opcode::RShift,
    Opcode::URShift,
    Opcode::BitNot,
    Opcode::ToInt32,
    Opcode::ToUint32,
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
