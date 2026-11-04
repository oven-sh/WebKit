/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: slice, substring and substr as the two bounds and one Substring, and what a reader learns of a piece by the record that cut it.
// A bound that the ranges decide is the position itself, the length or 0. The others come out of one StringRange, which clamps as the method does and writes nothing.
// A piece is looked at through its record alone, so neither a suspension nor a throw ends anything. Withheld (BuiltinCalls): the call goes by its row, and no piece is in sight.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <limits>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// ---- The bounds

Reference clampedPositionForStrings(Walker& walker, Reference position, Reference limit, bool countsFromEnd, Origin origin)
{
    constexpr double beyondEveryLength = 2147483648.0;
    Reference zero = integerForStrings(walker, 0);
    Type type = walker.types().at(walker.typeOf(position));
    if (!type.isInteger()) {
        PrimitiveKnownToFolding known = primitiveKnownForStrings(walker, position);
        if (known.kind() != PrimitiveKnownToFolding::Kind::Number)
            return { };
        if (known.asNumber() >= beyondEveryLength)
            return limit;
        return known.asNumber() <= -beyondEveryLength ? zero : Reference();
    }

    if (type.integers.minimum >= 0) {
        std::optional<bool> isInside = relationDecidedForStrings(walker, Opcode::LessEq, position, limit);
        if (!isInside)
            return { };
        return *isInside ? position : limit;
    }
    if (type.integers.maximum >= 0)
        return { };
    if (!countsFromEnd)
        return zero;

    Type typeOfLimit = walker.types().at(walker.typeOf(limit));
    if (!typeOfLimit.isInteger())
        return { };
    if (typeOfLimit.integers.maximum + type.integers.maximum < 0)
        return zero;
    if (typeOfLimit.integers.minimum + type.integers.minimum < 0)
        return { };
    return buildOperatorForStrings(walker, Opcode::Add, limit, position, origin);
}

bool buildPieceByMethod(Walker& walker, const CallByOneRow& call)
{
    if (!call.receiver || call.arguments.size() > 2 || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;

    StringRangeMethod method;
    switch (*call.codeInLine) {
    case InlineBuiltin::StringSlice:
        method = StringRangeMethod::Slice;
        break;
    case InlineBuiltin::StringSubstring:
        method = StringRangeMethod::Substring;
        break;
    case InlineBuiltin::StringSubstr:
        method = StringRangeMethod::Substr;
        break;
    default:
        return false;
    }

    auto isLeftOut = [&](size_t index) {
        return index >= call.arguments.size() || primitiveKnownForStrings(walker, call.arguments[index]).kind() == PrimitiveKnownToFolding::Kind::Undefined;
    };
    for (size_t index = 0; index < call.arguments.size(); ++index) {
        if (!isLeftOut(index) && !walker.types().at(walker.typeOf(call.arguments[index])).isNumber())
            return false;
    }

    Origin origin = call.origin;
    Reference string = call.receiver;
    Reference length = buildLengthOfString(walker, string, origin);
    if (!length)
        return true;
    Reference zero = integerForStrings(walker, 0);
    Reference first = isLeftOut(0) ? zero : call.arguments[0];
    Reference second = isLeftOut(1) ? Reference() : call.arguments[1];
    // slice() and substr() count a negative position from the end. substring() takes it for zero and puts the smaller position first.
    bool countsFromEnd = method != StringRangeMethod::Substring;

    Reference start = clampedPositionForStrings(walker, first, length, countsFromEnd, origin);
    Reference end = second ? Reference() : length;
    if (second && method != StringRangeMethod::Substr) {
        Reference other = clampedPositionForStrings(walker, second, length, countsFromEnd, origin);
        std::optional<bool> isInOrder = start && other ? relationDecidedForStrings(walker, Opcode::LessEq, start, other) : std::nullopt;
        if (!isInOrder) {
            if (method == StringRangeMethod::Substring && start != zero)
                start = { };
        } else if (*isInOrder)
            end = other;
        else if (method == StringRangeMethod::Slice) {
            walker.bind(call.call.id(), constantForStrings(walker, PrimitiveKnownToFolding::text(walker.program().strings.emptyString())));
            return true;
        } else
            end = std::exchange(start, other);
    } else if (second && start) {
        Reference room = buildOperatorForStrings(walker, Opcode::Sub, length, start, origin);
        Reference count = room ? clampedPositionForStrings(walker, second, room, false, origin) : Reference();
        if (count)
            end = buildOperatorForStrings(walker, Opcode::Add, start, count, origin);
    }
    if (walker.isUnreachable())
        return true;

    if (!start || !end) {
        Draft range(Opcode::StringRange, origin);
        range.setLengthOperand(length);
        range.setFirstOperand(first);
        if (second)
            range.setSecondOperand(second);
        range.setMethod(method);
        Reference bounds = walker.build(WTF::move(range));
        if (!bounds)
            return true;
        // Both bounds of one range are in order. Beside 0 every end is.
        if (!start || (!end && start != zero))
            start = buildResultOfRangeForStrings(walker, bounds, startOfStringRange, origin);
        if (!end)
            end = buildResultOfRangeForStrings(walker, bounds, endOfStringRange, origin);
    }

    Reference piece;
    if (start == zero && end == length)
        piece = string;
    else if (rootForStrings(walker, start) == rootForStrings(walker, end))
        piece = constantForStrings(walker, PrimitiveKnownToFolding::text(walker.program().strings.emptyString()));
    else
        piece = buildPieceOfString(walker, string, start, end, origin);
    if (piece)
        walker.bind(call.call.id(), piece);
    return true;
}

// ---- A piece in sight

std::optional<PieceOfString> pieceInSight(Walker& walker, Reference text)
{
    Reference root = rootForStrings(walker, text);
    if (!root.isValue() || walker.isUnreachable())
        return std::nullopt;
    const Code& code = walker.code();
    const Operation& record = code.at(root.id());
    if (record.opcode != Opcode::Substring)
        return std::nullopt;
    // The cut may run out of memory, and so may the read of characters that stands in its place: under the handler of the cut, or under none.
    BlockID cut = code.blockOf(root.id());
    BlockID here = walker.currentBlock();
    if (cut != here && (code.at(cut).handler || code.at(here).handler))
        return std::nullopt;
    return PieceOfString { referenceToRecordForStrings(walker, record.stringOperand()), referenceToRecordForStrings(walker, record.startOperand()), referenceToRecordForStrings(walker, record.endOperand()) };
}

static Reference rangeGivingBoundForStringPieces(const Walker& walker, Reference bound, uint32_t index)
{
    const Code& code = walker.code();
    Reference root = rootForStrings(walker, bound);
    if (!root.isValue() || code.at(root.id()).opcode != Opcode::ResultAt || code.at(root.id()).index() != index)
        return { };
    Reference results = referenceToRecordForStrings(walker, code.at(root.id()).resultsOperand());
    return results.isValue() && code.at(results.id()).opcode == Opcode::StringRange ? results : Reference();
}

// `s.substr(a, 3)`, a character: the end is the start and something that is not negative.
static bool endIsSumWithStartForStringPieces(const Walker& walker, const PieceOfString& piece)
{
    const Code& code = walker.code();
    Reference end = rootForStrings(walker, piece.end);
    if (!end.isValue() || code.at(end.id()).opcode != Opcode::Add)
        return false;
    Reference start = rootForStrings(walker, piece.start);
    for (uint32_t side = 0; side < 2; ++side) {
        Reference added = referenceToRecordForStrings(walker, code.at(end.id()).operand(1 - side));
        Type type = walker.types().at(walker.typeOf(added));
        if (rootForStrings(walker, referenceToRecordForStrings(walker, code.at(end.id()).operand(side))) == start && type.isInteger() && type.integers.minimum >= 0)
            return true;
    }
    return false;
}

bool isRangeOfPieceInOrder(Walker& walker, const PieceOfString& piece)
{
    if (Reference range = rangeGivingBoundForStringPieces(walker, piece.start, startOfStringRange)) {
        if (range == rangeGivingBoundForStringPieces(walker, piece.end, endOfStringRange))
            return true;
        Reference length = referenceToRecordForStrings(walker, walker.code().at(range.id()).lengthOperand());
        if (rootForStrings(walker, length) == rootForStrings(walker, piece.end))
            return true;
    }
    if (endIsSumWithStartForStringPieces(walker, piece))
        return true;
    return relationDecidedForStrings(walker, Opcode::LessEq, piece.start, piece.end).value_or(false);
}

// ---- What is read off a piece that need not be cut

bool buildLengthOfPieceInSight(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || value.isFreeName() || value.key() != walker.program().keyOf(BuiltinNames::length) || walker.isUnreachable())
        return false;
    Reference text = walker.referenceOf(value.objectOperand());
    if (!text || !isProvenString(walker, walker.typeOf(text)))
        return false;
    Origin origin = walker.originOf(value);

    // `for (const ch of text) ch.length`: the step has counted.
    Reference root = rootForStrings(walker, text);
    if (root.isValue()) {
        auto& stepped = notesOfStrings(walker).codePointsOfSteppedPieces;
        if (auto noted = stepped.find(root.id().index()); noted != stepped.end()) {
            bindForStrings(walker, value.id(), noted->value.numberOfCodeUnits, origin);
            return true;
        }
    }

    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !isRangeOfPieceInOrder(walker, *piece))
        return false;
    // The mask changes nothing. It says what the ranges of the bounds do not: no length is negative.
    Reference difference = buildOperatorForStrings(walker, Opcode::Sub, piece->end, piece->start, origin);
    Reference length = buildOperatorForStrings(walker, Opcode::BitAnd, difference, integerForStrings(walker, std::numeric_limits<int32_t>::max()), origin);
    if (length)
        bindForStrings(walker, value.id(), length, origin);
    return true;
}

// `+s.slice(2)`, `Number(s.substring(a, b))`, `parseInt(s.slice(a, b), 10)`: the characters are read where they lie.
static std::optional<PieceOfString> pieceToReadNumberOffForStringPieces(Walker& walker, Reference text)
{
    if (!text || !isProvenString(walker, walker.typeOf(text)))
        return std::nullopt;
    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !isRangeOfPieceInOrder(walker, *piece))
        return std::nullopt;
    return piece;
}

bool buildNumberOfPiece(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    std::optional<PieceOfString> piece = pieceToReadNumberOffForStringPieces(walker, walker.referenceOf(value.operand(0)));
    if (!piece)
        return false;
    Origin origin = walker.originOf(value);
    Reference number = buildNumberOfRange(walker, *piece, origin);
    if (!number)
        return false;
    bindForStrings(walker, value.id(), number, origin);
    return true;
}

bool buildNumberOfPieceByCall(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.empty() || call.arguments.size() > 2)
        return false;
    std::optional<PieceOfString> piece = pieceToReadNumberOffForStringPieces(walker, call.arguments[0]);
    if (!piece)
        return false;

    Reference number;
    if (*call.codeInLine == InlineBuiltin::CallNumberConstructor) {
        if (call.arguments.size() != 1)
            return false;
        number = buildNumberOfRange(walker, *piece, call.origin);
    } else {
        Reference radix;
        if (call.arguments.size() == 2 && primitiveKnownForStrings(walker, call.arguments[1]).kind() != PrimitiveKnownToFolding::Kind::Undefined) {
            radix = call.arguments[1];
            if (!walker.types().at(walker.typeOf(radix)).isInt32())
                return false;
        }
        number = buildParsedIntegerOfRange(walker, *piece, radix, call.origin);
    }
    if (!number)
        return false;
    bindForStrings(walker, call.call.id(), number, call.origin);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
