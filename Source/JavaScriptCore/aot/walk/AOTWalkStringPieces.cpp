/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotRepresentation.h"
#include "AOTWalkKnown.h"
#include "AOTWalkStringFolds.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkResultsInParts.h"
#include <limits>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool boundIsShownBySumsForStringPieces(Walker& walker, Reference lesser, Reference greater, Origin origin)
{
    if (!relationIsShownBySumsForStrings(walker, Opcode::LessEq, lesser, greater))
        return false;
    if (walker.checksProofs()) {
        Reference holds = buildOperatorForStrings(walker, Opcode::LessEq, lesser, greater, origin);
        if (holds && holds.isValue() && !walker.isUnreachable())
            walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::OtherOperation, origin);
    }
    return true;
}

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
            return boundIsShownBySumsForStringPieces(walker, position, limit, origin) ? position : Reference();
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

static bool bodyStoresValueForStringPieces(Walker& walker, const BodyIndex& index, ValueID value)
{
    if (index.useCount(value) < 2)
        return false;
    const BitVector& stored = notesOfStrings(walker).valuesThatBodiesStore.ensure(&index, [&] {
        BitVector found;
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            if (index.isParameter(ValueID(number)))
                continue;
            BodyValue operation = index.value(ValueID(number));
            switch (operation.opcode()) {
            case Opcode::NewObject:
            case Opcode::NewArray:
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    if (operand)
                        found.set(index.stripCasts(operand).index());
                });
                break;
            case Opcode::PropSet:
            case Opcode::PropDefine:
            case Opcode::ElemSet:
                if (ValueID laid = operation.valueOperand())
                    found.set(index.stripCasts(laid).index());
                break;
            default:
                break;
            }
        }
        return found;
    }).iterator->value;
    return stored.get(value.index());
}

static Reference buildPiecePosition(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    Reference result = buildOperatorForStrings(walker, opcode, left, right, origin);
    return buildOperatorForStrings(walker, Opcode::BitAnd, result, integerForStrings(walker, std::numeric_limits<int32_t>::max()), origin);
}

void noteOrderedPieceBounds(Walker& walker, Reference start, Reference end)
{
    start = rootForStrings(walker, start);
    end = rootForStrings(walker, end);
    if (start.isValue() && end.isValue())
        noteKnown(walker, KnownNote::OrderedPieceBounds, start, end, 0, LocationMasks(), 1);
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
    std::optional<PieceOfString> outer = pieceReadInLoop(walker, string, origin);
    Reference length = outer ? buildLengthOfPiece(walker, *outer, origin) : buildLengthOfString(walker, string, origin);
    if (!length)
        return true;
    Reference zero = integerForStrings(walker, 0);
    Reference first = isLeftOut(0) ? zero : call.arguments[0];
    Reference second = isLeftOut(1) ? Reference() : call.arguments[1];
    bool countsFromEnd = method != StringRangeMethod::Substring;

    Reference start = clampedPositionForStrings(walker, first, length, countsFromEnd, origin);
    Reference end = second ? Reference() : length;
    if (second && method != StringRangeMethod::Substr) {
        Reference other = clampedPositionForStrings(walker, second, length, countsFromEnd, origin);
        std::optional<bool> isInOrder = start && other ? relationDecidedForStrings(walker, Opcode::LessEq, start, other) : std::nullopt;
        if (!isInOrder && start && other && boundIsShownBySumsForStringPieces(walker, start, other, origin))
            isInOrder = true;
        if (!isInOrder) {
            if (method == StringRangeMethod::Substring && start != zero)
                start = { };
        } else if (*isInOrder)
            end = other;
        else if (method == StringRangeMethod::Slice) {
            walker.bind(call.call.id(), walker.constant(PrimitiveKnownToFolding::text(walker.program().strings.emptyString())));
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
        if (!start || (!end && start != zero))
            start = buildResultOfRangeForStrings(walker, bounds, startOfStringRange, origin);
        if (!end)
            end = buildResultOfRangeForStrings(walker, bounds, endOfStringRange, origin);
    }

    Reference piece;
    if (start == zero && end == length)
        piece = string;
    else if (rootForStrings(walker, start) == rootForStrings(walker, end))
        piece = walker.constant(PrimitiveKnownToFolding::text(walker.program().strings.emptyString()));
    else {
        if (outer) {
            string = outer->string;
            start = buildPiecePosition(walker, Opcode::Add, outer->start, start, origin);
            end = buildPiecePosition(walker, Opcode::Add, outer->start, end, origin);
            noteOrderedPieceBounds(walker, start, end);
        }
        piece = buildPieceOfString(walker, string, start, end, origin);
        if (piece && piece.isValue() && bodyStoresValueForStringPieces(walker, walker.currentBodyIndex(), call.call.id()))
            noteKnown(walker, KnownNote::PieceIsStoredByBodyThatCutIt, piece, { }, 0, LocationMasks(), 1);
    }
    if (piece)
        walker.bind(call.call.id(), piece);
    return true;
}

std::optional<PieceOfString> pieceInSight(Walker& walker, Reference text, bool alsoWhereNothingMayStand)
{
    Reference root = rootForStrings(walker, text);
    if (!root.isValue() || walker.isUnreachable())
        return std::nullopt;
    const Code& code = walker.code();
    const Operation& record = code.at(root.id());
    bool isMadeOfCodeUnit = record.opcode == Opcode::CallInline && record.inlineBuiltin() == InlineBuiltin::StringFromCharCode && record.argumentsOperands().size() == 1;
    bool isPlaceThatStands = record.opcode == Opcode::SubstringOrUndefined && (alsoWhereNothingMayStand || isProvenString(walker, walker.typeOf(text)));
    if (record.opcode != Opcode::Substring && !isMadeOfCodeUnit && !isPlaceThatStands)
        return std::nullopt;
    BlockID cut = code.blockOf(root.id());
    BlockID here = walker.currentBlock();
    if (cut != here && (code.at(cut).handler || code.at(here).handler))
        return std::nullopt;
    if (!isMadeOfCodeUnit)
        return PieceOfString { referenceToRecordForStrings(walker, record.stringOperand()), referenceToRecordForStrings(walker, record.startOperand()), referenceToRecordForStrings(walker, record.endOperand()) };

    Reference codeUnit = rootForStrings(walker, referenceToRecordForStrings(walker, record.argumentsOperands()[0]));
    if (!codeUnit.isValue() || code.at(codeUnit.id()).opcode != Opcode::GetStringCodeUnit)
        return std::nullopt;
    Origin origin = record.origin();
    Reference string = referenceToRecordForStrings(walker, code.at(codeUnit.id()).stringOperand());
    if (string.isValue() && code.at(string.id()).opcode == Opcode::FlattenString)
        string = referenceToRecordForStrings(walker, code.at(string.id()).stringOperand());
    Reference start = referenceToRecordForStrings(walker, code.at(codeUnit.id()).positionOperand());
    Reference end = buildOperatorForStrings(walker, Opcode::Add, start, integerForStrings(walker, 1), origin);
    if (!end)
        return std::nullopt;
    return PieceOfString { string, start, end };
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
    Reference rootOfStart = rootForStrings(walker, piece.start);
    Reference rootOfEnd = rootForStrings(walker, piece.end);
    if (rootOfStart.isValue() && rootOfEnd.isValue() && findKnown(walker, KnownNote::OrderedPieceBounds, rootOfStart, rootOfEnd))
        return true;
    if (Reference range = rangeGivingBoundForStringPieces(walker, piece.start, startOfStringRange)) {
        if (range == rangeGivingBoundForStringPieces(walker, piece.end, endOfStringRange))
            return true;
        Reference length = referenceToRecordForStrings(walker, walker.code().at(range.id()).lengthOperand());
        if (rootForStrings(walker, length) == rootForStrings(walker, piece.end))
            return true;
    }
    if (endIsSumWithStartForStringPieces(walker, piece))
        return true;
    return relationDecidedForStrings(walker, Opcode::LessEq, piece.start, piece.end).value_or(false) || boundsAreOfOnePart(walker, piece.start, piece.end) || (piece.end.isRecord() && boundIsShownBySumsForStringPieces(walker, piece.start, piece.end, walker.code().at(piece.end.id()).origin()));
}

Reference buildLengthOfPiece(Walker& walker, const PieceOfString& piece, Origin origin)
{
    return buildPiecePosition(walker, Opcode::Sub, piece.end, piece.start, origin);
}

static std::optional<PieceOfString> orderedStringPiece(Walker& walker, Reference text)
{
    Reference root = rootForStrings(walker, text);
    if (!root.isValue() || walker.isUnreachable())
        return std::nullopt;
    Opcode opcode = walker.code().at(root.id()).opcode;
    if (opcode != Opcode::Substring && opcode != Opcode::SubstringOrUndefined)
        return std::nullopt;
    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !walker.types().at(walker.typeOf(piece->start)).isInteger() || !walker.types().at(walker.typeOf(piece->end)).isInteger() || !isRangeOfPieceInOrder(walker, *piece))
        return std::nullopt;
    return piece;
}

std::optional<PieceOfString> pieceReadInLoop(Walker& walker, Reference text, Origin origin)
{
    if (walker.isUnreachable() || !walker.isInLoop() || isOrCanBeFlattenedBeforeLoop(walker, text))
        return std::nullopt;
    std::optional<PieceOfString> piece = orderedStringPiece(walker, text);
    while (piece && !isOrCanBeFlattenedBeforeLoop(walker, piece->string)) {
        std::optional<PieceOfString> outer = orderedStringPiece(walker, piece->string);
        if (!outer)
            break;
        Reference start = buildPiecePosition(walker, Opcode::Add, outer->start, piece->start, origin);
        Reference end = start ? buildPiecePosition(walker, Opcode::Add, outer->start, piece->end, origin) : Reference();
        if (!end)
            break;
        noteOrderedPieceBounds(walker, start, end);
        piece = PieceOfString { outer->string, start, end };
    }
    return piece;
}

bool buildLengthOfPieceInSight(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || value.isFreeName() || value.key() != walker.program().keyOf(BuiltinNames::length) || walker.isUnreachable())
        return false;
    Reference text = walker.referenceOf(value.objectOperand());
    if (!text || !isProvenString(walker, walker.typeOf(text)))
        return false;
    Origin origin = walker.originOf(value);

    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !isRangeOfPieceInOrder(walker, *piece))
        return false;
    Reference length = buildLengthOfPiece(walker, *piece, origin);
    if (length)
        bindForStrings(walker, value.id(), length, origin);
    return true;
}

bool buildKnownPieceTruthiness(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    Reference text = walker.referenceOf(value.operand(0));
    if (!text || !isProvenString(walker, walker.typeOf(text)))
        return false;
    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !isRangeOfPieceInOrder(walker, *piece))
        return false;
    Origin origin = walker.originOf(value);
    Reference length = buildLengthOfPiece(walker, *piece, origin);
    Reference truth = length ? buildOperatorForStrings(walker, Opcode::ToBoolean, length, { }, origin) : Reference();
    if (truth)
        bindForStrings(walker, value.id(), truth, origin);
    return true;
}

static std::optional<PieceOfString> pieceToReadNumberOffForStringPieces(Walker& walker, Reference text, Origin origin)
{
    if (!text || !isProvenString(walker, walker.typeOf(text)))
        return std::nullopt;
    if (std::optional<PieceOfString> piece = pieceReadInLoop(walker, text, origin))
        return piece;
    std::optional<PieceOfString> piece = pieceInSight(walker, text);
    if (!piece || !isRangeOfPieceInOrder(walker, *piece))
        return std::nullopt;
    return piece;
}

bool buildNumberOfPiece(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    Origin origin = walker.originOf(value);
    std::optional<PieceOfString> piece = pieceToReadNumberOffForStringPieces(walker, walker.referenceOf(value.operand(0)), origin);
    if (!piece)
        return false;
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
    std::optional<PieceOfString> piece = pieceToReadNumberOffForStringPieces(walker, call.arguments[0], call.origin);
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

using AnswerWithoutCallForStringPieces = ScopedLambda<Reference()>;

static void buildArmsAroundCallUnderPieceForStringPieces(Walker& walker, const CallByOneRow& call, const PieceOfString* piece, Reference isAnswered, BlockFrequency ofCall, const AnswerWithoutCallForStringPieces& answerWithoutCall)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = call.call.id();
    BlockID block = index.blockOf(written);
    uint32_t place = placeInBlockForStrings(index, written);

    BlockID answering = walker.addBlock();
    BlockID calling = walker.addBlock(ofCall);
    BlockID meeting = walker.addBlock();
    walker.branch(isAnswered, answering, calling);

    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(answering);
    Reference answer = answerWithoutCall();
    if (walker.hasFailed())
        return;
    if (!walker.isUnreachable()) {
        if (!answer)
            return walker.fail("a lookup under a piece of a text has no answer"_s);
        arms.append(Walker::JoinArm { walker.currentBlock(), { answer } });
    }

    walker.continueIn(calling);
    {
        Walker::ClosedArm closedArm(walker);
        bool firstCutStays = piece && findKnown(walker, KnownNote::PieceIsStoredByBodyThatCutIt, rootForStrings(walker, call.arguments[0]));
        Reference key = piece && !firstCutStays ? buildPieceOfString(walker, piece->string, piece->start, piece->end, call.origin) : call.arguments[0];
        if (key && piece && !firstCutStays)
            walker.bind(call.writtenArguments()[0], key);
        if (key)
            walker.walkStretch(block, place, place + 1);
        if (walker.hasFailed())
            return;
        if (!walker.isUnreachable())
            arms.append(Walker::JoinArm { walker.currentBlock(), { walker.referenceOf(written) } });
    }

    std::array<Reference, 1> met;
    if (!arms.isEmpty() && walker.joinArms(arms.span(), meeting, met))
        walker.bind(written, met[0]);
}

static std::optional<PieceOfString> pieceThatIsKeyForStringPieces(Walker& walker, const CallByOneRow& call, size_t numberOfArguments)
{
    if (!walker.opensGuesses() || !walker.has(Mechanism::Folding) || call.isForwarded || !call.receiver || call.arguments.size() != numberOfArguments || call.writtenArguments().size() != numberOfArguments)
        return std::nullopt;
    std::optional<PieceOfString> piece = pieceToReadNumberOffForStringPieces(walker, call.arguments[0], call.origin);
    if (!piece || !walker.types().at(walker.typeOf(piece->start)).isInteger() || !walker.types().at(walker.typeOf(piece->end)).isInteger())
        return std::nullopt;
    return piece;
}

static bool answerByPlaceForStringPieces(Walker& walker, const CallByOneRow& call, const PieceOfString* piece, Reference place)
{
    Reference moves = place ? walker.build(Draft(Opcode::MovesOfEntriesInCollections, call.origin)) : Reference();
    Reference isTold = moves ? buildOperatorForStrings(walker, Opcode::GreaterEq, place, integerForStrings(walker, placeOfAbsentKeyInMap), call.origin) : Reference();
    if (!isTold)
        return false;

    auto readAtPlace = [&] {
        Draft read(Opcode::GetValueAtPlaceInMap, call.origin);
        read.setMapOperand(call.receiver);
        read.setPlaceOperand(place);
        read.expected = call.expected;
        const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), call.call.id());
        TypeID before = walker.exchangeContentsBeingTyped(contents ? contents->value : TypeID());
        Reference value = walker.build(WTF::move(read));
        walker.exchangeContentsBeingTyped(before);
        return value;
    };
    buildArmsAroundCallUnderPieceForStringPieces(walker, call, piece, isTold, BlockFrequency::Rare, AnswerWithoutCallForStringPieces(readAtPlace));
    if (!walker.hasFailed() && !walker.isUnreachable()) {
        noteKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], 0, { }, static_cast<uint64_t>(moves.bits()) << 32 | place.bits(), true);
        walker.count(StatisticID::MapEntryIndicesCached);
    }
    return true;
}

static bool someCallStoresUnderKeyForStringPieces(Walker& walker, const BodyIndex& index, BlockID in, uint32_t from, ValueID key)
{
    auto& calls = notesOfStrings(walker).callsWithTwoArgumentsByFirst.ensure(&index, [&] {
        Vector<std::pair<uint32_t, uint32_t>> found;
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            if (index.isParameter(ValueID(number)) || index.opcodeOf(ValueID(number)) != Opcode::Call)
                continue;
            if (auto passed = index.value(ValueID(number)).as<Opcode::Call>().argumentsOperands(); passed.size() == 2)
                found.append({ index.stripCasts(passed[0]).index(), number });
        }
        std::ranges::sort(found);
        return found;
    }).iterator->value;
    auto next = std::ranges::lower_bound(calls, std::pair { key.index(), from });
    return next != calls.end() && next->first == key.index() && index.blockOf(ValueID(next->second)) == in;
}

static bool keyIsStoredUnderOnEveryWayBehindForStringPieces(Walker& walker, const BodyIndex& index, ValueID call, ValueID key)
{
    constexpr unsigned mostMeetingsLookedBehind = 3;
    BlockID in = index.blockOf(call);
    uint32_t from = call.index() + 1;
    for (unsigned number = 0; number < mostMeetingsLookedBehind; ++number) {
        if (someCallStoresUnderKeyForStringPieces(walker, index, in, from, key))
            return true;
        BlockID meeting;
        bool everyWayStores = true;
        for (BodyTarget target : index.terminator(in).targets()) {
            BlockID next = target.block();
            bool storesOnWay = false;
            if (index.onlyIncomingEdge(next) && index.terminator(next).kind() == TerminatorKind::Jump) {
                storesOnWay = someCallStoresUnderKeyForStringPieces(walker, index, next, index.block(next).firstValue().index(), key);
                next = index.terminator(next).targets()[0].block();
            }
            everyWayStores &= storesOnWay;
            if (meeting && meeting != next)
                return false;
            meeting = next;
        }
        if (!meeting || everyWayStores || index.isLoopHead(meeting))
            return meeting && everyWayStores;
        in = meeting;
        from = index.block(in).firstValue().index();
    }
    return false;
}

static bool buildLookupUnderKeyStoredBehindIt(Walker& walker, const CallByOneRow& call)
{
    if (!walker.isInLoop() || !walker.opensGuesses() || !walker.has(Mechanism::Folding) || call.isForwarded || !call.receiver || call.arguments.size() != 1 || !call.arguments[0] || call.writtenArguments().size() != 1)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID key = index.stripCasts(call.writtenArguments()[0]);
    if (index.useCount(key) < 2 && index.useCount(call.writtenArguments()[0]) < 2)
        return false;
    Reference root = rootForStrings(walker, call.arguments[0]);
    bool mayBeRope = root.isValue() && operationsThatJoinTexts.contains(walker.code().at(root.id()).opcode);
    const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), call.call.id());
    bool mayHoldNumbers = !contents || !contents->value || (walker.types().at(contents->value).primitives & Primitive::Number);
    if (mayBeRope || !mayHoldNumbers || !keyIsStoredUnderOnEveryWayBehindForStringPieces(walker, index, call.call.id(), key))
        return false;
    Draft find(Opcode::FindKeyInMap, call.origin);
    find.setMapOperand(call.receiver);
    find.setKeyOperand(call.arguments[0]);
    return answerByPlaceForStringPieces(walker, call, nullptr, walker.build(WTF::move(find)));
}

static constexpr uint32_t placeStandsWhileStorageIsNotWritten = 1;

static bool someLaterCallGetsUnderKeyForStringPieces(Walker& walker, const BodyIndex& index, ValueID call, ValueID key, bool nameReadAgainGivesSameValue)
{
    constexpr uint32_t isValueItself = UINT32_MAX;
    auto asReadOfName = [&](ValueID value) -> std::optional<std::array<uint32_t, 2>> {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet)
            return std::nullopt;
        auto read = index.value(value).as<Opcode::PropGet>();
        return std::array { index.stripCasts(read.objectOperand()).index(), read.key().index() };
    };
    auto& calls = notesOfStrings(walker).callsThatGetUnderOneKey.ensure(&index, [&] {
        Vector<std::array<uint32_t, 3>> found;
        PropertyKeyID get = walker.program().keyOf(BuiltinNames::get);
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            if (index.isParameter(ValueID(number)) || index.opcodeOf(ValueID(number)) != Opcode::Call)
                continue;
            auto taking = index.value(ValueID(number)).as<Opcode::Call>();
            if (taking.calleeKey() != get || taking.argumentsOperands().size() != 1)
                continue;
            ValueID under = index.stripCasts(taking.argumentsOperands()[0]);
            found.append({ under.index(), isValueItself, number });
            if (auto read = asReadOfName(under))
                found.append({ (*read)[0], (*read)[1], number });
        }
        std::ranges::sort(found);
        return found;
    }).iterator->value;
    auto someCallStandsBehind = [&](uint32_t first, uint32_t second) {
        auto next = std::ranges::lower_bound(calls, std::array { first, second, call.index() + 1 });
        return next != calls.end() && (*next)[0] == first && (*next)[1] == second;
    };
    if (someCallStandsBehind(key.index(), isValueItself))
        return true;
    auto read = nameReadAgainGivesSameValue ? asReadOfName(key) : std::nullopt;
    return read && someCallStandsBehind((*read)[0], (*read)[1]);
}

static bool buildKeyTestArmsForStringPieces(Walker& walker, const CallByOneRow& call, Reference place, Reference isFound)
{
    if (!isFound)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = call.call.id();
    BlockID block = index.blockOf(written);
    uint32_t placeInBlock = placeInBlockForStrings(index, written);
    walker.count(StatisticID::MapEntryIndicesCached);

    BlockID found = walker.addBlock();
    BlockID notFound = walker.addBlock();
    BlockID absent = walker.addBlock();
    BlockID calling = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isFound, found, notFound);
    auto endArm = [&](bool isPlaceNegative) {
        if (walker.hasFailed() || walker.isUnreachable())
            return;
        noteKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], placeStandsWhileStorageIsNotWritten, LocationMasks(AbstractHeapSet { AbstractHeap::CollectionStorage }), place.bits(), true);
        if (isPlaceNegative)
            noteKnown(walker, KnownNote::PlaceOfKeyIsNegative, call.receiver, call.arguments[0], 0, LocationMasks(AbstractHeapSet { AbstractHeap::CollectionStorage }), 1, true);
        if (!walker.threadBranchAfter(call.call))
            walker.fail("an answer of a map does not walk the end of its block"_s);
    };
    auto endArmWith = [&](bool answer) {
        Draft constant(Opcode::Boolean, call.origin);
        constant.setBoolean(answer);
        constant.representation = Representation::JSValue;
        walker.bind(written, walker.constant(WTF::move(constant)));
        endArm(!answer);
    };
    walker.continueIn(notFound);
    Reference isTold = buildOperatorForStrings(walker, Opcode::GreaterEq, place, integerForStrings(walker, placeOfAbsentKeyInMap), call.origin);
    if (!isTold)
        return true;
    walker.branch(isTold, absent, calling);
    walker.continueIn(calling);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block, placeInBlock, placeInBlock + 1);
        endArm(true);
    }
    if (walker.hasFailed())
        return true;
    walker.continueIn(absent);
    endArmWith(false);
    if (walker.hasFailed())
        return true;
    walker.continueIn(found);
    endArmWith(true);
    return true;
}

bool buildTestForKeyLookedUpBehindIt(Walker& walker, const CallByOneRow& call)
{
    if (!walker.isInLoop() || !walker.opensGuesses() || !walker.has(Mechanism::Folding) || call.isForwarded || !call.receiver || call.arguments.size() != 1 || !call.arguments[0] || call.writtenArguments().size() != 1 || call.call.opcode() != Opcode::Call)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    Reference root = rootForStrings(walker, call.arguments[0]);
    bool mayBeRope = root.isValue() && operationsThatJoinTexts.contains(walker.code().at(root.id()).opcode) && (walker.types().at(walker.typeOf(call.arguments[0])).primitives & Primitive::AnyString);
    bool nameReadAgainGivesSameValue = !root.isValue() || !walker.code().at(root.id()).traits().mayCallUnknownCode();
    if (mayBeRope || !someLaterCallGetsUnderKeyForStringPieces(walker, index, call.call.id(), index.stripCasts(call.writtenArguments()[0]), nameReadAgainGivesSameValue))
        return false;
    Draft find(Opcode::FindKeyInMap, call.origin);
    find.setMapOperand(call.receiver);
    find.setKeyOperand(call.arguments[0]);
    Reference place = walker.build(WTF::move(find));
    auto isFound = [&] {
        return buildOperatorForStrings(walker, Opcode::Greater, place, integerForStrings(walker, placeOfAbsentKeyInMap), call.origin);
    };
    if (place && walker.canThreadBranchAfter(call.call))
        return buildKeyTestArmsForStringPieces(walker, call, place, isFound());
    Reference isTold = place ? buildOperatorForStrings(walker, Opcode::GreaterEq, place, integerForStrings(walker, placeOfAbsentKeyInMap), call.origin) : Reference();
    if (!isTold)
        return false;
    buildArmsAroundCallUnderPieceForStringPieces(walker, call, nullptr, isTold, BlockFrequency::Rare, AnswerWithoutCallForStringPieces(isFound));
    if (!walker.hasFailed() && !walker.isUnreachable()) {
        noteKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], placeStandsWhileStorageIsNotWritten, LocationMasks(AbstractHeapSet { AbstractHeap::CollectionStorage }), place.bits() | static_cast<uint64_t>(walker.referenceOf(call.call.id()).bits()) << 32, true);
        walker.count(StatisticID::MapEntryIndicesCached);
    }
    return true;
}

static void holdPlaceKeptAgainstFreshSearchForStringPieces(Walker&, Reference kept, Reference foundAgain, Origin);

static bool buildLookupAtPlaceThatTestFound(Walker& walker, const CallByOneRow& call)
{
    if (!walker.opensGuesses() || call.isForwarded || !call.receiver || call.arguments.size() != 1 || !call.arguments[0] || call.writtenArguments().size() != 1)
        return false;
    std::optional<uint64_t> found = findKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], placeStandsWhileStorageIsNotWritten);
    if (!found)
        return false;
    Reference place = Reference::fromBits(static_cast<uint32_t>(*found));
    Reference isFound = buildOperatorForStrings(walker, Opcode::Greater, place, integerForStrings(walker, placeOfAbsentKeyInMap), call.origin);
    if (!isFound)
        return false;
    Reference foundAgain;
    if (walker.checksProofs()) {
        Draft find(Opcode::FindKeyInMap, call.origin);
        find.setMapOperand(call.receiver);
        find.setKeyOperand(call.arguments[0]);
        foundAgain = walker.buildTestOfCheck(WTF::move(find));
    }
    auto readAtPlace = [&] {
        if (foundAgain)
            holdPlaceKeptAgainstFreshSearchForStringPieces(walker, place, foundAgain, call.origin);
        Draft read(Opcode::GetValueAtPlaceInMap, call.origin);
        read.setMapOperand(call.receiver);
        read.setPlaceOperand(place);
        read.expected = call.expected;
        const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), call.call.id());
        TypeID before = walker.exchangeContentsBeingTyped(contents ? contents->value : TypeID());
        Reference value = walker.build(WTF::move(read));
        walker.exchangeContentsBeingTyped(before);
        return value;
    };
    buildArmsAroundCallUnderPieceForStringPieces(walker, call, nullptr, isFound, BlockFrequency::Rare, AnswerWithoutCallForStringPieces(readAtPlace));
    return true;
}

bool buildLookupOfPieceInMap(Walker& walker, const CallByOneRow& call)
{
    if (buildLookupAtPlaceThatTestFound(walker, call))
        return true;
    std::optional<PieceOfString> piece = pieceThatIsKeyForStringPieces(walker, call, 1);
    if (!piece)
        return buildLookupUnderKeyStoredBehindIt(walker, call);

    Draft find(Opcode::FindStringRangeInMap, call.origin);
    find.setMapOperand(call.receiver);
    Reference flat = flatStringForReaderOfCharacters(walker, piece->string, call.origin);
    find.setStringOperand(flat ? flat : piece->string);
    find.setStartOperand(piece->start);
    find.setEndOperand(piece->end);
    return answerByPlaceForStringPieces(walker, call, &*piece, walker.build(WTF::move(find)));
}

static void holdPlaceKeptAgainstFreshSearchForStringPieces(Walker& walker, Reference kept, Reference foundAgain, Origin origin)
{
    Reference isSame = walker.isUnreachable() ? Reference() : buildOperatorForStrings(walker, Opcode::StrictEq, foundAgain, kept, origin);
    if (!isSame || !isSame.isValue())
        return;
    BlockID whereSame = walker.addBlock();
    BlockID whereOther = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isSame, whereSame, whereOther);
    walker.continueIn(whereOther);
    Reference isNotTold = buildOperatorForStrings(walker, Opcode::Less, foundAgain, integerForStrings(walker, placeOfAbsentKeyInMap), origin);
    if (isNotTold && !walker.isUnreachable())
        walker.checkProof(isNotTold, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    if (!walker.isUnreachable())
        walker.jumpTo(whereSame);
    walker.continueIn(whereSame);
}

static bool buildStoreWhereTestFoundNoKeyForStringPieces(Walker& walker, const CallByOneRow& call)
{
    if (!walker.opensGuesses() || call.isForwarded || !call.receiver || call.arguments.size() != 2 || !call.arguments[0] || call.writtenArguments().size() != 2)
        return false;
    std::optional<uint64_t> kept = findKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], placeStandsWhileStorageIsNotWritten);
    Reference answerOfTest = kept && *kept >> 32 ? Reference::fromBits(static_cast<uint32_t>(*kept >> 32)) : Reference();
    bool isPlaceNegative = kept && (answerOfTest ? outcomeKnownOf(walker, answerOfTest) == std::optional<bool>(false) : !!findKnown(walker, KnownNote::PlaceOfKeyIsNegative, call.receiver, call.arguments[0]));
    if (!isPlaceNegative)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = call.call.id();
    uint32_t place = placeInBlockForStrings(index, written);
    Reference stored;
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(index.blockOf(written), place, place + 1);
        if (!walker.hasFailed() && !walker.isUnreachable())
            stored = walker.referenceOf(written);
    }
    if (!stored)
        return true;
    walker.bind(written, stored);
    noteKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0], placeStandsWhileStorageIsNotWritten, LocationMasks(AbstractHeapSet { AbstractHeap::CollectionStorage }), *kept, true);
    walker.count(StatisticID::MapEntryIndicesSurvivingInserts);
    return true;
}

bool buildStoreUnderPieceInMap(Walker& walker, const CallByOneRow& call)
{
    if (buildStoreWhereTestFoundNoKeyForStringPieces(walker, call))
        return true;
    std::optional<PieceOfString> piece = pieceThatIsKeyForStringPieces(walker, call, 2);
    bool mayGoByPlace = piece || (walker.opensGuesses() && !call.isForwarded && call.receiver && call.arguments.size() == 2 && call.arguments[0] && call.writtenArguments().size() == 2);
    std::optional<uint64_t> found = mayGoByPlace ? findKnown(walker, KnownNote::PlaceOfKeyInMap, call.receiver, call.arguments[0]) : std::nullopt;
    if (!found)
        return false;

    Reference foundAgain;
    if (walker.checksProofs()) {
        Draft find(piece ? Opcode::FindStringRangeInMap : Opcode::FindKeyInMap, call.origin);
        find.setMapOperand(call.receiver);
        if (piece) {
            Reference flat = flatStringInSight(walker, piece->string);
            find.setStringOperand(flat ? flat : piece->string);
            find.setStartOperand(piece->start);
            find.setEndOperand(piece->end);
        } else
            find.setKeyOperand(call.arguments[0]);
        foundAgain = walker.buildTestOfCheck(WTF::move(find));
    }
    Draft store(Opcode::TryPutValueAtPlaceInMap, call.origin);
    store.setMapOperand(call.receiver);
    store.setPlaceOperand(Reference::fromBits(static_cast<uint32_t>(*found)));
    store.setMovesOperand(Reference::fromBits(static_cast<uint32_t>(*found >> 32)));
    store.setValueOperand(call.arguments[1]);
    Reference isStored = walker.build(WTF::move(store));
    if (!isStored)
        return false;
    auto giveMap = [&] {
        if (foundAgain)
            holdPlaceKeptAgainstFreshSearchForStringPieces(walker, Reference::fromBits(static_cast<uint32_t>(*found)), foundAgain, call.origin);
        return call.receiver;
    };
    buildArmsAroundCallUnderPieceForStringPieces(walker, call, piece ? &*piece : nullptr, isStored, BlockFrequency::Normal, AnswerWithoutCallForStringPieces(giveMap));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
