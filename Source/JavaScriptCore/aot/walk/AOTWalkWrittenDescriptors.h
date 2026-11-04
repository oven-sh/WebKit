/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// Walker::walkThroughChainOf(), in front of buildFreshObject(), which takes the literal for handed out. BuiltinCalls, EscapeAnalysis. False: nothing was built.
inline constexpr OpcodeSet opcodesThatBuildDefinitionByWrittenDescriptorTakes { Opcode::Call };
AOT_LINK_OF_WALK(buildDefinitionByWrittenDescriptor, opcodesThatBuildDefinitionByWrittenDescriptorTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
