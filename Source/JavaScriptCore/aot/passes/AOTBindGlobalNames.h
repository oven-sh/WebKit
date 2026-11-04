/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

class IRBuilder;

inline constexpr OpcodeSet globalBindingAccesses { Opcode::GetGlobalBinding, Opcode::SetGlobalBinding };
// The opcodes that BindGlobalNames rewrites into them.
inline constexpr OpcodeSet bindableAccesses { Opcode::PropGet, Opcode::PropSet };

// Undoes BindGlobalNames for one access. The caller applies the builder's insertions.
void convertGlobalBindingToNamedAccess(IRBuilder&, ProgramPoint, ValueID access);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
