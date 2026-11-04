/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: how a piece is compared with a written text where StringRangeEqualsLiteral is absent or has refused: `s.slice(0, n) === "abc"`, `s.slice(k) === ".ts"`.
// A piece from the start, or up to the end, equals the text where it is as long as the text and its string begins or ends with the text. Any other piece is cut.
// It keeps nothing, and the two tests throw only for lack of memory, as the cut does. Withheld (TypedOperations): its caller is not asked.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

Reference buildComparisonOfPieceAtEndOfStringWithText(Walker& walker, const PieceOfString& piece, StringID text, Origin origin)
{
    unsigned length = lengthOfTextForStrings(walker, text);
    bool beginsAtStart = piece.start == integerForStrings(walker, 0);
    if (walker.isUnreachable() || !length || length > maxLengthOfTextComparedInPlace || (!beginsAtStart && !isLengthOfStringForStrings(walker, piece.end, piece.string)))
        return { };

    Reference lengthOfPiece = beginsAtStart ? piece.end : buildOperatorForStrings(walker, Opcode::Sub, piece.end, piece.start, origin);
    Reference isAsLong = buildOperatorForStrings(walker, Opcode::StrictEq, lengthOfPiece, integerForStrings(walker, length), origin);
    std::array<Reference, 1> answer;
    bool isBuilt = buildArmsForStrings(walker, isAsLong, BlockFrequency::Normal, [&](std::span<Reference> results) {
        Draft test(beginsAtStart ? Opcode::StringStartsWithLiteral : Opcode::StringEndsWithLiteral, origin);
        test.setStringOperand(piece.string);
        test.setLiteral(text);
        results[0] = walker.build(WTF::move(test));
    }, [&](std::span<Reference> results) {
        results[0] = constantForStrings(walker, PrimitiveKnownToFolding::boolean(false));
    }, answer);
    return isBuilt ? answer[0] : Reference();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
