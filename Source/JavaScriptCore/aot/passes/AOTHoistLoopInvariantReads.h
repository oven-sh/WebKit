/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet hoistableReadOpcodes { Opcode::GetModuleVariable, Opcode::GetEncodedModuleVariable, Opcode::GetEnvironmentField };
// Whether a value is truthy never changes.
inline constexpr OpcodeSet hoistableOpcodes = hoistableReadOpcodes | OpcodeSet { Opcode::ToBoolean };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
