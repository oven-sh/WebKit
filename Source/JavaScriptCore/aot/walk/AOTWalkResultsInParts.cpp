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
#include "AOTWalkKnown.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkStringFolds.h"
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
    bool readsGroups { false };
    bool isOnlyTakenApart { true };
    bool isReadByIndex { false };
    bool escapes { false };
    bool isReturned { false };
    uint32_t numberOfMergedParts { 0 };
    Vector<uint32_t, 2> meetsOthersAt;
    Vector<PropertyKeyID, 2> groupKeysRead;
};

using NotesOfResultInParts = Vector<std::pair<Reference, Reference>, 8>;

struct LoopOverResult {
    ValueID open;
    ValueID step;
};

struct MatchIteration {
    Reference regExpObject;
    Reference string;
};

struct MatchInFrameForParts {
    Reference match;
    Reference string;
};

struct LooksAtResultsInParts final : Walker::KeptForBuild {
    template<typename Kept> using ByNumber = HashMap<uint32_t, Kept, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByNumber<ByNumber<MentionsOfResultInParts>> byFunction;
    ByNumber<NotesOfResultInParts> partsOfListsThatAreOnlyTakenApart;
    PropertyKeyID keyOfLastIndex;
    ByNumber<MatchInFrameForParts> matchesInFrame;
    template<typename Iteration> using ByOpen = HashMap<uint64_t, Vector<Iteration, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    ByOpen<MatchIteration> matchIterations;
    std::optional<bool> engineStepsMatches;
};

struct MatchAskedForInParts {
    Reference string;
    MentionsOfResultInParts mentions;
    Vector<uint32_t, 2> placesOfGroupsRead;
    uint64_t groupsThatAreSurelyThere { 0 };
    uint32_t length { 0 };
    Reference regExpWithLastIndex { };
};

} // anonymous namespace

static constexpr uint32_t mostPlacesFollowedInResultsInParts = 63;
static constexpr size_t maxGroupKeysRead = 16;
static constexpr uint32_t namesGroupsOfResult = 1u << 31;

static bool hasMechanismsOfResultsInParts(const Walker& walker)
{
    return walker.has(Mechanism::Folding) && walker.has(Mechanism::EscapeAnalysis) && walker.has(Mechanism::BuiltinCalls) && walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::UnboxedNumbers);
}

static BuiltinName nameInTableForResultsInParts(const Walker& walker, ASCIILiteral text)
{
    return walker.program().builtins().nameOf(byteCast<uint8_t>(text.span()));
}

static Reference stringKeyInResultsInParts(Walker& walker, StringID text)
{
    Draft key(Opcode::String);
    key.setString(text);
    return walker.constant(WTF::move(key));
}

static Reference keyOfNameInResultsInParts(Walker& walker, BuiltinName name)
{
    return stringKeyInResultsInParts(walker, walker.program().stringOf(name));
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

static bool stepsOverCallOf(const BodyIndex& index, const BodyValue& step, PropertyKeyID key)
{
    ValueID open = step.operand(0);
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return false;
    ValueID iterable = index.value(open).as<Opcode::IteratorOpen>().iterableOperand();
    iterable = iterable ? index.stripCasts(iterable) : ValueID();
    return iterable && !index.isParameter(iterable) && index.opcodeOf(iterable) == Opcode::Call && index.value(iterable).calleeKey() == key;
}

static void lookAtResultsOfBody(Walker& walker, const BodyIndex& index, LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>& mentions)
{
    const Program& program = walker.program();
    BuiltinName nameOfSplit = nameInTableForResultsInParts(walker, "split"_s);
    std::array calleeKeys { program.keyOf(BuiltinNames::exec), program.keyOf(BuiltinNames::match), nameOfSplit ? program.keyOf(nameOfSplit) : program.keyOf(BuiltinNames::match) };
    PropertyKeyID length = program.keyOf(BuiltinNames::length);
    BuiltinName nameOfMatchAll = nameInTableForResultsInParts(walker, "matchAll"_s);
    PropertyKeyID matchAll = nameOfMatchAll ? program.keyOf(nameOfMatchAll) : PropertyKeyID();
    BuiltinName nameOfIndex = nameInTableForResultsInParts(walker, "index"_s);
    BuiltinName nameOfInput = nameInTableForResultsInParts(walker, "input"_s);
    BuiltinName nameOfGroups = nameInTableForResultsInParts(walker, "groups"_s);
    PropertyKeyID groups = nameOfGroups ? program.keyOf(nameOfGroups) : PropertyKeyID();
    PropertyKeyID indexOfMatch = nameOfIndex ? program.keyOf(nameOfIndex) : PropertyKeyID();
    PropertyKeyID input = nameOfInput ? program.keyOf(nameOfInput) : PropertyKeyID();

    Vector<uint32_t> callsBehindNames;
    callsBehindNames.fill(UINT32_MAX, index.numberOfValues());
    BitVector parametersArrivedAt(index.numberOfValues());
    BitVector stepsOverMatches;
    auto callBehind = [&](ValueID name) {
        return name ? callsBehindNames[name.index()] : UINT32_MAX;
    };
    auto mentionsOfCall = [&](uint32_t call) -> MentionsOfResultInParts* {
        auto found = mentions.find(call);
        return found == mentions.end() ? nullptr : &found->value;
    };
    auto mentionsBehind = [&](ValueID name) -> MentionsOfResultInParts* {
        uint32_t call = callBehind(name);
        return call & namesGroupsOfResult ? nullptr : mentionsOfCall(call);
    };
    auto groupsBehind = [&](ValueID name) -> MentionsOfResultInParts* {
        uint32_t call = callBehind(name);
        return call == UINT32_MAX || !(call & namesGroupsOfResult) ? nullptr : mentionsOfCall(call & ~namesGroupsOfResult);
    };
    auto refuse = [&](ValueID mentioned) {
        MentionsOfResultInParts* found = mentionsBehind(mentioned);
        if (!found)
            found = groupsBehind(mentioned);
        if (found) {
            found->isOnlyTakenApart = false;
            found->escapes = true;
        }
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
            else if (opcode == Opcode::ResultAt && stepsOverMatches.get(user.as<Opcode::ResultAt>().resultsOperand().index())) {
                if (user.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
                    callsBehindNames[user.id().index()] = user.as<Opcode::ResultAt>().resultsOperand().index();
            } else if (opcode == Opcode::ToBoolean) {
            } else if (opcode == Opcode::TypeTest && walker.types().isSubset(user.as<Opcode::TypeTest>().testedType(), TypeTable::nullish)) {
            } else if (allEqualities.contains(opcode) && (isWrittenNullishInResultsInParts(index, user.operand(0)) || isWrittenNullishInResultsInParts(index, user.operand(1)))) {
            } else if (MentionsOfResultInParts* read = opcode == Opcode::ElemGet ? mentionsBehind(user.objectOperand()) : nullptr) {
                refuse(user.propertyOperand());
                if (std::optional<uint32_t> place = placeWrittenInResultsInParts(index, user.propertyOperand()))
                    read->placesRead |= uint64_t(1) << *place;
                else {
                    read->isOnlyTakenApart = false;
                    read->isReadByIndex = true;
                }
            } else if (MentionsOfResultInParts* named = opcode == Opcode::PropGet && !user.isFreeName() ? mentionsBehind(user.objectOperand()) : nullptr) {
                named->readsLength |= user.key() == length;
                named->readsIndex |= user.key() == indexOfMatch;
                named->readsInput |= user.key() == input;
                if (groups && user.key() == groups) {
                    named->readsGroups = true;
                    callsBehindNames[user.id().index()] = callBehind(user.objectOperand()) | namesGroupsOfResult;
                } else if (user.key() != length && user.key() != indexOfMatch && user.key() != input) {
                    named->isOnlyTakenApart = false;
                    named->escapes = true;
                }
            } else if (MentionsOfResultInParts* grouped = opcode == Opcode::PropGet && !user.isFreeName() ? groupsBehind(user.objectOperand()) : nullptr) {
                if (!grouped->groupKeysRead.contains(user.key()))
                    grouped->groupKeysRead.append(user.key());
                if (grouped->groupKeysRead.size() > maxGroupKeysRead)
                    refuse(user.objectOperand());
            } else if (MentionsOfResultInParts* taken = opcode == Opcode::IteratorOpen ? mentionsBehind(user.as<Opcode::IteratorOpen>().iterableOperand()) : nullptr) {
                auto open = user.as<Opcode::IteratorOpen>();
                if (open.isDestructuring() && !open.hasRest() && open.elementCount() && *open.elementCount() <= mostPlacesFollowedInResultsInParts)
                    taken->placesRead |= (uint64_t(1) << *open.elementCount()) - 1;
                else {
                    taken->isOnlyTakenApart = false;
                    taken->escapes = true;
                }
            } else
                refuseOperandsOf(user);
            if (opcode == Opcode::Call && (std::ranges::contains(calleeKeys, user.calleeKey()) || isHandedRegExp(index, user))) {
                callsBehindNames[user.id().index()] = user.id().index();
                mentions.add(user.id().index(), MentionsOfResultInParts());
            }
            if (opcode == Opcode::IteratorResume && matchAll && stepsOverCallOf(index, user, matchAll)) {
                stepsOverMatches.set(user.id().index());
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
                    if ((arriving | arrivedBefore) & namesGroupsOfResult || (!isOfSeveral && target.block().index() <= blockIndex)) {
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

static LooksAtResultsInParts& ensureResultPartsAnalysis(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = notesOfStrings(walker).looksAtResultsInParts;
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<LooksAtResultsInParts>();
    return static_cast<LooksAtResultsInParts&>(*kept);
}

static void countMergedResultParts(LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>& mentions)
{
    for (auto& entry : mentions)
        entry.value.numberOfMergedParts = entry.value.numberOfParts();
    bool changes = true;
    while (changes) {
        changes = false;
        for (auto& entry : mentions) {
            for (uint32_t place : entry.value.meetsOthersAt) {
                MentionsOfResultInParts& met = mentions.find(place)->value;
                const uint32_t most = std::max(met.numberOfMergedParts, entry.value.numberOfMergedParts);
                changes |= most != met.numberOfMergedParts || most != entry.value.numberOfMergedParts;
                met.numberOfMergedParts = entry.value.numberOfMergedParts = most;
            }
        }
    }
}

static const LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>& mentionsOfResultsOfBody(Walker& walker, FunctionID function, const BodyIndex& index)
{
    auto added = ensureResultPartsAnalysis(walker).byFunction.add(function.index(), LooksAtResultsInParts::ByNumber<MentionsOfResultInParts>());
    if (added.isNewEntry) {
        lookAtResultsOfBody(walker, index, added.iterator->value);
        countMergedResultParts(added.iterator->value);
    }
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
            all.readsGroups |= found->value.readsGroups;
            all.numberOfMergedParts = std::max(all.numberOfMergedParts, found->value.numberOfMergedParts);
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

static std::optional<Vector<uint32_t, 2>> addReadGroupSlots(Walker& walker, MentionsOfResultInParts& mentions, const RegExpSummary& summary)
{
    Vector<uint32_t, 2> places;
    if (!mentions.readsGroups)
        return places;
    if (!mentions.isOnlyTakenApart || !summary.hasNamedGroups() || summary.numDuplicateNamedCaptureGroups)
        return std::nullopt;
    for (PropertyKeyID id : mentions.groupKeysRead) {
        const PropertyKey& key = walker.types().at(id);
        if (key.kind != PropertyKeyKind::Text)
            return std::nullopt;
        Vector<char16_t> buffer;
        StringView text = walker.types().viewOf(key.text, buffer);
        size_t place = summary.captureGroupNames.findIf([&](const String& name) { return !name.isEmpty() && StringView(name) == text; });
        if (place == notFound) {
            places.append(0);
            continue;
        }
        if (!place || place >= mostPlacesFollowedInResultsInParts || place > summary.numSubpatterns)
            return std::nullopt;
        mentions.placesRead |= uint64_t(1) << place;
        places.append(static_cast<uint32_t>(place));
    }
    return places;
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

static Reference buildPlaceOfMatch(Walker& walker, Reference results, uint32_t bound, Origin origin)
{
    Reference place = buildResultOfRangeForStrings(walker, results, bound, origin);
    return place ? buildOperatorForStrings(walker, Opcode::BitAnd, place, integerForStrings(walker, std::numeric_limits<int32_t>::max()), origin) : Reference();
}

static bool isKeyOfNotedLastIndex(Walker& walker, PropertyKeyID key)
{
    std::unique_ptr<Walker::KeptForBuild>& keptByStrings = walker.keptByStrings();
    if (!keptByStrings)
        return false;
    std::unique_ptr<Walker::KeptForBuild>& kept = static_cast<NotesOfStrings&>(*keptByStrings).looksAtResultsInParts;
    return kept && static_cast<LooksAtResultsInParts&>(*kept).keyOfLastIndex == key;
}

static void noteRegExpLastIndex(Walker& walker, Reference regExpObject, Reference lastIndexOrNull)
{
    BuiltinName lastIndex = nameInTableForResultsInParts(walker, "lastIndex"_s);
    Reference root = rootForStrings(walker, regExpObject);
    if (!lastIndex || !root.isValue())
        return;
    ensureResultPartsAnalysis(walker).keyOfLastIndex = walker.program().keyOf(lastIndex);
    noteKnown(walker, KnownNote::LastIndexOfRegExp, root, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::RegExpState }), lastIndexOrNull.bits(), true);
}

static NEVER_INLINE std::optional<Reference> findLastIndexOrNull(Walker& walker, Reference regExpObject)
{
    if (!regExpObject)
        return std::nullopt;
    Reference root = rootForStrings(walker, regExpObject);
    std::optional<uint64_t> noted = root.isValue() ? findKnown(walker, KnownNote::LastIndexOfRegExp, root) : std::nullopt;
    return noted ? std::optional(Reference::fromBits(static_cast<uint32_t>(*noted))) : std::nullopt;
}

Reference findLastIndexOfMatch(Walker& walker, Reference regExpObject, PropertyKeyID key)
{
    if (!key || !isKeyOfNotedLastIndex(walker, key))
        return { };
    return findLastIndexOrNull(walker, regExpObject).value_or(Reference());
}

void noteMatchStoresLastIndex(Walker& walker, const HeldRegExp& regExp, Reference regExpObject)
{
    if (hasMechanismsOfResultsInParts(walker) && (regExp.isPrivate || !walker.facts().mayRecompileRegExps()) && regExp.flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }))
        noteRegExpLastIndex(walker, regExpObject, { });
}

static NEVER_INLINE bool buildNotedLastIndex(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || walker.isUnreachable())
        return false;
    Reference regExpObject = walker.referenceOf(read.objectOperand());
    std::optional<Reference> noted = findLastIndexOrNull(walker, regExpObject);
    if (!noted || *noted)
        return false;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    if (walker.checksProofs()) {
        Draft generic = walker.draftOf(read);
        walker.chooseConservativeFields(generic);
        Reference found = walker.build(WTF::move(generic));
        if (!found || walker.isUnreachable())
            return true;
        if (found.isValue())
            walker.checkProof(found, TypeTable::length, TypeTable::anything, ProofSource::OtherOperation, origin);
    }
    Draft lastIndex(Opcode::GetRegExpLastIndex, origin);
    walker.chooseConservativeFields(lastIndex);
    lastIndex.setRegExpOperand(regExpObject);
    if (Reference built = walker.build(WTF::move(lastIndex)))
        walker.bind(read.id(), built);
    return true;
}

bool buildLastIndexOfMatch(Walker& walker, const BodyValue& read)
{
    return isKeyOfNotedLastIndex(walker, read.key()) && buildNotedLastIndex(walker, read);
}

static void noteGroupsOfMatch(Walker& walker, const MatchAskedForInParts& asked, Reference array, NotesOfResultInParts& notes, Origin origin)
{
    Draft groups(Opcode::GetMatchArrayGroups, origin);
    walker.chooseConservativeFields(groups);
    groups.setMatchArrayOperand(array);
    Reference object = walker.build(WTF::move(groups));
    if (!object || !object.isValue() || walker.isUnreachable())
        return;
    walker.count(StatisticID::MatchGroupObjectsElided);
    for (size_t position = 0; position < asked.placesOfGroupsRead.size(); ++position) {
        const uint32_t place = asked.placesOfGroupsRead[position];
        Reference value;
        if (!place)
            value = walker.constant(PrimitiveKnownToFolding::undefined());
        else {
            Reference index = integerForStrings(walker, place);
            size_t noted = notes.findIf([&](const std::pair<Reference, Reference>& note) { return note.first == index; });
            value = noted == notFound ? Reference() : notes[noted].second;
        }
        if (value)
            walker.state().noteElement(object, stringKeyInResultsInParts(walker, walker.types().at(asked.mentions.groupKeysRead[position]).text), value);
    }
    walker.state().noteElementsLieOutsideViews(object);
    walker.state().noteFresh(object);
    notes.append({ keyOfNameInResultsInParts(walker, nameInTableForResultsInParts(walker, "groups"_s)), object });
}

static Reference buildMatchArray(Walker& walker, Draft&& whole, const MatchAskedForInParts& asked, NotesOfResultInParts& notes, Origin origin)
{
    Draft bounds(Opcode::BoundsOfRegExpMatch, origin);
    walker.chooseConservativeFields(bounds);
    bounds.setCount(std::max<uint32_t>(asked.mentions.numberOfParts(), !!asked.regExpWithLastIndex));
    Reference results = walker.build(WTF::move(bounds));
    Reference array = walker.build(WTF::move(whole));
    appendPiecesReadInResultsInParts(walker, notes, results, asked.string, asked.mentions.placesRead, asked.groupsThatAreSurelyThere | 1, origin);
    if (asked.mentions.readsIndex) {
        if (Reference index = buildPlaceOfMatch(walker, results, firstBoundOfParts, origin))
            notes.append({ keyOfNameInResultsInParts(walker, nameInTableForResultsInParts(walker, "index"_s)), index });
    }
    if (asked.regExpWithLastIndex) {
        if (Reference end = buildPlaceOfMatch(walker, results, firstBoundOfParts + 1, origin))
            noteRegExpLastIndex(walker, asked.regExpWithLastIndex, end);
    }
    if (BuiltinName input = nameInTableForResultsInParts(walker, "input"_s))
        notes.append({ keyOfNameInResultsInParts(walker, input), asked.string });
    notes.append({ keyOfNameInResultsInParts(walker, BuiltinNames::length), integerForStrings(walker, asked.length) });
    return array;
}

static void noteMatchArray(Walker& walker, const MatchAskedForInParts& asked, Reference array, NotesOfResultInParts& notes, Origin origin)
{
    Reference cell = Reference::value(underlyingCellOf(walker.code(), array.id()));
    if (asked.mentions.readsGroups)
        noteGroupsOfMatch(walker, asked, array, notes, origin);
    for (auto& [key, value] : notes)
        walker.state().noteElement(cell, key, value);
    walker.state().noteElementsLieOutsideViews(cell);
    walker.state().noteFresh(cell);
}

static bool buildMatchOnTwoWaysInResultsInParts(Walker& walker, const BodyValue& call, Draft&& match, Draft&& whole, const MatchAskedForInParts& asked)
{
    Origin origin = match.origin();
    TypeArena& types = walker.types();
    TypeID typeOfWhole = walker.resultTypeOf(whole);
    TypeID widest = walker.isInLoop() ? widestTypeOfOperation(walker, call) : TypeID();
    if (!typeOfWhole || (widest && !types.isSubset(types.withPrimitives(typeOfWhole, Primitive::Null), widest)))
        return false;
    if (!endOfBlockAsksWhetherThereIsResult(walker, call) || !walker.canThreadBranchAfterMatch(call))
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
    NotesOfResultInParts notes;
    Reference array = buildMatchArray(walker, WTF::move(whole), asked, notes, origin);
    if (!array || !array.isValue() || walker.isUnreachable())
        return true;
    noteMatchArray(walker, asked, array, notes, origin);
    walker.bind(call.id(), array);
    if (!walker.threadBranchAfter(call))
        walker.fail("the way of a match that finds something does not walk the end of its block"_s);
    return true;
}

static Reference buildReadSlotList(Walker& walker, Reference string, const MentionsOfResultInParts& mentions, uint32_t length, const RegExpSummary& summary, Origin origin)
{
    NotesOfResultInParts notes;
    const uint32_t numberOfParts = mentions.numberOfParts();
    if (numberOfParts) {
        Draft bounds(Opcode::BoundsOfRegExpMatch, origin);
        walker.chooseConservativeFields(bounds);
        bounds.setCount(numberOfParts);
        Reference results = walker.build(WTF::move(bounds));
        appendPiecesReadInResultsInParts(walker, notes, results, string, mentions.placesRead, summary.groupsPresentInEveryMatch | 1, origin);
    }
    Vector<Reference, 4> pieces;
    for (uint32_t place = 0, read = 0; place < length; ++place) {
        bool isRead = place < numberOfParts && mentions.placesRead >> place & 1 && read < notes.size();
        pieces.append(isRead ? notes[read++].second : walker.constant(Draft(Opcode::Undefined, origin)));
    }
    Draft list(Opcode::NewArray, origin);
    walker.chooseConservativeFields(list);
    list.standsForNoValueOfInput = true;
    list.setContents(walker.types().withPrimitives(TypeTable::string, Primitive::Undefined));
    list.setVariadicOperands(pieces.span());
    return walker.build(WTF::move(list));
}

static bool isReturnedAtOnce(const Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyTerminator end = index.terminator(index.blockOf(call.id()));
    return (end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall) && end.operand(0) && index.stripCasts(end.operand(0)) == call.id();
}

static bool buildSlotsReadOnTwoPaths(Walker& walker, const BodyValue& call, Draft&& match, Reference string, const MentionsOfResultInParts& mentions, const RegExpSummary& summary)
{
    if (!(endOfBlockAsksWhetherThereIsResult(walker, call) || isReturnedAtOnce(walker, call)) || !walker.canThreadBranchAfterMatch(call))
        return false;
    Origin origin = match.origin();
    Reference isFound = walker.build(WTF::move(match));
    if (!isFound || walker.isUnreachable())
        return true;
    walker.count(StatisticID::RegExpMatchAllocationsElided);
    walker.noteWaysApart(walker.code().numberOfValues());
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
    Reference array = buildReadSlotList(walker, string, mentions, mentions.numberOfMergedParts, summary, origin);
    if (!array || walker.isUnreachable())
        return true;
    noteFreshList(walker, array);
    walker.bind(call.id(), array);
    if (!walker.threadBranchAfter(call))
        walker.fail("the way of a match that finds something does not walk the end of its block"_s);
    return true;
}

Reference buildPlacesReadOfMatch(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const RegExpWrittenForCall& regExp, Reference objectOfCall, Reference string, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || !walker.runsInIterations())
        return { };
    std::optional<InlineBuiltin> codeInLine = walker.program().builtins().at(overload).code.implementation.inlineBuiltin;
    bool givesOneMatch = codeInLine == InlineBuiltin::RegExpExec || (codeInLine == InlineBuiltin::StringMatch && !regExp.flags.contains(RegExpFlags::Global));
    if (!givesOneMatch || regExp.flags.contains(RegExpFlags::HasIndices) || !string || !isProvenString(walker, walker.typeOf(string)))
        return { };
    RegExpSummaries* summaries = walker.regExpSummaries();
    std::optional<MentionsOfResultInParts> mentions = summaries ? findAllResultMentions(walker, call) : std::nullopt;
    if (!mentions || mentions->readsLength || mentions->readsIndex || mentions->readsInput || mentions->readsGroups)
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
    if (buildSlotsReadOnTwoPaths(walker, call, WTF::move(match), string, *mentions, summary))
        return { };
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
    Reference array = buildReadSlotList(walker, string, *mentions, mentions->numberOfParts(), summary, origin);
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
    if (!mentions)
        return false;
    std::optional<Vector<uint32_t, 2>> groups = addReadGroupSlots(walker, *mentions, summary);
    if (!groups || mentions->numberOfParts() > summary.numSubpatterns + 1)
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

    return buildMatchOnTwoWaysInResultsInParts(walker, call, WTF::move(match), WTF::move(whole), { string, *mentions, WTF::move(*groups), summary.groupsPresentInEveryMatch, summary.numSubpatterns + 1 });
}

static Reference findStoredLastIndexInResultsInParts(Walker& walker, Reference regExpObject)
{
    const Code& code = walker.code();
    const Reference root = rootForStrings(walker, regExpObject);
    for (ValueID id = code.lastOperationOf(walker.currentBlock()); id; id = code.at(id).previous) {
        const Operation& operation = code.at(id);
        if (operation.traits().isUnobservable())
            continue;
        if (operation.opcode != Opcode::PropSet || operation.isFreeName() || liesInThread(operation.key()))
            return { };
        BuiltinName lastIndex = nameInTableForResultsInParts(walker, "lastIndex"_s);
        if (!lastIndex || operation.key() != walker.program().keyOf(lastIndex) || rootForStrings(walker, referenceToRecordForStrings(walker, operation.objectOperand())) != root)
            return { };
        Reference stored = referenceToRecordForStrings(walker, operation.valueOperand());
        if (stored.isValue()) {
            ValueID raw = stored.id();
            while (!code.isConstant(raw) && conversionsOfForm.contains(code.at(raw).opcode))
                raw = code.at(raw).operand(0);
            stored = referenceToRecordForStrings(walker, raw);
        }
        return walker.types().at(walker.typeOf(stored)).isInteger() ? stored : Reference();
    }
    return { };
}

bool buildHeldRegExpMatchInParts(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const HeldRegExp& regExp, Reference regExpObject, Reference string, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || !walker.runsInIterations())
        return false;
    RegExpSummaries* summaries = walker.regExpSummaries();
    if (!summaries || regExp.flags.contains(RegExpFlags::HasIndices) || !regExpObject || !string || !isProvenString(walker, walker.typeOf(string)))
        return false;
    std::optional<MentionsOfResultInParts> mentions = mentionsOfResultOnlyTakenApart(walker, call);
    if (!mentions)
        return false;
    Vector<char16_t> buffer;
    const RegExpSummary& summary = summaries->summaryOf(regExp.pattern, toYarrFlags(regExp.flags), walker.types().viewOf(regExp.pattern, buffer));
    std::optional<Vector<uint32_t, 2>> groups = summary.canBeCreated() ? addReadGroupSlots(walker, *mentions, summary) : std::nullopt;
    if (!groups || mentions->numberOfParts() > summary.numSubpatterns + 1)
        return false;
    const bool keepsPattern = regExp.isPrivate || !walker.facts().mayRecompileRegExps();
    if (!keepsPattern && (mentions->readsLength || mentions->readsGroups))
        return false;
    const bool storesLastIndex = regExp.flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky });

    Draft match(Opcode::MatchHeldRegExpParts, origin);
    walker.chooseConservativeFields(match);
    match.setOverload(overload);
    match.setRegExpOperand(regExpObject);
    match.setStringOperand(string);
    match.setPattern(regExp.pattern);
    match.setRegExpFlags(regExp.flags);
    match.setIsPrivate(regExp.isPrivate);
    match.setCount(std::max<uint32_t>(mentions->numberOfParts(), storesLastIndex));
    if (Reference stored = storesLastIndex ? findStoredLastIndexInResultsInParts(walker, regExpObject) : Reference())
        match.setLastIndexOperand(stored);

    Draft whole = draftOfWholeOfMatchInResultsInParts(walker, string, origin);
    whole.setPattern(regExp.pattern);
    whole.setRegExpFlags(regExp.flags);

    return buildMatchOnTwoWaysInResultsInParts(walker, call, WTF::move(match), WTF::move(whole), { string, *mentions, WTF::move(*groups), keepsPattern ? summary.groupsPresentInEveryMatch : 0, summary.numSubpatterns + 1, storesLastIndex && keepsPattern ? regExpObject : Reference() });
}

static uint64_t keyOfOpen(const Walker& walker, ValueID open)
{
    return static_cast<uint64_t>(walker.currentFunction().index()) << 32 | open.index();
}

static bool isConstantMemberOf(Walker& walker, BuiltinID holder, BuiltinName name)
{
    std::optional<BuiltinID> member = name ? findMemberAboveForIteration(walker, holder, name) : std::nullopt;
    return member && *member;
}

static bool engineStepsMatches(Walker& walker)
{
    std::optional<bool>& answer = ensureResultPartsAnalysis(walker).engineStepsMatches;
    if (answer)
        return *answer;
    const BuiltinTable& table = walker.program().builtins();
    const BuiltinID regExps = table.at(BuiltinClasses::RegExp).prototype;
    const BuiltinID iterators = table.at(BuiltinClasses::RegExpStringIterator).prototype;
    answer = [&] {
        for (ASCIILiteral name : { "exec"_s, "flags"_s, "global"_s, "ignoreCase"_s, "multiline"_s, "dotAll"_s, "unicode"_s, "unicodeSets"_s, "sticky"_s, "hasIndices"_s, "constructor"_s }) {
            if (!isConstantMemberOf(walker, regExps, nameInTableForResultsInParts(walker, name)))
                return false;
        }
        for (WellKnownSymbol symbol : { WellKnownSymbol::Match, WellKnownSymbol::MatchAll }) {
            if (!isConstantMemberOf(walker, regExps, table.nameOf(symbol)))
                return false;
        }
        std::optional<BuiltinID> constructor = findMemberAboveForIteration(walker, regExps, nameInTableForResultsInParts(walker, "constructor"_s));
        if (!isConstantMemberOf(walker, table.canonicalID(*constructor), table.nameOf(WellKnownSymbol::Species)))
            return false;
        if (!isConstantMemberOf(walker, iterators, BuiltinNames::next) || !isConstantMemberOf(walker, iterators, table.nameOf(WellKnownSymbol::Iterator)))
            return false;
        std::optional<BuiltinID> returns = findMemberAboveForIteration(walker, iterators, BuiltinNames::returnKeyword);
        return returns && !*returns;
    }();
    return *answer;
}

static std::optional<LoopOverResult> findLoopOverResult(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID openID = index.onlyUsingValue(call.id());
    while (openID && !index.isParameter(openID) && opcodesThatStateTypes.contains(index.opcodeOf(openID)))
        openID = index.onlyUsingValue(openID);
    if (!openID || index.isParameter(openID) || index.opcodeOf(openID) != Opcode::IteratorOpen || index.blockOf(openID) != index.blockOf(call.id()))
        return std::nullopt;
    for (uint32_t between = call.id().index() + 1; between < openID.index(); ++between) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(ValueID(between))))
            return std::nullopt;
    }
    auto open = index.value(openID).as<Opcode::IteratorOpen>();
    if (index.stripCasts(open.iterableOperand()) != call.id() || open.iteratorKind() != IteratorKind::Sync || open.isDestructuring() || open.isDelegating())
        return std::nullopt;
    BodyBlock block = index.block(walker.inputBlockOf(call.id()));
    if (walker.walksStretchInsideAnother() || openID.index() - block.firstValue().index() >= walker.endOfStretchBeingWalked())
        return std::nullopt;
    const ValueID step = findSoleLoopHeaderStep(index, openID);
    if (!step || !index.value(step).as<Opcode::IteratorResume>().heldOperands().empty())
        return std::nullopt;
    return LoopOverResult { openID, step };
}

static void openLoopOverResult(Walker& walker, const BodyValue& call, const LoopOverResult& loop, Reference state)
{
    for (uint32_t value = call.id().index(); value <= loop.open.index(); ++value)
        walker.bind(ValueID(value), state);
    BodyBlock block = walker.currentBodyIndex().block(walker.inputBlockOf(call.id()));
    const uint32_t place = loop.open.index() - block.firstValue().index() + 1;
    walker.walkStretch(block.id(), place, place);
}

bool namesMatchAll(Walker& walker, PropertyKeyID key)
{
    BuiltinName name = nameInTableForResultsInParts(walker, "matchAll"_s);
    return name && key == walker.program().keyOf(name);
}

bool matchesAreSteppedByExec(Walker& walker, StringID pattern, OptionSet<RegExpFlags> flags)
{
    RegExpSummaries* summaries = hasMechanismsOfResultsInParts(walker) && walker.has(Mechanism::Inlining) ? walker.regExpSummaries() : nullptr;
    if (!summaries || !flags.contains(RegExpFlags::Global) || flags.contains(RegExpFlags::HasIndices))
        return false;
    Vector<char16_t> buffer;
    const RegExpSummary& summary = summaries->summaryOf(pattern, toYarrFlags(flags), walker.types().viewOf(pattern, buffer));
    return summary.canBeCreated() && summary.minimumSize && engineStepsMatches(walker);
}

bool loopTakesMatchesApart(Walker& walker, ValueID step)
{
    auto& allMentions = mentionsOfResultsOfBody(walker, walker.currentFunction(), walker.currentBodyIndex());
    auto found = allMentions.find(step.index());
    return found != allMentions.end() && found->value.isOnlyTakenApart && !found->value.isReturned && found->value.meetsOthersAt.isEmpty();
}

bool openMatchIteration(Walker& walker, const CallByOneRow& matchAll)
{
    if (walker.isUnreachable() || matchAll.isForwarded || matchAll.call.opcode() != Opcode::Call)
        return false;
    if (matchAll.arguments.size() != 1 || matchAll.writtenArguments().size() != 1 || !matchAll.receiver || !isProvenString(walker, walker.typeOf(matchAll.receiver)))
        return false;
    std::optional<LoopOverResult> loop = findLoopOverResult(walker, matchAll.call);
    std::optional<RegExpWrittenForCall> regExp = loop ? findRegExpWrittenForCall(walker, matchAll.call, matchAll.writtenArguments()[0]) : std::nullopt;
    if (!regExp || !matchesAreSteppedByExec(walker, regExp->pattern, regExp->flags))
        return false;
    const Reference matcher = regExp->object;
    auto& iterations = ensureResultPartsAnalysis(walker).matchIterations.add(keyOfOpen(walker, loop->open), Vector<MatchIteration, 1>()).iterator->value;
    iterations.removeAllMatching([&](const MatchIteration& earlier) { return earlier.regExpObject == matcher; });
    iterations.append({ matcher, matchAll.receiver });
    walker.count(StatisticID::MatchIterationsInlined);
    openLoopOverResult(walker, matchAll.call, *loop, matcher);
    return true;
}

static Reference buildResultsOfStep(Walker& walker, Reference done, Reference value, Origin origin)
{
    if (!done || !value || walker.isUnreachable())
        return { };
    Draft results(Opcode::Results, origin);
    walker.chooseConservativeFields(results);
    results.setValuesOperands(std::array { done, value });
    return walker.build(WTF::move(results));
}

static Reference booleanInResultsInParts(Walker& walker, bool value)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(value);
    return walker.constant(WTF::move(constant));
}

static bool openSplitIteration(Walker& walker, const CallByOneRow& split, StringID separator)
{
    if (!walker.has(Mechanism::Inlining) || !isArrayIteratorProtocolIntact(walker) || !isArrayPrototypeChainSane(walker) || walker.program().admitsReplacedBuiltins)
        return false;
    std::optional<LoopOverResult> loop = findLoopOverResult(walker, split.call);
    if (!loop || separator == walker.program().strings.emptyString())
        return false;
    if (textKnownForStrings(walker, split.receiver))
        return false;
    Walker::IndexedIteration iteration;
    iteration.otherForm = Walker::IndexedIteration::OtherForm::PiecesOfSplit;
    iteration.contents = Walker::IndexedIteration::Contents::Text;
    iteration.list = split.receiver;
    iteration.subject = split.arguments[0];
    iteration.position = integerForStrings(walker, 0);
    iteration.inputHead = walker.currentBodyIndex().blockOf(loop->step);
    walker.count(StatisticID::SplitIterationsInlined);
    openLoopOverResult(walker, split.call, *loop, walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static Reference buildSearchForSeparator(Walker& walker, Reference string, StringID separator, Reference position, Origin origin)
{
    Vector<char16_t> buffer;
    StringView text = walker.types().viewOf(separator, buffer);
    Draft search(text.length() == 1 ? Opcode::StringIndexOfCharacter : Opcode::StringIndexOfLiteral, origin);
    walker.chooseConservativeFields(search);
    search.setStringOperand(string);
    search.setPositionOperand(position);
    if (text.length() == 1)
        search.setCodeUnit(text[0]);
    else
        search.setLiteral(separator);
    return walker.build(WTF::move(search));
}

void stepOverPiecesOfString(Walker& walker, Reference string, Reference separator, Reference enteredPosition, Origin origin, const ScopedLambda<void()>& finish, const ScopedLambda<void(Reference, Reference)>& give)
{
    const StringID literal = textKnownForStrings(walker, separator);
    if (!literal) {
        walker.fail("the separator of an iteration over pieces is not known"_s);
        return;
    }
    Reference length = buildLengthOfString(walker, string, origin);
    Reference hasPiece = length ? buildOperatorForStrings(walker, Opcode::LessEq, enteredPosition, length, origin) : Reference();
    if (!hasPiece || walker.isUnreachable())
        return;
    BlockID finished = walker.addBlock();
    BlockID more = walker.addBlock();
    walker.branch(hasPiece, more, finished);
    TypeArena& types = walker.types();

    walker.continueIn(finished);
    finish();
    if (walker.hasFailed())
        return;

    walker.continueIn(more);
    Vector<char16_t> buffer;
    const int64_t lengthOfSeparator = walker.types().viewOf(literal, buffer).length();
    Reference position = buildProofBehindTestForStrings(walker, enteredPosition, hasPiece, OperandIndex::LessEq::left, origin);
    Reference found = position ? buildSearchForSeparator(walker, string, literal, position, origin) : Reference();
    Reference isFound = found ? buildOperatorForStrings(walker, Opcode::LessEq, integerForStrings(walker, 0), found, origin) : Reference();
    if (!isFound || walker.isUnreachable())
        return;
    BlockID inner = walker.addBlock();
    BlockID last = walker.addBlock();
    BlockID hasEnd = walker.addBlock();
    Reference end = walker.addParameter(hasEnd, types.integer(Range { 0, INT32_MAX }), Representation::Int32);
    Reference next = walker.addParameter(hasEnd, types.integer(Range { 0, Walker::doneIterationPosition }), Representation::Int64);
    walker.branch(isFound, inner, last);
    walker.continueIn(inner);
    Reference foundInside = buildProofBehindTestForStrings(walker, found, isFound, OperandIndex::LessEq::right, origin);
    Reference behindSeparator = foundInside ? buildOperatorForStrings(walker, Opcode::Add, foundInside, integerForStrings(walker, lengthOfSeparator), origin) : Reference();
    if (behindSeparator)
        walker.jumpTo(hasEnd, std::array { foundInside, behindSeparator });
    walker.continueIn(last);
    walker.jumpTo(hasEnd, std::array { length, integerForStrings(walker, Walker::doneIterationPosition) });

    walker.continueIn(hasEnd);
    noteOrderedPieceBounds(walker, position, end);
    Reference piece = buildPieceOfString(walker, string, position, end, origin);
    if (piece && !walker.isUnreachable())
        give(piece, next);
}

static bool stepSplitIteration(Walker& walker, const BodyValue& step, Reference state)
{
    Origin origin = walker.originOf(step);
    origin.isTailCall = false;
    const Walker::IndexedIteration entered = walker.indexedIteration(state);
    const bool threads = walker.canThreadBranchAfter(step);
    BlockID joined = threads ? BlockID() : walker.addBlock();
    Reference doneMet = threads ? Reference() : walker.addParameter(joined, TypeArena::fromPrimitives(Primitive::Boolean), Representation::Boolean);
    Reference pieceMet = threads ? Reference() : walker.addParameter(joined, { }, Representation::JSValue);
    Reference positionMet = threads ? Reference() : walker.addParameter(joined, walker.types().integer(Range { 0, Walker::doneIterationPosition }), Representation::Int64);
    auto end = [&](bool isDone, Reference piece, Reference next) {
        Reference done = booleanInResultsInParts(walker, isDone);
        if (!threads) {
            walker.jumpTo(joined, std::array { done, piece, next });
            return;
        }
        if (!isDone)
            walker.indexedIteration(state).position = next;
        walker.bind(step.id(), buildResultsOfStep(walker, done, piece, origin));
        if (!walker.threadBranchAfter(step))
            walker.fail("a step of an iteration over pieces does not walk the end of its block"_s);
    };
    stepOverPiecesOfString(walker, entered.list, entered.subject, entered.position, origin, scopedLambda<void()>([&] {
        end(true, walker.constant(PrimitiveKnownToFolding::undefined()), integerForStrings(walker, Walker::doneIterationPosition));
    }), scopedLambda<void(Reference, Reference)>([&](Reference piece, Reference next) {
        end(false, piece, next);
    }));
    if (threads || walker.hasFailed())
        return true;
    walker.continueIn(joined);
    walker.indexedIteration(state).position = positionMet;
    walker.bind(step.id(), buildResultsOfStep(walker, doneMet, pieceMet, origin));
    return true;
}

static BuiltinOverloadID overloadOfMatchAll(const Walker& walker)
{
    const BuiltinTable& table = walker.program().builtins();
    return *table.overloadIDsOf(table.canonicalID(Builtins::StringPrototypeMatchAll)).begin();
}

static std::pair<StringID, OptionSet<RegExpFlags>> patternOfMatcher(const Walker& walker, Reference regExpObject)
{
    const Code& code = walker.code();
    const Operation& made = code.at(underlyingCellOf(code, regExpObject.id()));
    return { made.pattern(), made.regExpFlags() };
}

static Reference buildMatchOfStep(Walker& walker, Reference regExpObject, Reference string, uint32_t numberOfParts, Origin origin)
{
    auto [pattern, flags] = patternOfMatcher(walker, regExpObject);
    Draft match(Opcode::MatchHeldRegExpParts, origin);
    walker.chooseConservativeFields(match);
    match.setOverload(overloadOfMatchAll(walker));
    match.setRegExpOperand(regExpObject);
    match.setStringOperand(string);
    match.setPattern(pattern);
    match.setRegExpFlags(flags);
    match.setIsPrivate(true);
    match.setCount(numberOfParts);
    Reference isFound = walker.build(WTF::move(match));
    if (isFound && !walker.isUnreachable())
        walker.count(StatisticID::RegExpMatchAllocationsElided);
    return isFound;
}

static Draft draftOfEntireStep(Walker& walker, Reference regExpObject, Reference string, Origin origin)
{
    auto [pattern, flags] = patternOfMatcher(walker, regExpObject);
    Draft whole = draftOfWholeOfMatchInResultsInParts(walker, string, origin);
    whole.setPattern(pattern);
    whole.setRegExpFlags(flags);
    return whole;
}

static const RegExpSummary& summaryOfMatcher(Walker& walker, Reference regExpObject)
{
    auto [pattern, flags] = patternOfMatcher(walker, regExpObject);
    Vector<char16_t> buffer;
    return walker.regExpSummaries()->summaryOf(pattern, toYarrFlags(flags), walker.types().viewOf(pattern, buffer));
}

void stepOverMatchesOfString(Walker& walker, Reference string, Reference regExpObject, Origin origin, const ScopedLambda<void()>& finish, const ScopedLambda<void(Reference)>& give)
{
    Reference isFound = buildMatchOfStep(walker, regExpObject, string, summaryOfMatcher(walker, regExpObject).numSubpatterns + 1, origin);
    if (!isFound || walker.isUnreachable())
        return;
    BlockID present = walker.addBlock();
    BlockID absent = walker.addBlock();
    walker.branch(isFound, present, absent);
    walker.continueIn(absent);
    finish();
    if (walker.hasFailed())
        return;
    walker.continueIn(present);
    Reference array = walker.build(draftOfEntireStep(walker, regExpObject, string, origin));
    if (array && !walker.isUnreachable())
        give(array);
}

static bool stepMatchIteration(Walker&, const BodyValue& step, const MatchIteration&);

static bool walkIterationOverString(Walker& walker, const BodyValue& operation)
{
    Reference state = walker.referenceOf(operation.operand(0));
    if (!state || !state.isVirtual())
        return false;
    const Walker::IndexedIteration& iteration = walker.indexedIteration(state);
    const bool takesMatches = iteration.otherForm == Walker::IndexedIteration::OtherForm::MatchesOfRegExp;
    if (iteration.hasProtocolFallback || (!takesMatches && iteration.otherForm != Walker::IndexedIteration::OtherForm::PiecesOfSplit))
        return false;
    if (operation.opcode() == Opcode::IteratorClose)
        return true;
    return takesMatches ? stepMatchIteration(walker, operation, MatchIteration { iteration.subject, iteration.list }) : stepSplitIteration(walker, operation, state);
}

static bool stepMatchIteration(Walker& walker, const BodyValue& step, const MatchIteration& iteration)
{
    Origin origin = walker.originOf(step);
    origin.isTailCall = false;
    const RegExpSummary& summary = summaryOfMatcher(walker, iteration.regExpObject);
    auto& allMentions = mentionsOfResultsOfBody(walker, walker.currentFunction(), walker.currentBodyIndex());
    auto found = allMentions.find(step.id().index());
    MentionsOfResultInParts mentions = found == allMentions.end() ? MentionsOfResultInParts() : found->value;
    if (found == allMentions.end() || mentions.isReturned || !mentions.meetsOthersAt.isEmpty())
        mentions.isOnlyTakenApart = false;
    std::optional<Vector<uint32_t, 2>> groups;
    if (mentions.isOnlyTakenApart)
        groups = addReadGroupSlots(walker, mentions, summary);
    if (!groups || mentions.numberOfParts() > summary.numSubpatterns + 1)
        mentions.isOnlyTakenApart = false;

    const bool threads = walker.canThreadBranchAfter(step);
    Reference isFound = buildMatchOfStep(walker, iteration.regExpObject, iteration.string, mentions.isOnlyTakenApart ? std::max<uint32_t>(mentions.numberOfParts(), 1) : summary.numSubpatterns + 1, origin);
    if (!isFound || walker.isUnreachable())
        return true;
    BlockID present = walker.addBlock();
    BlockID absent = walker.addBlock();
    BlockID joined = threads ? BlockID() : walker.addBlock();
    Reference doneMet = threads ? Reference() : walker.addParameter(joined, TypeArena::fromPrimitives(Primitive::Boolean), Representation::Boolean);
    Reference valueMet = threads ? Reference() : walker.addParameter(joined, { }, Representation::JSValue);
    walker.branch(isFound, present, absent);

    walker.continueIn(absent);
    Reference done = booleanInResultsInParts(walker, true);
    Reference nothing = walker.constant(PrimitiveKnownToFolding::undefined());
    if (threads) {
        walker.bind(step.id(), buildResultsOfStep(walker, done, nothing, origin));
        if (!walker.threadBranchAfter(step)) {
            walker.fail("the end of an iteration over matches does not walk the end of its block"_s);
            return true;
        }
    } else
        walker.jumpTo(joined, std::array { done, nothing });

    walker.continueIn(present);
    Draft whole = draftOfEntireStep(walker, iteration.regExpObject, iteration.string, origin);
    const MatchAskedForInParts asked { iteration.string, mentions, groups ? *groups : Vector<uint32_t, 2>(), summary.groupsPresentInEveryMatch, summary.numSubpatterns + 1 };
    NotesOfResultInParts notes;
    Reference array = mentions.isOnlyTakenApart ? buildMatchArray(walker, WTF::move(whole), asked, notes, origin) : walker.build(WTF::move(whole));
    if (!array || !array.isValue() || walker.isUnreachable())
        return true;
    Reference further = booleanInResultsInParts(walker, false);
    if (!threads) {
        walker.jumpTo(joined, std::array { further, array });
        walker.continueIn(joined);
        walker.bind(step.id(), buildResultsOfStep(walker, doneMet, valueMet, origin));
        return true;
    }
    walker.bind(step.id(), buildResultsOfStep(walker, further, array, origin));
    if (mentions.isOnlyTakenApart)
        noteMatchArray(walker, asked, array, notes, origin);
    if (!walker.threadBranchAfter(step))
        walker.fail("a step of an iteration over matches does not walk the end of its block"_s);
    return true;
}

bool walkMatchIteration(Walker& walker, const BodyValue& operation)
{
    if (walkIterationOverString(walker, operation))
        return true;
    std::unique_ptr<Walker::KeptForBuild>& keptByStrings = walker.keptByStrings();
    if (!keptByStrings || walker.isUnreachable())
        return false;
    std::unique_ptr<Walker::KeptForBuild>& kept = static_cast<NotesOfStrings&>(*keptByStrings).looksAtResultsInParts;
    if (!kept)
        return false;
    auto& looks = static_cast<LooksAtResultsInParts&>(*kept);
    const ValueID open = operation.operand(0);
    if (looks.matchIterations.isEmpty() || !open)
        return false;
    auto found = looks.matchIterations.find(keyOfOpen(walker, open));
    Reference regExpObject = found == looks.matchIterations.end() ? Reference() : walker.referenceOf(open);
    if (!regExpObject || !regExpObject.isValue())
        return false;
    size_t place = found->value.findIf([&](const MatchIteration& iteration) {
        return iteration.regExpObject == regExpObject;
    });
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, regExpObject.id());
    if (place == notFound || code.isConstant(cell) || code.at(cell).opcode != Opcode::NewRegExp)
        return false;
    const MatchIteration iteration = found->value[place];
    return operation.opcode() == Opcode::IteratorClose || stepMatchIteration(walker, operation, iteration);
}

static constexpr size_t maxPiecesOfKnownString = 8;

static bool buildPiecesOfKnownString(Walker& walker, const CallByOneRow& call, StringID separator)
{
    if (!walker.has(Mechanism::Folding) || (!walker.isInLoop() && !walker.facts().linked().isCalledInLoops(walker.currentFunction())))
        return false;
    StringID text = textKnownForStrings(walker, call.receiver);
    if (!text)
        return false;
    Vector<char16_t> buffer;
    Vector<char16_t> bufferOfSeparator;
    StringView whole = walker.types().viewOf(text, buffer);
    StringView between = walker.types().viewOf(separator, bufferOfSeparator);
    Vector<std::pair<size_t, size_t>, maxPiecesOfKnownString> bounds;
    for (size_t start = 0;;) {
        size_t end = whole.find(between, static_cast<unsigned>(start));
        if (bounds.size() == maxPiecesOfKnownString)
            return false;
        bounds.append({ start, end == notFound ? whole.length() : end });
        if (end == notFound)
            break;
        start = end + between.length();
    }
    Vector<Reference, maxPiecesOfKnownString> pieces;
    for (auto& [start, end] : bounds) {
        Reference piece;
        if (start == end) {
            Draft empty(Opcode::String);
            empty.setString(walker.program().strings.emptyString());
            piece = walker.constant(WTF::move(empty));
        } else if (PrimitiveKnownToFolding known = pieceOfKnownTextForStrings(walker, text, static_cast<int64_t>(start), static_cast<int64_t>(end)))
            piece = walker.constant(known);
        if (!piece)
            return false;
        pieces.append(piece);
    }
    Origin origin = call.origin;
    origin.isTailCall = false;
    Draft literal(Opcode::NewArray, origin);
    literal.expected = call.expected;
    literal.inputValueGiven = call.call.id();
    literal.setContents(TypeTable::string);
    literal.setElementsOperands(pieces.span());
    walker.chooseConservativeFields(literal);
    Reference list = walker.build(WTF::move(literal));
    if (walker.hasFailed() || walker.isUnreachable())
        return true;
    noteFreshList(walker, list);
    walker.bind(call.call.id(), list);
    walker.count(StatisticID::KnownStringsSplit);
    return true;
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
    if (buildPiecesOfKnownString(walker, call, separator))
        return true;
    if (openSplitIteration(walker, call, separator))
        return true;
    std::optional<MentionsOfResultInParts> mentions = mentionsOfResultOnlyTakenApart(walker, call.call);
    if (!mentions || mentions->readsIndex || mentions->readsInput || mentions->readsGroups)
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

static LooksAtResultsInParts& looksAtResultsInParts(Walker& walker)
{
    return static_cast<LooksAtResultsInParts&>(*notesOfStrings(walker).looksAtResultsInParts);
}

static std::optional<std::pair<Reference, Reference>> buildFrameMatchBounds(Walker& walker, const MatchInFrameForParts& found, Reference index, Origin origin)
{
    Draft bounds(Opcode::FrameMatchBounds, origin);
    walker.chooseConservativeFields(bounds);
    bounds.setMatchOperand(found.match);
    bounds.setIndexOperand(index);
    Reference results = walker.build(WTF::move(bounds));
    if (!results || walker.isUnreachable())
        return std::nullopt;
    Reference start = buildResultOfRangeForStrings(walker, results, 0, origin);
    Reference end = buildResultOfRangeForStrings(walker, results, 1, origin);
    return start && end ? std::optional(std::pair(start, end)) : std::nullopt;
}

bool buildMatchIntoFrame(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const HeldRegExp* regExp, Reference regExpObject, Reference string, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || walker.program().at(walker.unit().function).isCoroutine())
        return false;
    if (walker.program().builtins().at(overload).code.implementation.inlineBuiltin != InlineBuiltin::RegExpExec || !regExpObject || !string || !isProvenString(walker, walker.typeOf(string)))
        return false;
    auto& allMentions = mentionsOfResultsOfBody(walker, walker.currentFunction(), walker.currentBodyIndex());
    auto foundMentions = allMentions.find(call.id().index());
    if (foundMentions == allMentions.end())
        return false;
    const MentionsOfResultInParts mentions = foundMentions->value;
    if (mentions.escapes || mentions.isReturned || !mentions.meetsOthersAt.isEmpty() || !mentions.isReadByIndex || mentions.numberOfParts() > 1 || mentions.readsGroups)
        return false;
    TypeArena& types = walker.types();
    Draft whole(Opcode::NewArrayOfRegExpMatch, origin);
    walker.chooseConservativeFields(whole);
    whole.setContents(types.withPrimitives(TypeTable::string, Primitive::Undefined));
    whole.setStringOperand(string);
    whole.setRegExpOperand(regExpObject);
    TypeID typeOfWhole = walker.resultTypeOf(whole);
    TypeID widest = walker.isInLoop() ? widestTypeOfOperation(walker, call) : TypeID();
    if (!typeOfWhole || (widest && !types.isSubset(types.withPrimitives(typeOfWhole, Primitive::Null), widest)))
        return false;
    if (!endOfBlockAsksWhetherThereIsResult(walker, call) || !walker.canThreadBranchAfterMatch(call))
        return false;

    Draft match(Opcode::MatchRegExpIntoFrame, origin);
    walker.chooseConservativeFields(match);
    match.setOverload(overload);
    match.setRegExpOperand(regExpObject);
    match.setStringOperand(string);
    std::optional<uint32_t> knownLength;
    if (regExp) {
        walker.count(StatisticID::KnownPatternsMatchedIntoFrame);
        match.setPattern(regExp->pattern);
        match.setRegExpFlags(regExp->flags);
        match.setIsPrivate(regExp->isPrivate);
        if (RegExpSummaries* summaries = regExp->isPrivate || !walker.facts().mayRecompileRegExps() ? walker.regExpSummaries() : nullptr) {
            Vector<char16_t> buffer;
            const RegExpSummary& summary = summaries->summaryOf(regExp->pattern, toYarrFlags(regExp->flags), types.viewOf(regExp->pattern, buffer));
            if (summary.canBeCreated())
                knownLength = summary.numSubpatterns + 1;
        }
    }
    MatchInFrameForParts found { walker.build(WTF::move(match)), string };
    if (!found.match || walker.isUnreachable())
        return true;
    Draft test(Opcode::IsMatchInFrame, origin);
    walker.chooseConservativeFields(test);
    test.setMatchOperand(found.match);
    Reference isFound = walker.build(WTF::move(test));
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
    whole.setMatchOperand(found.match);
    Reference numberOfParts;
    if (knownLength)
        numberOfParts = integerForStrings(walker, *knownLength);
    else {
        Draft length(Opcode::FrameMatchLength, origin);
        walker.chooseConservativeFields(length);
        length.setMatchOperand(found.match);
        numberOfParts = walker.build(WTF::move(length));
    }
    if (!numberOfParts || walker.isUnreachable())
        return true;
    Reference array = walker.build(WTF::move(whole));
    NotesOfResultInParts notes;
    if (mentions.placesRead || mentions.readsIndex) {
        auto bounds = buildFrameMatchBounds(walker, found, integerForStrings(walker, 0), origin);
        if (Reference piece = bounds && mentions.placesRead ? buildPieceOrUndefinedInResultsInParts(walker, string, bounds->first, bounds->second, origin) : Reference())
            notes.append({ integerForStrings(walker, 0), piece });
        if (Reference index = bounds && mentions.readsIndex ? buildOperatorForStrings(walker, Opcode::BitAnd, bounds->first, integerForStrings(walker, std::numeric_limits<int32_t>::max()), origin) : Reference())
            notes.append({ keyOfNameInResultsInParts(walker, nameInTableForResultsInParts(walker, "index"_s)), index });
    }
    if (BuiltinName input = nameInTableForResultsInParts(walker, "input"_s))
        notes.append({ keyOfNameInResultsInParts(walker, input), string });
    notes.append({ keyOfNameInResultsInParts(walker, BuiltinNames::length), numberOfParts });
    if (!array || !array.isValue() || walker.isUnreachable())
        return true;
    Reference cell = Reference::value(underlyingCellOf(walker.code(), array.id()));
    for (auto& [key, value] : notes)
        walker.state().noteElement(cell, key, value);
    walker.state().noteElementsLieOutsideViews(cell);
    walker.state().noteFresh(cell);
    looksAtResultsInParts(walker).matchesInFrame.set(cell.id().index(), found);
    walker.bind(call.id(), array);
    if (!walker.threadBranchAfter(call))
        walker.fail("the way of a match that finds something does not walk the end of its block"_s);
    return true;
}

static ValueID findOnlyLengthRead(const Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = call.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && !index.isParameter(user) && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(name);
    }
    if (!user || index.isParameter(user) || index.opcodeOf(user) != Opcode::PropGet)
        return { };
    BodyValue read = index.value(user);
    return !read.isFreeName() && read.objectOperand() == name && walker.program().nameClassOf(read.key()) == NameClass::Length ? user : ValueID();
}

ValueID findLengthReadAfterReplace(const Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::span<const ValueID> arguments = call.argumentsOperands();
    const ValueID replacement = arguments.size() == 2 ? index.stripCasts(arguments[1]) : ValueID();
    if (!replacement || index.isParameter(replacement) || index.opcodeOf(replacement) != Opcode::String || index.value(replacement).as<Opcode::String>().string() != walker.program().strings.emptyString())
        return { };
    const ValueID read = findOnlyLengthRead(walker, call);
    if (!read || index.blockOf(read) != index.blockOf(call.id()) || read.index() - index.block(index.blockOf(read)).firstValue().index() >= walker.endOfStretchBeingWalked())
        return { };
    for (uint32_t number = call.id().index() + 1; number < read.index(); ++number) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(ValueID(number))))
            return { };
    }
    return read;
}

bool buildLengthWithoutMatches(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, Reference string, Reference regExpObject, Origin origin)
{
    if (!hasMechanismsOfResultsInParts(walker) || walker.isUnreachable() || !string || !regExpObject || !isProvenString(walker, walker.typeOf(string)) || !findOnlyLengthRead(walker, call))
        return false;
    Draft length(Opcode::StringLengthWithoutMatches, origin);
    walker.chooseConservativeFields(length);
    length.setOverload(overload);
    length.setStringOperand(string);
    length.setRegExpOperand(regExpObject);
    Reference lengthOfRest = walker.build(WTF::move(length));
    if (!lengthOfRest || walker.isUnreachable())
        return true;
    Reference rest = buildPieceOfString(walker, string, integerForStrings(walker, 0), lengthOfRest, origin);
    if (!rest || walker.isUnreachable())
        return true;
    walker.count(StatisticID::StringsWithoutMatchesElided);
    walker.bind(call.id(), rest);
    return true;
}

static ALWAYS_INLINE Reference wholeBesidePartsNamedBy(Walker& walker, Reference name)
{
    if (!name || !name.isValue() || !walker.state().holdsFreshObjects())
        return { };
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, name.id());
    if (code.isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return { };
    static constexpr OpcodeSet arraysWithParts { Opcode::NewRegExpMatchArray, Opcode::GetMatchArrayGroups, Opcode::NewStringSplitArray, Opcode::NewArrayOfRegExpMatch };
    return arraysWithParts.contains(code.at(cell).opcode) ? Reference::value(cell) : Reference();
}

bool buildFrameMatchElement(Walker& walker, const BodyValue& read, const InputsOfTypedOperation& inputs)
{
    if (inputs.areOnlyTypes() || walker.isUnreachable())
        return false;
    Reference whole = wholeBesidePartsNamedBy(walker, walker.referenceOf(read.objectOperand()));
    if (!whole || walker.code().at(whole.id()).opcode != Opcode::NewArrayOfRegExpMatch)
        return false;
    Reference index = walker.referenceOf(read.propertyOperand());
    auto& matches = looksAtResultsInParts(walker).matchesInFrame;
    auto match = matches.find(whole.id().index());
    if (match == matches.end() || !walker.types().at(walker.typeOf(index)).isInteger())
        return false;

    const MatchInFrameForParts found = match->value;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    bool testsElement = endOfBlockAsksWhetherThereIsResult(walker, read) && walker.canThreadBranchAfterMatch(read);
    auto bounds = buildFrameMatchBounds(walker, found, index, origin);
    if (!bounds)
        return true;
    auto [start, end] = *bounds;
    if (!testsElement) {
        if (Reference piece = buildPieceOrUndefinedInResultsInParts(walker, found.string, start, end, origin))
            walker.bind(read.id(), piece);
        return true;
    }

    Reference isPresent = buildOperatorForStrings(walker, Opcode::LessEq, integerForStrings(walker, 0), start, origin);
    if (!isPresent || walker.isUnreachable())
        return true;
    BlockID present = walker.addBlock();
    BlockID absent = walker.addBlock();
    walker.branch(isPresent, present, absent);

    walker.continueIn(absent);
    walker.bind(read.id(), walker.constant(PrimitiveKnownToFolding::undefined()));
    if (!walker.threadBranchAfter(read)) {
        walker.fail("the way of a group that took no part does not walk the end of its block"_s);
        return true;
    }

    walker.continueIn(present);
    noteOrderedPieceBounds(walker, start, end);
    Reference piece = buildPieceOfString(walker, found.string, start, end, origin);
    if (!piece || walker.isUnreachable())
        return true;
    walker.bind(read.id(), piece);
    if (!walker.threadBranchAfter(read))
        walker.fail("the way of a group that took part does not walk the end of its block"_s);
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference propertyOfResultTakenInParts(Walker& walker, Reference name, PropertyKeyID key)
{
    bool isOnlyTakenApart = partsOfListThatIsOnlyTakenApart(walker, name);
    Reference whole = wholeBesidePartsNamedBy(walker, name);
    if ((!isOnlyTakenApart && !whole) || liesInThread(key))
        return { };
    if (whole && walker.code().at(whole.id()).opcode == Opcode::GetMatchArrayGroups) {
        const PropertyKey& name = walker.types().at(key);
        return name.kind == PropertyKeyKind::Text ? walker.state().element(whole, stringKeyInResultsInParts(walker, name.text)) : Reference();
    }
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
