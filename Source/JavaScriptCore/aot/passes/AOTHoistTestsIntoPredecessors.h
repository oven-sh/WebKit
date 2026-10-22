/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInductionVariables.h"

namespace JSC { namespace AOT { namespace Typed {

// These convert a number to int32, which costs nothing where the number is an integer.
inline constexpr OpcodeSet operatorsOnInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };

inline constexpr OpcodeSet operatorsHoistedIntoPredecessors = relationalOperators | operatorsOnInt32;

// The test of a parameter for truthiness that a block branches on, or the first operator of a block.
inline constexpr OpcodeSet operationsHoistedIntoPredecessors = operatorsHoistedIntoPredecessors | OpcodeSet { Opcode::ToBoolean };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
