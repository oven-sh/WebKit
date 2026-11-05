/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildTypedForInTakes { Opcode::ForInOpen, Opcode::ForInStep };
AOT_LINK_OF_WALK(buildTypedForIn, opcodesThatBuildTypedForInTakes)
bool countOfNamesComesRound(Walker&, const Walker::IterationByIndex&);

inline constexpr OpcodeSet opcodesThatUseKeyOfForInBehindTestTakes { Opcode::ElemGet, Opcode::ElemHas, Opcode::Call };
AOT_LINK_OF_WALK(useKeyOfForInBehindTest, opcodesThatUseKeyOfForInBehindTestTakes)
inline constexpr OpcodeSet opcodesThatBuildAccessUnderIndexOfForInTakes { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete };
AOT_LINK_OF_WALK(buildAccessUnderIndexOfForIn, opcodesThatBuildAccessUnderIndexOfForInTakes)
Walker::IterationByIndex* findForInThatHandedOut(Walker&, ValueID key);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
