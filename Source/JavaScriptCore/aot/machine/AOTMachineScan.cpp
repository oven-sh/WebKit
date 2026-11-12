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

static constexpr uint16_t flagsOfScannedOperationWithLeavesOfItsOwn = scanBitOf(ValueFlag::WorksInMemory) | scanBitOf(ValueFlag::UsesOverflowFlag);

static constexpr unsigned noHintOfScannedRead = Reg().index();

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

static constexpr bool childrenOfScannedOpcodeLieInLine(Opcode opcode)
{
    return opcode != Patch && opcode != Check && !namesSymbol(opcode);
}

enum TraitOfScannedOpcode : uint8_t {
    ScannedOpcodeHasEffects = 1 << 0,
    ScannedOpcodeIsLoad = 1 << 1,
    ScannedOpcodeHandsHintOnToFirstChild = 1 << 2,
    ScannedOpcodeIsReadTheLongWay = 1 << 3,
    ScannedOpcodeIsReadTheLongWayWhereFused = 1 << 4,
    ScannedOpcodeIsJumpOrStore = 1 << 5,
    ScannedOpcodeIsMemoryAccess = 1 << 6,
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
        if (!childrenOfScannedOpcodeLieInLine(opcode) || opcode == Return || (opcode == Select && resultPrefersRegisterOfFirstChild()))
            bits |= ScannedOpcodeIsReadTheLongWay;
        if (!childrenOfScannedOpcodeLieInLine(opcode) || opcode == Jump || opcode == Branch)
            bits |= ScannedOpcodeIsReadTheLongWayWhereFused;
        if (opcode == Jump || opcode == Store)
            bits |= ScannedOpcodeIsJumpOrStore;
        if (isMemoryAccess(opcode))
            bits |= ScannedOpcodeIsMemoryAccess;
        traits.of[index] = static_cast<uint8_t>(bits);
    }
    return traits;
}();

static uint32_t rareRegionOfScannedLoop(const Order& order, uint32_t loop)
{
    ASSERT(loop < order.loops().size());
    ASSERT(order.loops()[loop].firstBlock < order.blocksInOrder().size());
    return order.of(order.blocksInOrder().data()[order.loops().data()[loop].firstBlock]).rareRegion;
}

static ALWAYS_INLINE void lookAtCountsOfScan(BackwardScan& scan)
{
    static_assert(numberOfBanks == 2);
    uint16_t* mostLive = scan.m_mostLiveInBlock.data();
    mostLive[0] = std::max(mostLive[0], scan.m_numberLive[0]);
    mostLive[1] = std::max(mostLive[1], scan.m_numberLive[1]);
}

static ALWAYS_INLINE void lookAtCountsOfScanAtTerminator(BackwardScan& scan, bool isLastOfBlock)
{
    if (!isLastOfBlock) [[unlikely]] {
        lookAtCountsOfScan(scan);
        return;
    }
    static_assert(numberOfBanks == 2);
    uint16_t* mostLive = scan.m_mostLiveInBlock.data();
    mostLive[0] = scan.m_numberLive[0];
    mostLive[1] = scan.m_numberLive[1];
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

static ALWAYS_INLINE void noteLoopsLeftFromScannedBlock(BackwardScan& scan, const Block* block, uint32_t innermost, Position end)
{
    const BlockPlace* places = scan.m_places;
    const Loop* loops = scan.m_order.loops().data();
    BackwardScan::LoopState* states = scan.m_loopStates.begin();
    for (const Edge& edge : block->successors()) {
        uint32_t target = places[edge.block()->index()].orderIndex;
        if (innermost == noLoop || (target >= loops[innermost].firstBlock && target < loops[innermost].endBlock))
            continue;
        for (uint32_t loop = innermost;; loop = loops[loop].parent) {
            while (loop != noLoop && states[loop].endOfLastExitingBlock) {
                uint32_t around = states[loop].loopAroundThatMayLackExitingBlock;
                if (around != noLoop && states[around].endOfLastExitingBlock)
                    around = states[loop].loopAroundThatMayLackExitingBlock = states[around].loopAroundThatMayLackExitingBlock;
                loop = around;
            }
            if (loop == noLoop || (target >= loops[loop].firstBlock && target < loops[loop].endBlock))
                break;
            states[loop].endOfLastExitingBlock = end;
            states[loop].loopAroundThatMayLackExitingBlock = loops[loop].parent;
        }
    }
}

static void extendScannedRangeFrom(BackwardScan& scan, const Operation* value, uint32_t loop)
{
    if (loop == noLoop)
        return;
    const Loop* loops = scan.m_order.loops().data();
    Position home = scan.m_order.headOf(value->owner());
    ValueFacts& read = scan.facts(value);
    for (; loop != noLoop && loops[loop].begin > home; loop = loops[loop].loopAroundOutsideRun)
        read.lastUse = std::max(read.lastUse, loops[loop].end);
}

static ALWAYS_INLINE void countScannedReadInsideLoops(BackwardScan& scan, const Operation* value, uint32_t loop, Position readAhead, Position readBehind, bool revives)
{
    if (loop == noLoop || scan.m_loopCountedFor[value->index()] == loop)
        return;
    const Loop* loops = scan.m_order.loops().data();
    const BlockPlace& home = scan.m_places[value->owner()->index()];
    uint32_t firstNotCounted = innermostLoopAroundBoth(loops, home.loop, loops[loop].begin);
    if (readAhead != noPosition) [[unlikely]] {
        for (firstNotCounted = loop; firstNotCounted != noLoop && loops[firstNotCounted].begin > std::max(home.head, readAhead);)
            firstNotCounted = loops[firstNotCounted].parent;
    }
    for (uint32_t entered = loop; entered != firstNotCounted && readBehind >= loops[entered].end; entered = loops[entered].loopAroundOutsideRun) {
        BackwardScan::LoopState& state = scan.m_loopStates[entered];
        bool runEndsCount = firstNotCounted != noLoop && loops[firstNotCounted].loopAroundOutsideRun == loops[entered].loopAroundOutsideRun;
        forEachBankOfScannedValue(scan.m_proc, value, [&](unsigned bank) {
            ++state.outerValuesRead[bank];
            if (runEndsCount)
                ++scan.m_loopStates[firstNotCounted].outerValuesOfRunNotCounted[bank];
            if (revives) {
                incrementScanCount(state.mostLive[bank]);
                if (state.registersTakenByPatches)
                    incrementScanCount(state.mostLiveAcrossPatches[bank]);
            }
        });
        scan.m_loopCountedFor[value->index()] = loop;
        if (runEndsCount)
            break;
    }
}

static NEVER_INLINE void listScannedReadFromOutsideLoops(BackwardScan& scan, Operation* value, const PlaceOfScannedReads& at)
{
    if (!at.countsForLoops)
        return;
    const Loop* loops = scan.m_order.loops().data();
    uint32_t& loopOfLastListedRead = scan.m_loopOfLastListedRead[value->index()];
    for (uint32_t next = at.loop; next != noLoop;) {
        uint32_t loop = scan.m_loopStates[next].loopThatListsReads;
        if (loop == noLoop)
            break;
        next = loops[loop].parent;
        if (!value->hasNoPosition() && loops[loop].begin <= scan.m_order.headOf(value->owner()))
            break;
        if (loopOfLastListedRead != noLoop && loops[loop].firstBlock <= loops[loopOfLastListedRead].firstBlock && loops[loopOfLastListedRead].endBlock <= loops[loop].endBlock)
            break;
        scan.m_loopStates[loop].valuesReadFromOutside.append(scan.m_proc.arena(), value);
    }
    loopOfLastListedRead = at.loop;
}

static NEVER_INLINE bool scannedLeafWithoutPositionNeedsRegister(BackwardScan& scan, const Operation* user, unsigned childIndex, const Operation* leaf)
{
    const Liveness& liveness = scan.m_liveness;
    if (leaf->opcode() == ArgumentReg || user->as<PatchOperation>())
        return false;
    bool isTested = isComparison(user->opcode()) || (user->opcode() == BitAnd && liveness.isFused(user));
    if (std::optional<unsigned> held = isTested ? heldConstantIndex(*leaf) : std::nullopt) {
        uint8_t& comparisons = scan.m_loopStates[scan.m_placeOfReads.loop].comparisonsWithHeldConstants[*held];
        return comparisons < fewestHeldConstantReads && ++comparisons == fewestHeldConstantReads;
    }
    if ((leaf->hasInt64() || leaf->hasDouble()) && findPinnedConstant(static_cast<uint64_t>(leaf->constantBits())))
        return false;
    if (isX86_64() && !childIndex && !liveness.isFused(user) && !leaf->type().isFloat()) {
        Opcode opcode = user->opcode();
        if ((resultOverwritesFirstChild(opcode) && !isCommutative(opcode)) || isFreeWhenCoalesced(liveness, user))
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

static ALWAYS_INLINE Position headOfBlockPassedOnEveryPathToScannedBlock(const BackwardScan& scan, const BlockPlace& place, const Block* origin)
{
    const Block* passed = origin;
    if (!passed && !place.predecessors.empty())
        passed = place.predecessors.back();
    return passed ? scan.m_places[passed->index()].head : noPosition;
}

static ALWAYS_INLINE uint16_t flagsOfWhatIsReadInScannedRegion(const BackwardScan& scan, uint32_t rareRegion)
{
    return rareRegion != noRareRegion && !scan.m_kindsOfRegions.isEmpty() && (scan.m_kindsOfRegions[rareRegion] & BackwardScan::regionHoldsLandingPad) ? scanBitOf(ValueFlag::LivesIntoLandingPad) : 0;
}

template<bool regionsHaveKinds = true>
static ALWAYS_INLINE void findPlaceOfReadsInScannedBlock(const BackwardScan& scan, PlaceOfScannedReads& at, uint32_t loop, uint32_t rareRegion, const Block* origin, Position passedOnEveryPath)
{
    at.passedOnEveryPath = passedOnEveryPath;
    at.loop = loop;
    at.rareRegion = rareRegion;
    at.traitsOfUses = rareRegion != noRareRegion ? Liveness::useIsInRareRegion : 0;
    if constexpr (regionsHaveKinds)
        at.flagsOfWhatIsRead = flagsOfWhatIsReadInScannedRegion(scan, rareRegion);
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
    uint32_t behind;
    Position readAhead;
};

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

static ALWAYS_INLINE void noteRareRegionOfScannedUse(uint32_t& regionOfUses, uint32_t region)
{
    ASSERT(region != noRareRegion);
    if (regionOfUses != noRareRegion && regionOfUses != region)
        regionOfUses = BackwardScan::severalRegions;
    else
        regionOfUses = region;
}

static ALWAYS_INLINE void noteRegionOfScannedUse(uint32_t& regionOfUses, uint32_t region)
{
    if (region == noRareRegion)
        regionOfUses = BackwardScan::severalRegions;
    else
        noteRareRegionOfScannedUse(regionOfUses, region);
}

static ALWAYS_INLINE uint16_t flagsWithWhetherScannedValueLivesIntoLandingPad(const BackwardScan& scan, uint16_t flags, Position lastUse, uint32_t regionOfDefinition)
{
    constexpr uint16_t livesIntoLandingPad = scanBitOf(ValueFlag::LivesIntoLandingPad);
    if (regionOfDefinition != noRareRegion && !scan.m_kindsOfRegions.isEmpty() && (scan.m_kindsOfRegions[regionOfDefinition] & BackwardScan::regionHoldsLandingPad)) [[unlikely]]
        return static_cast<uint16_t>(lastUse >= scan.m_nextLandingPad ? flags | livesIntoLandingPad : flags & ~livesIntoLandingPad);
    return static_cast<uint16_t>(lastUse >= scan.m_nextLandingPadThatComesBack ? flags | livesIntoLandingPad : flags);
}

static NEVER_INLINE bool failScannedRead(BackwardScan& scan, const PlaceOfScannedReads& at, const Operation* value, bool notesRegion)
{
    scan.m_proc.fail(FailureKind::MalformedProcedure, value);
    if (value->hasNoPosition())
        return false;
    if (notesRegion && scan.m_hasRareRegions)
        noteRegionOfScannedUse(scan.m_summariesOfReads[value->index()].regionOfUses, at.rareRegion);
    return at.loop != noLoop && scan.m_order.headOf(value->owner()) < at.beginOfLoop;
}

static void countScannedReadExtendedOverLoopsForCalls(BackwardScan& scan, const Operation* value, SummaryOfScannedReads& summary, uint32_t loop, Position position)
{
    ASSERT(loop != noLoop);
    const Loop* loops = scan.m_order.loops().data();
    Position home = scan.m_order.headOf(value->owner());
    uint32_t walkedFrom = loops[loop].parent == noLoop ? noLoop : std::exchange(scan.m_loopOfLastReadExtendedForCalls[value->index()], loop);
    for (; loop != noLoop && loops[loop].begin > home; loop = loops[loop].loopAroundOutsideRun) {
        if (walkedFrom != noLoop && loops[loop].firstBlock <= loops[walkedFrom].firstBlock && loops[walkedFrom].endBlock <= loops[loop].endBlock)
            break;
        position = std::max(position, loops[loop].end);
    }
    summary.lastReadCountedForCalls = std::max(summary.lastReadCountedForCalls, position);
}

enum class KindOfScannedRead : uint8_t {
    ByOperation,
    ByUpsilon,
    Added,
};

template<KindOfScannedRead kind>
static ALWAYS_INLINE bool recordScannedRead(BackwardScan& scan, const PlaceOfScannedReads& at, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    constexpr bool notesRegion = kind != KindOfScannedRead::Added;

    uint16_t flags = read.flags | at.flagsOfWhatIsRead;
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
    if (scan.m_hasLoops && value->opcode() == Phi)
        noteScannedReadOfPhi(scan, value, position, revives);
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
    SummaryOfScannedReads& summary = scan.m_summariesOfReads[value->index()];
    if (loopThatCountsForCalls != noLoop)
        countScannedReadExtendedOverLoopsForCalls(scan, value, summary, loopThatCountsForCalls, position);
    else
        summary.lastReadCountedForCalls = std::max(summary.lastReadCountedForCalls, position);
    if constexpr (notesRegion)
        noteRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
    return isDefinedAheadOfLoop;
}

template<KindOfScannedRead kind>
static NEVER_INLINE void readValueInScannedBlockTheLongWay(BackwardScan& scan, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    [[maybe_unused]] bool isDefinedAheadOfLoop = recordScannedRead<kind>(scan, at, value, read, position, hint, readsUpperHalf);
    if constexpr (kind == KindOfScannedRead::ByOperation) {
        if (!at.countsForLoops || value->hasNoPosition())
            return;
        if (isDefinedAheadOfLoop)
            listScannedReadFromOutsideLoops(scan, value, at);
        else
            scan.m_loopOfLastListedRead[value->index()] = at.loop;
    }
}

static NEVER_INLINE void noteScannedReadThatCountsForCallsInLoop(BackwardScan& scan, const Operation* value)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    SummaryOfScannedReads& summary = scan.m_summariesOfReads[value->index()];
    countScannedReadExtendedOverLoopsForCalls(scan, value, summary, at.loopThatCountsForCalls, at.positionThatCountsForCalls);
    noteRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
}

template<KindOfScannedRead kind, bool regionsHaveKinds = true>
static ALWAYS_INLINE void readValueInScannedBlock(BackwardScan& scan, Operation* value, ValueFacts& read, Position position, unsigned hint, bool readsUpperHalf)
{
    static_assert(kind != KindOfScannedRead::Added);
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    ArenaVector<Use>& uses = scan.uses();
    uint8_t*& traitsOfUses = scan.traitsOfUses();
    uint16_t flags = read.flags;
    if constexpr (regionsHaveKinds)
        flags |= at.flagsOfWhatIsRead;
    Type type = value->type();
    const Block* owner = value->owner();
    uint32_t newUse = uses.size();
    uint32_t behind = read.firstUse;
    Use* firstOfUses = uses.begin();

    if (read.definition != noPosition || (flags & scanBitOf(ValueFlag::IsDropped)) || !type.isNumeric() || newUse == uses.capacity() || firstOfUses[behind].position < position) [[unlikely]]
        return readValueInScannedBlockTheLongWay<kind>(scan, value, read, position, hint, readsUpperHalf);
    if (at.loop != noLoop && owner && scan.m_places[owner->index()].head < at.beginOfLoop) [[unlikely]]
        return readValueInScannedBlockTheLongWay<kind>(scan, value, read, position, hint, readsUpperHalf);

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
            uint16_t& mostLive = scan.m_mostLiveInBlock.data()[bank];
            mostLive = std::max(mostLive, numberLive);
        }
        read.lastUse = position;
    } else if (position >= lastUse)
        read.lastUse = position;
    if (value->opcode() == Phi && scan.m_hasLoops && scan.m_places[owner->index()].isLoopHead) [[unlikely]]
        noteScannedReadOfPhi(scan, value, position, lastUse == noPosition);

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
    if (at.isRare())
        noteRareRegionOfScannedUse(summary.regionOfUses, at.rareRegion);
    else
        summary.regionOfUses = BackwardScan::severalRegions;
}

static ALWAYS_INLINE void defineScannedValue(BackwardScan& scan, Operation* operation, ValueFacts& own, Position position, uint32_t regionOfBlock)
{
    own.definition = position;
    Position lastUse = own.lastUse;
    if (lastUse == noPosition)
        return;

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
    if (lastUse > scan.m_nextSafepoint)
        flags |= static_cast<uint16_t>(ValueFlag::LivesAcrossSafepoint);
    if (lastUse > scan.m_nextSuspension)
        flags |= static_cast<uint16_t>(ValueFlag::LivesAcrossSuspension);
    flags = flagsWithWhetherScannedValueLivesIntoLandingPad(scan, flags, lastUse, regionOfBlock);
    own.flags = flags;

    if (operation->has(OperationFlag::IsInnerPointer) || (flags & static_cast<uint16_t>(ValueFlag::LivesAcrossSafepoint)))
        scan.applyCollectorRules(operation);
}

static NEVER_INLINE void closeScannedRangeTheLongWay(BackwardScan& scan, Operation* operation, ValueFacts& own, Position definition, uint32_t regionOfBlock)
{
    defineScannedValue(scan, operation, own, definition, regionOfBlock);
    if (own.lastUse == noPosition)
        return;
    forEachBankOfScannedValue(scan.m_proc, operation, [&](unsigned bank) {
        decrementScanCount(scan.m_numberLive[bank]);
    });
    lookAtCountsOfScan(scan);
    if (scan.m_hasLoops)
        endCountOfPhiReadOnlyBehindLoop(scan, operation);
}

template<bool regionsHaveKinds = true>
static ALWAYS_INLINE void closeScannedRangeOfReadValue(BackwardScan& scan, Operation* operation, ValueFacts& own, Position lastUse, Position lastReadCountedForCalls, Position definition, uint32_t regionOfBlock)
{
    ASSERT(lastUse != noPosition && lastUse == own.lastUse);
    ASSERT(lastReadCountedForCalls == (scan.m_summariesOfReads.isEmpty() ? lastUse : scan.m_summariesOfReads[operation->index()].lastReadCountedForCalls));
    uint16_t flags = own.flags;
    Type type = operation->type();
    if ((flags & scanBitOf(ValueFlag::IsSunk)) || !type.isNumeric()) [[unlikely]] {
        closeScannedRangeTheLongWay(scan, operation, own, definition, regionOfBlock);
        return;
    }

    own.definition = definition;
    unsigned bank = static_cast<unsigned>(bankOf(type));
    if (std::max(lastUse, lastReadCountedForCalls) >= scan.m_nearestEventBehind || (regionsHaveKinds && (flags & scanBitOf(ValueFlag::LivesIntoLandingPad)))) {
        Position nextCall = scan.m_nextCall[bank];
        if (regionOfBlock < scan.m_regions.size())
            nextCall = std::min(nextCall, scan.m_regions[regionOfBlock].nextCall[bank]);
        if (lastReadCountedForCalls > nextCall)
            flags |= scanBitOf(ValueFlag::LivesAcrossCall);
        if (lastUse > scan.m_nextSafepoint)
            flags |= scanBitOf(ValueFlag::LivesAcrossSafepoint);
        if (lastUse > scan.m_nextSuspension)
            flags |= scanBitOf(ValueFlag::LivesAcrossSuspension);
        flags = flagsWithWhetherScannedValueLivesIntoLandingPad(scan, flags, lastUse, regionOfBlock);
        own.flags = flags;
    }

    bool isSubjectToCollectorRules = operation->has(OperationFlag::IsInnerPointer) || (flags & scanBitOf(ValueFlag::LivesAcrossSafepoint));
    if (isSubjectToCollectorRules)
        scan.applyCollectorRules(operation);
    decrementScanCount(scan.m_numberLive[bank]);
    if (isSubjectToCollectorRules)
        lookAtCountsOfScan(scan);
    if (operation->opcode() == Phi && scan.m_hasLoops)
        endCountOfPhiReadOnlyBehindLoop(scan, operation);
}

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

static NEVER_INLINE void noteScannedReadOfWholeWordTheLongWay(BackwardScan& scan, const Operation* operation)
{
    if (operation->opcode() != Load || operation->type() != Int64 || scan.m_order.of(operation->owner()).isRare())
        return;
    scan.m_wordsReadWhole[operation->lastChild()->index()] |= BackwardScan::bitOfWord(operation->offset());
}

static ALWAYS_INLINE void noteScannedReadOfWholeWord(BackwardScan& scan, const Operation* operation)
{
    if (!scan.m_wordsReadWhole.isEmpty()) [[unlikely]]
        noteScannedReadOfWholeWordTheLongWay(scan, operation);
}

static bool functionEndsBehindScannedPatch(const PatchOperation* patch)
{
    const Block* block = patch->owner();
    if (patch->endsBlock())
        return !block->numSuccessors() && !patch->leavesByReturning();
    return patch->indexInBlock() + 2 == block->size() && block->last()->opcode() == Oops;
}

static NEVER_INLINE uint32_t findNearestScannedLoopAroundOfOtherRegion(BackwardScan& scan, uint32_t loop)
{
    const Loop* loops = scan.m_order.loops().data();
    BackwardScan::LoopState* states = scan.m_loopStates.begin();
    uint32_t region = rareRegionOfScannedLoop(scan.m_order, loop);
    uint32_t found = loops[loop].parent;
    while (found != noLoop && rareRegionOfScannedLoop(scan.m_order, found) == region) {
        uint32_t known = states[found].nearestLoopAroundOfOtherRegion;
        found = known != BackwardScan::LoopState::notLookedFor ? known : loops[found].parent;
    }
    while (loop != found && states[loop].nearestLoopAroundOfOtherRegion == BackwardScan::LoopState::notLookedFor) {
        states[loop].nearestLoopAroundOfOtherRegion = found;
        loop = loops[loop].parent;
    }
    return found;
}

static ALWAYS_INLINE uint32_t innermostLoopThatCountsScannedCall(BackwardScan& scan, uint32_t loop, uint32_t region)
{
    while (loop != noLoop && rareRegionOfScannedLoop(scan.m_order, loop) != region) {
        uint32_t known = scan.m_loopStates[loop].nearestLoopAroundOfOtherRegion;
        loop = known != BackwardScan::LoopState::notLookedFor ? known : findNearestScannedLoopAroundOfOtherRegion(scan, loop);
    }
    return loop;
}

static NEVER_INLINE void noteRegistersTakenByScannedCallInLoops(BackwardScan& scan, const PatchOperation* patch, const Block* block, uint32_t loop, uint32_t region, RegisterMask clobbered, bool isReachedByScan)
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
    ASSERT(rareRegionOfScannedLoop(scan.m_order, loop) == region);
    if (!taken)
        return;
    BackwardScan::LoopState& state = scan.m_loopStates[loop];
    state.registersTakenByPatches |= taken;
    if (!isReachedByScan)
        return;
    const bool standsBehindLastExit = region == noRareRegion && scan.m_order.headOf(block) >= state.endOfLastExitingBlock;
    for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
        uint16_t numberLive = scan.m_numberLive[bank];
        if (standsBehindLastExit)
            numberLive -= std::min(numberLive, state.phisReadOnlyBehindLoop[bank]);
        state.mostLiveAcrossPatches[bank] = std::max(state.mostLiveAcrossPatches[bank], numberLive);
    }
}

static NEVER_INLINE void makeListsOfLastCallsInScannedBlocks(BackwardScan& scan)
{
    scan.lastCallsInBlocks().fill(scan.m_proc.arena(), scan.m_order.mostBlocks(), std::array<Position, numberOfBanks> { 0, 0 });
    scan.nextCallsBehindEnds().fill(scan.m_proc.arena(), scan.m_order.mostBlocks(), std::array<Position, numberOfBanks> { noPosition, noPosition });
}

static ALWAYS_INLINE void noteScannedCall(BackwardScan& scan, const PatchOperation* patch, Position late, const Block* block, uint32_t region, uint32_t loop, bool isReachedByScan)
{
    bool leadsToLandingPad = false;
    if (patch->effects.terminal) {
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
        if (patch->runsWithoutFrame() && !(patch->effects.terminal && !patch->owner()->numSuccessors()) && !patch->showsRegistersToCollector()) [[unlikely]]
            scan.m_proc.fail(FailureKind::MissingFrameSetup, patch);
        scan.m_nextSafepoint = late;
        isEvent = true;
    }
    if (patch->isSuspension()) {
        scan.m_nextSuspension = late;
        isEvent = true;
    }
    if (isEvent)
        scan.m_nearestEventBehind = std::min(scan.m_nearestEventBehind, late);

    loop = innermostLoopThatCountsScannedCall(scan, loop, region);
    if (loop != noLoop)
        noteRegistersTakenByScannedCallInLoops(scan, patch, block, loop, region, clobbered, isReachedByScan);
}

template<bool regionsHaveKinds = true>
static ALWAYS_INLINE void readLeafOfScannedOperation(BackwardScan& scan, const Operation* user, unsigned childIndex, Operation* leaf, ValueFacts& read, Position early, unsigned hint, bool readsUpperHalf, bool userIsPatch = false)
{
    readValueInScannedBlock<KindOfScannedRead::ByOperation, regionsHaveKinds>(scan, leaf, read, early, hint, readsUpperHalf);
    if (userIsPatch)
        return;
    if (scan.m_placeOfReads.countsForLoops && leaf->hasNoPosition() && scan.m_loopOfLastListedRead[leaf->index()] != scan.m_placeOfReads.loop && scannedLeafWithoutPositionNeedsRegister(scan, user, childIndex, leaf)) [[unlikely]]
        listScannedReadFromOutsideLoops(scan, leaf, scan.m_placeOfReads);
}

static NEVER_INLINE void readLeafOfScannedOperationOutOfLine(BackwardScan& scan, const Operation* user, unsigned childIndex, Operation* leaf, Position early, unsigned hint, bool readsUpperHalf)
{
    readLeafOfScannedOperation(scan, user, childIndex, leaf, scan.facts(leaf), early, hint, readsUpperHalf);
}

static NEVER_INLINE void readLeavesBelowScannedOperation(BackwardScan& scan, Operation* operation, const Operation* below, const Operation* selectAbove, Position early)
{
    const ValueFacts& own = scan.facts(operation);
    const PatchOperation* patch = operation->as<PatchOperation>();
    Reg hintOfResult = own.hint;
    bool isBranchOnOverflow = own.has(ValueFlag::UsesOverflowFlag) && operation->opcode() == Branch;
    bool nextLeafStandsForFirstChild = own.has(ValueFlag::UsesOverflowFlag) && !isBranchOnOverflow;
    forEachLeafBelow(scan.m_liveness, below, [&](const Operation* user, unsigned childIndex, Operation* leaf) {
        bool standsForFirstChild = (user->opcode() == Trunc && user == operation->child(0)) || std::exchange(nextLeafStandsForFirstChild, false);
        Reg hint = user == operation || standsForFirstChild ? hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult) : Reg();
        readLeafOfScannedOperationOutOfLine(scan, user, childIndex, leaf, early, hint.index(), user->opcode() != Trunc && !isBranchOnOverflow);
    }, selectAbove);
}

static NEVER_INLINE void readLeavesOfScannedOperationTheLongWay(BackwardScan& scan, Operation* operation, Position early)
{
    if (!operation->numChildren() || operation->isUnconditionalJump())
        return;
    if (operation->opcode() != Patch)
        scan.fuseInto(operation);
    const PatchOperation* patch = operation->as<PatchOperation>();
    bool readsUpperHalf = operation->opcode() != Trunc;
    Reg hintOfResult = scan.facts(operation).hint;
    forEachLeafInParts(scan.m_liveness, operation, [&](unsigned childIndex, Operation* leaf) ALWAYS_INLINE_LAMBDA {
        readLeafOfScannedOperationOutOfLine(scan, operation, childIndex, leaf, early, hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult).index(), readsUpperHalf);
    }, [&](const Operation* user, unsigned childIndex, Operation* leaf) ALWAYS_INLINE_LAMBDA {
        bool isBelowTrunc = user->opcode() == Trunc;
        Reg hint = (isBelowTrunc || user->opcode() == ZExt32) && user == operation->child(0) ? hintOfScannedRead(operation, patch, childIndex, leaf, hintOfResult) : Reg();
        readLeafOfScannedOperationOutOfLine(scan, user, childIndex, leaf, early, hint.index(), !isBelowTrunc);
    }, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        readLeavesBelowScannedOperation(scan, operation, below, selectAbove, early);
    });
}

static NEVER_INLINE void readChildrenOfScannedReturn(BackwardScan& scan, Operation* operation, const ValueFacts& own, Position early)
{
    ASSERT(operation->opcode() == Return);
    ValueFacts* facts = scan.m_facts;
    Operation* const* child = operation->inlineChildren();
    unsigned numberOfChildren = operation->numberOfInlineChildren();
    unsigned flagsThatLeadTheLongWay = own.flags & flagsOfScannedOperationWithLeavesOfItsOwn;
    for (unsigned index = 0; index < numberOfChildren; ++index)
        flagsThatLeadTheLongWay |= facts[child[index]->index()].flags & scanBitOf(ValueFlag::IsFused);
    if (flagsThatLeadTheLongWay) [[unlikely]]
        return readLeavesOfScannedOperationTheLongWay(scan, operation, early);
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        Operation* leaf = child[index];
        Reg hint = leaf->type().isFloat() ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR);
        readLeafOfScannedOperationOutOfLine(scan, operation, index, leaf, early, hint.index(), true);
    }
}

template<bool regionsHaveKinds>
static NEVER_INLINE void readChildrenOfScannedSelect(BackwardScan& scan, Operation* select, const ValueFacts& own, Position early)
{
    ASSERT(select->opcode() == Select);
    scan.fuseInto(select);
    if (own.flags & flagsOfScannedOperationWithLeavesOfItsOwn) [[unlikely]] {
        readLeavesBelowScannedOperation(scan, select, select, nullptr, early);
        return;
    }
    ValueFacts* facts = scan.m_facts;
    Operation* const* child = select->inlineChildren();
    unsigned numberOfChildren = select->numberOfInlineChildren();
    unsigned hintOfArms = own.hint.index();
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        Operation* below = child[index];
        ValueFacts& factsBelow = facts[below->index()];
        if (factsBelow.flags & scanBitOf(ValueFlag::IsFused)) {
            readLeavesBelowScannedOperation(scan, select, below, select, early);
            continue;
        }
        bool isMadeAheadOfLoop = scan.m_placeOfReads.loop != noLoop && below->owner() && scan.m_places[below->owner()->index()].head < scan.m_placeOfReads.beginOfLoop;
        bool diesHere = !isMadeAheadOfLoop && (factsBelow.lastUse == noPosition || factsBelow.lastUse == early);
        readLeafOfScannedOperation<regionsHaveKinds>(scan, select, index, below, factsBelow, early, index && diesHere ? hintOfArms : noHintOfScannedRead, true);
    }
}

template<bool regionsHaveKinds = true>
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
        const ValueRep& constraint = constraints[index];
        Reg hint = constraint.isReg() ? constraint.reg() : patch->registerToBringOperandTo(index);
        readLeafOfScannedOperation<regionsHaveKinds>(scan, patch, index, leaf, read, early, hint.index(), true, true);
    }
}

static ALWAYS_INLINE bool scannedOperationMustStay(const Operation* operation, Opcode opcode, unsigned traits, const ValueFacts& own)
{
    bool mustStay = opcode == Patch ? static_cast<const PatchOperation*>(operation)->effects.mustStay() : (traits & ScannedOpcodeHasEffects) || operation->traps();
    ASSERT(mustStay == operation->mustStay());
    return mustStay || own.has(ValueFlag::UsesOverflowFlag);
}

template<bool regionsHaveKinds = true>
static NEVER_INLINE void readLeavesBelowFusedChildOfScannedOperation(BackwardScan& scan, Operation* operation, const Operation* below, Opcode opcodeBelow, unsigned hintOfFirstChild, Position early)
{
    ValueFacts* facts = scan.m_facts;
    bool isBelowTrunc = opcodeBelow == Trunc;
    unsigned hintOfFirstChildBelow = (isBelowTrunc || opcodeBelow == ZExt32) && below == operation->inlineChildren()[0] ? hintOfFirstChild : noHintOfScannedRead;
    Operation* const* childBelow = below->inlineChildren();
    for (unsigned indexBelow = 0, numberOfChildrenBelow = below->numberOfInlineChildren(); indexBelow < numberOfChildrenBelow; ++indexBelow) {
        Operation* leaf = childBelow[indexBelow];
        ValueFacts& read = facts[leaf->index()];
        uint16_t flagsOfLeaf = read.flags;
        if (flagsOfLeaf & scanBitOf(ValueFlag::IsFused)) {
            Opcode opcodeOfLeaf = leaf->opcode();
            if (opcodeBelow != Select && !(flagsOfLeaf & flagsOfScannedOperationWithLeavesOfItsOwn) && !(traitsOfScannedOpcodes.of[opcodeOfLeaf] & ScannedOpcodeIsReadTheLongWayWhereFused)) [[likely]] {
                readLeavesBelowFusedChildOfScannedOperation<regionsHaveKinds>(scan, operation, leaf, opcodeOfLeaf, hintOfFirstChild, early);
                continue;
            }
            readLeavesBelowScannedOperation(scan, operation, leaf, opcodeBelow == Select ? below : nullptr, early);
            continue;
        }
        readLeafOfScannedOperation<regionsHaveKinds>(scan, below, indexBelow, leaf, read, early, indexBelow ? noHintOfScannedRead : hintOfFirstChildBelow, !isBelowTrunc);
    }
}

template<bool regionsHaveKinds = true>
static ALWAYS_INLINE bool scanOperationOfBlock(BackwardScan& scan, Block* block, Operation* operation, Position early, Position earlyOfLastOperation)
{
    const PlaceOfScannedReads& at = scan.m_placeOfReads;
    ValueFacts* facts = scan.m_facts;
    ValueFacts& own = facts[operation->index()];
    if (own.has(ValueFlag::IsFused)) {
        noteScannedReadOfWholeWord(scan, operation);
        return false;
    }

    Opcode opcode = operation->opcode();
    unsigned traits = traitsOfScannedOpcodes.of[opcode];
    Position lastUse = own.lastUse;
    if (traits & ScannedOpcodeIsJumpOrStore) {
        if (opcode == Jump) {
            if (lastUse == noPosition) {
                own.definition = early + 1;
                lookAtCountsOfScanAtTerminator(scan, early == earlyOfLastOperation);
                return true;
            }
        } else if (scan.tryUpdateInMemory(operation)) {
            own.flags |= scanBitOf(ValueFlag::IsDropped);
            return false;
        }
    }

    uint32_t rareRegion = at.rareRegion;
    if (lastUse == noPosition) {
        if (!scannedOperationMustStay(operation, opcode, traits, own) || ((traits & ScannedOpcodeIsLoad) && operation->traps() && scan.tryHandOverTrap(operation))) {
            own.flags |= scanBitOf(ValueFlag::IsDropped);
            return false;
        }
        noteScannedReadOfWholeWord(scan, operation);
        own.definition = early + 1;
    } else {
        Position lastReadCountedForCalls = lastUse;
        if (scan.m_hasRareRegions) {
            const SummaryOfScannedReads& summary = scan.m_summariesOfReads[operation->index()];
            uint32_t region = summary.regionOfUses;
            if (region < BackwardScan::severalRegions && region != rareRegion && !scannedOperationMustStay(operation, opcode, traits, own) && scan.trySink(operation)) {
                own.flags |= scanBitOf(ValueFlag::IsSunk);
                closeScannedRangeTheLongWay(scan, operation, own, scan.m_order.headOf(scan.m_order.rareRegions()[region].onlyEntry), rareRegion);
                return true;
            }
            lastReadCountedForCalls = summary.lastReadCountedForCalls;
        }
        noteScannedReadOfWholeWord(scan, operation);
        closeScannedRangeOfReadValue<regionsHaveKinds>(scan, operation, own, lastUse, lastReadCountedForCalls, early + 1, rareRegion);
    }
    if (opcode == Patch) {
        PatchOperation* patch = static_cast<PatchOperation*>(operation);
        if (patch->bringsOperandsToTheirRegisters() && (rareRegion != noRareRegion || functionEndsBehindScannedPatch(patch)))
            patch->leaveOperandsWhereTheyLie();
        noteScannedCall(scan, patch, early + 1, block, rareRegion, at.loop, true);
        readOperandsOfScannedPatch<regionsHaveKinds>(scan, patch, own, early);
        return true;
    }

    if (isTerminal(opcode)) {
        lookAtCountsOfScanAtTerminator(scan, early == earlyOfLastOperation);
        if (operation->isUnconditionalJump())
            return true;
    }
    if (traits & ScannedOpcodeIsReadTheLongWay) [[unlikely]] {
        if (opcode == Return)
            readChildrenOfScannedReturn(scan, operation, own, early);
        else if (resultPrefersRegisterOfFirstChild() && opcode == Select)
            readChildrenOfScannedSelect<regionsHaveKinds>(scan, operation, own, early);
        else
            readLeavesOfScannedOperationTheLongWay(scan, operation, early);
        return true;
    }
    unsigned numberOfChildren = operation->numberOfInlineChildren();
    if (!numberOfChildren)
        return true;
    Operation* const* child = operation->inlineChildren();
    if (!(traits & ScannedOpcodeIsMemoryAccess) || mayBeFusedAsAddress(child[numberOfChildren - 1]->opcode()))
        scan.fuseInto(operation);

    if (own.flags & flagsOfScannedOperationWithLeavesOfItsOwn) [[unlikely]] {
        readLeavesBelowScannedOperation(scan, operation, operation, nullptr, early);
        return true;
    }
    ASSERT(operation->type().isNumeric() || !own.hint);
    unsigned hintOfFirstChild = (traits & ScannedOpcodeHandsHintOnToFirstChild) ? own.hint.index() : noHintOfScannedRead;
    bool readsUpperHalf = opcode != Trunc;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        Operation* below = child[index];
        ValueFacts& factsBelow = facts[below->index()];
        uint16_t flagsBelow = factsBelow.flags;
        if (!(flagsBelow & scanBitOf(ValueFlag::IsFused))) {
            readLeafOfScannedOperation<regionsHaveKinds>(scan, operation, index, below, factsBelow, early, index ? noHintOfScannedRead : hintOfFirstChild, readsUpperHalf);
            continue;
        }
        Opcode opcodeBelow = below->opcode();
        if (opcode == Select || (flagsBelow & flagsOfScannedOperationWithLeavesOfItsOwn) || (traitsOfScannedOpcodes.of[opcodeBelow] & ScannedOpcodeIsReadTheLongWayWhereFused)) [[unlikely]] {
            readLeavesBelowScannedOperation(scan, operation, below, opcode == Select ? operation : nullptr, early);
            continue;
        }
        readLeavesBelowFusedChildOfScannedOperation<regionsHaveKinds>(scan, operation, below, opcodeBelow, hintOfFirstChild, early);
    }
    return true;
}

Liveness* scanBackward(Procedure& proc)
{
    proc.settleUpperHalvesOfPhis();
    Liveness* liveness = proc.arena().create<Liveness>();
    BackwardScan scan(proc, *liveness);
    scan.run();
    scan.letGoOfWorkingLists();
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
    ValueFacts* nextFacts = facts;
    for (ValueFacts* endOfGroupsOfFacts = facts + (mostOperations & ~7u); nextFacts != endOfGroupsOfFacts; nextFacts += 8) {
        new (NotNull, nextFacts) ValueFacts();
        new (NotNull, nextFacts + 1) ValueFacts();
        new (NotNull, nextFacts + 2) ValueFacts();
        new (NotNull, nextFacts + 3) ValueFacts();
        new (NotNull, nextFacts + 4) ValueFacts();
        new (NotNull, nextFacts + 5) ValueFacts();
        new (NotNull, nextFacts + 6) ValueFacts();
        new (NotNull, nextFacts + 7) ValueFacts();
    }
    for (ValueFacts* endOfFacts = facts + mostOperations; nextFacts != endOfFacts; ++nextFacts)
        new (NotNull, nextFacts) ValueFacts();
    m_facts = facts;
    for (const Operation* name : proc.namesOfWords()) {
        if (isFreeZeroExtension(name) || isFreeTruncation(name))
            facts[name->index()].flags = scanBitOf(ValueFlag::IsFused);
    }
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
    for (uint32_t loop = 0; loop < numberOfLoops; ++loop) {
        uint32_t parent = m_order.loops()[loop].parent;
        m_loopStates[loop].loopThatListsReads = parent == noLoop || m_order.hasBackEdgeOutsideRareRegions(m_order.loops()[loop]) ? loop : m_loopStates[parent].loopThatListsReads;
    }
    if (numberOfLoops) {
        m_loopCountedFor.fill(arena, numberOfOperations, noLoop);
        m_loopOfLastListedRead.fill(arena, numberOfOperations, noLoop);
        if (m_hasRareRegions && std::ranges::any_of(m_order.loops(), [](const Loop& loop) { return loop.parent != noLoop; }))
            m_loopOfLastReadExtendedForCalls.fill(arena, numberOfOperations, noLoop);
        m_isPhiReadOnlyBehindLoop = BitSpan::create(arena, numberOfOperations);
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

template<bool regionsHaveKinds>
ALWAYS_INLINE void BackwardScan::scanBlock(Block* block)
{
    ASSERT(block->isTerminated());
    uint32_t blockIndex = block->index();
    const BlockPlace& place = m_places[blockIndex];
    uint32_t loopOfBlock = place.loop;
    uint32_t rareRegion = place.rareRegion;
    Position head = place.head;
    Position end = head + 2 * (block->size() + 1);
    ASSERT(end == m_order.endOf(block));
    noteWaysOnFrom(block, end);
    if (loopOfBlock != noLoop) {
        noteLoopsLeftFromScannedBlock(*this, block, loopOfBlock, end);
        const Loop& loop = m_order.loops().data()[loopOfBlock];
        if (loop.endBlock == place.orderIndex + 1)
            enterLoopsEndingAt(loopOfBlock, loop.end);
    }

    Position early = end - 2;
    const PlaceOfReads& at = m_placeOfReads;
    const Block* origin = rareRegion != noRareRegion ? m_liveness.blockThatRareBlockBranchesOff(block) : nullptr;
    findPlaceOfReadsInScannedBlock<regionsHaveKinds>(*this, m_placeOfReads, loopOfBlock, rareRegion, origin, headOfBlockPassedOnEveryPathToScannedBlock(*this, place, origin));
    for (Operation* upsilon : block->upsilons()) {
        ValueFacts& factsOfUpsilon = m_facts[upsilon->index()];
        if (!upsilonStaysInScan(*this, block, upsilon, early)) {
            factsOfUpsilon.flags |= scanBitOf(ValueFlag::IsDropped);
            continue;
        }
        if (factsOfUpsilon.lastUse == noPosition) [[likely]]
            factsOfUpsilon.definition = early + 1;
        else
            define(upsilon, early + 1);
        Operation* child = upsilon->valueHandedOn();
        const Operation* phi = upsilon->phi();
        readValueInScannedBlock<KindOfScannedRead::ByUpsilon, regionsHaveKinds>(*this, child, m_facts[child->index()], early, m_facts[phi->index()].hint.index(), phi->has(OperationFlag::UpperHalfIsRead));
    }

    bool countsForPressureOfLoop = at.countsForLoops;
    bool standsBehindLastExit = countsForPressureOfLoop && rareRegion == noRareRegion && head >= m_loopStates[loopOfBlock].endOfLastExitingBlock;
    m_mostLiveInBlock = { 0, 0 };
    Operation* const* operations = block->operations().data();
    Position earlyOfLastOperation = early;
    for (unsigned index = block->size(); index--; early -= 2) {
        if (scanOperationOfBlock<regionsHaveKinds>(*this, block, operations[index], early, earlyOfLastOperation) && countsForPressureOfLoop)
            samplePressureOfScan(*this, loopOfBlock, standsBehindLastExit);
    }
    m_liveness.m_mostLiveInBlocks[blockIndex] = m_mostLiveInBlock;

    for (Operation* phi : block->phis()) {
        ValueFacts& factsOfPhi = m_facts[phi->index()];
        Position lastUse = factsOfPhi.lastUse;
        if (lastUse == noPosition)
            factsOfPhi.flags |= scanBitOf(ValueFlag::IsDropped);
        else
            closeScannedRangeOfReadValue<regionsHaveKinds>(*this, phi, factsOfPhi, lastUse, m_summariesOfReads.isEmpty() ? lastUse : m_summariesOfReads[phi->index()].lastReadCountedForCalls, head, rareRegion);
    }
    if (block->isLandingPad()) {
        if (m_kindsOfRegions.isEmpty() || rareRegion == noRareRegion || (m_kindsOfRegions[rareRegion] & regionComesBack))
            m_nextLandingPadThatComesBack = head;
        m_nextLandingPad = head;
        m_nearestEventBehind = std::min(m_nearestEventBehind, head);
        m_liveness.m_hasLandingPads = true;
    }
    if (rareRegion == noRareRegion)
        m_liveness.m_nextCallsBehindHeads[blockIndex] = m_nextCallThatTakesRegisters;
    if (loopOfBlock != noLoop && at.beginOfLoop == head)
        leaveLoopsBeginningAt(loopOfBlock, head);
}

void BackwardScan::run()
{
    std::span<Block* const> blocks = m_order.blocksInOrder();
    Block* const* firstBlock = blocks.data();
    if (m_hasRareRegions && m_proc.traits().setsUpFrameAtEntry()) {
        m_kindsOfRegions.fill(m_proc.arena(), m_order.rareRegions().size(), 0);
        for (const Block* block : blocks) {
            const BlockPlace& place = m_places[block->index()];
            if (!place.isRare())
                continue;
            uint8_t& kinds = m_kindsOfRegions[place.rareRegion];
            if (block->isLandingPad())
                kinds |= regionHoldsLandingPad;
            else {
                for (const Block* predecessor : place.predecessors) {
                    if (m_places[predecessor->index()].rareRegion != place.rareRegion)
                        kinds |= regionComesBack;
                }
            }
            for (const Edge& edge : block->successors()) {
                if (m_places[edge.block()->index()].rareRegion != place.rareRegion && !edge.block()->isLandingPad())
                    kinds |= regionComesBack;
            }
        }
    }
    if (m_kindsOfRegions.isEmpty()) {
        for (size_t index = blocks.size(); index--;)
            scanBlock<false>(firstBlock[index]);
    } else {
        for (size_t index = blocks.size(); index--;)
            scanBlock<true>(firstBlock[index]);
    }

    for (ArenaVector<Operation*>& sunk : m_liveness.m_sunk) {
        std::sort(sunk.begin(), sunk.end(), [](const Operation* left, const Operation* right) {
            return left->index() < right->index();
        });
    }
}

void BackwardScan::letGoOfWorkingLists()
{
    if (static_cast<size_t>(m_proc.numberOfOperations()) * sizeof(SummaryOfReads) < Arena::smallestRoomLetGo)
        return;
    Arena& arena = m_proc.arena();
    m_lowHalfUpdatesInLoops.letGo(arena);
    m_wordsReadWhole.letGo(arena);
    m_loopOfLastReadExtendedForCalls.letGo(arena);
    m_loopOfLastListedRead.letGo(arena);
    m_loopCountedFor.letGo(arena);
    m_loopStates.letGo(arena);
    m_summariesOfReads.letGo(arena);
    m_regions.letGo(arena);
    m_kindsOfRegions.letGo(arena);
}

void BackwardScan::addUse(Operation* value, Position position, Reg hint)
{
    std::span<Block* const> blocks = m_order.blocksInOrder();
    auto behind = std::ranges::upper_bound(blocks, position, { }, [&](const Block* block) {
        return m_order.headOf(block);
    });
    ASSERT(behind != blocks.begin());
    const Block* blockAround = *(behind - 1);
    const BlockPlace& placeAround = m_order.of(blockAround);
    const Block* origin = blockThatScannedBlockBranchesOff(*this, blockAround, placeAround);
    Position passedOnEveryPath = headOfBlockPassedOnEveryPathToScannedBlock(*this, placeAround, origin);
    bool placeCounts = !m_order.loops().empty() && position < m_order.endOfProcedure();
    PlaceOfReads at;
    findPlaceOfReadsInScannedBlock(*this, at, placeCounts ? placeAround.loop : noLoop, placeCounts ? placeAround.rareRegion : noRareRegion, placeCounts ? origin : nullptr, passedOnEveryPath);
    at.flagsOfWhatIsRead = flagsOfWhatIsReadInScannedRegion(*this, placeAround.rareRegion);
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

void BackwardScan::findBlocksThatRareBlocksBranchOff()
{
    ArenaVector<const Block*>& origins = m_liveness.m_blocksThatRareBlocksBranchOff;
    origins.fill(m_proc.arena(), m_order.mostBlocks(), nullptr);
    const BlockPlace* places = m_order.places();
    const Block** originOfBlock = origins.begin();
    std::span<Block* const> layout = m_order.blocksInLayout();
    size_t numberOfRareBlocks = layout.size() - m_order.numberOfBlocksOutsideRareRegions();
    std::span<Block* const> rareBlocksInOrder = m_proc.traits().fallsIntoWhatLiesBehind ? layout.first(numberOfRareBlocks) : layout.last(numberOfRareBlocks);
    for (const Block* block : rareBlocksInOrder) {
        const BlockPlace& place = places[block->index()];
        ASSERT(place.isRare());
        if (block->isLandingPad() || place.predecessors.size() != 1)
            continue;
        const Block* predecessor = place.predecessors.front();
        const BlockPlace& placeOfPredecessor = places[predecessor->index()];
        if (placeOfPredecessor.orderIndex < place.orderIndex)
            originOfBlock[block->index()] = placeOfPredecessor.rareRegion == place.rareRegion ? originOfBlock[predecessor->index()] : predecessor;
    }
}

void BackwardScan::enterLoopsEndingAt(uint32_t innermostLoop, Position end)
{
    std::span<const Loop> loops = m_order.loops();
    for (uint32_t loop = innermostLoop; loop != noLoop && loops[loop].end == end; loop = loops[loop].parent)
        samplePressureOfScan(*this, loop);
}

void BackwardScan::leaveLoopsBeginningAt(uint32_t innermostLoop, Position begin)
{
    std::span<const Loop> loops = m_order.loops();
    for (uint32_t loop = innermostLoop; loop != noLoop && loops[loop].begin == begin; loop = loops[loop].parent) {
        settleLowHalfUpdatesOf(loop);
        const LoopState& state = m_loopStates[loop];
        LoopFacts& result = m_liveness.m_loops[loop];
        for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
            unsigned outerValuesRead = std::min<uint32_t>(state.outerValuesRead[bank], UINT16_MAX);
            unsigned liveThrough = m_numberLive[bank] > outerValuesRead ? m_numberLive[bank] - outerValuesRead : 0;
            result.pressure[bank] = static_cast<uint16_t>(state.mostLive[bank] > liveThrough ? state.mostLive[bank] - liveThrough : 0);
            result.pressureAcrossPatches[bank] = static_cast<uint16_t>(state.mostLiveAcrossPatches[bank] > liveThrough ? state.mostLiveAcrossPatches[bank] - liveThrough : 0);
        }
        result.registersTakenByPatches = state.registersTakenByPatches;
        result.endOfLastExitingBlock = state.endOfLastExitingBlock;
        result.valuesReadFromOutside = state.valuesReadFromOutside.span();

        uint32_t parent = loops[loop].parent;
        if (parent != noLoop && loops[parent].loopAroundOutsideRun == loops[loop].loopAroundOutsideRun) {
            LoopState& nextOfRun = m_loopStates[parent];
            for (unsigned bank = 0; bank < numberOfBanks; ++bank)
                nextOfRun.outerValuesRead[bank] += state.outerValuesRead[bank] - nextOfRun.outerValuesOfRunNotCounted[bank];
        }
        if (parent != noLoop && state.registersTakenByPatches) {
            if (uint32_t next = innermostLoopThatCountsScannedCall(*this, parent, rareRegionOfScannedLoop(m_order, loop)); next != noLoop) {
                m_loopStates[next].registersTakenByPatches |= state.registersTakenByPatches;
                for (unsigned bank = 0; bank < numberOfBanks; ++bank)
                    m_loopStates[next].mostLiveAcrossPatches[bank] = std::max(m_loopStates[next].mostLiveAcrossPatches[bank], state.mostLiveAcrossPatches[bank]);
            }
        }
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
