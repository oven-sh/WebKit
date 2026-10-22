/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet hoistableReadOpcodes { Opcode::GetModuleVariable, Opcode::GetEncodedModuleVariable, Opcode::GetEnvironmentField };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
