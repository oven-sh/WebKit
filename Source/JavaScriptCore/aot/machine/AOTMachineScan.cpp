/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include "FPRInfo.h"
#include "GPRInfo.h"
#include "AOTMachineCallsOnWaysInScan.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

using PlaceOfScannedReads = BackwardScan::PlaceOfReads;
using SummaryOfScannedReads = BackwardScan::SummaryOfReads;

static void incrementScanCount(uint16_t& count)
{
    if (count != UINT16_MAX)
        ++count;
}

static void decrementScanCount(uint16_t& count)
{
    if (count)
        --count;
}

static constexpr uint16_t scanBitOf(ValueFlag flag) { return static_cast<uint16_t>(flag); }

// forEachLeaf() lists the leaves of an operation with one of these in a way of its own and not by the children.
static constexpr uint16_t flagsOfScannedOperationWithLeavesOfItsOwn = scanBitOf(ValueFlag::WorksInMemory) | scanBitOf(ValueFlag::GoesByOverflow);

// A hint goes from hand to hand as its Reg::index(). This is that of no register.
static constexpr unsigned noHintOfScannedRead = Reg().index();

// functor(unsigned bank), once for each register that the value takes.
template<typename Functor>
static ALWAYS_INLINE void forEachBankOfScannedValue(const Procedure& proc, const Operation* value, const Functor& functor)
{
    Type type = value->type();
    if (type.isNumeric()) [[likely]] {
        functor(static_cast<unsigned>(bankOf(type)));
        return;
    }
    if (!type.isTuple())
        return;
    for (Type member : proc.tupleOf(type))
        functor(static_cast<unsigned>(bankOf(member)));
}

static constexpr bool resultOfScannedOpcodeMayOverwriteFirstChild(Opcode opcode)
{
    return !isMemoryAccess(opcode) && !isComparison(opcode) && opcode != Select && opcode != Extract;
}

// Whether Operation::inlineChildren() may be asked.
static constexpr bool childrenOfScannedOpcodeLieInLine(Opcode opcode)
{
    return opcode != Patch && opcode != Check && !namesSymbol(opcode);
}

// What the scan asks of an opcode, one byte for all of it. The table below is made of the functions that are named here, so it says what they say.
enum TraitOfScannedOpcode : uint8_t {
    // OpcodeTrait::HasEffects.
    ScannedOpcodeHasEffects = 1 << 0,
    // isLoad().
    ScannedOpcodeIsLoad = 1 << 1,
    // resultOfScannedOpcodeMayOverwriteFirstChild(), and resultPrefersRegisterOfFirstChild() or changesWidthOfInteger().
    // In the register of its child an extension or a Trunc may be no instruction at all: isFreeInRegisterOfChild().
    ScannedOpcodeHandsHintOnToFirstChild = 1 << 2,
    // Not childrenOfScannedOpcodeLieInLine(), or a Return, of whose reads hintOfScannedRead() says more than the bit above: readLeavesOfScannedOperationTheLongWay() reads its leaves.
    ScannedOpcodeIsReadTheLongWay = 1 << 3,
    // Not childrenOfScannedOpcodeLieInLine(), or Operation::isUnconditionalJump() may hold: where it is fused, readLeavesBelowScannedOperation() reads its leaves.
    ScannedOpcodeIsReadTheLongWayWhereFused = 1 << 4,
};

struct TraitsOfScannedOpcodes {
    uint8_t of[numberOfOpcodes] { };
};

static constexpr TraitsOfScannedOpcodes traitsOfScannedOpcodes = [] {
    TraitsOfScannedOpcodes traits;
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        unsigned bits = 0;
        if (hasTrait(opcode, OpcodeTrait::HasEffects))
            bits |= ScannedOpcodeHasEffects;
        if (isLoad(opcode))
            bits |= ScannedOpcodeIsLoad;
        if (resultOfScannedOpcodeMayOverwriteFirstChild(opcode) && (resultPrefersRegisterOfFirstChild() || changesWidthOfInteger(opcode)))
            bits |= ScannedOpcodeHandsHintOnToFirstChild;
        if (!childrenOfScannedOpcodeLieInLine(opcode) || opcode == Return)
            bits |= ScannedOpcodeIsReadTheLongWay;
        if (!childrenOfScannedOpcodeLieInLine(opcode) || opcode == Jump || opcode == Branch)
            bits |= ScannedOpcodeIsReadTheLongWayWhereFused;
        traits.of[index] = static_cast<uint8_t>(bits);
    }
    return traits;
}();

// Null in a procedure without loops, where nobody asks what stands around a position.
static const BlockPlace* placeAroundScanPosition(const Order& order, Position position)
{
    if (order.loops().empty() || position >= order.endOfProcedure())
        return nullptr;
    std::span<Block* const> blocks = order.blocksInOrder();
    size_t first = 0;
    size_t end = blocks.size();
    while (end - first > 1) {
        size_t middle = first + (end - first) / 2;
        if (order.headOf(blocks[middle]) <= position)
            first = middle;
        else
            end = middle;
    }
    return &order.of(blocks[first]);
}

static uint32_t innermostLoopAtScanPosition(const Order& order, Position position)
{
    const BlockPlace* place = placeAroundScanPosition(order, position);
    return place ? place->loop : noLoop;
}

static uint32_t rareRegionOfScannedLoop(const Order& order, uint32_t loop)
{
    ASSERT(loop < order.loops().size());
    ASSERT(order.loops()[loop].firstBlock < order.blocksInOrder().size());
    return order.of(order.blocksInOrder().data()[order.loops().data()[loop].firstBlock]).rareRegion;
}

// Liveness::mostLiveIn() is the greatest count that stands behind the scan of some operation of the block.
// While one operation is scanned a count falls only where its range closes, which comes ahead of its reads. So a look anywhere behind that finds no more than stands at the end.
// And a count stands higher at the end than behind the operation before only where a read has revived a value: a read of the operation itself, which looks at the count that it raises,
// or one that BackwardScan::addUse() makes while the range closes, behind which closeScannedRange() looks. What remains is the first operation that the scan comes to, the one that ends the block.
static ALWAYS_INLINE void lookAtCountsOfScan(BackwardScan& scan)
{
    static_assert(numberOfBanks == 2);
    uint16_t* mostLive = scan.m_mostLiveInBlock.data();
    mostLive[0] = std::max(mostLive[0], scan.m_numberLive[0]);
    mostLive[1] = std::max(mostLive[1], scan.m_numberLive[1]);
}

static void samplePressureOfScan(BackwardScan& scan, uint32_t loop, bool standsBehindLastExit = false)
{
    if (loop == noLoop)
        return;
    BackwardScan::LoopState& state = scan.m_loopStates[loop];
    for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
        uint16_t numberLive = scan.m_numberLive[bank];
        if (standsBehindLastExit)
            numberLive -= std::min(numberLive, state.phisReadOnlyBehindLoop[bank]);
        state.mostLive[bank] = std::max(state.mostLive[bank], numberLive);
    }
}

static void endCountOfPhiReadOnlyBehindLoop(BackwardScan& scan, const Operation* phi)
{
    if (!scan.m_isPhiReadOnlyBehindLoop.get(phi->index()))
        return;
    scan.m_isPhiReadOnlyBehindLoop.clear(phi->index());
    decrementScanCount(scan.m_loopStates[scan.m_order.of(phi->owner()).loop].phisReadOnlyBehindLoop[static_cast<unsigned>(bankOf(phi->type()))]);
}

// The innermost loop around the head of a loop is that loop.
static NEVER_INLINE void noteScannedReadOfPhi(BackwardScan& scan, const Operation* phi, Position position, bool revives)
{
    const BlockPlace& home = scan.m_order.of(phi->owner());
    if (!home.isLoopHead || position >= scan.m_order.loops()[home.loop].end) {
        if (home.isLoopHead && revives) {
            scan.m_isPhiReadOnlyBehindLoop.set(phi->index());
            incrementScanCount(scan.m_loopStates[home.loop].phisReadOnlyBehindLoop[static_cast<unsigned>(bankOf(phi->type()))]);
        }
        return;
    }
    endCountOfPhiReadOnlyBehindLoop(scan, phi);
}

// The blocks come in rising order, so the last one to write counts.
static void findLastExitingBlocksOfScannedLoops(BackwardScan& scan)
{
    const Order& order = scan.m_order;
    const BlockPlace* places = order.places();
    const Loop* loops = order.loops().data();
    BackwardScan::LoopState* states = scan.m_loopStates.begin();
    for (const Block* block : order.blocksInOrder()) {
        const BlockPlace& place = places[block->index()];
        uint32_t innermost = place.loop;
        if (innermost == noLoop)
            continue;
        Position end = place.head + 2 * (block->size() + 1);
        ASSERT(end == order.endOf(block));
        for (const Edge& edge : block->successors()) {
            uint32_t target = places[edge.block()->index()].orderIndex;
            for (uint32_t loop = innermost; loop != noLoop && (target < loops[loop].firstBlock || target >= loops[loop].endBlock); loop = loops[loop].parent)
                states[loop].endOfLastExitingBlock = end;
        }
    }
}

// `loop` is the innermost one around a read that has been recorded. A loop holds whole blocks, so the head of the block of the value says on which side of a loop's start it is defined.
static void extendScannedRangeFrom(BackwardScan& scan, const Operation* value, uint32_t loop)
{
    if (loop == noLoop)
        return;
    std::span<const Loop> loops = scan.m_order.loops();
    Position home = scan.m_order.headOf(value->owner());
    ValueFacts& read = scan.facts(value);
    for (; loop != noLoop && loops[loop].begin > home; loop = loops[loop].parent)
        read.lastUse = std::max(read.lastUse, loops[loop].end);
}

// Counts the value once for each loop around the read that it is defined ahead of. `readAhead` and `readBehind` are its neighbors in the chain of uses.
// A value that this read revives is live to the end of each such loop, where the scan has not counted it.
static void countScannedReadInsideLoops(BackwardScan& scan, const Operation* value, uint32_t loop, Position readAhead, Position readBehind, bool revives)
{
    if (loop == noLoop || scan.m_loopCountedFor[value->index()] == loop)
        return;
    std::span<const Loop> loops = scan.m_order.loops();
    Position home = scan.m_order.headOf(value->owner());
    uint32_t outermost = noLoop;
    for (; loop != noLoop && loops[loop].begin > home; loop = loops[loop].parent) {
        // A neighbor inside the loop has counted the value for it and for every loop around it.
        if (readBehind < loops[loop].end || (readAhead != noPosition && readAhead >= loops[loop].begin))
            return;
        BackwardScan::LoopState& state = scan.m_loopStates[loop];
        forEachBankOfScannedValue(scan.m_proc, value, [&](unsigned bank) {
            incrementScanCount(state.outerValuesRead[bank]);
            if (revives)
                incrementScanCount(state.mostLive[bank]);
        });
        outermost = loop;
    }
    if (outermost != noLoop)
        scan.m_loopCountedFor[value->index()] = outermost;
}

// A read lists the value for every loop around it that the value is defined ahead of. So a loop that holds the loop of the last read that listed it has it already, and so has every loop around that one.
static NEVER_INLINE void listScannedReadFromOutsideLoops(BackwardScan& scan, Operation* value, const PlaceOfScannedReads& at)
{
    if (!at.countsForLoops)
        return;
    std::span<const Loop> loops = scan.m_order.loops();
    uint32_t& loopOfLastListedRead = scan.m_loopOfLastListedRead[value->index()];
    for (uint32_t loop = at.loop; loop != noLoop; loop = loops[loop].parent) {
        if (!value->hasNoPosition() && loops[loop].begin <= scan.m_order.headOf(value->owner()))
            break;
        if (loopOfLastListedRead != noLoop && loops[loop].firstBlock <= loops[loopOfLastListedRead].firstBlock && loops[loopOfLastListedRead].endBlock <= loops[loop].endBlock)
            break;
        scan.m_loopStates[loop].valuesReadFromOutside.append(scan.m_proc.arena(), value);
    }
    loopOfLastListedRead = at.loop;
}

// As Allocator::placeLeaf() decides. Where a constant lies in a pinned register or in a word of the instance, no register is worth keeping for it.
static bool scannedLeafWithoutPositionNeedsRegister(const Liveness& liveness, const Operation* user, unsigned childIndex, const Operation* leaf)
{
    if (leaf->opcode() == ArgumentReg || user->as<PatchOperation>())
        return false;
    if ((leaf->hasInt64() || leaf->hasDouble()) && findPinnedConstant(static_cast<uint64_t>(leaf->constantBits())))
        return false;
    // The result takes the register of the leaf over, so the user needs a copy of its own. On x86-64 one instruction makes an integer, which is what a copy costs.
    if (isX86_64() && !childIndex && !liveness.isFused(user) && !leaf->type().isFloat()) {
        Opcode opcode = user->opcode();
        if ((resultOverwritesFirstChild(opcode) && !isCommutative(opcode)) || isFreeInRegisterOfChild(liveness, user))
            return false;
    }
    return !standsInInstruction(liveness, user, childIndex, leaf);
}

static NEVER_INLINE void markScannedOperationWithoutPosition(BackwardScan& scan, const Operation* operation)
{
    ASSERT(operation->hasNoPosition());
    scan.set(operation, ValueFlag::IsRematerialized);
    for (const Operation* child : operation->children())
        markScannedOperationWithoutPosition(scan, child);
}

static ALWAYS_INLINE const Block* blockThatScannedBlockBranchesOff(const BackwardScan& scan, const Block* block, const BlockPlace& place)
{
    return place.isRare() ? scan.m_liveness.blockThatRareBlockBranchesOff(block) : nullptr;
}

// `origin`: blockThatScannedBlockBranchesOff().
static ALWAYS_INLINE Position headOfBlockPassedOnEveryPathToScannedBlock(const BackwardScan& scan, const BlockPlace& place, const Block* origin)
{
    const Block* passed = origin;
    if (!passed && place.predecessors.size() == 1)
        passed = place.predecessors.front();
    return passed ? scan.m_places[passed->index()].head : noPosition;
}

// `place` is that of the block, `origin` its blockThatScannedBlockBranchesOff(). Both null in a procedure without loops, for a read that no operation makes.
// Each word of `at` is written once, but for the two that speak of something that is not there.
static ALWAYS_INLINE void findPlaceOfReadsInScannedBlock(const BackwardScan& scan, PlaceOfScannedReads& at, const BlockPlace* place, const Block* origin, Position passedOnEveryPath)
{
    at.passedOnEveryPath = passedOnEveryPath;
    uint32_t loop = place ? place->loop : noLoop;
    uint32_t rareRegion = place ? place->rareRegion : noRareRegion;
    at.loop = loop;
    at.rareRegion = rareRegion;
    at.traitsOfUses = rareRegion != noRareRegion ? Liveness::useIsInRareRegion : 0;
    if (loop != noLoop) {
        at.beginOfLoop = scan.m_order.loops().data()[loop].begin;
        at.countsForLoops = rareRegion == rareRegionOfScannedLoop(scan.m_order, loop);
    } else
        at.countsForLoops = false;
    if (origin && !scan.m_summariesOfReads.isEmpty()) {
        ASSERT(rareRegion != noRareRegion);
        const BlockPlace& placeOfOrigin = scan.m_places[origin->index()];
        at.positionThatCountsForCalls = placeOfOrigin.head + 2 * origin->size();
        ASSERT(at.positionThatCountsForCalls == scan.m_order.endOf(origin) - 2);
        at.loopThatCountsForCalls = placeOfOrigin.loop;
        return;
    }
    at.positionThatCountsForCalls = noPosition;
}

struct NeighborsOfScannedRead {
    // The read that it stands ahead of, zero if there is none.
    uint32_t behind;
    // Where the read stands that it stands behind.
    Position readAhead;
};

// The read stands behind the first one that is known.
static NEVER_INLINE NeighborsOfScannedRead linkScannedReadBehindEarlierReads(Use* uses, uint32_t first, uint32_t newUse, Position position, Position passedOnEveryPath)
{
    uint32_t ahead = first;
    auto noteLaterRead = [&](Use& earlier) {
        earlier.greatestHeadOfBlockPassedOnEveryPath = std::max(earlier.greatestHeadOfBlockPassedOnEveryPath, passedOnEveryPath);
    };
    noteLaterRead(uses[ahead]);
    uint32_t behind;
    for (behind = uses[ahead].next; behind && uses[behind].position < position; behind = uses[ahead].next) {
        ahead = behind;
        noteLaterRead(uses[ahead]);
    }
    uses[ahead].next = newUse;
    return { behind, uses[ahead].position };
}

static NEVER_INLINE void makeRoomForScannedReads(BackwardScan& scan)
{
    uint32_t capacity = scan.uses().capacity() ? scan.uses().capacity() * 2 : 4;
    scan.uses().reserve(scan.m_proc.arena(), capacity);
    uint8_t* traits = scan.m_proc.arena().allocateArray<uint8_t>(capacity);
    std::copy_n(scan.traitsOfUses(), scan.uses().size(), traits);
    scan.traitsOfUses() = traits;
}

// BackwardScan::noteRegionOfUse(), for whoever knows the rare region of the block that reads. `regionOfUses` is BackwardScan::SummaryOfReads::regionOfUses of the value.
static ALWAYS_INLINE void noteRegionOfScannedUse(uint32_t& regionOfUses, uint32_t region)
{
    if (region == noRareRegion || (regionOfUses != noRareRegion && regionOfUses != region))
        regionOfUses = BackwardScan::severalRegions;
    else
        regionOfUses = region;
}

// What recordScannedRead() does and answers where the scan has been at the definition of the value.
static NEVER_INLINE bool failScannedRead(BackwardScan& scan, const PlaceOfScannedReads& at, const Operation* value, bool notesRegion)
{
    scan.m_proc.fail(FailureKind::MalformedProcedure, value);
    if (value->hasNoPosition())
        return false;
    if (notesRegion && scan.m_hasRareRegions)
        noteRegionOfScannedUse(scan.m_summariesOfReads[value->index()].regionOfUses, at.rareRegion);
    return at.loop != noLoop && scan.m_order.headOf(value->owner()) < at.beginOfLoop;
}

// `loop` is the innermost one around the position at which the read counts for calls.
static Position extendScannedReadOverLoopsForCalls(const BackwardScan& scan, const Operation* value, uint32_t loop, Position position)
{
    std::span<const Loop> loops = scan.m_order.loops();
    Position home = scan.m_order.headOf(value->owner());
    for (; loop != noLoop && loops[loop].begin > home; loop = loops[loop].parent)
        position = std::max(position, loops[loop].end);
    return position;
}

enum class KindOfScannedRead : uint8_t {
    // An operation of the block of the scan makes it.
    ByOperation,
    // An Upsilon of that block makes it. The scan has not come to an operation of the block, so it looks at no count: lookAtCountsOfScan().
    ByUpsilon,
    // BackwardScan::addUse(). It counts for BackwardScan::SummaryOfReads::regionOfUses only where the caller says so.
    Added,
};

// Any read. `read` is BackwardScan::facts() of the value, `hint` the Reg::index() of the register that the read would like it in.
// Returns whether the value has a position and is defined ahead of the innermost loop around the read.
template<KindOfScannedRead kind>
static ALWAYS_INLINE bool recordScannedRead(BackwardScan& scan, const PlaceOfScannedReads& at, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    constexpr bool notesRegion = kind != KindOfScannedRead::Added;

    // Whatever reads a value stands behind it in the order, so the scan has not been at its definition.
    uint16_t flags = read.flags;
    if (read.definition != noPosition || (flags & scanBitOf(ValueFlag::IsDropped))) [[unlikely]]
        return failScannedRead(scan, at, value, notesRegion);
    if (readsUpperHalf)
        flags |= scanBitOf(ValueFlag::UpperHalfIsRead);

    ArenaVector<Use>& uses = scan.uses();
    uint8_t*& traitsOfUses = scan.traitsOfUses();
    uint32_t newUse = uses.size();
    if (newUse == uses.capacity()) [[unlikely]]
        makeRoomForScannedReads(scan);
    Use* firstOfUses = uses.begin();

    // The scan goes back through the order, so nearly every read stands ahead of those that are known.
    Position readAhead = noPosition;
    uint32_t behind = read.firstUse;
    if (behind && firstOfUses[behind].position < position) {
        NeighborsOfScannedRead neighbors = linkScannedReadBehindEarlierReads(firstOfUses, behind, newUse, position, at.passedOnEveryPath);
        behind = neighbors.behind;
        readAhead = neighbors.readAhead;
    }
    bool isFirstRead = readAhead == noPosition;
    Position readBehind = noPosition;
    Position greatestPassedOnEveryPath = at.passedOnEveryPath;
    if (behind) {
        readBehind = firstOfUses[behind].position;
        greatestPassedOnEveryPath = std::max(greatestPassedOnEveryPath, firstOfUses[behind].greatestHeadOfBlockPassedOnEveryPath);
    }
    uses.appendWithinCapacity(Use { position, behind, at.passedOnEveryPath, greatestPassedOnEveryPath });
    traitsOfUses[newUse] = static_cast<uint8_t>(at.traitsOfUses | (readsUpperHalf ? Liveness::useReadsUpperHalf : 0));
    if (isFirstRead)
        read.firstUse = newUse;

    Type type = value->type();
    const Block* owner = value->owner();
    if (hint != noHintOfScannedRead && type.isNumeric() && bankOf(Reg::fromIndex(hint)) == bankOf(type)) {
        // What a rare region would like of a value from outside gives way to what a read elsewhere would like, however far on: the move then stands in the region.
        bool isHintOfRareRegion = at.isRare() && owner && scan.m_places[owner->index()].rareRegion != at.rareRegion;
        if (isFirstRead ? !isHintOfRareRegion || !read.hint || (flags & scanBitOf(ValueFlag::HintIsOfRareRegion)) : !read.hint) {
            constexpr uint16_t flag = scanBitOf(ValueFlag::HintIsOfRareRegion);
            read.hint = Reg::fromIndex(hint);
            flags = static_cast<uint16_t>(isHintOfRareRegion ? flags | flag : flags & ~flag);
        }
    }
    read.flags = flags;

    Position lastUse = read.lastUse;
    bool revives = lastUse == noPosition;
    if (!owner) {
        read.lastUse = revives ? position : std::max(lastUse, position);
        if (revives)
            markScannedOperationWithoutPosition(scan, value);
        return false;
    }

    if (revives) {
        forEachBankOfScannedValue(scan.m_proc, value, [&](unsigned bank) {
            incrementScanCount(scan.m_numberLive[bank]);
        });
        if constexpr (kind == KindOfScannedRead::ByOperation)
            lookAtCountsOfScan(scan);
    }
    // No Phi stands at the head of a loop where there is no loop.
    if (scan.m_hasLoops && value->opcode() == Phi)
        noteScannedReadOfPhi(scan, value, position, revives);
    // Loops nest, so a read ahead of the end of the range lies in no loop that the value is defined ahead of and that ends behind the range.
    bool isDefinedAheadOfLoop = at.loop != noLoop && scan.m_places[owner->index()].head < at.beginOfLoop;
    if (revives || position >= lastUse) {
        read.lastUse = position;
        if (isDefinedAheadOfLoop)
            extendScannedRangeFrom(scan, value, at.loop);
    }
    if (isDefinedAheadOfLoop)
        countScannedReadInsideLoops(scan, value, at.loop, readAhead, readBehind, revives);

    if (scan.m_summariesOfReads.isEmpty())
        return isDefinedAheadOfLoop;
    uint32_t loopThatCountsForCalls = isDefinedAheadOfLoop ? at.loop : noLoop;
    if (at.positionThatCountsForCalls != noPosition && at.rareRegion != scan.m_places[owner->index()].rareRegion) {
        position = at.positionThatCountsForCalls;
        loopThatCountsForCalls = at.loopThatCountsForCalls;
    }
    if (loopThatCountsForCalls != noLoop)
        position = extendScannedReadOverLoopsForCalls(scan, value, loopThatCountsForCalls, position);
    SummaryOfScannedReads& summary = scan.m_summariesOfReads[value->index()];
    summary.lastReadCountedForCalls = std::max(summary.lastReadCountedForCalls, position);
    if constexpr (notesRegion)
        noteRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
    return isDefinedAheadOfLoop;
}

// Any read that stands in the block of the scan, and of an operation's read what the loops around the block are told of a value with a position.
template<KindOfScannedRead kind>
static NEVER_INLINE void readValueInScannedBlockTheLongWay(BackwardScan& scan, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    [[maybe_unused]] bool isDefinedAheadOfLoop = recordScannedRead<kind>(scan, at, value, read, position, hint, readsUpperHalf);
    if constexpr (kind == KindOfScannedRead::ByOperation) {
        if (!at.countsForLoops || value->hasNoPosition())
            return;
        // For what is defined in the innermost loop listScannedReadFromOutsideLoops() lists nothing and notes the loop.
        if (isDefinedAheadOfLoop)
            listScannedReadFromOutsideLoops(scan, value, at);
        else
            scan.m_loopOfLastListedRead[value->index()] = at.loop;
    }
}

// The end of readValueInScannedBlock() for a read in a rare region, of what is defined outside it, that counts for calls at a place with a loop around it.
static NEVER_INLINE void noteScannedReadThatCountsForCallsInLoop(BackwardScan& scan, const Operation* value)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    SummaryOfScannedReads& summary = scan.m_summariesOfReads[value->index()];
    summary.lastReadCountedForCalls = std::max(summary.lastReadCountedForCalls, extendScannedReadOverLoopsForCalls(scan, value, at.loopThatCountsForCalls, at.positionThatCountsForCalls));
    noteRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
}

// What readValueInScannedBlockTheLongWay() does, for nearly every read: of a number whose definition the scan has not been at, ahead of the reads that are known, with room for it in the list,
// of no Phi at the head of a loop and of nothing that is defined ahead of the innermost loop around the block.
// It asks all of that before it writes anything, so whatever else comes goes the long way from the start. It calls nothing that it has to come back from, so it needs no register saved.
template<KindOfScannedRead kind>
static NEVER_INLINE void readValueInScannedBlock(BackwardScan& scan, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    static_assert(kind != KindOfScannedRead::Added);
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    ArenaVector<Use>& uses = scan.uses();
    uint8_t*& traitsOfUses = scan.traitsOfUses();
    uint16_t flags = read.flags;
    Type type = value->type();
    const Block* owner = value->owner();
    uint32_t newUse = uses.size();
    uint32_t behind = read.firstUse;
    Use* firstOfUses = uses.begin();

    // Entry zero of the list has noPosition for its position, so a value of which no read is known passes the last test.
    if (read.definition != noPosition || (flags & scanBitOf(ValueFlag::IsDropped)) || !type.isNumeric() || newUse == uses.capacity() || firstOfUses[behind].position < position) [[unlikely]]
        return readValueInScannedBlockTheLongWay<kind>(scan, value, read, position, hint, readsUpperHalf);
    if (owner) {
        if (at.loop != noLoop && scan.m_places[owner->index()].head < at.beginOfLoop) [[unlikely]]
            return readValueInScannedBlockTheLongWay<kind>(scan, value, read, position, hint, readsUpperHalf);
        // Of a Phi that stands elsewhere noteScannedReadOfPhi() notes nothing.
        if (value->opcode() == Phi && scan.m_hasLoops && scan.m_places[owner->index()].isLoopHead) [[unlikely]]
            return readValueInScannedBlockTheLongWay<kind>(scan, value, read, position, hint, readsUpperHalf);
    }

    Position passedOnEveryPath = at.passedOnEveryPath;
    Position greatestPassedOnEveryPath = passedOnEveryPath;
    if (behind)
        greatestPassedOnEveryPath = std::max(greatestPassedOnEveryPath, firstOfUses[behind].greatestHeadOfBlockPassedOnEveryPath);
    uses.appendWithinCapacity(Use { position, behind, passedOnEveryPath, greatestPassedOnEveryPath });
    traitsOfUses[newUse] = static_cast<uint8_t>(at.traitsOfUses | (readsUpperHalf ? Liveness::useReadsUpperHalf : 0));
    read.firstUse = newUse;

    if (readsUpperHalf)
        flags |= scanBitOf(ValueFlag::UpperHalfIsRead);
    if (hint != noHintOfScannedRead && bankOf(Reg::fromIndex(hint)) == bankOf(type)) {
        // What a rare region would like of a value from outside gives way to what a read elsewhere would like, however far on: the move then stands in the region.
        bool isHintOfRareRegion = at.isRare() && owner && scan.m_places[owner->index()].rareRegion != at.rareRegion;
        if (!isHintOfRareRegion || !read.hint || (flags & scanBitOf(ValueFlag::HintIsOfRareRegion))) {
            constexpr uint16_t flag = scanBitOf(ValueFlag::HintIsOfRareRegion);
            read.hint = Reg::fromIndex(hint);
            flags = static_cast<uint16_t>(isHintOfRareRegion ? flags | flag : flags & ~flag);
        }
    }

    Position lastUse = read.lastUse;
    if (!owner) {
        if (lastUse != noPosition) {
            read.flags = flags;
            read.lastUse = std::max(lastUse, position);
            return;
        }
        // markScannedOperationWithoutPosition(), which has more to do only where there are children.
        read.flags = static_cast<uint16_t>(flags | scanBitOf(ValueFlag::IsRematerialized));
        read.lastUse = position;
        if (value->numChildren()) [[unlikely]]
            return markScannedOperationWithoutPosition(scan, value);
        return;
    }
    read.flags = flags;
    if (lastUse == noPosition) {
        unsigned bank = static_cast<unsigned>(bankOf(type));
        uint16_t& numberLive = scan.m_numberLive[bank];
        incrementScanCount(numberLive);
        if constexpr (kind == KindOfScannedRead::ByOperation) {
            // lookAtCountsOfScan(), at the one count that has changed.
            uint16_t& mostLive = scan.m_mostLiveInBlock.data()[bank];
            mostLive = std::max(mostLive, numberLive);
        }
        read.lastUse = position;
    } else if (position >= lastUse)
        read.lastUse = position;

    uint32_t index = value->index();
    if constexpr (kind == KindOfScannedRead::ByOperation) {
        if (at.countsForLoops)
            scan.m_loopOfLastListedRead[index] = at.loop;
    }
    if (scan.m_summariesOfReads.isEmpty())
        return;
    if (at.positionThatCountsForCalls != noPosition && at.rareRegion != scan.m_places[owner->index()].rareRegion) {
        if (at.loopThatCountsForCalls != noLoop) [[unlikely]]
            return noteScannedReadThatCountsForCallsInLoop(scan, value);
        position = at.positionThatCountsForCalls;
    }
    SummaryOfScannedReads& summary = scan.m_summariesOfReads[index];
    summary.lastReadCountedForCalls = std::max(summary.lastReadCountedForCalls, position);
    noteRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
}

// BackwardScan::define(). `regionOfBlock`: BlockPlace::rareRegion of the block that the operation was appended to.
static ALWAYS_INLINE void defineScannedValue(BackwardScan& scan, Operation* operation, ValueFacts& own, Position position, uint32_t regionOfBlock)
{
    own.definition = position;
    Position lastUse = own.lastUse;
    if (lastUse == noPosition)
        return;

    // What sank stands behind the place of the scan. Between it and its reads lie the calls of its region alone, and the scan has been through all of that region.
    uint16_t flags = own.flags;
    bool isSunk = flags & static_cast<uint16_t>(ValueFlag::IsSunk);
    uint32_t region = isSunk ? scan.m_summariesOfReads[operation->index()].regionOfUses : regionOfBlock;
    Position lastReadCountedForCalls = isSunk || scan.m_summariesOfReads.isEmpty() ? lastUse : scan.m_summariesOfReads[operation->index()].lastReadCountedForCalls;
    forEachBankOfScannedValue(scan.m_proc, operation, [&](unsigned bank) {
        Position nextCall = isSunk ? noPosition : scan.m_nextCall[bank];
        if (region < scan.m_regions.size())
            nextCall = std::min(nextCall, scan.m_regions[region].nextCall[bank]);
        if (lastReadCountedForCalls > nextCall)
            flags |= static_cast<uint16_t>(ValueFlag::LivesAcrossCall);
    });
    // For what sank these three may lie ahead of it, which claims more and never less.
    if (lastUse > scan.m_nextSafepoint)
        flags |= static_cast<uint16_t>(ValueFlag::LivesAcrossSafepoint);
    if (lastUse > scan.m_nextSuspension)
        flags |= static_cast<uint16_t>(ValueFlag::LivesAcrossSuspension);
    if (lastUse >= scan.m_nextLandingPad)
        flags |= static_cast<uint16_t>(ValueFlag::LivesIntoLandingPad);
    own.flags = flags;

    // It does nothing for any other value.
    if (operation->has(OperationFlag::IsInnerPointer) || (flags & static_cast<uint16_t>(ValueFlag::LivesAcrossSafepoint)))
        scan.applyCollectorRules(operation);
}

// Whatever the operation is.
static NEVER_INLINE void closeScannedRangeTheLongWay(BackwardScan& scan, Operation* operation, ValueFacts& own, Position definition, uint32_t regionOfBlock)
{
    defineScannedValue(scan, operation, own, definition, regionOfBlock);
    if (own.lastUse == noPosition)
        return;
    forEachBankOfScannedValue(scan.m_proc, operation, [&](unsigned bank) {
        decrementScanCount(scan.m_numberLive[bank]);
    });
    // BackwardScan::trySink() and BackwardScan::applyCollectorRules() may have added reads.
    lookAtCountsOfScan(scan);
    if (scan.m_hasLoops)
        endCountOfPhiReadOnlyBehindLoop(scan, operation);
}

// In line: defineScannedValue() and what stands behind it above, for a number that has not sunk.
static ALWAYS_INLINE void closeScannedRange(BackwardScan& scan, Operation* operation, ValueFacts& own, Position definition, uint32_t regionOfBlock)
{
    Position lastUse = own.lastUse;
    if (lastUse == noPosition) {
        own.definition = definition;
        return;
    }
    uint16_t flags = own.flags;
    Type type = operation->type();
    if ((flags & scanBitOf(ValueFlag::IsSunk)) || !type.isNumeric()) [[unlikely]] {
        closeScannedRangeTheLongWay(scan, operation, own, definition, regionOfBlock);
        return;
    }

    own.definition = definition;
    unsigned bank = static_cast<unsigned>(bankOf(type));
    Position lastReadCountedForCalls = scan.m_summariesOfReads.isEmpty() ? lastUse : scan.m_summariesOfReads[operation->index()].lastReadCountedForCalls;
    // Each of the positions asked about below stands no nearer than BackwardScan::m_nearestEventBehind.
    if (std::max(lastUse, lastReadCountedForCalls) >= scan.m_nearestEventBehind) {
        Position nextCall = scan.m_nextCall[bank];
        if (regionOfBlock < scan.m_regions.size())
            nextCall = std::min(nextCall, scan.m_regions[regionOfBlock].nextCall[bank]);
        if (lastReadCountedForCalls > nextCall)
            flags |= scanBitOf(ValueFlag::LivesAcrossCall);
        if (lastUse > scan.m_nextSafepoint)
            flags |= scanBitOf(ValueFlag::LivesAcrossSafepoint);
        if (lastUse > scan.m_nextSuspension)
            flags |= scanBitOf(ValueFlag::LivesAcrossSuspension);
        if (lastUse >= scan.m_nextLandingPad)
            flags |= scanBitOf(ValueFlag::LivesIntoLandingPad);
        own.flags = flags;
    }

    // It does nothing for any other value. It never takes the last use away, so the range is one that closes. It may add reads, which revive what they read.
    bool isSubjectToCollectorRules = operation->has(OperationFlag::IsInnerPointer) || (flags & scanBitOf(ValueFlag::LivesAcrossSafepoint));
    if (isSubjectToCollectorRules)
        scan.applyCollectorRules(operation);
    decrementScanCount(scan.m_numberLive[bank]);
    if (isSubjectToCollectorRules)
        lookAtCountsOfScan(scan);
    // noteScannedReadOfPhi() alone sets a bit of BackwardScan::m_isPhiReadOnlyBehindLoop, and it is called for a Phi.
    if (operation->opcode() == Phi && scan.m_hasLoops)
        endCountOfPhiReadOnlyBehindLoop(scan, operation);
}

// An Upsilon that Order left behind when it retargeted or split an edge feeds nothing. Neither does one whose Phi stands behind it and has turned out unread.
// The Phi of a back edge has not been scanned, so its Upsilon stays.
static ALWAYS_INLINE bool upsilonStaysInScan(const BackwardScan& scan, const Block* block, const Operation* upsilon, Position position)
{
    const Operation* phi = upsilon->phi();
    if (!phi)
        return false;
    const Block* target = phi->owner();
    bool leadsToPhi = false;
    for (const Edge& edge : block->successors()) {
        if (edge.block() == target) {
            leadsToPhi = true;
            break;
        }
    }
    if (!leadsToPhi)
        return false;
    return scan.m_places[target->index()].head <= position || !scan.m_facts[phi->index()].has(ValueFlag::IsDropped);
}

static ALWAYS_INLINE Reg hintOfScannedRead(const Operation* operation, const PatchOperation* patch, unsigned childIndex, const Operation* leaf, Reg hintOfResult)
{
    if (patch) {
        const ValueRep& constraint = patch->constraintOf(childIndex);
        return constraint.isReg() ? constraint.reg() : patch->registerToBringOperandTo(childIndex);
    }
    if (operation->opcode() == Return)
        return leaf->type().isFloat() ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR);
    if (!childIndex && (traitsOfScannedOpcodes.of[operation->opcode()] & ScannedOpcodeHandsHintOnToFirstChild) && operation->type().isNumeric())
        return hintOfResult;
    return Reg();
}

// `operation` stays, or is read where its user stands.
static ALWAYS_INLINE void noteScannedReadOfWholeWord(BackwardScan& scan, const Operation* operation)
{
    if (scan.m_wordsReadWhole.isEmpty() || operation->opcode() != Load || operation->type() != Int64 || scan.m_order.of(operation->owner()).isRare())
        return;
    scan.m_wordsReadWhole[operation->lastChild()->index()] |= BackwardScan::bitOfWord(operation->offset());
}

// It throws or stops the program, so it runs once at most each time the function is entered, and what it costs is bytes.
static bool functionEndsBehindScannedPatch(const PatchOperation* patch)
{
    const Block* block = patch->owner();
    if (patch->endsBlock())
        return !block->numSuccessors() && !patch->leavesByReturning();
    return patch->indexInBlock() + 2 == block->size() && block->last()->opcode() == Oops;
}

// `loop` is the innermost one around the block, `region` the rare region of the block, `clobbered` what the Patch clobbers late.
static NEVER_INLINE void noteRegistersTakenByScannedCallInLoops(BackwardScan& scan, const PatchOperation* patch, const Block* block, uint32_t loop, uint32_t region, RegisterMask clobbered)
{
    RegisterMask taken = clobbered | patch->earlyClobberedMask();
    if (patch->effects.terminal) {
        for (unsigned index = 0; index < block->numSuccessors(); ++index)
            taken |= patch->lateClobberedOnSuccessor(index);
    }
    for (const ValueRep& constraint : patch->resultConstraints.span()) {
        if (constraint.isReg())
            taken |= maskOf(constraint.reg());
    }
    for (unsigned child = 0, numberOfOperands = patch->numberOfOperands(); child < numberOfOperands; ++child) {
        const ValueRep& constraint = patch->constraintOf(child);
        if (constraint.isReg())
            taken |= maskOf(constraint.reg());
        else if (Reg reg = patch->registerToBringOperandTo(child))
            taken |= maskOf(reg);
    }
    const Loop* loops = scan.m_order.loops().data();
    for (; loop != noLoop; loop = loops[loop].parent) {
        if (rareRegionOfScannedLoop(scan.m_order, loop) == region)
            scan.m_loopStates[loop].registersTakenByPatches |= taken;
    }
}

static NEVER_INLINE void makeListsOfLastCallsInScannedBlocks(BackwardScan& scan)
{
    scan.lastCallsInBlocks().fill(scan.m_proc.arena(), scan.m_order.mostBlocks(), std::array<Position, numberOfBanks> { 0, 0 });
    scan.nextCallsBehindEnds().fill(scan.m_proc.arena(), scan.m_order.mostBlocks(), std::array<Position, numberOfBanks> { noPosition, noPosition });
}

// BackwardScan::noteCall(). `region` and `loop` are BlockPlace::rareRegion and BlockPlace::loop of the block. `isReachedByScan`: the scan stands at the Patch, behind the close of its range.
static ALWAYS_INLINE void noteScannedCall(BackwardScan& scan, const PatchOperation* patch, Position late, const Block* block, uint32_t region, uint32_t loop, bool isReachedByScan)
{
    bool leadsToLandingPad = false;
    if (patch->effects.terminal) {
        // It is the first operation of its block that the scan comes to.
        if (isReachedByScan)
            lookAtCountsOfScan(scan);
        for (const Edge& edge : block->successors())
            leadsToLandingPad |= edge.block()->isLandingPad();
    }

    static_assert(numberOfBanks == 2);
    RegisterMask clobbered = patch->lateClobberedMask();
    bool takesFirstBank = (clobbered & scan.m_callerSaved[0]) == scan.m_callerSaved[0];
    bool takesSecondBank = (clobbered & scan.m_callerSaved[1]) == scan.m_callerSaved[1];
    bool isEvent = leadsToLandingPad || takesFirstBank || takesSecondBank;
    if (isEvent) {
        Position* nextCall = region == noRareRegion ? scan.m_nextCall : scan.m_regions[region].nextCall;
        if (leadsToLandingPad || takesFirstBank)
            nextCall[0] = late;
        if (leadsToLandingPad || takesSecondBank)
            nextCall[1] = late;
        if (region == noRareRegion && (takesFirstBank || takesSecondBank)) {
            // Liveness::lastCallIn() and nextCallBehindEndOf(), ahead of the line that makes the call the nearest one.
            // The scan meets the last call of a block first, and the nearest call until then stands behind the block.
            if (scan.lastCallsInBlocks().isEmpty()) [[unlikely]]
                makeListsOfLastCallsInScannedBlocks(scan);
            uint32_t blockIndex = block->index();
            Position* lastCalls = scan.lastCallsInBlocks()[blockIndex].data();
            Position* nextCallsBehindEnd = scan.nextCallsBehindEnds()[blockIndex].data();
            Position* nextCallThatTakesRegisters = scan.m_nextCallThatTakesRegisters.data();
            if (takesFirstBank) {
                if (!lastCalls[0]) {
                    lastCalls[0] = late;
                    nextCallsBehindEnd[0] = nextCallThatTakesRegisters[0];
                }
                nextCallThatTakesRegisters[0] = late;
            }
            if (takesSecondBank) {
                if (!lastCalls[1]) {
                    lastCalls[1] = late;
                    nextCallsBehindEnd[1] = nextCallThatTakesRegisters[1];
                }
                nextCallThatTakesRegisters[1] = late;
            }
        }
    }
    if (patch->effects.mayCollect) {
        scan.m_nextSafepoint = late;
        isEvent = true;
    }
    if (patch->isSuspension()) {
        scan.m_nextSuspension = late;
        isEvent = true;
    }
    if (isEvent)
        scan.m_nearestEventBehind = std::min(scan.m_nearestEventBehind, late);

    if (loop != noLoop)
        noteRegistersTakenByScannedCallInLoops(scan, patch, block, loop, region, clobbered);
}

// `read` is BackwardScan::facts() of the leaf. `userIsPatch`: of such a user scannedLeafWithoutPositionNeedsRegister() says no.
static ALWAYS_INLINE void readLeafOfScannedOperation(BackwardScan& scan, const Operation* user, unsigned childIndex, Operation* leaf, ValueFacts& read, Position early, unsigned hint, bool readsUpperHalf, bool userIsPatch = false)
{
    readValueInScannedBlock<KindOfScannedRead::ByOperation>(scan, leaf, read, early, hint, readsUpperHalf);
    if (userIsPatch)
        return;
    if (scan.m_placeOfReads.countsForLoops && leaf->hasNoPosition() && scannedLeafWithoutPositionNeedsRegister(scan.m_liveness, user, childIndex, leaf)) [[unlikely]]
        listScannedReadFromOutsideLoops(scan, leaf, scan.m_placeOfReads);
}

// The leaves that forEachLeafBelow() lists for `below` and `selectAbove`: what is fused into the operation, or the operation itself where it works in memory or goes by overflow.
static NEVER_INLINE void readLeavesBelowScannedOperation(BackwardScan& scan, Operation* operation, const Operation* below, const Operation* selectAbove, Position early)
{
    const ValueFacts& own = scan.facts(operation);
    const PatchOperation* patch = operation->as<PatchOperation>();
    Reg hintOfResult = own.hint;
    // The Branch reads the sum where its register holds the low half alone. The first leaf of the sum lies where its first child would.
    bool isBranchOnOverflow = own.has(ValueFlag::GoesByOverflow) && operation->opcode() == Branch;
    bool nextLeafStandsForFirstChild = own.has(ValueFlag::GoesByOverflow) && !isBranchOnOverflow;
    forEachLeafBelow(scan.m_liveness, below, [&](const Operation* user, unsigned childIndex, Operation* leaf) {
        // A fused Trunc has one child, which lies where the Trunc would.
        bool standsForFirstChild = (user->opcode() == Trunc && user == operation->child(0)) || std::exchange(nextLeafStandsForFirstChild, false);
        Reg hint = user == operation || standsForFirstChild ? hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult) : Reg();
        readLeafOfScannedOperation(scan, user, childIndex, leaf, scan.facts(leaf), early, hint.index(), user->opcode() != Trunc && !isBranchOnOverflow);
    }, selectAbove);
}

// What stands behind the close of the range of any operation that stays, but for what a Patch is noted for.
static NEVER_INLINE void readLeavesOfScannedOperationTheLongWay(BackwardScan& scan, Operation* operation, Position early)
{
    if (!operation->numChildren() || operation->isUnconditionalJump())
        return;
    // Nothing is fused into a Patch.
    if (operation->opcode() != Patch)
        scan.fuseInto(operation);
    const PatchOperation* patch = operation->as<PatchOperation>();
    bool readsUpperHalf = operation->opcode() != Trunc;
    Reg hintOfResult = scan.facts(operation).hint;
    forEachLeafInParts(scan.m_liveness, operation, [&](unsigned childIndex, Operation* leaf) ALWAYS_INLINE_LAMBDA {
        readLeafOfScannedOperation(scan, operation, childIndex, leaf, scan.facts(leaf), early, hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult).index(), readsUpperHalf);
    }, [&](const Operation* user, unsigned childIndex, Operation* leaf) ALWAYS_INLINE_LAMBDA {
        // A fused Trunc has one child, which lies where the Trunc would.
        bool isBelowTrunc = user->opcode() == Trunc;
        Reg hint = isBelowTrunc && user == operation->child(0) ? hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult) : Reg();
        readLeafOfScannedOperation(scan, user, childIndex, leaf, scan.facts(leaf), early, hint.index(), !isBelowTrunc);
    }, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        readLeavesBelowScannedOperation(scan, operation, below, selectAbove, early);
    });
}

// As forEachLeaf() lists them. `own` is BackwardScan::facts() of the Patch.
static ALWAYS_INLINE void readOperandsOfScannedPatch(BackwardScan& scan, PatchOperation* patch, const ValueFacts& own, Position early)
{
    std::span<Operation* const> operands = patch->operands();
    unsigned numberOfOperands = static_cast<unsigned>(operands.size());
    if (!numberOfOperands)
        return;
    if (own.flags & flagsOfScannedOperationWithLeavesOfItsOwn) [[unlikely]] {
        readLeavesBelowScannedOperation(scan, patch, patch, nullptr, early);
        return;
    }
    Operation* const* operand = operands.data();
    const ValueRep* constraints = &patch->constraintOf(0);
    ValueFacts* facts = scan.m_facts;
    for (unsigned index = 0; index < numberOfOperands; ++index) {
        Operation* leaf = operand[index];
        ValueFacts& read = facts[leaf->index()];
        if (read.has(ValueFlag::IsFused)) [[unlikely]] {
            readLeavesBelowScannedOperation(scan, patch, leaf, nullptr, early);
            continue;
        }
        // hintOfScannedRead().
        const ValueRep& constraint = constraints[index];
        Reg hint = constraint.isReg() ? constraint.reg() : patch->registerToBringOperandTo(index);
        readLeafOfScannedOperation(scan, patch, index, leaf, read, early, hint.index(), true, true);
    }
}

// Operation::mustStay(). And the Branch behind a sum that goes by overflow reads the flags that it leaves.
static ALWAYS_INLINE bool scannedOperationMustStay(const Operation* operation, Opcode opcode, unsigned traits, const ValueFacts& own)
{
    bool mustStay = opcode == Patch ? static_cast<const PatchOperation*>(operation)->effects.mustStay() : (traits & ScannedOpcodeHasEffects) || operation->traps();
    ASSERT(mustStay == operation->mustStay());
    return mustStay || own.has(ValueFlag::GoesByOverflow);
}

// The operation stands in the block of the scan.
// Returns false where the operation is fused or dropped. Then no read was recorded and no range closed, so every count of live values is what it was.
static ALWAYS_INLINE bool scanOperationOfBlock(BackwardScan& scan, Block* block, Operation* operation, Position early)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    ValueFacts* facts = scan.m_facts;
    ValueFacts& own = facts[operation->index()];
    if (own.has(ValueFlag::IsFused)) {
        noteScannedReadOfWholeWord(scan, operation);
        return false;
    }

    Opcode opcode = operation->opcode();
    // It stays, and nothing is read by it or of it. It ends the block.
    if (opcode == Jump && own.lastUse == noPosition) {
        own.definition = early + 1;
        lookAtCountsOfScan(scan);
        return true;
    }
    if (opcode == Store && scan.tryUpdateInMemory(operation)) {
        own.flags |= scanBitOf(ValueFlag::IsDropped);
        return false;
    }

    unsigned traits = traitsOfScannedOpcodes.of[opcode];
    if (own.lastUse == noPosition) {
        if (!scannedOperationMustStay(operation, opcode, traits, own) || ((traits & ScannedOpcodeIsLoad) && operation->traps() && scan.tryHandOverTrap(operation))) {
            own.flags |= scanBitOf(ValueFlag::IsDropped);
            return false;
        }
    } else if (scan.m_hasRareRegions) {
        uint32_t region = scan.m_summariesOfReads[operation->index()].regionOfUses;
        if (region < BackwardScan::severalRegions && region != at.rareRegion && !scannedOperationMustStay(operation, opcode, traits, own) && scan.trySink(operation)) {
            own.flags |= scanBitOf(ValueFlag::IsSunk);
            closeScannedRangeTheLongWay(scan, operation, own, scan.m_order.headOf(scan.m_order.rareRegions()[region].onlyEntry), at.rareRegion);
            return true;
        }
    }

    // The results of a call do not live across it, so they are defined ahead of noting it.
    noteScannedReadOfWholeWord(scan, operation);
    closeScannedRange(scan, operation, own, early + 1, at.rareRegion);
    if (opcode == Patch) {
        PatchOperation* patch = static_cast<PatchOperation*>(operation);
        // In a loop a jump more at every turn counts. Elsewhere the bytes of the moves do, at every site.
        if (patch->bringsOperandsToTheirRegisters() && (at.isRare() || at.loop == noLoop || functionEndsBehindScannedPatch(patch)))
            patch->leaveOperandsWhereTheyLie();
        noteScannedCall(scan, patch, early + 1, block, at.rareRegion, at.loop, true);
        // Nothing is fused into a Patch.
        readOperandsOfScannedPatch(scan, patch, own, early);
        return true;
    }

    // Of no other opcode does Operation::isUnconditionalJump() hold. What ends the block is the first of its operations that the scan comes to.
    if (isTerminal(opcode)) {
        lookAtCountsOfScan(scan);
        if (operation->isUnconditionalJump())
            return true;
    }
    if (traits & ScannedOpcodeIsReadTheLongWay) [[unlikely]] {
        readLeavesOfScannedOperationTheLongWay(scan, operation, early);
        return true;
    }
    unsigned numberOfChildren = operation->numberOfInlineChildren();
    if (!numberOfChildren)
        return true;
    scan.fuseInto(operation);

    // From here on as forEachLeafInParts() goes through the leaves, with what hintOfScannedRead() says of an operation that is no Patch, Check or Return.
    if (own.flags & flagsOfScannedOperationWithLeavesOfItsOwn) [[unlikely]] {
        readLeavesBelowScannedOperation(scan, operation, operation, nullptr, early);
        return true;
    }
    // A read leaves its hint with a number alone, so what is no number has none to hand on.
    ASSERT(operation->type().isNumeric() || !own.hint);
    unsigned hintOfFirstChild = (traits & ScannedOpcodeHandsHintOnToFirstChild) ? own.hint.index() : noHintOfScannedRead;
    bool readsUpperHalf = opcode != Trunc;
    Operation* const* child = operation->inlineChildren();
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        Operation* below = child[index];
        ValueFacts& factsBelow = facts[below->index()];
        uint16_t flagsBelow = factsBelow.flags;
        if (!(flagsBelow & scanBitOf(ValueFlag::IsFused))) {
            readLeafOfScannedOperation(scan, operation, index, below, factsBelow, early, index ? noHintOfScannedRead : hintOfFirstChild, readsUpperHalf);
            continue;
        }
        Opcode opcodeBelow = below->opcode();
        if (opcode == Select || (flagsBelow & flagsOfScannedOperationWithLeavesOfItsOwn) || (traitsOfScannedOpcodes.of[opcodeBelow] & ScannedOpcodeIsReadTheLongWayWhereFused)) [[unlikely]] {
            readLeavesBelowScannedOperation(scan, operation, below, opcode == Select ? operation : nullptr, early);
            continue;
        }
        // A comparison below a Branch and a sum below a load are most of what is fused. A fused Trunc has one child, which lies where the Trunc would.
        bool isBelowTrunc = opcodeBelow == Trunc;
        unsigned hintOfFirstChildBelow = isBelowTrunc && below == child[0] ? hintOfFirstChild : noHintOfScannedRead;
        Operation* const* childBelow = below->inlineChildren();
        for (unsigned indexBelow = 0, numberOfChildrenBelow = below->numberOfInlineChildren(); indexBelow < numberOfChildrenBelow; ++indexBelow) {
            Operation* leaf = childBelow[indexBelow];
            ValueFacts& read = facts[leaf->index()];
            if (read.has(ValueFlag::IsFused)) {
                readLeavesBelowScannedOperation(scan, operation, leaf, opcodeBelow == Select ? below : nullptr, early);
                continue;
            }
            readLeafOfScannedOperation(scan, below, indexBelow, leaf, read, early, indexBelow ? noHintOfScannedRead : hintOfFirstChildBelow, !isBelowTrunc);
        }
    }
    return true;
}

Liveness* scanBackward(Procedure& proc)
{
    Liveness* liveness = proc.arena().create<Liveness>();
    BackwardScan scan(proc, *liveness);
    scan.run();
    return liveness;
}

BackwardScan::BackwardScan(Procedure& proc, Liveness& liveness)
    : m_proc(proc)
    , m_order(proc.order())
    , m_liveness(liveness)
{
    RegisterFile file = registerFileFor(proc.traits());
    for (unsigned bank = 0; bank < numberOfBanks; ++bank)
        m_callerSaved[bank] = file.allocatable[bank] & ~file.calleeSaved;

    Arena& arena = proc.arena();
    uint32_t numberOfOperations = proc.numberOfOperations();
    uint32_t numberOfLoops = static_cast<uint32_t>(m_order.loops().size());
    uint32_t mostOperations = m_order.mostOperations();
    m_liveness.m_values.growWithoutInitializing(arena, mostOperations);
    ValueFacts* facts = m_liveness.m_values.begin();
    uint32_t numberOfFactsMade = 0;
    for (; mostOperations - numberOfFactsMade >= 4; numberOfFactsMade += 4) {
        new (NotNull, facts + numberOfFactsMade) ValueFacts();
        new (NotNull, facts + numberOfFactsMade + 1) ValueFacts();
        new (NotNull, facts + numberOfFactsMade + 2) ValueFacts();
        new (NotNull, facts + numberOfFactsMade + 3) ValueFacts();
    }
    for (; numberOfFactsMade < mostOperations; ++numberOfFactsMade)
        new (NotNull, facts + numberOfFactsMade) ValueFacts();
    m_facts = facts;
    m_places = m_order.places();
    m_liveness.m_uses.reserve(arena, 2 * numberOfOperations + 1);
    m_liveness.m_uses.append(arena, Use { noPosition, 0, noPosition, noPosition });
    m_liveness.m_traitsOfUses = arena.allocateArray<uint8_t>(m_liveness.m_uses.capacity());
    m_liveness.m_traitsOfUses[0] = 0;
    m_liveness.m_loops.grow(arena, numberOfLoops);
    m_liveness.m_nextCallsBehindHeads.fill(arena, m_order.mostBlocks(), std::array<Position, numberOfBanks> { noPosition, noPosition });
    m_liveness.m_mostLiveInBlocks.fill(arena, m_order.mostBlocks(), std::array<uint16_t, numberOfBanks> { 0, 0 });
    m_liveness.m_endsOfWhatWaysLeadTo.fill(arena, m_order.mostBlocks(), noPosition);
    m_regions.grow(arena, static_cast<uint32_t>(m_order.rareRegions().size()));
    m_hasRareRegions = !m_regions.isEmpty();
    m_hasLoops = numberOfLoops;
    if (m_hasRareRegions) {
        m_summariesOfReads.fill(arena, numberOfOperations, SummaryOfReads { });
        findBlocksThatRareBlocksBranchOff();
    }
    m_loopStates.grow(arena, numberOfLoops);
    if (numberOfLoops) {
        m_loopCountedFor.fill(arena, numberOfOperations, noLoop);
        m_loopOfLastListedRead.fill(arena, numberOfOperations, noLoop);
        m_isPhiReadOnlyBehindLoop = BitSpan::create(arena, numberOfOperations);
        findLastExitingBlocksOfScannedLoops(*this);
    }
    if (isX86_64() && proc.holdsIntegersAddedTo())
        m_wordsReadWhole.fill(arena, numberOfOperations, 0);

#if ASSERT_ENABLED
    for (const Loop& loop : m_order.loops()) {
        ASSERT(loop.begin == m_order.headOf(m_order.blocksInOrder()[loop.firstBlock]));
        ASSERT(loop.end >= m_order.endOf(m_order.blocksInOrder()[loop.endBlock - 1]));
    }
#endif
}

// run() alone calls it.
ALWAYS_INLINE void BackwardScan::scanBlock(Block* block)
{
    ASSERT(block->isTerminated());
    uint32_t blockIndex = block->index();
    const BlockPlace& place = m_places[blockIndex];
    Position head = place.head;
    Position end = head + 2 * (block->size() + 1);
    ASSERT(end == m_order.endOf(block));
    noteWaysOnFrom(block, end);
    if (place.loop != noLoop) {
        const Loop& loop = m_order.loops().data()[place.loop];
        if (loop.endBlock == place.orderIndex + 1)
            enterLoopsEndingAt(loop.end);
    }

    // The Upsilons read at the place of the last operation and are scanned ahead of it: a result of that operation which one of them hands on has then been read when it is defined.
    Position early = end - 2;
    const PlaceOfReads& at = m_placeOfReads;
    const Block* origin = blockThatScannedBlockBranchesOff(*this, block, place);
    findPlaceOfReadsInScannedBlock(*this, m_placeOfReads, &place, origin, headOfBlockPassedOnEveryPathToScannedBlock(*this, place, origin));
    for (Operation* upsilon : block->upsilons()) {
        ValueFacts& factsOfUpsilon = m_facts[upsilon->index()];
        if (!upsilonStaysInScan(*this, block, upsilon, early)) {
            factsOfUpsilon.flags |= scanBitOf(ValueFlag::IsDropped);
            continue;
        }
        // define() does no more for what nothing reads.
        if (factsOfUpsilon.lastUse == noPosition) [[likely]]
            factsOfUpsilon.definition = early + 1;
        else
            define(upsilon, early + 1);
        Operation* child = upsilon->child(0);
        const Operation* phi = upsilon->phi();
        readValueInScannedBlock<KindOfScannedRead::ByUpsilon>(*this, child, m_facts[child->index()], early, m_facts[phi->index()].hint.index(), phi->has(OperationFlag::UpperHalfIsRead));
    }

    bool countsForPressureOfLoop = at.countsForLoops;
    bool standsBehindLastExit = countsForPressureOfLoop && !at.isRare() && head >= m_loopStates[at.loop].endOfLastExitingBlock;
    m_mostLiveInBlock = { 0, 0 };
    Operation* const* operations = block->operations().data();
    for (unsigned index = block->size(); index--; early -= 2) {
        // Where no count has changed since the last sample, another finds nothing. What ends the block is neither fused nor dropped, so there is a first one.
        if (scanOperationOfBlock(*this, block, operations[index], early) && countsForPressureOfLoop)
            samplePressureOfScan(*this, at.loop, standsBehindLastExit);
    }
    m_liveness.m_mostLiveInBlocks[blockIndex] = m_mostLiveInBlock;

    for (Operation* phi : block->phis()) {
        ValueFacts& factsOfPhi = m_facts[phi->index()];
        if (factsOfPhi.lastUse == noPosition)
            factsOfPhi.flags |= scanBitOf(ValueFlag::IsDropped);
        else
            closeScannedRange(*this, phi, factsOfPhi, head, at.rareRegion);
    }
    if (block->isLandingPad()) {
        m_nextLandingPad = head;
        m_nearestEventBehind = std::min(m_nearestEventBehind, head);
        m_liveness.m_hasLandingPads = true;
    }
    if (!at.isRare())
        m_liveness.m_nextCallsBehindHeads[blockIndex] = m_nextCallThatTakesRegisters;
    if (at.loop != noLoop && at.beginOfLoop == head)
        leaveLoopsBeginningAt(head);
}

void BackwardScan::run()
{
    std::span<Block* const> blocks = m_order.blocksInOrder();
    Block* const* firstBlock = blocks.data();
    for (size_t index = blocks.size(); index--;)
        scanBlock(firstBlock[index]);

    // Liveness::sunkInto() lists them as they were appended, and the scan meets the last one first.
    for (ArenaVector<Operation*>& sunk : m_liveness.m_sunk) {
        std::sort(sunk.begin(), sunk.end(), [](const Operation* left, const Operation* right) {
            return left->index() < right->index();
        });
    }
}

void BackwardScan::addUse(Operation* value, Position position, Reg hint)
{
    // The last block whose head does not stand behind the position, in a procedure without loops too.
    std::span<Block* const> blocks = m_order.blocksInOrder();
    auto behind = std::ranges::upper_bound(blocks, position, { }, [&](const Block* block) {
        return m_order.headOf(block);
    });
    ASSERT(behind != blocks.begin());
    const Block* blockAround = *(behind - 1);
    const BlockPlace& placeAround = m_order.of(blockAround);
    Position passedOnEveryPath = headOfBlockPassedOnEveryPathToScannedBlock(*this, placeAround, blockThatScannedBlockBranchesOff(*this, blockAround, placeAround));
    const BlockPlace* place = placeAroundScanPosition(m_order, position);
    PlaceOfReads at;
    findPlaceOfReadsInScannedBlock(*this, at, place, place ? blockThatScannedBlockBranchesOff(*this, blocks[place->orderIndex], *place) : nullptr, passedOnEveryPath);
    recordScannedRead<KindOfScannedRead::Added>(*this, at, value, facts(value), position, hint.index(), true);
}

void BackwardScan::define(Operation* operation, Position position)
{
    defineScannedValue(*this, operation, facts(operation), position, m_order.of(operation->owner()).rareRegion);
}

void BackwardScan::noteCall(const PatchOperation* patch, Position late, const Block* block)
{
    const BlockPlace& place = m_order.of(block);
    noteScannedCall(*this, patch, late, block, place.rareRegion, place.loop, false);
}

// A predecessor stands ahead of the block here, so it has its answer.
void BackwardScan::findBlocksThatRareBlocksBranchOff()
{
    ArenaVector<const Block*>& origins = m_liveness.m_blocksThatRareBlocksBranchOff;
    origins.fill(m_proc.arena(), m_order.mostBlocks(), nullptr);
    const BlockPlace* places = m_order.places();
    const Block** originOfBlock = origins.begin();
    for (const Block* block : m_order.blocksInOrder()) {
        const BlockPlace& place = places[block->index()];
        if (!place.isRare() || block->isLandingPad() || place.predecessors.size() != 1)
            continue;
        const Block* predecessor = place.predecessors.front();
        const BlockPlace& placeOfPredecessor = places[predecessor->index()];
        if (placeOfPredecessor.orderIndex < place.orderIndex)
            originOfBlock[block->index()] = placeOfPredecessor.rareRegion == place.rareRegion ? originOfBlock[predecessor->index()] : predecessor;
    }
}

void BackwardScan::enterLoopsEndingAt(Position end)
{
    std::span<const Loop> loops = m_order.loops();
    for (uint32_t loop = innermostLoopAtScanPosition(m_order, end - 1); loop != noLoop && loops[loop].end == end; loop = loops[loop].parent)
        samplePressureOfScan(*this, loop);
}

void BackwardScan::leaveLoopsBeginningAt(Position begin)
{
    std::span<const Loop> loops = m_order.loops();
    for (uint32_t loop = innermostLoopAtScanPosition(m_order, begin); loop != noLoop && loops[loop].begin == begin; loop = loops[loop].parent) {
        settleLowHalfUpdatesOf(loop);
        const LoopState& state = m_loopStates[loop];
        LoopFacts& result = m_liveness.m_loops[loop];
        for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
            // Whatever is live here is defined ahead of the loop.
            unsigned liveThrough = m_numberLive[bank] > state.outerValuesRead[bank] ? m_numberLive[bank] - state.outerValuesRead[bank] : 0;
            result.pressure[bank] = static_cast<uint16_t>(state.mostLive[bank] > liveThrough ? state.mostLive[bank] - liveThrough : 0);
        }
        result.registersTakenByPatches = state.registersTakenByPatches;
        result.endOfLastExitingBlock = state.endOfLastExitingBlock;
        result.valuesReadFromOutside = state.valuesReadFromOutside.span();

        uint32_t parent = loops[loop].parent;
        if (parent == noLoop || rareRegionOfScannedLoop(m_order, parent) != rareRegionOfScannedLoop(m_order, loop))
            continue;
        LoopState& outer = m_loopStates[parent];
        for (unsigned bank = 0; bank < numberOfBanks; ++bank)
            outer.mostLive[bank] = std::max(outer.mostLive[bank], state.mostLive[bank]);
    }
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
