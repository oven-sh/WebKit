/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTestRuns.cpp . Folding, in the slow version of a function, in a test run alone. A link of the chain.
// `Math.sqrt(x)`: an executable names the two objects. A test run writes two reads, because a test may replace either member, and the program locks whichever no write was followed to.
// Such a read is the object that an executable names there, as TypedOperations has it in every other version. The lock holds whoever writes, so nothing that Link has proven is leaned on. False: nothing was built.
inline constexpr OpcodeSet opcodesThatBuildBuiltinThatExecutableNamesTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(buildBuiltinThatExecutableNames, opcodesThatBuildBuiltinThatExecutableNamesTakes)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
