/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildTypedForInTakes { Opcode::ForInOpen, Opcode::ForInStep };
AOT_LINK_OF_WALK(buildTypedForIn, opcodesThatBuildTypedForInTakes)
bool countOfNamesComesRound(Walker&, const Walker::IndexedIteration&);
void markEnumeratorCurrent(Walker&, Reference enumerator, Reference object);

inline constexpr OpcodeSet opcodesThatUseKeyOfForInBehindTestTakes { Opcode::ElemGet, Opcode::ElemHas, Opcode::Call };
bool useKeyOfForInBehindTestOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool useKeyOfForInBehindTest(Walker& walker, const BodyValue& value) { return opcodesThatUseKeyOfForInBehindTestTakes.contains(value.opcode()) && walker.currentBodyIndex().opcodes().contains(Opcode::ForInStep) && useKeyOfForInBehindTestOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool useKeyOfForInBehindTest(Walker& walker, const BodyValueOf<known>& value) { return opcodesThatUseKeyOfForInBehindTestTakes.contains(known) && walker.currentBodyIndex().opcodes().contains(Opcode::ForInStep) && useKeyOfForInBehindTestOfItsOpcodes(walker, value); }
inline constexpr OpcodeSet opcodesThatBuildAccessUnderIndexOfForInTakes { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete };
bool buildAccessUnderIndexOfForInOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool buildAccessUnderIndexOfForIn(Walker& walker, const BodyValue& value) { return opcodesThatBuildAccessUnderIndexOfForInTakes.contains(value.opcode()) && walker.currentBodyIndex().opcodes().contains(Opcode::ForInStep) && buildAccessUnderIndexOfForInOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool buildAccessUnderIndexOfForIn(Walker& walker, const BodyValueOf<known>& value) { return opcodesThatBuildAccessUnderIndexOfForInTakes.contains(known) && walker.currentBodyIndex().opcodes().contains(Opcode::ForInStep) && buildAccessUnderIndexOfForInOfItsOpcodes(walker, value); }
Walker::IndexedIteration* findForInOfKey(Walker&, ValueID key);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
