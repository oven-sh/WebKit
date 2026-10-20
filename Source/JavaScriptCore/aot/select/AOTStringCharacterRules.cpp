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
#include "AOTStringRuleSupport.h"
#include <optional>
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The string is flattened on the entry of a loop, or without a loop in front of the first of its readers.
struct Pair {
    BlockID header;
    ValueID string;
    ValueID firstReader { };
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
    bool resolvesRopes { false };
};

struct PlacedReader {
    uint32_t reader;
    ProgramPoint point;
};

} // anonymous namespace

static constexpr OpcodeSet readersWithoutRow { Opcode::CallBuiltin, Opcode::ElemGet };

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
        if (analyses.effectsOfTerminator(block).normal.maySuspend)
            return true;
        for (ValueID id : function.at(block).values) {
            if (analyses.effectsOf(id).normal.maySuspend)
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
    if (!context.analyses().naturalLoops().innerMostLoopOf(block))
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
        return context.analyses().effectsOf(id).normal.maySuspend;
    });
}

static bool suspendsAnywhere(SelectionContext& context)
{
    FunctionAnalyses& analyses = context.analyses();
    return std::ranges::any_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        return analyses.effectsOfTerminator(block).normal.maySuspend || std::ranges::any_of(context.function().at(block).values, [&](ValueID id) {
            return analyses.effectsOf(id).normal.maySuspend;
        });
    });
}

// The first reader that resolves ropes leaves the string flat for every reader it dominates.
static void pairReadersBehindFirstReader(SelectionContext& context, Vector<Pair>& pairs, Vector<Match>& readers)
{
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
        std::ranges::sort(remaining, { }, [&](const PlacedReader& placed) {
            return std::pair { cfg.reversePostOrderIndex(placed.point.block), placed.point.index };
        });
        while (remaining.size() > 1) {
            PlacedReader first = remaining.first();
            remaining.removeAt(0);
            if (!readers[first.reader].resolvesRopes)
                continue;
            if (!functionSuspends)
                functionSuspends = suspendsAnywhere(context);

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
            pairs.append(Pair { .header = { }, .string = readers[first.reader].string, .firstReader = readers[first.reader].group.root });
        }
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
        pair.flat = rewriter.insert(IRBuilder::make(Opcode::FlattenString, { pair.string }));
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
        Value& row = rewriter.function().at(root);
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
    readers.removeAllMatching([&](const Match& reader) {
        bool isUnchangedRow = !reader.read && !reader.pair;
        return isUnchangedRow || context.tryClaim(SelectionRuleID::HoistStringCharacters, reader.group) != ClaimResult::Claimed;
    });

    // Only a reader that resolves ropes may create FlattenString, and a session may reach another reader of the pair first.
    Vector<Match> flatteningReaders;
    Vector<Match> otherReaders;
    for (Match& reader : readers) {
        bool mayFlatten = reader.pair && reader.resolvesRopes && (!pairs[*reader.pair].firstReader || pairs[*reader.pair].firstReader == reader.group.root);
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
