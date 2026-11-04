/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTextBuilderRows.cpp. What AOTWalkTypedTexts.cpp, Guesses and BuiltinCalls ask about rows of joins. takePartsOfJoinReturnedByCopy() is declared in AOTWalkTextBuilders.h.

// For CallInWalkBuiltins::build(), every operand settled. It asks, in this order: writeJSONTextOfCall() . handJSONTextToBuiltin() . joinOfWhatIsInSight(). True: the call is bound, or no way leads on. False: nothing was built.
bool buildCallOfBuiltinForTexts(Walker&, const BodyValue& call);
// `[a, b].join("")`, `a.concat(b, c)`, String.raw`${a}/${b}`: the call is the Strcat of what is in sight. True: it is built and bound to the call. False: nothing was built.
bool joinOfWhatIsInSight(Walker&, const BodyValue& call);

// For findJoinThatAloneTakesInWalkTexts(), of an operation between two links of a row. `link`: the join in front, up to which the block is walked.
// True: it is an operator whose operands are proven to be such that nobody can tell whether it has run: `name + "[" + (i + 1) + "]"`. It builds nothing.
bool mayStandInsideRowOfJoins(const Walker&, const BodyIndex&, ValueID link, ValueID between);
// For buildTypedJoinOfTexts(), of a join that no other join takes. True: takePartsOfJoinReturnedByCopy() has put parts in the place of an operand, so the join is built as the Strcat of `parts`.
bool holdsPartsTakenOutOfTexts(const Walker&, const BodyValue& join, std::span<const Reference> parts);
// For Guesses, of an Add. `"id-" + x`: one operand is a string, and nothing proves that the other has text by itself. One test of that operand, the row of joins as one Strcat, the row as written in an unlikely block, and the two meet.
// True: both arms are built. False: nothing was built.
inline constexpr OpcodeSet opcodesThatGuessThatOperandBesideStringHasTextTakes { Opcode::Add };
AOT_LINK_OF_WALK(guessThatOperandBesideStringHasText, opcodesThatGuessThatOperandBesideStringHasTextTakes)
// For Guesses, of the `${c}` of a template, in front of TypedOperations, which builds the conversion of anything. The same test and the same arms, where the row that takes the conversion ends in a text that lies in a builder.
inline constexpr OpcodeSet opcodesThatGuessThatConvertedPartOfTextInBuilderHasTextTakes { Opcode::ToString };
AOT_LINK_OF_WALK(guessThatConvertedPartOfTextInBuilderHasText, opcodesThatGuessThatConvertedPartOfTextInBuilderHasTextTakes)

// It decides what pays, never what is right. `made` stands in the block in which the walk stands, and no record behind it takes it, but for other names and forms of it.
// False, too, where more records stand behind it than are looked at.
bool isReadByNoRecordSoFar(const Walker&, Reference made);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
