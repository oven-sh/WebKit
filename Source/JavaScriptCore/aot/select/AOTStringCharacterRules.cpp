/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTEffectsDeclared.h"
#include "AOTLoopEntry.h"
#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTStringRuleSupport.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The string is flattened on the entry of a loop, or without a loop in front of the first of its readers. There the rewrite of flatteningReader, which resolves ropes, makes the flat string.
struct Pair {
    BlockID header;
    ValueID string;
    ValueID firstReader { };
    BlockID blockOfFirstReader { };
    ValueID flatteningReader { };
    bool hasLikelyReader { false };
    bool hasResolvingReader { false };
    bool resolvesUnderOtherHandler { false };
    bool hasFlatteningReader { false };
    ValueID flat { };
};

struct PairTable {
    Vector<Pair> pairs;
    HashMap<std::pair<BlockID, ValueID>, uint32_t> indexes;
    HashMap<BlockID, bool> suspendingLoops;
};

struct Match {
    SelectionGroup group;
    ValueID string;
    std::optional<CharacterRead> read { };
    std::optional<uint32_t> pair { };
    // Its effects admit a FlattenString in its place.
    bool resolvesRopes { false };
    // It looks at characters, so it gains from a flat string. A piece cut out of a string does not: it only names the string it is cut from.
    bool readsCharacters { false };
};

struct PlacedReader {
    uint32_t reader;
    ProgramPoint point;
};

struct PlacedPair {
    uint32_t pair;
    ProgramPoint place;
};

} // anonymous namespace

static constexpr OpcodeSet readersWithoutRow { Opcode::CallBuiltin, Opcode::ElemGet };
static constexpr OpcodeSet readersThatOnlyCutPieces { Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::StringRangeSubstringOrUndefined };

static constexpr EffectsOnEdges effectsOfFlattening = EffectsOnEdges::onBothEdges(declaredBoundOf(Opcode::FlattenString));

static std::optional<uint32_t> stringOperandIndexOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::CallBuiltin:
        return OperandIndex::CallBuiltin::receiver;
    case Opcode::ElemGet:
        return OperandIndex::ElemGet::object;
    default:
        return OperandPosition::string(opcode);
    }
}

static std::optional<Match> findReader(SelectionContext& context, BlockID block, ValueID id)
{
    if (context.isOnSlowPath(id))
        return std::nullopt;

    const Value& value = context.function().at(id);
    std::optional<uint32_t> operandIndex = stringOperandIndexOf(value.opcode);
    if (!operandIndex)
        return std::nullopt;
    ValueID string = value.operand(*operandIndex);
    if (!string || !context.typeOf(string).isAnyString())
        return std::nullopt;

    Match reader { groupOfOne(id), string };
    if (!readersWithoutRow.contains(value.opcode))
        return reader;

    reader.read = characterReadOf(context, Use { .kind = Use::Kind::Operand, .block = block, .user = id, .operandIndex = *operandIndex });
    if (!reader.read || reader.read->needsBounds)
        return std::nullopt;
    return reader;
}

static bool resolvesRopes(SelectionContext& context, ValueID reader)
{
    return effectsOfFlattening.isSubsetOf(context.analyses().effectsOf(reader));
}

static bool suspends(SelectionContext& context, BlockID header)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const NaturalLoop* loop = analyses.naturalLoops().headerOf(header);
    RELEASE_ASSERT(loop);

    for (unsigned index = 0; index < loop->size(); ++index) {
        BlockID block = loop->at(index);
        if (declaredBoundOf(function.at(block).terminator.kind).maySuspend && analyses.effectsOfTerminator(block).normal.maySuspend)
            return true;
        for (ValueID id : function.at(block).values) {
            if (maySuspend(function.at(id).opcode) && analyses.maySuspend(id))
                return true;
        }
    }
    return false;
}

static std::optional<BlockID> findLoopWithoutSuspension(SelectionContext& context, PairTable& table, BlockID user, BlockID outermostHeader)
{
    const NaturalLoops& loops = context.analyses().naturalLoops();
    std::optional<BlockID> header;
    // What is invariant in a loop is invariant in the loops inside it, and a loop around a suspending loop suspends too.
    for (const NaturalLoop* loop = loops.innerMostLoopOf(user); loop; loop = loops.innerMostOuterLoop(*loop)) {
        bool loopSuspends = table.suspendingLoops.ensure(loop->header(), [&] {
            return suspends(context, loop->header());
        }).iterator->value;
        if (loopSuspends)
            break;
        header = loop->header();
        if (loop->header() == outermostHeader)
            break;
    }
    return header;
}

// The reader has a string. Outside the loop the same value may have a wider type, which the loop entry tests.
static std::optional<InvariantOperand> findInvariantString(SelectionContext& context, BlockID block, ValueID string)
{
    if (!context.analyses().cfg().hasCycle() || !context.analyses().naturalLoops().innerMostLoopOf(block))
        return std::nullopt;

    auto mayBeString = [&](ValueID candidate) {
        return !!(context.typeOf(candidate).primitives & Primitive::AnyString);
    };
    if (std::optional<InvariantOperand> invariant = findOutermostInvariantLoop(context, block, string, mayBeString))
        return invariant;
    return findOutermostInvariantLoop(context, block, context.analyses().identityOf(string), mayBeString);
}

static std::optional<uint32_t> findOrAddPair(SelectionContext& context, PairTable& table, BlockID block, ValueID string)
{
    std::optional<InvariantOperand> invariant = findInvariantString(context, block, string);
    if (!invariant)
        return std::nullopt;
    std::optional<BlockID> header = findLoopWithoutSuspension(context, table, block, invariant->header);
    if (!header)
        return std::nullopt;

    ValueID object = context.analyses().identityOf(invariant->value);
    auto entry = table.indexes.add(std::pair { *header, object }, static_cast<uint32_t>(table.pairs.size()));
    if (entry.isNewEntry) {
        table.pairs.append(Pair { *header, invariant->value });
        return entry.iterator->value;
    }

    Pair& pair = table.pairs[entry.iterator->value];
    if (!context.typeOf(pair.string).isAnyString() && context.typeOf(invariant->value).isAnyString())
        pair.string = invariant->value;
    return entry.iterator->value;
}

static Vector<Match> collectReaders(SelectionContext& context, PairTable& table)
{
    const Function& function = context.function();

    Vector<Match> readers;
    // The schema lists the readers: a rewrite may create FlattenString only in place of one of them.
    for (Opcode opcode : replacedOperationsOf(Opcode::FlattenString)) {
        for (ValueID id : context.candidates(opcode)) {
            BlockID block = context.analyses().useLists().blockOf(id);
            std::optional<Match> reader = findReader(context, block, id);
            if (!reader)
                continue;

            reader->pair = findOrAddPair(context, table, block, reader->string);
            reader->resolvesRopes = resolvesRopes(context, id);
            reader->readsCharacters = reader->resolvesRopes && !readersThatOnlyCutPieces.contains(opcode);
            if (reader->pair) {
                Pair& pair = table.pairs[*reader->pair];
                pair.hasLikelyReader |= function.at(block).frequency != BlockFrequency::Rare;
                pair.hasResolvingReader |= reader->resolvesRopes;
                pair.resolvesUnderOtherHandler |= reader->resolvesRopes && function.at(block).handler != function.at(pair.header).handler;
            }
            readers.append(WTF::move(*reader));
        }
    }
    return readers;
}

static bool shouldFlatten(const Pair& pair)
{
    // A rewrite adds no effect, so a rope that no reader resolves is not flattened either. Running out of memory on the loop entry reaches the handler of the header.
    return pair.hasLikelyReader && pair.hasResolvingReader && !pair.resolvesUnderOtherHandler;
}

static void clearUnflattenedPairs(const Vector<Pair>& pairs, Vector<Match>& readers)
{
    for (Match& reader : readers) {
        if (reader.pair && !shouldFlatten(pairs[*reader.pair]))
            reader.pair = std::nullopt;
    }
}

static bool suspendsBetween(SelectionContext& context, ProgramPoint first, ProgramPoint last, bool functionSuspends)
{
    if (!functionSuspends)
        return false;
    if (first.block != last.block)
        return true;
    const Block& block = context.function().at(first.block);
    return std::ranges::any_of(block.values.span().subspan(first.index, last.index - first.index), [&](ValueID id) {
        return maySuspend(context.function().at(id).opcode) && context.analyses().maySuspend(id);
    });
}

static bool suspendsAnywhere(SelectionContext& context)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    return std::ranges::any_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        if (declaredBoundOf(function.at(block).terminator.kind).maySuspend && analyses.effectsOfTerminator(block).normal.maySuspend)
            return true;
        return std::ranges::any_of(function.at(block).values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode) && analyses.maySuspend(id);
        });
    });
}

static bool runsRightBehind(SelectionContext& context, BlockID block, BlockID follower)
{
    if (block == follower)
        return true;
    const Function& function = context.function();
    std::span<const BlockID> predecessors = context.analyses().cfg().predecessors(follower);
    return predecessors.size() == 1 && predecessors[0] == block && function.at(follower).frequency == function.at(block).frequency && function.at(follower).handler == function.at(block).handler;
}

// The first reader that resolves ropes leaves the string flat for every reader it dominates.
static void pairReadersBehindFirstReader(SelectionContext& context, Vector<Pair>& pairs, Vector<Match>& readers)
{
    auto isUnpaired = [](const Match& reader) {
        return !reader.pair;
    };
    auto mayComeFirst = [](const Match& reader) {
        return !reader.pair && reader.resolvesRopes;
    };
    if (std::ranges::count_if(readers, isUnpaired) < 2 || std::ranges::none_of(readers, mayComeFirst))
        return;

    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();

    HashMap<ValueID, Vector<PlacedReader, 4>> readersOfString;
    for (uint32_t index = 0; index < readers.size(); ++index) {
        const Match& reader = readers[index];
        if (reader.pair)
            continue;
        ValueLocation location = analyses.useLists().locationOf(reader.group.root);
        if (location.placement != ValuePlacement::InBlock)
            continue;
        readersOfString.ensure(analyses.identityOf(reader.string), [] {
            return Vector<PlacedReader, 4>();
        }).iterator->value.append(PlacedReader { index, { location.block, location.index } });
    }

    std::optional<bool> functionSuspends;
    for (auto& remaining : readersOfString.values()) {
        AOT::sort(remaining, { }, [&](const PlacedReader& placed) {
            return std::pair { cfg.reversePostOrderIndex(placed.point.block), placed.point.index };
        });
        while (remaining.size() > 1) {
            PlacedReader first = remaining.first();
            remaining.removeAt(0);
            if (!functionSuspends)
                functionSuspends = suspendsAnywhere(context);

            std::optional<uint32_t> flatteningReader;
            if (readers[first.reader].resolvesRopes)
                flatteningReader = first.reader;
            unsigned numberOfLikelyReads = readers[first.reader].readsCharacters;
            for (const PlacedReader& follower : remaining) {
                if (!readers[follower.reader].readsCharacters || context.function().at(follower.point.block).frequency != context.function().at(first.point.block).frequency)
                    continue;
                if (!analyses.dominates(readers[first.reader].group.root, follower.point) || suspendsBetween(context, first.point, follower.point, *functionSuspends))
                    continue;
                ++numberOfLikelyReads;
                if (!flatteningReader && runsRightBehind(context, first.point.block, follower.point.block))
                    flatteningReader = follower.reader;
            }
            // One read alone flattens nothing.
            if (!flatteningReader || numberOfLikelyReads < 2)
                continue;

            uint32_t pair = static_cast<uint32_t>(pairs.size());
            bool hasFollower = remaining.removeAllMatching([&](const PlacedReader& follower) {
                if (!analyses.dominates(readers[first.reader].group.root, follower.point) || suspendsBetween(context, first.point, follower.point, *functionSuspends))
                    return false;
                readers[follower.reader].pair = pair;
                return true;
            });
            if (!hasFollower)
                continue;
            readers[first.reader].pair = pair;
            pairs.append(Pair { .header = { }, .string = readers[first.reader].string, .firstReader = readers[first.reader].group.root, .blockOfFirstReader = first.point.block, .flatteningReader = readers[*flatteningReader].group.root });
        }
    }
}

static ProgramPoint placeOf(SelectionContext& context, const Pair& pair)
{
    return pair.firstReader ? context.analyses().useLists().pointOf(pair.firstReader) : ProgramPoint::atHeadOf(pair.header);
}

static bool hasReaders(const Pair& pair)
{
    return pair.firstReader || shouldFlatten(pair);
}

static bool isMade(SelectionContext& context, const Pair& pair)
{
    return pair.firstReader || (shouldFlatten(pair) && canEmitOnLoopEntry(context, pair.header));
}

// The flat string of a loop is defined on the only entry of the header, so it reaches all that the header dominates, the code behind the loop included.
static bool reaches(SelectionContext& context, const Pair& pair, ProgramPoint point)
{
    FunctionAnalyses& analyses = context.analyses();
    if (pair.firstReader)
        return analyses.dominates(pair.firstReader, point);
    return pair.header != point.block && analyses.dominators().dominates(pair.header, point.block);
}

// A string that was flattened stays flat: a place that the flattening of the same string dominates takes that flat string.
static void sharePairsOfOneString(SelectionContext& context, Vector<Pair>& pairs, Vector<Match>& readers)
{
    // A flat string does not live across a suspension.
    if (pairs.isEmpty() || suspendsAnywhere(context))
        return;

    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();

    Vector<PlacedPair, 8> placedPairs;
    for (uint32_t index = 0; index < pairs.size(); ++index) {
        if (hasReaders(pairs[index]))
            placedPairs.append(PlacedPair { index, placeOf(context, pairs[index]) });
    }
    // What dominates a place comes before it.
    AOT::sort(placedPairs, { }, [&](const PlacedPair& placed) {
        return std::tuple { cfg.reversePostOrderIndex(placed.place.block), placed.place.index, placed.pair };
    });

    HashMap<ValueID, Vector<uint32_t, 2>> flattenedPairsOfString;
    auto findPairThatReaches = [&](ValueID string, ProgramPoint point) -> std::optional<uint32_t> {
        auto flattenedPairs = flattenedPairsOfString.find(analyses.identityOf(string));
        if (flattenedPairs == flattenedPairsOfString.end())
            return std::nullopt;
        for (uint32_t pair : flattenedPairs->value) {
            if (reaches(context, pairs[pair], point))
                return pair;
        }
        return std::nullopt;
    };

    Vector<std::optional<uint32_t>> sharedWith(pairs.size());
    for (const PlacedPair& placed : placedPairs) {
        const Pair& pair = pairs[placed.pair];
        sharedWith[placed.pair] = findPairThatReaches(pair.string, placed.place);
        if (sharedWith[placed.pair] || !isMade(context, pair))
            continue;
        flattenedPairsOfString.ensure(analyses.identityOf(pair.string), [] {
            return Vector<uint32_t, 2>();
        }).iterator->value.append(placed.pair);
    }

    for (Match& reader : readers) {
        if (reader.pair) {
            if (std::optional<uint32_t> shared = sharedWith[*reader.pair])
                reader.pair = shared;
            continue;
        }
        ValueLocation location = analyses.useLists().locationOf(reader.group.root);
        if (location.placement == ValuePlacement::InBlock)
            reader.pair = findPairThatReaches(reader.string, { location.block, location.index });
    }
}

// A reader runs only if the value is a string, so the other path has no flat string to provide.
static ValueID appendFlattenIfString(IRBuilder& builder, ValueID value)
{
    Function& function = builder.function();
    BlockFrequency frequency = function.at(builder.currentBlock()).frequency;
    ValueID test = builder.appendTypeTest(value, TypeTable::string);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, rarerOf(frequency, BlockFrequency::Unlikely));

    ValueID flat = builder.append(Opcode::FlattenString, { branch.provenOnTaken });
    TypeID flatStringType = function.at(flat).type;
    BlockID join = builder.addBlock(frequency);
    ValueID result = builder.addParameter(join, flatStringType);
    builder.emitJump(join, Arguments { flat });

    builder.moveToEndOf(branch.notTaken);
    Value unspecified = IRBuilder::make(Opcode::Unspecified);
    unspecified.setValueType(flatStringType);
    builder.emitJump(join, Arguments { builder.append(WTF::move(unspecified)) });

    builder.moveToEndOf(join);
    return result;
}

static void emitFlatString(SelectionContext& context, GroupRewriter& rewriter, Pair& pair)
{
    if (pair.firstReader) {
        Value flatten = IRBuilder::make(Opcode::FlattenString, { pair.string });
        if (pair.firstReader == pair.flatteningReader) {
            pair.flat = rewriter.insert(WTF::move(flatten));
            return;
        }
        size_t index = rewriter.function().at(pair.blockOfFirstReader).values.find(pair.firstReader);
        RELEASE_ASSERT(index != notFound);
        pair.flat = rewriter.builder().insertBefore(ProgramPoint { pair.blockOfFirstReader, static_cast<uint32_t>(index) }, WTF::move(flatten));
        return;
    }
    bool isString = context.typeOf(pair.string).isAnyString();
    emitOnLoopEntry(context, rewriter.builder(), pair.header, [&](IRBuilder& atEntry) {
        pair.flat = isString ? atEntry.append(Opcode::FlattenString, { pair.string }) : appendFlattenIfString(atEntry, pair.string);
    });
}

static void rewriteReader(SelectionContext& context, GroupRewriter& rewriter, const Match& match, ValueID flat)
{
    ValueID root = match.group.root;
    if (!match.read) {
        Value& row = rewriter.function().edit(root);
        if (flat) {
            row.setStringOperand(flat);
            context.analyses().didChangeValues();
        }
        if (row.opcode == Opcode::Substring)
            rewriter.leaveUnclaimed(root);
        return;
    }

    CharacterRead read = *match.read;
    if (!read.position)
        read.position = rewriter.insert(makeInteger(0));
    Value row = makeCharacterRead(read, flat ? flat : match.string, std::nullopt);
    copyOperands(rewriter.replaceRoot(row.opcode), 0, row, 0);
}

void selectHoistStringCharacters(SelectionContext& context)
{
    PairTable table;
    Vector<Match> readers = collectReaders(context, table);
    Vector<Pair>& pairs = table.pairs;
    clearUnflattenedPairs(pairs, readers);
    pairReadersBehindFirstReader(context, pairs, readers);
    sharePairsOfOneString(context, pairs, readers);
    readers.removeAllMatching([&](const Match& reader) {
        bool isUnchangedRow = !reader.read && !reader.pair;
        return isUnchangedRow || context.tryClaim(SelectionRuleID::HoistStringCharacters, reader.group) != ClaimResult::Claimed;
    });

    // Only a reader that resolves ropes may create FlattenString, and a session may reach another reader of the pair first.
    Vector<Match> flatteningReaders;
    Vector<Match> otherReaders;
    for (Match& reader : readers) {
        bool mayFlatten = reader.pair && reader.resolvesRopes && (!pairs[*reader.pair].firstReader || pairs[*reader.pair].flatteningReader == reader.group.root);
        bool flattens = mayFlatten && !std::exchange(pairs[*reader.pair].hasFlatteningReader, true);
        (flattens ? flatteningReaders : otherReaders).append(WTF::move(reader));
    }

    context.rewriteGroups(SelectionRuleID::HoistStringCharacters, flatteningReaders, [&](GroupRewriter& rewriter, const Match& match) {
        Pair& pair = pairs[*match.pair];
        emitFlatString(context, rewriter, pair);
        rewriteReader(context, rewriter, match, pair.flat);
    });
    context.rewriteGroups(SelectionRuleID::HoistStringCharacters, otherReaders, [&](GroupRewriter& rewriter, const Match& match) {
        rewriteReader(context, rewriter, match, match.pair ? pairs[*match.pair].flat : ValueID());
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
