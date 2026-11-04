/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: what is read off a range of a string in the place of a piece that is then never cut: a comparison with a written text, a number, a parsed integer.
// This file alone names StringRangeEqualsLiteral, StringRangeToNumber and StringRangeParseInt, and it names them through a parameter: while their rows are out of the schema it builds nothing.
// The three keep nothing and throw only for lack of memory, as the cut does in whose place they stand. Withheld: their callers are not asked.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ReadOfRangeInWalkStrings : uint8_t { EqualsLiteral, ToNumber, ParseInt };

} // anonymous namespace

template<typename Opcodes, typename DraftOfRead>
static Reference buildReadOfRangeForStringRanges(Walker& walker, ReadOfRangeInWalkStrings read, const PieceOfString& piece, StringID text, Reference radixOrNull, Origin origin)
{
    if constexpr (requires { Opcodes::StringRangeEqualsLiteral; Opcodes::StringRangeToNumber; Opcodes::StringRangeParseInt; }) {
        Opcodes opcode = read == ReadOfRangeInWalkStrings::EqualsLiteral ? Opcodes::StringRangeEqualsLiteral : read == ReadOfRangeInWalkStrings::ToNumber ? Opcodes::StringRangeToNumber : Opcodes::StringRangeParseInt;
        DraftOfRead draft(opcode, origin);
        draft.setStringOperand(piece.string);
        draft.setStartOperand(piece.start);
        draft.setEndOperand(piece.end);
        if (read == ReadOfRangeInWalkStrings::EqualsLiteral)
            draft.setLiteral(text);
        if (radixOrNull)
            draft.setRadixOperand(radixOrNull);
        return walker.build(WTF::move(draft));
    } else
        return { };
}

static bool mayReadRangeForStringRanges(const Walker& walker, const PieceOfString& piece)
{
    return !walker.isUnreachable() && walker.types().at(walker.typeOf(piece.start)).isInteger() && walker.types().at(walker.typeOf(piece.end)).isInteger();
}

Reference buildComparisonOfRangeWithText(Walker& walker, const PieceOfString& piece, StringID text, Origin origin)
{
    unsigned length = lengthOfTextForStrings(walker, text);
    if (!mayReadRangeForStringRanges(walker, piece) || length > maxLengthOfTextComparedInPlace)
        return { };
    if (!length)
        return buildOperatorForStrings(walker, Opcode::StrictEq, piece.start, piece.end, origin);

    // Bounds that are known lie as far apart as the text is long, or the two differ.
    Type start = walker.types().at(walker.typeOf(piece.start));
    Type end = walker.types().at(walker.typeOf(piece.end));
    if (start.integers.isSingleValue() && end.integers.isSingleValue() && end.integers.minimum - start.integers.minimum != static_cast<int64_t>(length))
        return constantForStrings(walker, PrimitiveKnownToFolding::boolean(false));
    return buildReadOfRangeForStringRanges<Opcode, Draft>(walker, ReadOfRangeInWalkStrings::EqualsLiteral, piece, text, { }, origin);
}

Reference buildNumberOfRange(Walker& walker, const PieceOfString& piece, Origin origin)
{
    if (!mayReadRangeForStringRanges(walker, piece))
        return { };
    return buildReadOfRangeForStringRanges<Opcode, Draft>(walker, ReadOfRangeInWalkStrings::ToNumber, piece, { }, { }, origin);
}

Reference buildParsedIntegerOfRange(Walker& walker, const PieceOfString& piece, Reference radixOrNull, Origin origin)
{
    if (!mayReadRangeForStringRanges(walker, piece))
        return { };
    return buildReadOfRangeForStringRanges<Opcode, Draft>(walker, ReadOfRangeInWalkStrings::ParseInt, piece, { }, radixOrNull, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
