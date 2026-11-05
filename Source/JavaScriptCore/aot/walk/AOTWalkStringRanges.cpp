/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ReadOfRangeInWalkStrings : uint8_t { EqualsLiteral, ToNumber, ParseInt };

} // anonymous namespace

static Reference buildReadOfRangeForStringRanges(Walker& walker, ReadOfRangeInWalkStrings read, const PieceOfString& piece, StringID text, Reference radixOrNull, Origin origin)
{
    Opcode opcode = read == ReadOfRangeInWalkStrings::EqualsLiteral ? Opcode::StringRangeEqualsLiteral : read == ReadOfRangeInWalkStrings::ToNumber ? Opcode::StringRangeToNumber : Opcode::StringRangeParseInt;
    Draft draft(opcode, origin);
    draft.setStringOperand(piece.string);
    draft.setStartOperand(piece.start);
    draft.setEndOperand(piece.end);
    if (read == ReadOfRangeInWalkStrings::EqualsLiteral)
        draft.setLiteral(text);
    if (radixOrNull)
        draft.setRadixOperand(radixOrNull);
    return walker.build(WTF::move(draft));
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

    Type start = walker.types().at(walker.typeOf(piece.start));
    Type end = walker.types().at(walker.typeOf(piece.end));
    if (start.integers.isSingleValue() && end.integers.isSingleValue() && end.integers.minimum - start.integers.minimum != static_cast<int64_t>(length))
        return walker.constant(PrimitiveKnownToFolding::boolean(false));
    return buildReadOfRangeForStringRanges(walker, ReadOfRangeInWalkStrings::EqualsLiteral, piece, text, { }, origin);
}

Reference buildNumberOfRange(Walker& walker, const PieceOfString& piece, Origin origin)
{
    if (!mayReadRangeForStringRanges(walker, piece))
        return { };
    return buildReadOfRangeForStringRanges(walker, ReadOfRangeInWalkStrings::ToNumber, piece, { }, { }, origin);
}

Reference buildParsedIntegerOfRange(Walker& walker, const PieceOfString& piece, Reference radixOrNull, Origin origin)
{
    if (!mayReadRangeForStringRanges(walker, piece))
        return { };
    return buildReadOfRangeForStringRanges(walker, ReadOfRangeInWalkStrings::ParseInt, piece, { }, radixOrNull, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
