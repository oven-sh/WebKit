/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

// The links of for-in, each with the operations that it takes (AOTWalkLinks.h). AOTWalk.h includes this file at its end.

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkForIn.cpp. The enumerator in a register, the position and the number of current names round the loop.
inline constexpr OpcodeSet opcodesThatBuildTypedForInTakes { Opcode::ForInOpen, Opcode::ForInStep };
AOT_LINK_OF_WALK(buildTypedForIn, opcodesThatBuildTypedForInTakes)
bool countOfNamesComesRound(Walker&, const Walker::IterationByIndex&);

// AOTWalkForInKeys.cpp. `o[k]`, `k in o`, `Object.hasOwn(o, k)` under the key of a for-in over the same `o`.
inline constexpr OpcodeSet opcodesThatUseKeyOfForInBehindTestTakes { Opcode::ElemGet, Opcode::ElemHas, Opcode::Call };
AOT_LINK_OF_WALK(useKeyOfForInBehindTest, opcodesThatUseKeyOfForInBehindTestTakes)
inline constexpr OpcodeSet opcodesThatBuildAccessUnderIndexOfForInTakes { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete };
AOT_LINK_OF_WALK(buildAccessUnderIndexOfForIn, opcodesThatBuildAccessUnderIndexOfForInTakes)
Walker::IterationByIndex* findForInThatHandedOut(Walker&, ValueID key);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
