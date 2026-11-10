/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTRepresentationRules.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "YarrFlags.h"
#include <array>
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OptionSet<RegExpFlags> flagsOfRegExpsThatSearchForNoText { RegExpFlags::Global, RegExpFlags::Sticky, RegExpFlags::IgnoreCase, RegExpFlags::Unicode, RegExpFlags::UnicodeSets };

static Reference booleanForStringSearches(Walker& walker, bool value)
{
    return walker.constant(PrimitiveKnownToFolding::boolean(value));
}

static Reference stringReadByStringSearches(Walker& walker, Reference string, Origin origin)
{
    Reference flat = flatStringForReaderOfCharacters(walker, string, origin);
    return flat ? flat : string;
}

static Reference buildTestOfWrittenEndForStringSearches(Walker& walker, Reference string, StringID text, bool testsStart, Origin origin)
{
    Draft test(testsStart ? Opcode::StringStartsWithLiteral : Opcode::StringEndsWithLiteral, origin);
    test.setStringOperand(stringReadByStringSearches(walker, string, origin));
    test.setLiteral(text);
    return walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
}

static bool buildTestOfJoinedTextsForStringSearches(Walker& walker, const CallByOneRow& call, bool testsStart)
{
    if (call.call.opcode() != Opcode::Call || call.writtenArguments().empty())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = call.writtenArguments()[0];
    Reference joined = rootForStrings(walker, call.arguments[0]);
    if (!joined.isValue() || !index.hasOneUse(written) || rootForStrings(walker, walker.referenceBoundTo(written)) != joined)
        return false;
    const Operation& record = walker.code().at(joined.id());
    if (!operationsThatJoinTexts.contains(record.opcode) || record.numberOfOperands() != 2 || !walker.opensGuesses())
        return false;
    if (!walker.isInLoop() && !record.traits().isRemovableWhenUnused())
        return false;

    Reference atEnd = referenceToRecordForStrings(walker, record.operand(testsStart ? 0 : 1));
    Reference beside = referenceToRecordForStrings(walker, record.operand(testsStart ? 1 : 0));
    StringID textBeside = textKnownForStrings(walker, beside);
    std::optional<char16_t> unit = textBeside ? onlyCodeUnitOfText(walker, textBeside) : std::nullopt;
    if (!unit || !hasTextByItself(walker.types().at(walker.typeOf(atEnd))) || textKnownForStrings(walker, atEnd))
        return false;
    if (!isProvenString(walker, walker.typeOf(atEnd)))
        atEnd = buildOperatorForStrings(walker, Opcode::ToString, atEnd, { }, call.origin);
    if (!atEnd)
        return true;

    walker.bind(call.call.id(), { });
    bool isCalled;
    {
        Walker::StandIn partInPlaceOfWholeText(walker, written, atEnd);
        isCalled = callBuiltin(walker, call.call);
    }
    if (!isCalled)
        return false;
    Reference holdsOfPart = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.call.id());
    if (!holdsOfPart)
        return true;

    Reference lengthOfPart = buildLengthOfString(walker, atEnd, call.origin);
    walker.bind(call.call.id(), buildTestOfCodeUnitBesidePartOfJoin(walker, call.receiver, lengthOfPart, holdsOfPart, *unit, testsStart, call.origin));
    return true;
}

bool buildTestOfEndOfString(Walker& walker, const CallByOneRow& call)
{
    if (!call.receiver || call.arguments.empty() || call.arguments.size() > 2 || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;
    if (call.arguments.size() == 2 && primitiveKnownForStrings(walker, call.arguments[1]).kind() != PrimitiveKnownToFolding::Kind::Undefined)
        return false;

    bool testsStart = *call.codeInLine == InlineBuiltin::StringStartsWith;
    StringID sought = textKnownForStrings(walker, call.arguments[0]);
    if (!sought)
        return buildTestOfJoinedTextsForStringSearches(walker, call, testsStart);

    unsigned length = lengthOfTextForStrings(walker, sought);
    if (length > maxLengthOfTextComparedInPlace)
        return false;
    Reference answer = length ? buildTestOfWrittenEndForStringSearches(walker, call.receiver, sought, testsStart, call.origin) : booleanForStringSearches(walker, true);
    if (answer)
        walker.bind(call.call.id(), answer);
    return true;
}

static Reference buildStartOfSearchForStringSearches(Walker& walker, Reference string, const std::optional<PieceOfString>& piece, Reference position, Origin origin)
{
    if (position == integerForStrings(walker, 0))
        return position;
    Reference length = piece ? buildLengthOfPiece(walker, *piece, origin) : buildLengthOfString(walker, string, origin);
    if (!length)
        return { };
    if (Reference clamped = clampedPositionForStrings(walker, position, length, false, origin); clamped && walker.types().at(walker.typeOf(clamped)).isInt32())
        return clamped;
    if (walker.isUnreachable())
        return { };
    Draft range(Opcode::StringRange, origin);
    range.setLengthOperand(length);
    range.setFirstOperand(position);
    range.setMethod(StringRangeMethod::Substring);
    return buildResultOfRangeForStrings(walker, walker.build(WTF::move(range)), startOfStringRange, origin);
}

static std::optional<PieceOfString> pieceToSearchForStringSearches(Walker& walker, Reference string, StringID text, Origin origin)
{
    Vector<char16_t> buffer;
    StringView view = walker.types().viewOf(text, buffer);
    if (!view.is8Bit() && !WTF::charactersAreAllLatin1(view.span16()))
        return std::nullopt;
    return pieceReadInLoop(walker, string, origin);
}

static Reference buildSearchForWrittenTextForStringSearches(Walker& walker, Reference string, const std::optional<PieceOfString>& piece, StringID text, Reference position, bool searchesFromEnd, Origin origin)
{
    if (walker.isUnreachable() || !position)
        return { };
    std::optional<char16_t> unit = searchesFromEnd ? std::nullopt : onlyCodeUnitOfText(walker, text);
    Draft search(searchesFromEnd ? Opcode::StringLastIndexOfLiteral : unit ? Opcode::StringIndexOfCharacter : Opcode::StringIndexOfLiteral, origin);
    search.setStringOperand(stringReadByStringSearches(walker, piece ? piece->string : string, origin));
    search.setPositionOperand(position);
    if (piece) {
        search.setStartOperand(piece->start);
        search.setEndOperand(piece->end);
    }
    if (unit)
        search.setCodeUnit(*unit);
    else
        search.setLiteral(text);
    return walker.isUnreachable() ? Reference() : walker.build(WTF::move(search));
}

bool buildSearchInString(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.empty() || call.arguments.size() > 2 || !call.receiver || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;
    StringID sought = textKnownForStrings(walker, call.arguments[0]);
    bool isWritten = !!sought;
    if (!isWritten)
        return false;

    Origin origin = call.origin;
    bool searchesFromEnd = *call.codeInLine == InlineBuiltin::StringLastIndexOf;
    bool asksWhetherFound = *call.codeInLine == InlineBuiltin::StringIncludes;
    bool hasPosition = call.arguments.size() == 2 && primitiveKnownForStrings(walker, call.arguments[1]).kind() != PrimitiveKnownToFolding::Kind::Undefined;

    if (!lengthOfTextForStrings(walker, sought)) {
        if (hasPosition)
            return false;
        Reference answer = asksWhetherFound ? booleanForStringSearches(walker, true) : searchesFromEnd ? buildLengthOfString(walker, call.receiver, origin) : integerForStrings(walker, 0);
        if (answer)
            bindForStrings(walker, call.call.id(), answer, origin);
        return true;
    }

    Reference position = integerForStrings(walker, searchesFromEnd ? std::numeric_limits<int32_t>::max() : 0);
    if (hasPosition) {
        Type type = walker.types().at(walker.typeOf(call.arguments[1]));
        if (searchesFromEnd ? !type.isInt32() : !type.isNumber())
            return false;
    }
    std::optional<PieceOfString> piece = pieceToSearchForStringSearches(walker, call.receiver, sought, origin);
    if (hasPosition)
        position = searchesFromEnd ? call.arguments[1] : buildStartOfSearchForStringSearches(walker, call.receiver, piece, call.arguments[1], origin);

    Reference found = buildSearchForWrittenTextForStringSearches(walker, call.receiver, piece, sought, position, searchesFromEnd, origin);
    if (found && asksWhetherFound)
        found = buildNegationForStrings(walker, buildOperatorForStrings(walker, Opcode::StrictEq, found, integerForStrings(walker, -1), origin), origin);
    if (found)
        bindForStrings(walker, call.call.id(), found, origin);
    return true;
}

Reference buildSearchForAtomOfRegExp(Walker& walker, const BodyValue&, BuiltinOverloadID overload, const RegExpWrittenForCall& regExp, Reference string, Origin origin)
{
    if (walker.facts().mayReadLegacyRegExpStatics())
        return { };
    if (regExp.flags.containsAny(flagsOfRegExpsThatSearchForNoText))
        return { };
    std::optional<InlineBuiltin> codeInLine = walker.program().builtins().at(overload).code.implementation.inlineBuiltin;
    if (!codeInLine || (*codeInLine != InlineBuiltin::RegExpTest && *codeInLine != InlineBuiltin::StringSearch))
        return { };
    if (!string || !isProvenString(walker, walker.typeOf(string)))
        return { };

    RegExpSummaries* summaries = walker.regExpSummaries();
    if (!summaries)
        return { };
    Vector<char16_t> buffer;
    StringView pattern = walker.types().viewOf(regExp.pattern, buffer);
    const RegExpSummary& summary = summaries->summaryOf(regExp.pattern, toYarrFlags(regExp.flags), pattern);
    if (!summary.canBeCreated() || !summary.isValid() || summary.atom.isEmpty())
        return { };

    StringID text = pattern == StringView(summary.atom) ? regExp.pattern : walker.types().internText(encodeProgramString(StringView(summary.atom)).span());
    std::optional<PieceOfString> piece = pieceToSearchForStringSearches(walker, string, text, origin);
    Reference found = buildSearchForWrittenTextForStringSearches(walker, string, piece, text, integerForStrings(walker, 0), false, origin);
    if (!found)
        return { };
    if (*codeInLine == InlineBuiltin::StringSearch)
        return boxNumberIfNeeded(walker, found, origin);
    return buildOperatorForStrings(walker, Opcode::GreaterEq, found, integerForStrings(walker, 0), origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
