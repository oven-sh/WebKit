/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkResultsInParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTOpcodeSets.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MentionsOfResultInParts {
    uint32_t numberOfParts() const { return std::max<uint32_t>(std::bit_width(placesRead), readsIndex); }

    uint64_t placesRead { 0 };
    bool readsLength { false };
    bool readsIndex { false };
    bool readsInput { false };
    bool isOnlyTakenApart { true };
    bool isReturned { false };
    Vector<uint32_t, 2> meetsOthersAt;
};

using NotesOfResultInParts = Vector<std::pair<Reference, Reference>, 8>;

struct LooksAtResultsInParts final : Walker::KeptForBuild {
    template<typename Kept> using ByNumber = HashMap<uint32_t, Kept, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByNumber<ByNumber<MentionsOfResultInParts>> byFunction;
    ByNumber<NotesOfResultInParts> partsOfListsThatAreOnlyTakenApart;
};

struct MatchAskedForInParts {
    Reference string;
    MentionsOfResultInParts mentions;
    uint64_t groupsThatAreSurelyThere { 0 };
    uint32_t length { 0 };
};

} // anonymous namespace

static constexpr uint32_t mostPlacesFollowedInResultsInParts = 63;

static bool hasMechanismsOfResultsInParts(const Walker& walker)
{
    return walker.has(Mechanism::Folding) && walker.has(Mechanism::EscapeAnalysis) && walker.has(Mechanism::BuiltinCalls) && walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::UnboxedNumbers);
}

static BuiltinName nameInTableForResultsInParts(const Walker& walker, ASCIILiteral text)
{
    return walker.program().builtins().nameOf(byteCast<uint8_t>(text.span()));
}

static Reference keyOfNameInResultsInParts(Walker& walker, BuiltinName name)
{
    Draft key(Opcode::String);
    key.setString(walker.program().stringOf(name));
    return walker.constant(WTF::move(key));
}

static bool isWrittenNullishInResultsInParts(const BodyIndex& index, ValueID value)
{
    ValueID written = index.stripCasts(value);
    return written && !index.isParameter(written) && (index.opcodeOf(written) == Opcode::Null || index.opcodeOf(written) == Opcode::Undefined);
}

static std::optional<uint32_t> placeWrittenInResultsInParts(const BodyIndex& index, ValueID key)
{
    ValueID written = index.stripCasts(key);
    if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::Integer)
        return std::nullopt;
    int64_t place = index.value(written).as<Opcode::Integer>().integer();
    if (place < 0 || place >= mostPlacesFollowedInResultsInParts)
        return std::nullopt;
    return static_cast<uint32_t>(place);
}

static bool isHandedRegExp(const BodyIndex& index, const BodyValue& call)
{
    return std::ranges::any_of(call.argumentsOperands(), [&](ValueID argument) {
        ValueID written = argument ? index.stripCasts(argument) : ValueID();
        return written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::NewRegExp;
    });
}

static void lookAtResultsOfBody(Walker& walker, const BodyIndex& index, LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>& mentions)
{
    const Program& program = walker.program();
    BuiltinName nameOfSplit = nameInTableForResultsInParts(walker, "split"_s);
    std::array calleeKeys { program.keyOf(BuiltinNames::exec), program.keyOf(BuiltinNames::match), nameOfSplit ? program.keyOf(nameOfSplit) : program.keyOf(BuiltinNames::match) };
    PropertyKeyID length = program.keyOf(BuiltinNames::length);
    BuiltinName nameOfIndex = nameInTableForResultsInParts(walker, "index"_s);
    BuiltinName nameOfInput = nameInTableForResultsInParts(walker, "input"_s);
    PropertyKeyID indexOfMatch = nameOfIndex ? program.keyOf(nameOfIndex) : PropertyKeyID();
    PropertyKeyID input = nameOfInput ? program.keyOf(nameOfInput) : PropertyKeyID();

    Vector<uint32_t> callsBehindNames;
    callsBehindNames.fill(UINT32_MAX, index.numberOfValues());
    BitVector parametersArrivedAt(index.numberOfValues());
    auto callBehind = [&](ValueID name) {
        return name ? callsBehindNames[name.index()] : UINT32_MAX;
    };
    auto mentionsBehind = [&](ValueID name) -> MentionsOfResultInParts* {
        uint32_t call = callBehind(name);
        if (call == UINT32_MAX)
            return nullptr;
        auto found = mentions.find(call);
        return found == mentions.end() ? nullptr : &found->value;
    };
    auto refuse = [&](ValueID mentioned) {
        if (MentionsOfResultInParts* found = mentionsBehind(mentioned))
            found->isOnlyTakenApart = false;
    };
    auto refuseOperandsOf = [&](const BodyValue& user) {
        user.forEachOperand([&](ValueID operand, uint32_t) {
            refuse(operand);
        });
    };

    for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = index.block(BlockID(blockIndex));
        refuse(block.subject());
        for (BodyValue user : block.values()) {
            Opcode opcode = user.opcode();
            if (inputPassThroughOpcodes.contains(opcode))
                callsBehindNames[user.id().index()] = callBehind(user.operand(0));
            else if (opcode == Opcode::ToBoolean) {
            } else if (opcode == Opcode::TypeTest && walker.types().isSubset(user.as<Opcode::TypeTest>().testedType(), TypeTable::nullish)) {
            } else if (allEqualities.contains(opcode) && (isWrittenNullishInResultsInParts(index, user.operand(0)) || isWrittenNullishInResultsInParts(index, user.operand(1)))) {
            } else if (MentionsOfResultInParts* read = opcode == Opcode::ElemGet ? mentionsBehind(user.objectOperand()) : nullptr) {
                refuse(user.propertyOperand());
                if (std::optional<uint32_t> place = placeWrittenInResultsInParts(index, user.propertyOperand()))
                    read->placesRead |= uint64_t(1) << *place;
                else
                    read->isOnlyTakenApart = false;
            } else if (MentionsOfResultInParts* named = opcode == Opcode::PropGet && !user.isFreeName() ? mentionsBehind(user.objectOperand()) : nullptr) {
                named->readsLength |= user.key() == length;
                named->readsIndex |= user.key() == indexOfMatch;
                named->readsInput |= user.key() == input;
                if (user.key() != length && user.key() != indexOfMatch && user.key() != input)
                    named->isOnlyTakenApart = false;
            } else if (MentionsOfResultInParts* taken = opcode == Opcode::IteratorOpen ? mentionsBehind(user.as<Opcode::IteratorOpen>().iterableOperand()) : nullptr) {
                auto open = user.as<Opcode::IteratorOpen>();
                if (open.isDestructuring() && !open.hasRest() && open.elementCount() && *open.elementCount() <= mostPlacesFollowedInResultsInParts)
                    taken->placesRead |= (uint64_t(1) << *open.elementCount()) - 1;
                else
                    taken->isOnlyTakenApart = false;
            } else
                refuseOperandsOf(user);
            if (opcode == Opcode::Call && (std::ranges::contains(calleeKeys, user.calleeKey()) || isHandedRegExp(index, user))) {
                callsBehindNames[user.id().index()] = user.id().index();
                mentions.add(user.id().index(), MentionsOfResultInParts());
            }
        }
        const bool returns = block.terminator().kind() == TerminatorKind::Return || block.terminator().kind() == TerminatorKind::ReturnCall;
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            MentionsOfResultInParts* found = mentionsBehind(operand);
            if (found && returns)
                found->isReturned = true;
            else
                refuse(operand);
        });
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& handed) {
            if (callBehind(handed.subject) == UINT32_MAX)
                return;
            if (index.valueNarrowedInto(handed.parameter) == handed.subject && index.blockOf(handed.parameter).index() > blockIndex)
                callsBehindNames[handed.parameter.index()] = callBehind(handed.subject);
            else
                refuse(handed.subject);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t position = 0; position < arguments.size(); ++position) {
                ValueID parameter = index.block(target.block()).parameter(position);
                if (isHandler) {
                    refuse(arguments[position]);
                    continue;
                }
                if (index.isUnused(parameter))
                    continue;
                const uint32_t arriving = callBehind(arguments[position]);
                const uint32_t arrivedBefore = callBehind(parameter);
                if (!parametersArrivedAt.set(parameter.index()))
                    callsBehindNames[parameter.index()] = arriving;
                else if (arrivedBefore != arriving) {
                    const bool isOfSeveral = arrivedBefore == parameter.index();
                    if (arriving == UINT32_MAX || arrivedBefore == UINT32_MAX || (!isOfSeveral && target.block().index() <= blockIndex)) {
                        refuse(arguments[position]);
                        refuse(parameter);
                        continue;
                    }
                    if (!isOfSeveral) {
                        mentions.add(parameter.index(), MentionsOfResultInParts());
                        mentions.find(arrivedBefore)->value.meetsOthersAt.append(parameter.index());
                        callsBehindNames[parameter.index()] = parameter.index();
                    }
                    mentions.find(arriving)->value.meetsOthersAt.append(parameter.index());
                }
            }
        });
    }
}

static const LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>& mentionsOfResultsOfBody(Walker& walker, FunctionID function, const BodyIndex& index)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = notesOfStrings(walker).looksAtResultsInParts;
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<LooksAtResultsInParts>();
    auto added = static_cast<LooksAtResultsInParts&>(*kept).byFunction.add(function.index(), LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>());
    if (added.isNewEntry)
        lookAtResultsOfBody(walker, index, added.iterator->value);
    return added.iterator->value;
}

static std::optional<MentionsOfResultInParts> mentionsOfResultOnlyTakenApart(Walker& walker, const BodyValue& call)
{
    auto& mentions = mentionsOfResultsOfBody(walker, walker.currentFunction(), walker.currentBodyIndex());
    auto found = mentions.find(call.id().index());
    if (found == mentions.end())
        return std::nullopt;
    Walker::ChoiceOfCost drawn = walker.drawChoice(DrawnFor::ResultInParts, walker.currentFunction().index(), call.id().index());
    bool isOnlyTakenApart = found->value.isOnlyTakenApart && !found->value.isReturned && found->value.meetsOthersAt.isEmpty();
    if (drawn == Walker::ChoiceOfCost::No || (!isOnlyTakenApart && drawn != Walker::ChoiceOfCost::Yes))
        return std::nullopt;
    return found->value;
}

static std::optional<MentionsOfResultInParts> findAllResultMentions(Walker& walker, const BodyValue& call)
{
    MentionsOfResultInParts all;
    auto addMentions = [&](FunctionID function, const BodyIndex& index, uint32_t first) {
        auto& mentions = mentionsOfResultsOfBody(walker, function, index);
        Vector<uint32_t, 8> toLookAt { first };
        bool isReturned = false;
        for (size_t place = 0; place < toLookAt.size(); ++place) {
            auto found = mentions.find(toLookAt[place]);
            if (found == mentions.end() || !found->value.isOnlyTakenApart)
                return std::optional<bool>();
            all.placesRead |= found->value.placesRead;
            all.readsLength |= found->value.readsLength;
            all.readsIndex |= found->value.readsIndex;
            all.readsInput |= found->value.readsInput;
            isReturned |= found->value.isReturned;
            for (uint32_t next : found->value.meetsOthersAt) {
                if (!toLookAt.contains(next))
                    toLookAt.append(next);
            }
        }
        return std::optional<bool>(isReturned);
    };
    std::optional<bool> isReturned = addMentions(walker.currentFunction(), walker.currentBodyIndex(), call.id().index());
    if (!isReturned || walker.drawChoice(DrawnFor::ResultInParts, walker.currentFunction().index(), call.id().index()) == Walker::ChoiceOfCost::No)
        return std::nullopt;
    if (*isReturned) {
        std::optional<Walker::CallOfCopy> copied = walker.callOfCopy();
        std::optional<bool> isReturnedAgain = copied ? addMentions(copied->caller, *copied->index, copied->call.index()) : std::nullopt;
        if (!isReturnedAgain || *isReturnedAgain)
            return std::nullopt;
    }
    return all;
}

static bool endOfBlockAsksWhetherThereIsResult(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyTerminator end = index.terminator(index.blockOf(call.id()));
    ValueID test = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
    while (test && !index.isParameter(test) && index.opcodeOf(test) == Opcode::LogicalNot)
        test = index.value(test).operand(0);
    if (!test || index.isParameter(test))
        return false;

    BodyValue asked = index.value(test);
    auto namesResult = [&](ValueID operand) {
        return index.stripCasts(operand) == call.id();
    };
    if (asked.opcode() == Opcode::ToBoolean)
        return namesResult(asked.operand(0));
    if (asked.opcode() == Opcode::TypeTest)
        return namesResult(asked.operand(0)) && walker.types().isSubset(asked.as<Opcode::TypeTest>().testedType(), TypeTable::nullish);
    if (!allEqualities.contains(asked.opcode()))
        return false;
    return (namesResult(asked.operand(0)) && isWrittenNullishInResultsInParts(index, asked.operand(1))) || (namesResult(asked.operand(1)) && isWrittenNullishInResultsInParts(index, asked.operand(0)));
}

static Reference buildPieceOrUndefinedInResultsInParts(Walker& walker, Reference string, Reference start, Reference end, Origin origin)
{
    if (walker.isUnreachable() || !string || !start || !end)
        return { };
    Draft piece(Opcode::SubstringOrUndefined, origin);
    piece.setStringOperand(string);
    piece.setStartOperand(start);
    piece.setEndOperand(end);
    return walker.build(WTF::move(piece));
}

static void appendPiecesReadInResultsInParts(Walker& walker, NotesOfResultInParts& notes, Reference results, Reference string, uint64_t placesRead, uint64_t placesThatAreSurelyThere, Origin origin)
{
    for (uint32_t place = 0; placesRead >> place; ++place) {
        if (!(placesRead >> place & 1))
            continue;
        Reference start = buildResultOfRangeForStrings(walker, results, firstBoundOfParts + 2 * place, origin);
        Reference end = buildResultOfRangeForStrings(walker, results, firstBoundOfParts + 2 * place + 1, origin);
        bool isSurelyThere = placesThatAreSurelyThere >> place & 1;
        Reference piece = isSurelyThere ? buildPieceOfString(walker, string, start, end, origin) : buildPieceOrUndefinedInResultsInParts(walker, string, start, end, origin);
        if (piece)
            notes.append({ integerForStrings(walker, place), piece });
    }
}

static bool buildMatchOnTwoWaysInResultsInParts(Walker& walker, const BodyValue& call, Draft&& match, Draft&& whole, const MatchAskedForInParts& asked)
{
    Origin origin = match.origin();
    TypeArena& types = walker.types();
    TypeID typeOfWhole = walker.resultTypeOf(whole);
    TypeID widest = walker.isInLoop() ? widestTypeOfOperation(walker, call) : TypeID();
    if (!typeOfWhole || (widest && !types.isSubset(types.withPrimitives(typeOfWhole, Primitive::Null), widest)))
        return false;
    if (!endOfBlockAsksWhetherThereIsResult(walker, call) || !walker.canThreadBranchAfter(call))
        return false;

    Reference isFound = walker.build(WTF::move(match));
    if (!isFound || walker.isUnreachable())
        return true;
    walker.count(StatisticID::RegExpMatchAllocationsElided);
    BlockID present = walker.addBlock();
    BlockID absent = walker.addBlock();
    walker.branch(isFound, present, absent);

    walker.continueIn(absent);
    walker.bind(call.id(), walker.constant(PrimitiveKnownToFolding::null()));
    if (!walker.threadBranchAfter(call)) {
        walker.fail("the way of a match that finds nothing does not walk the end of its block"_s);
        return true;
    }

    walker.continueIn(present);
    Draft bounds(Opcode::BoundsOfRegExpMatch, origin);
    walker.chooseConservativeFields(bounds);
    bounds.setCount(asked.mentions.numberOfParts());
    Reference results = walker.build(WTF::move(bounds));
    Reference array = walker.build(WTF::move(whole));
    NotesOfResultInParts notes;
    appendPiecesReadInResultsInParts(walker, notes, results, asked.string, asked.mentions.placesRead, asked.groupsThatAreSurelyThere | 1, origin);
    if (asked.mentions.readsIndex) {
        Reference start = buildResultOfRangeForStrings(walker, results, firstBoundOfParts, origin);
        if (Reference index = buildOperatorForStrings(walker, Opcode::BitAnd, start, integerForStrings(walker, std::numeric_limits<int32_t>::max()), origin))
            notes.append({ keyOfNameInResultsInParts(walker, nameInTableForResultsInParts(walker, "index"_s)), index });
    }
    if (BuiltinName input = nameInTableForResultsInParts(walker, "input"_s))
        notes.append({ keyOfNameInResultsInParts(walker, input), asked.string });
    notes.append({ keyOfNameInResultsInParts(walker, BuiltinNames::length), integerForStrings(walker, asked.length) });
    if (!array || !array.isValue() || walker.isUnreachable())
        return true;
    Reference cell = Reference::value(underlyingCellOf(walker.code(), array.id()));
    for (auto& [key, value] : notes)
        walker.state().noteElement(cell, key, value);
    walker.state().noteElementsLieOutsideViews(cell);
    walker.state().noteFresh(cell);
    walker.bind(call.id(), array);
    if (!walker.threadBranchAfter(call))
        walker.fail("the way of a match that finds something does not walk the end of its block"_s);
    return true;
}

Reference buildPlacesReadOfMatch(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const RegExpWrittenForCall& regExp, Reference objectOfCall, Reference string, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || !walker.isInLoop())
        return { };
    std::optional<InlineBuiltin> codeInLine = walker.program().builtins().at(overload).code.implementation.inlineBuiltin;
    bool givesOneMatch = codeInLine == InlineBuiltin::RegExpExec || (codeInLine == InlineBuiltin::StringMatch && !regExp.flags.contains(RegExpFlags::Global));
    if (!givesOneMatch || regExp.flags.contains(RegExpFlags::HasIndices) || !string || !isProvenString(walker, walker.typeOf(string)))
        return { };
    RegExpSummaries* summaries = walker.regExpSummaries();
    std::optional<MentionsOfResultInParts> mentions = summaries ? findAllResultMentions(walker, call) : std::nullopt;
    if (!mentions || mentions->readsLength || mentions->readsIndex || mentions->readsInput)
        return { };
    Vector<char16_t> buffer;
    const RegExpSummary& summary = summaries->summaryOf(regExp.pattern, toYarrFlags(regExp.flags), walker.types().viewOf(regExp.pattern, buffer));
    if (!summary.canBeCreated() || !summary.atom.isNull() || mentions->numberOfParts() > summary.numSubpatterns + 1)
        return { };

    Draft match(Opcode::RegExpMatchLiteralInParts, origin);
    walker.chooseConservativeFields(match);
    match.setStringOperand(string);
    if (regExp.flags.containsAny(flagsOfMatchesUsingObject) || walker.facts().mayReadLegacyRegExpStatics())
        match.setRegExpOperand(objectOfCall);
    match.setPattern(regExp.pattern);
    match.setRegExpFlags(regExp.flags);
    Reference isFound = walker.build(WTF::move(match));
    if (!isFound || walker.isUnreachable())
        return { };
    walker.count(StatisticID::RegExpMatchAllocationsElided);
    BlockID present = walker.addBlock();
    BlockID absent = walker.addBlock();
    BlockID joined = walker.addBlock();
    Reference result = walker.addParameter(joined, { }, Representation::JSValue);
    walker.branch(isFound, present, absent);

    walker.continueIn(absent);
    walker.jumpTo(joined, std::array { walker.constant(PrimitiveKnownToFolding::null()) });

    walker.continueIn(present);
    NotesOfResultInParts notes;
    const uint32_t numberOfParts = mentions->numberOfParts();
    if (numberOfParts) {
        Draft bounds(Opcode::BoundsOfRegExpMatch, origin);
        walker.chooseConservativeFields(bounds);
        bounds.setCount(numberOfParts);
        Reference results = walker.build(WTF::move(bounds));
        appendPiecesReadInResultsInParts(walker, notes, results, string, mentions->placesRead, summary.groupsPresentInEveryMatch | 1, origin);
    }
    Vector<Reference, 4> pieces;
    for (uint32_t place = 0, read = 0; place < numberOfParts; ++place)
        pieces.append(mentions->placesRead >> place & 1 && read < notes.size() ? notes[read++].second : walker.constant(Draft(Opcode::Undefined, origin)));
    Draft list(Opcode::NewArray, origin);
    walker.chooseConservativeFields(list);
    list.standsForNoValueOfInput = true;
    list.setContents(walker.types().withPrimitives(TypeTable::string, Primitive::Undefined));
    list.setVariadicOperands(pieces.span());
    Reference array = walker.build(WTF::move(list));
    if (!array || walker.isUnreachable())
        return { };
    walker.jumpTo(joined, std::array { array });
    walker.continueIn(joined);
    return result;
}

static Draft draftOfWholeOfMatchInResultsInParts(Walker& walker, Reference string, Origin origin)
{
    Draft whole(Opcode::NewRegExpMatchArray, origin);
    walker.chooseConservativeFields(whole);
    whole.setContents(walker.types().withPrimitives(TypeTable::string, Primitive::Undefined));
    whole.setStringOperand(string);
    return whole;
}

bool buildMatchInPartsByCodeOfRegExp(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const RegExpWrittenForCall& regExp, Reference objectOfCall, Reference string, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable())
        return false;
    std::optional<InlineBuiltin> codeInLine = walker.program().builtins().at(overload).code.implementation.inlineBuiltin;
    bool givesOneMatch = codeInLine == InlineBuiltin::RegExpExec || (codeInLine == InlineBuiltin::StringMatch && !regExp.flags.contains(RegExpFlags::Global));
    if (!givesOneMatch || regExp.flags.contains(RegExpFlags::HasIndices) || !string || !isProvenString(walker, walker.typeOf(string)))
        return false;

    RegExpSummaries* summaries = walker.regExpSummaries();
    if (!summaries)
        return false;
    Vector<char16_t> buffer;
    const RegExpSummary& summary = summaries->summaryOf(regExp.pattern, toYarrFlags(regExp.flags), walker.types().viewOf(regExp.pattern, buffer));
    if (!summary.canBeCreated() || !summary.atom.isNull())
        return false;
    std::optional<MentionsOfResultInParts> mentions = mentionsOfResultOnlyTakenApart(walker, call);
    if (!mentions || mentions->numberOfParts() > summary.numSubpatterns + 1)
        return false;

    Draft match(Opcode::RegExpMatchLiteralInParts, origin);
    walker.chooseConservativeFields(match);
    match.setStringOperand(string);
    if (regExp.flags.containsAny(flagsOfMatchesUsingObject) || walker.facts().mayReadLegacyRegExpStatics())
        match.setRegExpOperand(objectOfCall);
    match.setPattern(regExp.pattern);
    match.setRegExpFlags(regExp.flags);

    Draft whole = draftOfWholeOfMatchInResultsInParts(walker, string, origin);
    whole.setPattern(regExp.pattern);
    whole.setRegExpFlags(regExp.flags);

    return buildMatchOnTwoWaysInResultsInParts(walker, call, WTF::move(match), WTF::move(whole), { string, *mentions, summary.groupsPresentInEveryMatch, summary.numSubpatterns + 1 });
}

bool buildSplitInParts(Walker& walker, const CallByOneRow& call)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || call.isForwarded || call.call.opcode() != Opcode::Call)
        return false;
    if (call.arguments.size() != 1 || !call.receiver || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;
    StringID separator = textKnownForStrings(walker, call.arguments[0]);
    if (!separator)
        return false;
    Vector<char16_t> buffer;
    if (walker.types().viewOf(separator, buffer).isEmpty())
        return false;
    std::optional<MentionsOfResultInParts> mentions = mentionsOfResultOnlyTakenApart(walker, call.call);
    if (!mentions || mentions->readsIndex || mentions->readsInput)
        return false;

    Origin origin = call.origin;
    origin.isTailCall = false;
    Draft whole(Opcode::NewStringSplitArray, origin);
    walker.chooseConservativeFields(whole);
    whole.setContents(TypeTable::string);
    whole.setStringOperand(call.receiver);
    whole.setSeparatorOperand(call.arguments[0]);
    TypeID typeOfWhole = walker.resultTypeOf(whole);
    TypeID widest = walker.isInLoop() ? widestTypeOfOperation(walker, call.call) : TypeID();
    if (!typeOfWhole || (widest && !walker.types().isSubset(typeOfWhole, widest)))
        return false;

    uint32_t numberOfParts = mentions->numberOfParts();
    Draft bounds(Opcode::BoundsOfStringSplit, origin);
    walker.chooseConservativeFields(bounds);
    bounds.setStringOperand(call.receiver);
    bounds.setLiteral(separator);
    bounds.setCount(numberOfParts);
    bounds.setCountsEveryPiece(mentions->readsLength);
    Reference results = walker.build(WTF::move(bounds));
    if (!results || walker.isUnreachable())
        return true;
    walker.count(StatisticID::StringSplitAllocationsElided);
    Reference array = walker.build(WTF::move(whole));
    NotesOfResultInParts notes;
    appendPiecesReadInResultsInParts(walker, notes, results, call.receiver, mentions->placesRead, 1, origin);
    if (mentions->readsLength) {
        if (Reference length = buildResultOfRangeForStrings(walker, results, firstBoundOfParts + 2 * numberOfParts, origin))
            notes.append({ keyOfNameInResultsInParts(walker, BuiltinNames::length), length });
    }
    if (!array || !array.isValue() || walker.isUnreachable())
        return true;
    Reference cell = Reference::value(underlyingCellOf(walker.code(), array.id()));
    for (auto& [key, value] : notes)
        walker.state().noteElement(cell, key, value);
    walker.state().noteElementsLieOutsideViews(cell);
    walker.state().noteFresh(cell);
    auto& partsOfLists = static_cast<LooksAtResultsInParts&>(*notesOfStrings(walker).looksAtResultsInParts).partsOfListsThatAreOnlyTakenApart;
    if (mentions->isOnlyTakenApart)
        partsOfLists.set(cell.id().index(), WTF::move(notes));
    else
        partsOfLists.remove(cell.id().index());
    walker.bind(call.call.id(), array);
    return true;
}

static ALWAYS_INLINE const NotesOfResultInParts* partsOfListThatIsOnlyTakenApart(Walker& walker, Reference name)
{
    std::unique_ptr<Walker::KeptForBuild>& keptByStrings = walker.keptByStrings();
    if (!keptByStrings || !name || !name.isValue())
        return nullptr;
    std::unique_ptr<Walker::KeptForBuild>& kept = static_cast<NotesOfStrings&>(*keptByStrings).looksAtResultsInParts;
    if (!kept)
        return nullptr;
    auto& lists = static_cast<LooksAtResultsInParts&>(*kept).partsOfListsThatAreOnlyTakenApart;
    if (lists.isEmpty())
        return nullptr;
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, name.id());
    if (code.isConstant(cell) || code.at(cell).opcode != Opcode::NewStringSplitArray)
        return nullptr;
    auto found = lists.find(cell.index());
    return found == lists.end() ? nullptr : &found->value;
}

Reference partOfListThatIsOnlyTakenApart(Walker& walker, Reference name, Reference key)
{
    const NotesOfResultInParts* parts = key ? partsOfListThatIsOnlyTakenApart(walker, name) : nullptr;
    if (!parts)
        return { };
    for (auto& [noted, part] : *parts) {
        if (noted == key)
            return part;
    }
    return { };
}

static ALWAYS_INLINE Reference wholeBesidePartsNamedBy(Walker& walker, Reference name)
{
    if (!name || !name.isValue() || !walker.state().holdsFreshObjects())
        return { };
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, name.id());
    if (code.isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return { };
    return code.at(cell).opcode == Opcode::NewRegExpMatchArray || code.at(cell).opcode == Opcode::NewStringSplitArray ? Reference::value(cell) : Reference();
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference propertyOfResultTakenInParts(Walker& walker, Reference name, PropertyKeyID key)
{
    bool isOnlyTakenApart = partsOfListThatIsOnlyTakenApart(walker, name);
    Reference whole = wholeBesidePartsNamedBy(walker, name);
    if ((!isOnlyTakenApart && !whole) || liesInThread(key))
        return { };
    BuiltinName written = walker.program().builtinNameOf(key);
    if (!written || walker.program().builtins().isSymbolName(written))
        return { };
    if (Reference part = partOfListThatIsOnlyTakenApart(walker, name, keyOfNameInResultsInParts(walker, written)))
        return part;
    return whole ? walker.state().element(whole, keyOfNameInResultsInParts(walker, written)) : Reference();
}

std::optional<Vector<Reference, 4>> partsThatPatternTakes(Walker& walker, Reference iterable, uint32_t numberOfSteps)
{
    Reference whole = wholeBesidePartsNamedBy(walker, iterable);
    if (!whole && !partsOfListThatIsOnlyTakenApart(walker, iterable))
        return std::nullopt;
    Vector<Reference, 4> parts;
    for (uint32_t place = 0; place < numberOfSteps; ++place) {
        Reference part = partOfListThatIsOnlyTakenApart(walker, iterable, integerForStrings(walker, place));
        if (!part && whole)
            part = walker.state().element(whole, integerForStrings(walker, place));
        if (!part)
            return std::nullopt;
        parts.append(part);
    }
    return parts;
}

bool boundsAreOfOnePart(const Walker& walker, Reference start, Reference end)
{
    Reference first = rootForStrings(walker, start);
    Reference second = rootForStrings(walker, end);
    if (!first.isValue() || !second.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& lower = code.at(first.id());
    const Operation& upper = code.at(second.id());
    if (lower.opcode != Opcode::ResultAt || upper.opcode != Opcode::ResultAt || lower.resultsOperand() != upper.resultsOperand())
        return false;
    if (!callsThatGiveBoundsOfParts.contains(code.at(lower.resultsOperand()).opcode))
        return false;
    return lower.index() >= firstBoundOfParts && !((lower.index() - firstBoundOfParts) & 1) && upper.index() == lower.index() + 1;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
