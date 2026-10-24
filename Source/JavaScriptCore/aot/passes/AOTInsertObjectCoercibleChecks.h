/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet objectCoercibleCheckOpcodes {
    Opcode::PropGet, Opcode::ElemGet, Opcode::PropSet, Opcode::ElemSet, Opcode::IteratorOpen, Opcode::RequireObjectCoercible
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
