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
#include "AOTMemoryLocation.h"
#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
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

    // A rewrite adds no effect, so a rope that no reader resolves is not flattened either. Running out of memory on the loop entry reaches the handler of the header.
    bool hasReaders() const { return firstReader || (hasLikelyReader && hasResolvingReader && !resolvesUnderOtherHandler); }
};

struct PairTable {
    Vector<Pair> pairs;
    HashMap<std::pair<BlockID, ValueID>, uint32_t> indexes;
    HashMap<BlockID, bool> suspendingLoops;
};

struct StringCharacterMatch {
    SelectionGroup group;
    ValueID string;
    std::optional<CharacterRead> read { };
    std::optional<uint32_t> pair { };
    // Its effects admit a FlattenString in its place.
    bool resolvesRopes { false };
    // It looks at characters, so it gains from a flat string. A piece cut out of a string does not: it only names the string it is cut from.
    bool readsCharacters { false };
};

struct PlacedIndex {
    uint32_t index;
    ProgramPoint point;
};

} // anonymous namespace

static constexpr OpcodeSet readersWithoutRow { Opcode::CallBuiltin, Opcode::ElemGet };
static constexpr OpcodeSet readersThatOnlyCutPieces { Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::StringRangeSubstringOrUndefined };

static constexpr EffectsOnEdges effectsOfFlatteningString = EffectsOnEdges::onBothEdges(declaredBoundOf(Opcode::FlattenString));

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

static std::optional<StringCharacterMatch> findReader(SelectionContext& context, BlockID block, ValueID id)
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

    StringCharacterMatch reader { groupOfOne(id), string };
    if (!readersWithoutRow.contains(value.opcode))
        return reader;

    reader.read = characterReadOf(context, Use { .kind = Use::Kind::Operand, .block = block, .user = id, .operandIndex = *operandIndex });
    if (!reader.read || reader.read->needsBounds)
        return std::nullopt;
    return reader;
}

static bool suspendsAt(SelectionContext& context, ValueID id)
{
    return maySuspend(context.function().at(id).opcode) && context.analyses().maySuspend(id);
}

static bool suspendsIn(SelectionContext& context, BlockID id)
{
    const Block& block = context.function().at(id);
    if (declaredBoundOf(block.terminator.kind).maySuspend && context.analyses().effectsOfTerminator(id).normal.maySuspend)
        return true;
    return std::ranges::any_of(block.values, [&](ValueID value) {
        return suspendsAt(context, value);
    });
}

static bool suspendsAnywhere(SelectionContext& context)
{
    return std::ranges::any_of(context.analyses().cfg().reversePostOrder(), [&](BlockID block) {
        return suspendsIn(context, block);
    });
}

static bool suspendsBetween(SelectionContext& context, ProgramPoint first, ProgramPoint last, bool functionSuspends)
{
    if (!functionSuspends)
        return false;
    if (first.block != last.block)
        return true;
    return std::ranges::any_of(context.function().at(first.block).values.span().subspan(first.index, last.index - first.index), [&](ValueID id) {
        return suspendsAt(context, id);
    });
}

// A loop that puts its stores off covers what may run out of memory with a handler that makes the stores and throws the error again (EliminateRedundantReads).
// Where that loop is entered no store waits, so what throws there goes where that handler sends the error.
// The loop whose stores wait holds no store to the variable. So where the loop around this one holds one, the stores wait in this one or inside it.
static const OutOfLineOptional<Target>& handlerOfReaderOnEntryOf(SelectionContext& context, const NaturalLoop& loop, BlockID reader)
{
    const Function& function = context.function();
    const OutOfLineOptional<Target>& handler = function.at(reader).handler;
    if (!handler)
        return handler;
    if (!onlyMakesStoresAndRethrows(function, handler->block))
        return handler;
    const Block& cleanup = function.at(handler->block);
    std::span<const ValueID> stores = cleanup.values.span().subspan(1);
    if (const NaturalLoop* around = context.analyses().naturalLoops().innerMostOuterLoop(loop)) {
        MemoryAccess delayed = memoryAccessOf(function, function.at(stores.front()));
        auto storesToSamePlace = [&](ValueID id) {
            MemoryAccess access = memoryAccessOf(function, function.at(id));
            return access.kind == MemoryAccess::Kind::Write && access.location == delayed.location && (!access.base == !delayed.base) && (!access.base || context.analyses().identityOf(access.base) == context.analyses().identityOf(delayed.base));
        };
        bool isStoredAround = false;
        for (unsigned index = 0; index < around->size() && !isStoredAround; ++index)
            isStoredAround = std::ranges::any_of(function.at(around->at(index)).values, storesToSamePlace);
        if (!isStoredAround)
            return handler;
    }
    return cleanup.handler;
}

// A reader that resolves ropes may throw, and so may the flattening that replaces it, so the flattening is hoisted no further than the reader's handler covers.
static std::optional<BlockID> findLoopWithoutSuspension(SelectionContext& context, PairTable& table, BlockID user, BlockID outermostHeader, bool userResolvesRopes)
{
    const Function& function = context.function();
    const NaturalLoops& loops = context.analyses().naturalLoops();
    std::optional<BlockID> header;
    std::optional<BlockID> headerSharingUserHandler;
    // What is invariant in a loop is invariant in the loops inside it, and a loop around a suspending loop suspends too.
    for (const NaturalLoop* loop = loops.innerMostLoopOf(user); loop; loop = loops.innerMostOuterLoop(*loop)) {
        bool loopSuspends = table.suspendingLoops.ensure(loop->header(), [&] {
            for (unsigned index = 0; index < loop->size(); ++index) {
                if (suspendsIn(context, loop->at(index)))
                    return true;
            }
            return false;
        }).iterator->value;
        if (loopSuspends)
            break;
        header = loop->header();
        if (function.at(loop->header()).handler == handlerOfReaderOnEntryOf(context, *loop, user))
            headerSharingUserHandler = header;
        if (loop->header() == outermostHeader)
            break;
    }
    return userResolvesRopes && headerSharingUserHandler ? headerSharingUserHandler : header;
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

static std::optional<uint32_t> findOrAddPair(SelectionContext& context, PairTable& table, BlockID block, ValueID string, bool readerResolvesRopes)
{
    std::optional<InvariantOperand> invariant = findInvariantString(context, block, string);
    if (!invariant)
        return std::nullopt;
    std::optional<BlockID> header = findLoopWithoutSuspension(context, table, block, invariant->header, readerResolvesRopes);
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

static Vector<StringCharacterMatch> collectReaders(SelectionContext& context, PairTable& table)
{
    const Function& function = context.function();

    Vector<StringCharacterMatch> readers;
    // The schema lists the readers: a rewrite may create FlattenString only in place of one of them.
    for (Opcode opcode : replacedOperationsOf(Opcode::FlattenString)) {
        for (ValueID id : context.candidates(opcode)) {
            BlockID block = context.analyses().useLists().blockOf(id);
            std::optional<StringCharacterMatch> reader = findReader(context, block, id);
            if (!reader)
                continue;

            reader->resolvesRopes = effectsOfFlatteningString.isSubsetOf(context.analyses().effectsOf(id));
            reader->pair = findOrAddPair(context, table, block, reader->string, reader->resolvesRopes);
            reader->readsCharacters = reader->resolvesRopes && !readersThatOnlyCutPieces.contains(opcode);
            if (reader->pair) {
                Pair& pair = table.pairs[*reader->pair];
                pair.hasLikelyReader |= function.at(block).frequency.predicted() != BlockFrequency::Rare;
                pair.hasResolvingReader |= reader->resolvesRopes;
                pair.resolvesUnderOtherHandler |= reader->resolvesRopes && handlerOfReaderOnEntryOf(context, *context.analyses().naturalLoops().headerOf(pair.header), block) != function.at(pair.header).handler;
            }
            readers.append(WTF::move(*reader));
        }
    }
    for (StringCharacterMatch& reader : readers) {
        if (reader.pair && !table.pairs[*reader.pair].hasReaders())
            reader.pair = std::nullopt;
    }
    return readers;
}

static bool runsRightBehind(SelectionContext& context, BlockID block, BlockID follower)
{
    if (block == follower)
        return true;
    const Function& function = context.function();
    std::span<const BlockID> predecessors = context.analyses().cfg().predecessors(follower);
    return predecessors.size() == 1 && predecessors[0] == block && function.at(follower).frequency.isSamePredictionAs(function.at(block).frequency) && function.at(follower).handler == function.at(block).handler;
}

// The first reader that resolves ropes leaves the string flat for every reader it dominates.
static void pairReadersBehindFirstReader(SelectionContext& context, Vector<Pair>& pairs, Vector<StringCharacterMatch>& readers)
{
    auto isUnpaired = [](const StringCharacterMatch& reader) {
        return !reader.pair;
    };
    auto mayComeFirst = [](const StringCharacterMatch& reader) {
        return !reader.pair && reader.resolvesRopes;
    };
    if (std::ranges::count_if(readers, isUnpaired) < 2 || std::ranges::none_of(readers, mayComeFirst))
        return;

    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();

    HashMap<ValueID, Vector<PlacedIndex, 4>> readersOfString;
    for (uint32_t index = 0; index < readers.size(); ++index) {
        const StringCharacterMatch& reader = readers[index];
        if (reader.pair)
            continue;
        ValueLocation location = analyses.useLists().locationOf(reader.group.root);
        if (location.placement == ValuePlacement::InBlock)
            readersOfString.add(analyses.identityOf(reader.string), Vector<PlacedIndex, 4>()).iterator->value.append(PlacedIndex { index, { location.block, location.index } });
    }

    std::optional<bool> functionSuspends;
    for (auto& remaining : readersOfString.values()) {
        AOT::sortInLine(remaining, { }, [&](const PlacedIndex& placed) {
            return std::pair { cfg.reversePostOrderIndex(placed.point.block), placed.point.index };
        });
        while (remaining.size() > 1) {
            PlacedIndex first = remaining.first();
            remaining.removeAt(0);
            if (!functionSuspends)
                functionSuspends = suspendsAnywhere(context);

            StringCharacterMatch& firstReader = readers[first.index];
            auto staysFlatUntil = [&](const PlacedIndex& follower) {
                return analyses.dominates(firstReader.group.root, follower.point) && !suspendsBetween(context, first.point, follower.point, *functionSuspends);
            };

            std::optional<uint32_t> flatteningReader;
            if (firstReader.resolvesRopes)
                flatteningReader = first.index;
            unsigned numberOfLikelyReads = firstReader.readsCharacters;
            for (const PlacedIndex& follower : remaining) {
                if (!readers[follower.index].readsCharacters || !function.at(follower.point.block).frequency.isSamePredictionAs(function.at(first.point.block).frequency) || !staysFlatUntil(follower))
                    continue;
                ++numberOfLikelyReads;
                if (!flatteningReader && runsRightBehind(context, first.point.block, follower.point.block))
                    flatteningReader = follower.index;
            }
            // One read alone flattens nothing.
            if (!flatteningReader || numberOfLikelyReads < 2)
                continue;

            uint32_t pair = static_cast<uint32_t>(pairs.size());
            bool hasFollower = remaining.removeAllMatching([&](const PlacedIndex& follower) {
                if (!staysFlatUntil(follower))
                    return false;
                readers[follower.index].pair = pair;
                return true;
            });
            if (!hasFollower)
                continue;
            firstReader.pair = pair;
            ValueID flattening = readers[*flatteningReader].group.root;
            pairs.append(Pair { .header = { }, .string = firstReader.string, .firstReader = firstReader.group.root, .blockOfFirstReader = first.point.block, .flatteningReader = flattening });
        }
    }
}

// The flat string of a loop is defined on the only entry of the header, so it reaches all that the header dominates, the code behind the loop included.
// When the entry gets its own block under the header's handler, the handler gains a predecessor before the header, so code after the handler can be reached without the flat string.
static bool reaches(SelectionContext& context, const Pair& pair, ProgramPoint point)
{
    FunctionAnalyses& analyses = context.analyses();
    if (pair.firstReader)
        return analyses.dominates(pair.firstReader, point);
    if (pair.header == point.block || !analyses.dominators().dominates(pair.header, point.block))
        return false;
    return !loopEntryGetsNewHandlerEdge(context, pair.header) || analyses.naturalLoops().belongsTo(point.block, *analyses.naturalLoops().headerOf(pair.header));
}

// A string that was flattened stays flat: a place that the flattening of the same string dominates takes that flat string.
static void sharePairsOfOneString(SelectionContext& context, Vector<Pair>& pairs, Vector<StringCharacterMatch>& readers)
{
    // A flat string does not live across a suspension.
    if (pairs.isEmpty() || suspendsAnywhere(context))
        return;

    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();

    Vector<PlacedIndex, 8> placedPairs;
    for (uint32_t index = 0; index < pairs.size(); ++index) {
        const Pair& pair = pairs[index];
        if (pair.hasReaders())
            placedPairs.append(PlacedIndex { index, pair.firstReader ? analyses.useLists().pointOf(pair.firstReader) : ProgramPoint::atHeadOf(pair.header) });
    }
    // What dominates a place comes before it.
    AOT::sort(placedPairs, { }, [&](const PlacedIndex& placed) {
        return std::tuple { cfg.reversePostOrderIndex(placed.point.block), placed.point.index, placed.index };
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
    for (const PlacedIndex& placed : placedPairs) {
        const Pair& pair = pairs[placed.index];
        sharedWith[placed.index] = findPairThatReaches(pair.string, placed.point);
        if (!sharedWith[placed.index] && (pair.firstReader || canEmitOnLoopEntry(context, pair.header)))
            flattenedPairsOfString.add(analyses.identityOf(pair.string), Vector<uint32_t, 2>()).iterator->value.append(placed.index);
    }

    for (StringCharacterMatch& reader : readers) {
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
    Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
    ValueID test = builder.appendTypeTest(value, TypeTable::string);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, rarerOf(frequency, BlockFrequency::Unlikely));

    ValueID flat = builder.append(Opcode::FlattenString, { branch.provenOnTaken });
    TypeID flatStringType = function.at(flat).type;
    BlockID join = builder.addBlock(frequency);
    ValueID result = builder.addParameter(join, builder.types().intern(builder.types().at(flatStringType).withPrimitives(Primitive::Unspecified)));
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

static void rewriteReader(SelectionContext& context, GroupRewriter& rewriter, const StringCharacterMatch& match, ValueID flat)
{
    ValueID root = match.group.root;
    if (flat && match.resolvesRopes && rewriter.function().at(rewriter.rootBlock()).handler)
        context.noteRemovedAllocationUnderHandler();
    // The reader runs only where the value is a string, and there the flat string has been made.
    if (flat && context.typeOf(flat).mayBeUnspecified())
        flat = rewriter.insert(makeCastBehindFact(rewriter.function(), rewriter.builder().types(), flat, rewriter.insert(IRBuilder::make(Opcode::Specified))));
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
    Vector<StringCharacterMatch> readers = collectReaders(context, table);
    Vector<Pair>& pairs = table.pairs;
    pairReadersBehindFirstReader(context, pairs, readers);
    sharePairsOfOneString(context, pairs, readers);
    readers.removeAllMatching([&](const StringCharacterMatch& reader) {
        bool isUnchangedRow = !reader.read && !reader.pair;
        return isUnchangedRow || context.tryClaim(SelectionRuleID::HoistStringCharacters, reader.group) != ClaimResult::Claimed;
    });

    // Only a reader that resolves ropes may create FlattenString, and a session may reach another reader of the pair first.
    Vector<StringCharacterMatch> flatteningReaders;
    Vector<StringCharacterMatch> otherReaders;
    for (StringCharacterMatch& reader : readers) {
        bool mayFlatten = reader.pair && reader.resolvesRopes && (!pairs[*reader.pair].firstReader || pairs[*reader.pair].flatteningReader == reader.group.root);
        bool flattens = mayFlatten && !std::exchange(pairs[*reader.pair].hasFlatteningReader, true);
        (flattens ? flatteningReaders : otherReaders).append(WTF::move(reader));
    }

    context.rewriteGroups(SelectionRuleID::HoistStringCharacters, flatteningReaders, [&](GroupRewriter& rewriter, const StringCharacterMatch& match) {
        Pair& pair = pairs[*match.pair];
        emitFlatString(context, rewriter, pair);
        rewriteReader(context, rewriter, match, pair.flat);
    });
    context.rewriteGroups(SelectionRuleID::HoistStringCharacters, otherReaders, [&](GroupRewriter& rewriter, const StringCharacterMatch& match) {
        rewriteReader(context, rewriter, match, match.pair ? pairs[*match.pair].flat : ValueID());
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
