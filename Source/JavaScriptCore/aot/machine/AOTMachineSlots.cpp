/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineSlots.h"

#if ENABLE(AOT)

#include "AOTSavedFrame.h"
#include "StackAlignment.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static constexpr uint32_t bytesPerPlaceWord = sizeof(uint64_t);
static constexpr uint32_t bytesPerPlaceHalf = sizeof(uint32_t);

struct SpillPlaceRange {
    bool hasGap() const { return beginOfGap != noPosition; }
    // Behind it the place goes back to the free places.
    Position endOfFirstPart() const { return hasGap() ? beginOfGap - 1 : end; }

    Position begin;
    Position end;
    // SpillFacts::beginOfGap and endOfGap.
    Position beginOfGap { noPosition };
    Position endOfGap { noPosition };
};

static constexpr uint32_t roundUpPlaceDistance(uint32_t distance, uint32_t alignment)
{
    return (distance + alignment - 1) & ~(alignment - 1);
}

static constexpr uint32_t wordHoldingPlace(uint32_t distance)
{
    return (distance - 1) / bytesPerPlaceWord;
}

static uint32_t bytesOfStackSlotPlace(const StackSlot* slot)
{
    return std::max(bytesPerPlaceWord, roundUpPlaceDistance(slot->byteSize(), bytesPerPlaceWord));
}

static uint32_t alignmentOfStackSlotPlace(const StackSlot* slot)
{
    return slot->byteSize() >= stackAlignmentBytes() ? stackAlignmentBytes() : bytesPerPlaceWord;
}

static SlotAssigner::Size sizeOfSpillPlace(const SpillFacts& facts)
{
    if (facts.isPointerBesideNarrowReference)
        return SlotAssigner::Word;
    RELEASE_ASSERT(facts.width == Width32 || facts.width == Width64);
    return facts.width == Width32 ? SlotAssigner::Half : SlotAssigner::Word;
}

// From the first write to the place to the last read of it: never less than SpillFacts::begin to end.
// No check sees a write to a place that another spill holds: checkAllocation() goes by spills, and checkFrame() by their ranges.
static SpillPlaceRange rangeOfSpillPlace(const Procedure& proc, const SpillFacts& facts)
{
    SpillPlaceRange range { facts.begin, std::max(facts.begin, facts.end), facts.beginOfGap, facts.endOfGap };

    // The Save stands ahead of the operation that took the register, at whichever of the two positions of that operation the range begins.
    if (facts.isSaveInRareRegion) {
        if (range.begin)
            --range.begin;
        return range;
    }

    const Operation* value = facts.value;
    if (facts.isPointerBesideNarrowReference || !value || value->opcode() != Phi)
        return range;
    if (proc.allocation().registerAtDefinitionOf(value).isSet())
        return range;

    // The moves of the incoming edges write it. They stand at the ends of the predecessors.
    const Order& order = proc.order();
    for (const Block* predecessor : order.of(value->owner()).predecessors) {
        Position endOfPredecessor = order.endOf(predecessor);
        range.begin = std::min(range.begin, endOfPredecessor - 2);
        range.end = std::max(range.end, endOfPredecessor - 1);
    }
    return range;
}

static constexpr uint64_t makePlaceSortKey(Position position, uint32_t index)
{
    return static_cast<uint64_t>(position) << 32 | index;
}

static constexpr Position positionInPlaceSortKey(uint64_t key)
{
    return static_cast<Position>(key >> 32);
}

static constexpr uint32_t indexInPlaceSortKey(uint64_t key)
{
    return static_cast<uint32_t>(key);
}

// By the late position. The index is the one in Allocation::suspensions().
static std::span<const uint64_t> sortSuspensionsForPlaces(Procedure& proc)
{
    std::span<PatchOperation* const> suspensions = proc.allocation().suspensions();
    std::span<uint64_t> keys = proc.arena().createSpan<uint64_t>(suspensions.size());
    for (uint32_t index = 0; index < suspensions.size(); ++index)
        keys[index] = makePlaceSortKey(proc.order().latePositionOf(suspensions[index]), index);
    std::sort(keys.begin(), keys.end());
    return keys;
}

Position positionOfStoreToSpillPlace(const Procedure& proc, const SpillFacts& facts)
{
    const Operation* value = facts.value;
    if (facts.isSaveInRareRegion || !value)
        return noPosition;
    // It is stored at the head of the block that it sank into, where it is defined.
    if (proc.liveness().isSunk(value))
        return facts.begin;
    // The result of a Patch that ends its block is stored at the head of the first successor. The moves of the incoming edges have written a Phi without a register where its block begins.
    const Block* block = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
    return std::max(facts.begin, proc.order().headOf(block));
}

bool spillPlaceIsWrittenBehindDefinitionAlone(const Procedure& proc, const SpillFacts& facts)
{
    const Operation* value = facts.value;
    if (facts.isSaveInRareRegion || !value)
        return false;
    // The moves of the incoming edges write a Phi without a register, ahead of whatever ends the predecessors. The pointer beside it is stored at the head of its block like any other.
    return facts.isPointerBesideNarrowReference || value->opcode() != Phi || proc.allocation().registerAtDefinitionOf(value).isSet();
}

static bool spillPlaceIsFreedLater(const SlotAssigner::Holder& a, const SlotAssigner::Holder& b)
{
    if (a.end != b.end)
        return a.end > b.end;
    return a.distanceWithinPart > b.distanceWithinPart;
}

// A distance is a multiple of four, so the part and the size have room below it. The places that are free again behind one position go back to the free places in the order of these numbers.
// No two holders that go back to one free list have the same distance, so the order of each free list follows from the position and the distance alone.
static constexpr uint32_t makeHeldPlace(uint32_t distanceWithinPart, SlotAssigner::Part part, SlotAssigner::Size size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!(distanceWithinPart % bytesPerPlaceHalf));
    return distanceWithinPart | static_cast<uint32_t>(part) << 1 | static_cast<uint32_t>(size);
}

static constexpr uint32_t distanceOfHeldPlace(uint32_t place)
{
    return place & ~(bytesPerPlaceHalf - 1);
}

static constexpr SlotAssigner::Part partOfHeldPlace(uint32_t place)
{
    return static_cast<SlotAssigner::Part>(place >> 1 & 1);
}

static constexpr SlotAssigner::Size sizeOfHeldPlace(uint32_t place)
{
    return static_cast<SlotAssigner::Size>(place & 1);
}

SlotAssigner::SlotAssigner(Procedure& proc, SlotAssignment& slots)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_allocation(proc.allocation())
    , m_slots(slots)
{
}

void SlotAssigner::run()
{
    std::span<const SpillFacts> spills = m_allocation.spills();
    const uint32_t numberOfSpills = static_cast<uint32_t>(spills.size());
    const bool savesFrame = m_proc.traits().savesFrameOnSuspend;
    RELEASE_ASSERT(savesFrame || m_allocation.suspensions().empty());

    placeStackSlots();
    sortSpillsByBegin();
    // The scan comes to each spill once and writes both entries.
    m_partOfSpill.growWithoutInitializing(m_arena, numberOfSpills);
    m_slots.m_distanceOfSpill.growWithoutInitializing(m_arena, numberOfSpills);

    const SpillFacts* const factsOfSpills = spills.data();
    const SpillPlaceRange* const rangesOfSpillPlaces = m_rangesOfSpillPlaces;
    Part* const partOfSpill = m_partOfSpill.begin();
    uint32_t* const distanceOfSpill = m_slots.m_distanceOfSpill.begin();
    uint32_t* const placesOfSpills = m_placesOfSpills;
    for (SpillIndex spill : m_spillsByBegin) {
        const SpillFacts& facts = factsOfSpills[spill];
        const SpillPlaceRange range = rangesOfSpillPlaces[spill];
        releasePlacesFreeAt(range.begin);

        // A range that reaches over a suspension does not say that anything reads the spill from there on. What lies in the second part is gone behind the next resumption, and nothing holds on to it.
        Part part = savesFrame && facts.livesAcrossSuspension ? KeptAtSuspensions : DeadAtSuspensions;
        Size size = sizeOfSpillPlace(facts);
        uint32_t distance = takePlace(part, size, range.begin, range.endOfFirstPart(), range.hasGap() ? range.endOfGap + 1 : noPosition, range.end);
        partOfSpill[spill] = part;
        distanceOfSpill[spill] = distance;
        placesOfSpills[spill] = makeHeldPlace(distance, part, size);
    }

    joinParts();
    findWordsDeadAcrossSuspensions();
    if (isX86_64())
        bringBusyWordsNearEndsOfFrame();
}

void SlotAssigner::bringBusyWordsNearEndsOfFrame()
{
    // As planFrame() lays the frame out.
    constexpr uint32_t reachOfOneByte = 128;
    const uint32_t numberOfWords = m_slots.m_bytesInAll / bytesPerPlaceWord;
    const uint32_t bytesOfSaves = roundUpPlaceDistance(static_cast<uint32_t>(std::popcount(m_allocation.usedCalleeSaves())) * bytesPerPlaceWord, stackAlignmentBytes());
    const uint32_t bytesBelowSaves = roundUpPlaceDistance(bytesOfSaves + m_slots.m_bytesInAll + m_proc.callArgAreaSizeInBytes(), stackAlignmentBytes()) - bytesOfSaves;
    FrameWords words;
    words.endOfReachOfFramePointer = (reachOfOneByte - std::min(reachOfOneByte, bytesOfSaves)) / bytesPerPlaceWord;
    words.beginOfReachOfStackPointer = (bytesBelowSaves - std::min(reachOfOneByte, bytesBelowSaves)) / bytesPerPlaceWord;
    if (numberOfWords <= words.endOfReachOfFramePointer)
        return;

    words.numberOfAccesses = m_arena.createSpan<uint32_t>(numberOfWords);
    words.numberOfAccessesToStackSlot = m_arena.createSpan<uint32_t>(m_proc.stackSlots().size());
    words.newWordOf = m_arena.createSpan<uint32_t>(numberOfWords);
    for (uint32_t word = 0; word < numberOfWords; ++word)
        words.newWordOf[word] = word;
    countAccessesToFrame(words);

    const uint32_t numberOfWordsKept = m_slots.numberOfWordsKeptAtSuspensions();
    uint32_t endOfWordsKeptThatMayMove = 0;
    if (m_slots.m_firstWordClearedAtSetup >= m_slots.m_endOfWordsClearedAtSetup) {
        // The word below a first part with an odd number of words is the lowest of them.
        uint32_t numberOfLowestWordsCleared = numberOfWordsKept % 2;
        for (const WordsClearedAtSuspension& cleared : m_slots.m_wordsClearedAtSuspension)
            numberOfLowestWordsCleared = std::max(numberOfLowestWordsCleared, cleared.numberOfLowestWords);
        endOfWordsKeptThatMayMove = numberOfWordsKept + numberOfWordsKept % 2 - numberOfLowestWordsCleared;
        arrangeWordsOfPart(words, KeptAtSuspensions, 0, endOfWordsKeptThatMayMove);
    }
    arrangeWordsOfPart(words, DeadAtSuspensions, numberOfWords - m_bytesOfPart[DeadAtSuspensions] / bytesPerPlaceWord, numberOfWords);

    bool movesWordsKept = false;
    for (uint32_t word = 0; word < numberOfWordsKept; ++word) {
        RELEASE_ASSERT(word < endOfWordsKeptThatMayMove ? words.newWordOf[word] < endOfWordsKeptThatMayMove : words.newWordOf[word] == word);
        movesWordsKept |= words.newWordOf[word] != word;
    }

    for (uint32_t& distance : m_slots.m_distanceOfSpill) {
        uint32_t word = wordHoldingPlace(distance);
        distance = distance - word * bytesPerPlaceWord + words.newWordOf[word] * bytesPerPlaceWord;
    }
    for (const StackSlot* slot : m_proc.stackSlots()) {
        uint32_t& distance = m_slots.m_distanceOfStackSlot[slot->index()];
        uint32_t firstWord = (distance - bytesOfStackSlotPlace(slot)) / bytesPerPlaceWord;
        distance = distance - firstWord * bytesPerPlaceWord + words.newWordOf[firstWord] * bytesPerPlaceWord;
        RELEASE_ASSERT(!(distance % alignmentOfStackSlotPlace(slot)));
    }

    if (!movesWordsKept)
        return;
    auto moveBitsOfWordsKept = [&](BitSpan& bits) {
        BitSpan moved = BitSpan::create(m_arena, numberOfWordsKept);
        bits.forEachSetBit([&](size_t word) {
            RELEASE_ASSERT(word < numberOfWordsKept);
            moved.set(words.newWordOf[word]);
        });
        bits = moved;
    };
    for (BitSpan& deadWords : m_slots.m_wordsDeadAcrossSuspension)
        moveBitsOfWordsKept(deadWords);
    for (WordsClearedAtSuspension& cleared : m_slots.m_wordsClearedAtSuspension) {
        if (cleared.others.size())
            moveBitsOfWordsKept(cleared.others);
    }
}

// The destination of a step and its sources lie side by side: three words of 64 bits with two locations each.
static_assert(sizeof(Location) == sizeof(uint32_t));
static_assert(offsetof(Step, sources) == offsetof(Step, destination) + sizeof(Location));
static_assert((1 + maxLeaves) * sizeof(Location) == 3 * sizeof(uint64_t));
// A location holds its kind above its payload. Every spill and all bytes of a StackSlot have this bit of the kind, and no register has it.
// An immediate has it too, with the bit above it, which those two lack.
static constexpr uint64_t bitOfLocationInFrame = std::bit_cast<uint32_t>(Location::spill(0));
static_assert(std::has_single_bit(bitOfLocationInFrame));
static_assert(std::bit_cast<uint32_t>(Location::slotBytes(0)) & bitOfLocationInFrame);
static_assert(!((std::bit_cast<uint32_t>(Location::spill(0)) | std::bit_cast<uint32_t>(Location::slotBytes(0))) & bitOfLocationInFrame << 1));
static constexpr uint64_t bitsOfPairWithLocationInFrame = bitOfLocationInFrame << 32 | bitOfLocationInFrame;

// Not zero where one of the two locations may be a spill or the bytes of a StackSlot.
static constexpr uint64_t pairMayHoldLocationInFrame(uint64_t pair)
{
    return pair & ~(pair >> 1) & bitsOfPairWithLocationInFrame;
}

void SlotAssigner::countAccessesToFrame(FrameWords& words)
{
    std::span<const SpillFacts> spills = m_allocation.spills();
    const uint32_t* const distanceOfSpill = m_slots.m_distanceOfSpill.begin();
    uint32_t* const numberOfAccesses = words.numberOfAccesses.data();
    uint32_t* const numberOfAccessesToStackSlot = words.numberOfAccessesToStackSlot.data();
    auto wordOf = [&](SpillIndex spill) {
        return wordHoldingPlace(distanceOfSpill[spill]);
    };

    auto note = [&](Location location) {
        if (location.isSpill())
            ++numberOfAccesses[wordOf(location.spillIndex())];
        else if (location.kind() == Location::SlotBytes)
            ++numberOfAccessesToStackSlot[location.indexOfStackSlot()];
    };

    // The store behind a definition has no step.
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        const SpillFacts& facts = spills[spill];
        if (facts.value && !facts.isSaveInRareRegion && !facts.isPointerBesideNarrowReference && m_allocation.isStoredBehindDefinition(facts.value))
            note(Location::spill(spill));
    }

    // Most steps name registers alone.
    auto noteLocationsOf = [&](const Step& step) ALWAYS_INLINE_LAMBDA {
        uint64_t pairs[3];
        memcpy(pairs, reinterpret_cast<const char*>(&step) + offsetof(Step, destination), sizeof(pairs));
        if (!((pairs[0] | pairs[1] | pairs[2]) & bitsOfPairWithLocationInFrame))
            return;
        const uint64_t firstPairMayCount = pairMayHoldLocationInFrame(pairs[0]);
        const uint64_t secondPairMayCount = pairMayHoldLocationInFrame(pairs[1]);
        const uint64_t thirdPairMayCount = pairMayHoldLocationInFrame(pairs[2]);
        if (!(firstPairMayCount | secondPairMayCount | thirdPairMayCount))
            return;
        if (step.kind == StepKind::Execute && step.operation->opcode() == Extract)
            return;
        if (firstPairMayCount) {
            note(step.destination);
            note(step.sources[0]);
        }
        if (secondPairMayCount) {
            note(step.sources[1]);
            note(step.sources[2]);
        }
        if (thirdPairMayCount) {
            note(step.sources[3]);
            note(step.sources[4]);
        }
    };

    std::span<const Step> steps = m_allocation.steps();
    if (m_proc.stackSlots().empty()) {
        // No operation is a SlotBase.
        for (const Step& step : steps)
            noteLocationsOf(step);
    } else {
        for (const Step& step : steps) {
            // A Materialize goes into a register, and its source is no place.
            if (step.kind != StepKind::Materialize) {
                noteLocationsOf(step);
                continue;
            }
            const Operation* operation = step.operation;
            while (operation->opcode() == Add && operation->hasNoPosition())
                operation = operation->child(0);
            if (operation->opcode() == SlotBase)
                ++numberOfAccessesToStackSlot[operation->slot()->index()];
        }
    }

    // No code of this function reads an operand of a Patch where it lies in the frame: a map lists it, or a thunk that many functions share loads it.
    // Allocation::suspensions() lists each suspension that has a step. It has one, among those of its block.
    ArenaVector<uint32_t> wordsEncodedAsIndex;
    for (const PatchOperation* patch : m_allocation.suspensions()) {
        if (!patch->operandEncodedAsFrameWordIndex())
            continue;
        const Step* step = m_allocation.findExecuteOf(patch);
        if (!step)
            step = &m_allocation.step(m_allocation.stepsOf(patch->owner()).terminator);
        RELEASE_ASSERT(step->kind == StepKind::Execute && step->operation == patch);
        Location location = m_allocation.patchAssignment(step->patchAssignment).locations[m_proc.resultCount(patch->type()) + *patch->operandEncodedAsFrameWordIndex()];
        if (location.isSpill())
            wordsEncodedAsIndex.append(m_arena, wordOf(location.spillIndex()));
    }

    for (const WordsClearedAtSuspension& cleared : m_slots.m_wordsClearedAtSuspension) {
        cleared.others.forEachSetBit([&](size_t word) {
            ++numberOfAccesses[word];
        });
    }
    for (uint32_t word : wordsEncodedAsIndex)
        numberOfAccesses[word] = UINT32_MAX;
}

void SlotAssigner::arrangeWordsOfPart(FrameWords& words, Part part, uint32_t begin, uint32_t end)
{
    RELEASE_ASSERT(begin <= end && !(begin % 2));
    // From begin on: the words near the frame pointer, those far from both ends, those near the stack pointer.
    const uint32_t endOfHighWords = std::clamp(words.endOfReachOfFramePointer, begin, end);
    const uint32_t beginOfLowWords = std::clamp(words.beginOfReachOfStackPointer, endOfHighWords, end);
    if ((begin < endOfHighWords) + (endOfHighWords < beginOfLowWords) + (beginOfLowWords < end) < 2)
        return;

    struct StackSlotInPart {
        uint32_t numberOfWords;
        uint32_t numberOfAccesses;
        uint32_t firstWord;
        uint32_t newFirstWord;
        bool endsAtEvenWord;
        bool liesNearEnd;
    };
    auto firstWordAtOrBehind = [](uint32_t word, const StackSlotInPart& slot) {
        return word + (slot.endsAtEvenWord && (word + slot.numberOfWords) % 2);
    };

    ArenaVector<StackSlotInPart> slots;
    BitSpan holdsStackSlot = BitSpan::create(m_arena, end);
    for (const StackSlot* slot : m_proc.stackSlots()) {
        if (m_partOfStackSlot[slot->index()] != part)
            continue;
        uint32_t numberOfWords = bytesOfStackSlotPlace(slot) / bytesPerPlaceWord;
        uint32_t firstWord = m_slots.m_distanceOfStackSlot[slot->index()] / bytesPerPlaceWord - numberOfWords;
        RELEASE_ASSERT(firstWord >= begin && firstWord + numberOfWords <= end);
        for (uint32_t word = firstWord; word < firstWord + numberOfWords; ++word)
            holdsStackSlot.set(word);
        slots.append(m_arena, StackSlotInPart { numberOfWords, words.numberOfAccessesToStackSlot[slot->index()], firstWord, firstWord, alignmentOfStackSlotPlace(slot) > bytesPerPlaceWord, false });
    }

    // Among equals the word that lies higher comes first, so equal procedures get equal frames.
    // A key holds how far the word falls short of the most accesses that can be counted, above the word.
    uint64_t* const keysOfWords = static_cast<uint64_t*>(m_arena.allocate(static_cast<size_t>(end - begin) * sizeof(uint64_t), alignof(uint64_t)));
    uint32_t numberOfWordsWithoutStackSlot = 0;
    for (uint32_t word = begin; word < end; ++word) {
        if (!holdsStackSlot.get(word))
            keysOfWords[numberOfWordsWithoutStackSlot++] = static_cast<uint64_t>(UINT32_MAX - words.numberOfAccesses[word]) << 32 | word;
    }
    std::sort(keysOfWords, keysOfWords + numberOfWordsWithoutStackSlot);
    ArenaVector<uint32_t> busiestFirst;
    busiestFirst.reserve(m_arena, numberOfWordsWithoutStackSlot);
    for (uint32_t index = 0; index < numberOfWordsWithoutStackSlot; ++index)
        busiestFirst.append(m_arena, static_cast<uint32_t>(keysOfWords[index]));
    uint32_t numberOfWordsEncodedAsIndex = 0;
    while (numberOfWordsEncodedAsIndex < busiestFirst.size() && words.numberOfAccesses[busiestFirst[numberOfWordsEncodedAsIndex]] == UINT32_MAX)
        ++numberOfWordsEncodedAsIndex;

    auto chooseWordsOfStackSlots = [&] {
        ArenaVector<uint32_t> densestFirst;
        for (uint32_t index = 0; index < slots.size(); ++index)
            densestFirst.append(m_arena, index);
        std::sort(densestFirst.begin(), densestFirst.end(), [&](uint32_t a, uint32_t b) {
            uint64_t left = static_cast<uint64_t>(slots[a].numberOfAccesses) * slots[b].numberOfWords;
            uint64_t right = static_cast<uint64_t>(slots[b].numberOfAccesses) * slots[a].numberOfWords;
            return left != right ? left > right : a < b;
        });

        // However the slots without a place come to lie: they begin at an even word, and each may leave a word free in front of it.
        uint32_t mostWordsOfSlotsWithoutPlace = 1;
        for (const StackSlotInPart& slot : slots)
            mostWordsOfSlotsWithoutPlace += slot.numberOfWords + slot.endsAtEvenWord;

        // Words and slots take their turns, whichever has the most accesses to a word first: down from the top as far as the frame pointer reaches, then up from the bottom as far as the stack pointer reaches.
        uint32_t nextWord = numberOfWordsEncodedAsIndex;
        uint32_t top = begin + numberOfWordsEncodedAsIndex;
        uint32_t bottom = end;
        uint32_t endOfSlotsAtTop = top;
        uint32_t beginOfSlotsAtBottom = bottom;
        for (uint32_t index : densestFirst) {
            StackSlotInPart& slot = slots[index];
            for (; nextWord < busiestFirst.size() && static_cast<uint64_t>(words.numberOfAccesses[busiestFirst[nextWord]]) * slot.numberOfWords > slot.numberOfAccesses; ++nextWord) {
                if (top < endOfHighWords)
                    ++top;
                else if (bottom > std::max(beginOfLowWords, top))
                    --bottom;
                else
                    break;
            }

            const uint32_t mostWordsOfOthers = mostWordsOfSlotsWithoutPlace - slot.numberOfWords - slot.endsAtEvenWord;
            const uint32_t firstWordAtTop = firstWordAtOrBehind(top, slot);
            const uint32_t endAtBottom = bottom - (slot.endsAtEvenWord && bottom % 2);
            if (firstWordAtTop < endOfHighWords && firstWordAtTop + slot.numberOfWords + mostWordsOfOthers <= beginOfSlotsAtBottom) {
                slot.newFirstWord = firstWordAtTop;
                top = endOfSlotsAtTop = firstWordAtTop + slot.numberOfWords;
            } else if (endAtBottom > beginOfLowWords && endOfSlotsAtTop + mostWordsOfOthers + slot.numberOfWords <= endAtBottom) {
                slot.newFirstWord = endAtBottom - slot.numberOfWords;
                bottom = beginOfSlotsAtBottom = slot.newFirstWord;
            } else
                continue;
            slot.liesNearEnd = true;
            mostWordsOfSlotsWithoutPlace = mostWordsOfOthers;
        }

        // The others follow each other as placeStackSlots() has laid them out, behind the reach of the frame pointer where there is room.
        uint32_t numberOfWordsBetween = 0;
        for (StackSlotInPart& slot : slots) {
            if (slot.liesNearEnd)
                continue;
            slot.newFirstWord = firstWordAtOrBehind(numberOfWordsBetween, slot);
            numberOfWordsBetween = slot.newFirstWord + slot.numberOfWords;
        }
        if (!numberOfWordsBetween)
            return true;
        uint32_t firstWordBetween = roundUpPlaceDistance(std::max(endOfSlotsAtTop, endOfHighWords), 2);
        if (firstWordBetween + numberOfWordsBetween > beginOfSlotsAtBottom) {
            if (endOfSlotsAtTop + numberOfWordsBetween > beginOfSlotsAtBottom)
                return false;
            firstWordBetween = (beginOfSlotsAtBottom - numberOfWordsBetween) & ~1u;
            if (firstWordBetween < endOfSlotsAtTop)
                return false;
        }
        for (StackSlotInPart& slot : slots) {
            if (!slot.liesNearEnd)
                slot.newFirstWord += firstWordBetween;
        }
        return true;
    };
    if (!slots.isEmpty() && !chooseWordsOfStackSlots()) {
        for (StackSlotInPart& slot : slots)
            slot.newFirstWord = slot.firstWord;
    }

    BitSpan willHoldStackSlot = BitSpan::create(m_arena, end);
    for (const StackSlotInPart& slot : slots) {
        RELEASE_ASSERT(slot.newFirstWord >= begin && slot.newFirstWord + slot.numberOfWords <= end);
        for (uint32_t offset = 0; offset < slot.numberOfWords; ++offset) {
            RELEASE_ASSERT(!willHoldStackSlot.get(slot.newFirstWord + offset));
            willHoldStackSlot.set(slot.newFirstWord + offset);
            words.newWordOf[slot.firstWord + offset] = slot.newFirstWord + offset;
        }
    }

    // Among words that are equally near the order of the scan stays: the map of a suspension is as long as the last word that it lists is far down.
    uint32_t numberOfWordsGivenOut = 0;
    uint32_t firstOfEquallyNear = numberOfWordsEncodedAsIndex;
    auto giveOutWords = [&](uint32_t first, uint32_t last) {
        uint32_t firstGivenOut = numberOfWordsGivenOut;
        uint32_t numberFree = 0;
        for (uint32_t word = first; word < last; ++word)
            numberFree += !willHoldStackSlot.get(word);
        RELEASE_ASSERT(firstGivenOut + numberFree <= busiestFirst.size());
        uint32_t endOfEquallyNear = std::max(firstOfEquallyNear, firstGivenOut + numberFree);
        std::sort(busiestFirst.begin() + firstOfEquallyNear, busiestFirst.begin() + endOfEquallyNear);
        firstOfEquallyNear = endOfEquallyNear;
        for (uint32_t word = first; word < last; ++word) {
            if (!willHoldStackSlot.get(word))
                words.newWordOf[busiestFirst[numberOfWordsGivenOut++]] = word;
        }
    };
    giveOutWords(begin, endOfHighWords);
    giveOutWords(beginOfLowWords, end);
    giveOutWords(endOfHighWords, beginOfLowWords);
    RELEASE_ASSERT(numberOfWordsGivenOut == busiestFirst.size());
}

void SlotAssigner::placeStackSlots()
{
    std::span<StackSlot* const> stackSlots = m_proc.stackSlots();
    const uint32_t numberOfStackSlots = static_cast<uint32_t>(stackSlots.size());
    const bool savesFrame = m_proc.traits().savesFrameOnSuspend;
    m_partOfStackSlot.fill(m_arena, numberOfStackSlots, DeadAtSuspensions);
    m_slots.m_distanceOfStackSlot.fill(m_arena, numberOfStackSlots, 0);

    for (const StackSlot* slot : stackSlots) {
        Part part = savesFrame && !slot->isDeadAtSuspensions() ? KeptAtSuspensions : DeadAtSuspensions;
        uint32_t bytes = bytesOfStackSlotPlace(slot);
        uint32_t distance = roundUpPlaceDistance(m_bytesOfPart[part] + bytes, alignmentOfStackSlotPlace(slot));
        for (uint32_t skipped = m_bytesOfPart[part] + bytesPerPlaceWord; skipped + bytes <= distance; skipped += bytesPerPlaceWord)
            m_freePlaces[part][Word].append(m_arena, skipped);
        m_bytesOfPart[part] = distance;
        m_partOfStackSlot[slot->index()] = part;
        m_slots.m_distanceOfStackSlot[slot->index()] = distance;
    }
}

void SlotAssigner::sortSpillsByBegin()
{
    std::span<const SpillFacts> spills = m_allocation.spills();
    const uint32_t numberOfSpills = static_cast<uint32_t>(spills.size());
    if (!numberOfSpills)
        return;

    const SpillFacts* const factsOfSpills = spills.data();
    uint64_t* const keys = m_arena.allocateArray<uint64_t>(numberOfSpills);
    uint64_t* const keysByEndOfHold = m_arena.allocateArray<uint64_t>(numberOfSpills);
    SpillPlaceRange* const ranges = m_arena.allocateArray<SpillPlaceRange>(numberOfSpills);
    bool keysRise = true;
    bool keysByEndOfHoldRise = true;
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const SpillPlaceRange* range = new (NotNull, ranges + spill) SpillPlaceRange(rangeOfSpillPlace(m_proc, factsOfSpills[spill]));
        ASSERT(range->begin <= range->endOfFirstPart());
        keys[spill] = makePlaceSortKey(range->begin, spill);
        keysRise &= !spill || keys[spill - 1] < keys[spill];
        keysByEndOfHold[spill] = makePlaceSortKey(range->endOfFirstPart(), spill);
        keysByEndOfHoldRise &= !spill || keysByEndOfHold[spill - 1] < keysByEndOfHold[spill];
    }
    m_rangesOfSpillPlaces = ranges;
    if (!keysRise)
        std::sort(keys, keys + numberOfSpills);
    if (!keysByEndOfHoldRise)
        std::sort(keysByEndOfHold, keysByEndOfHold + numberOfSpills);
    m_spillsByEndOfHold = keysByEndOfHold;
    m_placesOfSpills = m_arena.allocateArray<uint32_t>(numberOfSpills);

    m_spillsByBegin.reserve(m_arena, numberOfSpills);
    for (uint32_t index = 0; index < numberOfSpills; ++index)
        m_spillsByBegin.appendWithinCapacity(indexInPlaceSortKey(keys[index]));
}

ALWAYS_INLINE void SlotAssigner::releasePlacesFreeAt(Position position)
{
    if (m_nextSpillToLetGo < m_spillsByBegin.size() && positionInPlaceSortKey(m_spillsByEndOfHold[m_nextSpillToLetGo]) < position)
        releasePlacesOfNextSpillsFreeAt(position);
}

// The next spill in m_spillsByEndOfHold is one of them.
NEVER_INLINE void SlotAssigner::releasePlacesOfNextSpillsFreeAt(Position position)
{
    uint64_t* const keys = m_spillsByEndOfHold;
    const uint32_t* const placesOfSpills = m_placesOfSpills;
    const uint32_t numberOfSpills = m_spillsByBegin.size();
    auto release = [&](uint32_t place) {
        m_freePlaces[partOfHeldPlace(place)][sizeOfHeldPlace(place)].append(m_arena, distanceOfHeldPlace(place));
    };

    uint32_t next = m_nextSpillToLetGo;
    do {
        const Position endOfHold = positionInPlaceSortKey(keys[next]);
        uint32_t endOfEquals = next + 1;
        while (endOfEquals < numberOfSpills && positionInPlaceSortKey(keys[endOfEquals]) == endOfHold)
            ++endOfEquals;
        if (endOfEquals == next + 1)
            release(placesOfSpills[indexInPlaceSortKey(keys[next])]);
        else {
            // Nobody reads these keys again: each makes room for the place of its spill.
            for (uint32_t index = next; index < endOfEquals; ++index)
                keys[index] = placesOfSpills[indexInPlaceSortKey(keys[index])];
            std::sort(keys + next, keys + endOfEquals);
            for (uint32_t index = next; index < endOfEquals; ++index)
                release(static_cast<uint32_t>(keys[index]));
        }
        next = endOfEquals;
    } while (next < numberOfSpills && positionInPlaceSortKey(keys[next]) < position);
    m_nextSpillToLetGo = next;
}

// `beginBehindGap`: noPosition for a spill without a gap.
ALWAYS_INLINE uint32_t SlotAssigner::takePlace(Part part, Size size, Position begin, Position endOfFirstPart, Position beginBehindGap, Position end)
{
    ArenaVector<PositionsTakenBehindGaps>& taken = m_positionsTakenBehindGaps[part];
    auto isFreeFor = [&](uint32_t distance) {
        uint32_t place = distance / bytesPerPlaceHalf;
        if (place >= taken.size() || taken[place].end < begin)
            return true;
        auto reaches = [&](Position first, Position last) {
            return first <= taken[place].end && taken[place].begin <= last;
        };
        return !reaches(begin, endOfFirstPart) && (beginBehindGap == noPosition || !reaches(beginBehindGap, end));
    };
    auto take = [&](uint32_t distance) {
        if (beginBehindGap == noPosition)
            return distance;
        uint32_t place = distance / bytesPerPlaceHalf;
        if (place >= taken.size())
            taken.grow(m_arena, place + 1);
        PositionsTakenBehindGaps& positions = taken[place];
        if (positions.end < begin)
            positions = { beginBehindGap, end };
        else
            positions = { std::min(positions.begin, beginBehindGap), std::max(positions.end, end) };
        return distance;
    };

    ArenaVector<uint32_t>& freePlaces = m_freePlaces[part][size];
    for (uint32_t index = freePlaces.size(); index--;) {
        uint32_t distance = freePlaces[index];
        if (!isFreeFor(distance))
            continue;
        for (uint32_t next = index + 1; next < freePlaces.size(); ++next)
            freePlaces[next - 1] = freePlaces[next];
        freePlaces.takeLast();
        return take(distance);
    }

    m_bytesOfPart[part] += bytesPerPlaceWord;
    uint32_t distance = m_bytesOfPart[part];
    switch (size) {
    case Half:
        freePlaces.append(m_arena, distance);
        return take(distance - bytesPerPlaceHalf);
    case Word:
        return take(distance);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotAssigner::joinParts()
{
    std::span<StackSlot* const> stackSlots = m_proc.stackSlots();
    const uint32_t bytesKept = m_bytesOfPart[KeptAtSuspensions];
    // A suspension keeps whole pairs of words. Whatever the function left in the other word of the last pair would be kept with it, and every resumption would put it back on the stack.
    // So nothing lies there: no spill, no StackSlot, no outgoing word.
    uint32_t topOfSecondPart = roundUpPlaceDistance(bytesKept, 2 * bytesPerPlaceWord);
    for (const StackSlot* slot : stackSlots) {
        if (m_partOfStackSlot[slot->index()] == DeadAtSuspensions)
            topOfSecondPart = roundUpPlaceDistance(topOfSecondPart, alignmentOfStackSlotPlace(slot));
    }

    if (topOfSecondPart) {
        for (const StackSlot* slot : stackSlots) {
            if (m_partOfStackSlot[slot->index()] == DeadAtSuspensions)
                m_slots.m_distanceOfStackSlot[slot->index()] += topOfSecondPart;
        }
        for (SpillIndex spill = 0; spill < m_partOfSpill.size(); ++spill) {
            if (m_partOfSpill[spill] == DeadAtSuspensions)
                m_slots.m_distanceOfSpill[spill] += topOfSecondPart;
        }
    }

    m_slots.m_bytesKeptAtSuspensions = bytesKept;
    m_slots.m_bytesInAll = topOfSecondPart + m_bytesOfPart[DeadAtSuspensions];
}

void SlotAssigner::findWordsDeadAcrossSuspensions()
{
    if (m_allocation.suspensions().empty())
        return;

    std::span<const uint64_t> suspensions = sortSuspensionsForPlaces(m_proc);
    m_slots.m_wordsDeadAcrossSuspension.grow(m_arena, static_cast<uint32_t>(suspensions.size()));
    m_slots.m_wordsClearedAtSuspension.grow(m_arena, static_cast<uint32_t>(suspensions.size()));

    const uint32_t numberOfWords = m_slots.numberOfWordsKeptAtSuspensions();
    std::span<uint32_t> numberOfHolders = m_arena.createSpan<uint32_t>(numberOfWords);
    std::span<uint64_t> bitsOfDeadWords = m_arena.createSpan<uint64_t>((numberOfWords + 63) / 64);
    BitSpan deadWords(bitsOfDeadWords);
    for (uint32_t word = 0; word < numberOfWords; ++word)
        deadWords.set(word);

    auto hold = [&](uint32_t word) {
        if (!numberOfHolders[word]++)
            deadWords.clear(word);
    };
    auto letGo = [&](uint32_t word) {
        if (!--numberOfHolders[word])
            deadWords.set(word);
    };

    for (const StackSlot* slot : m_proc.stackSlots()) {
        if (m_partOfStackSlot[slot->index()] != KeptAtSuspensions)
            continue;
        uint32_t distance = m_slots.m_distanceOfStackSlot[slot->index()];
        for (uint32_t word = (distance - bytesOfStackSlotPlace(slot)) / bytesPerPlaceWord; word < distance / bytesPerPlaceWord; ++word)
            hold(word);
    }

    std::span<const SpillFacts> spills = m_allocation.spills();
    m_holders.clear();
    uint32_t nextSpill = 0;
    auto holdSpillsThatBeginInFrontOf = [&](Position position, Position early) {
        for (; nextSpill < m_spillsByBegin.size(); ++nextSpill) {
            SpillIndex spill = m_spillsByBegin[nextSpill];
            if (m_partOfSpill[spill] != KeptAtSuspensions)
                continue;
            const SpillPlaceRange range = m_rangesOfSpillPlaces[spill];
            if (range.begin >= position)
                break;
            if (range.end < early)
                continue;
            uint32_t distance = m_slots.m_distanceOfSpill[spill];
            hold(wordHoldingPlace(distance));
            m_holders.append(m_arena, Holder { range.end, distance, KeptAtSuspensions, sizeOfSpillPlace(spills[spill]), spillPlaceIsWrittenBehindDefinitionAlone(m_proc, spills[spill]), spill });
            std::push_heap(m_holders.begin(), m_holders.end(), spillPlaceIsFreedLater);
        }
    };

    // The half words that every path to the place of the scan has stored to since the frame was set up: two bits to a word of the first part, the upper half first, and two for the word above the first part.
    const uint32_t wordAboveFirstPart = numberOfWords;
    std::span<uint64_t> bitsOfStoredHalfWords = m_arena.createSpan<uint64_t>((2 * (numberOfWords + 1) + 63) / 64);
    BitSpan storedHalfWords(bitsOfStoredHalfWords);
    auto isStoredTo = [&](uint32_t word) {
        return storedHalfWords.get(2 * word) && storedHalfWords.get(2 * word + 1);
    };
    auto noteStoreTo = [&](uint32_t word) {
        storedHalfWords.set(2 * word);
        storedHalfWords.set(2 * word + 1);
    };
    auto forEachHalfWordOfPlace = [](uint32_t distance, Size size, const auto& functor) {
        uint32_t lastHalfWord = distance / bytesPerPlaceHalf - 1;
        if (size == Word)
            functor(lastHalfWord - 1);
        functor(lastHalfWord);
    };

    uint32_t firstWordClearedAtSetup = numberOfWords;
    uint32_t endOfWordsClearedAtSetup = 0;
    // A half word that does not pass for stored has a path to the place of the scan without a store to it. While this holds, that path begins at an entrypoint.
    bool everyPathBeginsAtEntrypoint = true;
    ArenaVector<uint32_t> wordsHeldByRangeAlone;
    ArenaVector<uint32_t> wordsNotHeldHere;
    auto visitSuspension = [&](uint64_t key) {
        const Position late = positionInPlaceSortKey(key);
        const Position early = late - 1;
        const BitSpan& spillsHeldHere = m_allocation.spillsHeldAt(indexInPlaceSortKey(key));

        holdSpillsThatBeginInFrontOf(late, early);
        while (!m_holders.isEmpty() && m_holders[0].end < early) {
            std::pop_heap(m_holders.begin(), m_holders.end(), spillPlaceIsFreedLater);
            letGo(wordHoldingPlace(m_holders.takeLast().distanceWithinPart));
        }

        for (const Holder& holder : m_holders) {
            // The place stays its own, and from here no path leads to a read of it.
            if (!spillsHeldHere.get(holder.spill)) {
                letGo(wordHoldingPlace(holder.distanceWithinPart));
                wordsNotHeldHere.append(m_arena, wordHoldingPlace(holder.distanceWithinPart));
                continue;
            }
            bool isStoredOnEveryPath = true;
            forEachHalfWordOfPlace(holder.distanceWithinPart, holder.size, [&](uint32_t halfWord) {
                isStoredOnEveryPath &= storedHalfWords.get(halfWord);
            });
            if (isStoredOnEveryPath)
                continue;
            uint32_t word = wordHoldingPlace(holder.distanceWithinPart);
            if (holder.isWrittenBehindDefinitionAlone && everyPathBeginsAtEntrypoint) {
                letGo(word);
                wordsHeldByRangeAlone.append(m_arena, word);
                continue;
            }
            firstWordClearedAtSetup = std::min(firstWordClearedAtSetup, word);
            endOfWordsClearedAtSetup = std::max(endOfWordsClearedAtSetup, word + 1);
        }

        // Nothing ever stores to the word below a first part with an odd number of words: joinParts().
        WordsClearedAtSuspension& cleared = m_slots.m_wordsClearedAtSuspension[indexInPlaceSortKey(key)];
        uint32_t numberOfLowestWordsHeldByNothing = numberOfWords % 2;
        cleared.numberOfLowestWords = numberOfLowestWordsHeldByNothing;
        uint32_t word = numberOfWords;
        for (; word && numberOfLowestWordsHeldByNothing < SavedFrame::maxClearedWords && deadWords.get(word - 1); --word) {
            ++numberOfLowestWordsHeldByNothing;
            if (!isStoredTo(word - 1))
                cleared.numberOfLowestWords = numberOfLowestWordsHeldByNothing;
        }

        // The suspension may go on without suspending the function, and then nobody has cleared the lowest words. The stores in front of it run either way.
        // What is done for one of the words above changes nothing for another, so they are taken as the bits of the dead words list them.
        const uint32_t endOfOtherWords = word;
        for (uint32_t firstWordOfBits = 0; firstWordOfBits < endOfOtherWords; firstWordOfBits += 64) {
            uint64_t otherDeadWords = bitsOfDeadWords[firstWordOfBits / 64];
            if (endOfOtherWords - firstWordOfBits < 64)
                otherDeadWords &= (uint64_t { 1 } << (endOfOtherWords - firstWordOfBits)) - 1;
            for (; otherDeadWords; otherDeadWords &= otherDeadWords - 1) {
                uint32_t otherWord = firstWordOfBits + static_cast<uint32_t>(std::countr_zero(otherDeadWords));
                if (isStoredTo(otherWord))
                    continue;
                if (!cleared.others.size())
                    cleared.others = BitSpan::create(m_arena, numberOfWords);
                cleared.others.set(otherWord);
                noteStoreTo(otherWord);
            }
        }
        cleared.wordAboveFirstPart = !isStoredTo(wordAboveFirstPart);
        noteStoreTo(wordAboveFirstPart);

        // The list of dead words goes by what the suspension holds, as checkFrame() does.
        for (uint32_t wordHeldAgain : wordsHeldByRangeAlone)
            hold(wordHeldAgain);
        wordsHeldByRangeAlone.clear();

        holdSpillsThatBeginInFrontOf(late + 1, early);
        m_slots.m_wordsDeadAcrossSuspension[indexInPlaceSortKey(key)] = BitSpan(m_arena.copySpan<uint64_t>(bitsOfDeadWords));
        for (uint32_t wordHeldAgain : wordsNotHeldHere)
            hold(wordHeldAgain);
        wordsNotHeldHere.clear();
    };

    ArenaVector<uint64_t> stores;
    for (SpillIndex spill : m_spillsByBegin) {
        if (m_partOfSpill[spill] != KeptAtSuspensions)
            continue;
        if (Position position = positionOfStoreToSpillPlace(m_proc, spills[spill]); position != noPosition)
            stores.append(m_arena, makePlaceSortKey(position, spill));
    }
    std::sort(stores.begin(), stores.end());
    uint32_t nextStore = 0;
    auto noteStoresInFrontOf = [&](Position position) {
        for (; nextStore < stores.size() && positionInPlaceSortKey(stores[nextStore]) < position; ++nextStore) {
            SpillIndex spill = indexInPlaceSortKey(stores[nextStore]);
            forEachHalfWordOfPlace(m_slots.m_distanceOfSpill[spill], sizeOfSpillPlace(spills[spill]), [&](uint32_t halfWord) {
                storedHalfWords.set(halfWord);
            });
        }
    };

    // Each block is visited once, behind the blocks in front of it that lead to it. What comes round a loop has run through its head, so an edge that leads back takes nothing away.
    // Where such an edge leads to a block that does not dominate it, a word may pass for stored that is not: it is cleared nowhere then, and no word is cleared that should not be.
    const Order& order = m_proc.order();
    std::span<Block* const> blocks = order.blocksInOrder();
    // There is at least one word of bits, and seldom another: whatever goes over all of them does the first in line.
    const size_t numberOfWordsOfBits = bitsOfStoredHalfWords.size();
    uint64_t* const storedHere = bitsOfStoredHalfWords.data();
    auto copyBitsOfStoredHalfWords = [&](uint64_t* to, const uint64_t* from) {
        to[0] = from[0];
        for (size_t wordOfBits = 1; wordOfBits < numberOfWordsOfBits; ++wordOfBits)
            to[wordOfBits] = from[wordOfBits];
    };
    // By the place in the order. A block asks for the blocks in front of it alone, and each of those has written its entry.
    const uint64_t** const storedAtEndOfBlock = m_arena.allocateArray<const uint64_t*>(blocks.size());
    Block* const* const blocksInOrder = blocks.data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocks.size());
    const BlockPlace* const places = order.places();
    const uint64_t* const keysOfSuspensions = suspensions.data();
    const size_t numberOfSuspensions = suspensions.size();
    size_t nextSuspension = 0;
    // The next suspension or the next store, whichever comes first. Only a block that holds one of them moves it on.
    auto findNextSuspensionOrStore = [&] {
        Position position = nextSuspension < numberOfSuspensions ? positionInPlaceSortKey(keysOfSuspensions[nextSuspension]) : noPosition;
        return nextStore < stores.size() ? std::min(position, positionInPlaceSortKey(stores[nextStore])) : position;
    };
    Position nextSuspensionOrStore = findNextSuspensionOrStore();
    for (uint32_t index = 0; nextSuspension < numberOfSuspensions; ++index) {
        // Most blocks have one predecessor, which stands in front of them, and hold neither a suspension nor a store: what is stored at their end is what was stored at its end.
        // Every other block begins by writing all of storedHere. A run of such blocks moves no suspension on, so one is still to come behind it.
        for (;; ++index) {
            RELEASE_ASSERT(index < numberOfBlocks);
            const Block* plainBlock = blocksInOrder[index];
            const BlockPlace& placeOfPlainBlock = places[plainBlock->index()];
            if (placeOfPlainBlock.predecessors.size() != 1 || nextSuspensionOrStore < placeOfPlainBlock.head + 2 * (plainBlock->size() + 1))
                break;
            const uint32_t indexOfPredecessor = places[(*placeOfPlainBlock.predecessors.data())->index()].orderIndex;
            if (indexOfPredecessor >= index)
                break;
            storedAtEndOfBlock[index] = storedAtEndOfBlock[indexOfPredecessor];
        }

        const Block* block = blocksInOrder[index];
        const BlockPlace& place = places[block->index()];
        std::span<Block* const> predecessors = place.predecessors;
        // Order::endOf().
        const Position endOfBlock = place.head + 2 * (block->size() + 1);

        const uint64_t* storedAtEndOfFirstPredecessor = nullptr;
        for (const Block* predecessor : predecessors) {
            uint32_t indexOfPredecessor = places[predecessor->index()].orderIndex;
            if (indexOfPredecessor >= index)
                continue;
            const uint64_t* storedAtEnd = storedAtEndOfBlock[indexOfPredecessor];
            if (!storedAtEndOfFirstPredecessor) {
                storedAtEndOfFirstPredecessor = storedAtEnd;
                copyBitsOfStoredHalfWords(storedHere, storedAtEnd);
                continue;
            }
            storedHere[0] &= storedAtEnd[0];
            for (size_t wordOfBits = 1; wordOfBits < numberOfWordsOfBits; ++wordOfBits)
                storedHere[wordOfBits] &= storedAtEnd[wordOfBits];
        }
        if (!storedAtEndOfFirstPredecessor) {
            storedHere[0] = 0;
            for (size_t wordOfBits = 1; wordOfBits < numberOfWordsOfBits; ++wordOfBits)
                storedHere[wordOfBits] = 0;
            everyPathBeginsAtEntrypoint &= block->isEntry();
        }

        if (nextSuspensionOrStore < endOfBlock) {
            for (; nextSuspension < numberOfSuspensions && positionInPlaceSortKey(keysOfSuspensions[nextSuspension]) < endOfBlock; ++nextSuspension) {
                noteStoresInFrontOf(positionInPlaceSortKey(keysOfSuspensions[nextSuspension]));
                visitSuspension(keysOfSuspensions[nextSuspension]);
            }
            noteStoresInFrontOf(endOfBlock);
            nextSuspensionOrStore = findNextSuspensionOrStore();
        }

        bool isAsAtEndOfFirstPredecessor = !!storedAtEndOfFirstPredecessor;
        for (size_t wordOfBits = 0; isAsAtEndOfFirstPredecessor && wordOfBits < numberOfWordsOfBits; ++wordOfBits)
            isAsAtEndOfFirstPredecessor = storedHere[wordOfBits] == storedAtEndOfFirstPredecessor[wordOfBits];
        if (isAsAtEndOfFirstPredecessor) {
            storedAtEndOfBlock[index] = storedAtEndOfFirstPredecessor;
            continue;
        }
        uint64_t* storedAtEnd = static_cast<uint64_t*>(m_arena.allocate(numberOfWordsOfBits * sizeof(uint64_t), alignof(uint64_t)));
        copyBitsOfStoredHalfWords(storedAtEnd, storedHere);
        storedAtEndOfBlock[index] = storedAtEnd;
    }

    if (firstWordClearedAtSetup < endOfWordsClearedAtSetup) {
        m_slots.m_firstWordClearedAtSetup = firstWordClearedAtSetup;
        m_slots.m_endOfWordsClearedAtSetup = endOfWordsClearedAtSetup;
    }
}

void SlotAssigner::publishDeadWords(Procedure& proc, unsigned numberOfCalleeSaves)
{
    std::span<PatchOperation* const> suspensions = proc.allocation().suspensions();
    if (suspensions.empty())
        return;

    Arena& arena = proc.arena();
    const SlotAssignment& slots = proc.slots();
    const uint32_t numberOfWordsAboveSlots = numberOfCalleeSaves + numberOfCalleeSaves % 2;
    const uint32_t numberOfWordsKept = slots.numberOfWordsKeptAtSuspensions();
    const uint32_t firstWordKept = numberOfWordsAboveSlots + 1;
    const uint32_t bytesBelowFrameRecord = roundUpPlaceDistance(numberOfWordsAboveSlots * bytesPerPlaceWord + slots.bytesInAll() + proc.callArgAreaSizeInBytes(), stackAlignmentBytes());
    const uint32_t numberOfWordsInFrame = bytesBelowFrameRecord / bytesPerPlaceWord;

    // A suspension keeps whole pairs of words, so it may keep the first word below the first part.
    std::span<uint64_t> bitsOfWordsAlwaysDead = arena.createSpan<uint64_t>(numberOfWordsInFrame / 64 + 1);
    BitSpan wordsAlwaysDead(bitsOfWordsAlwaysDead);
    if (numberOfCalleeSaves % 2)
        wordsAlwaysDead.set(numberOfCalleeSaves + 1);
    for (uint32_t word = firstWordKept + numberOfWordsKept; word <= numberOfWordsInFrame; ++word)
        wordsAlwaysDead.set(word);

    for (uint32_t index = 0; index < suspensions.size(); ++index) {
        BitSpan deadWordsOfFrame(arena.copySpan<uint64_t>(bitsOfWordsAlwaysDead));
        const BitSpan& deadWordsOfFirstPart = slots.wordsDeadAcross(index);
        deadWordsOfFirstPart.forEachSetBit([&](size_t word) {
            deadWordsOfFrame.set(firstWordKept + word);
        });
        suspensions[index]->m_frameWordsDeadAcrossSuspension = deadWordsOfFrame;

        const WordsClearedAtSuspension& cleared = slots.wordsClearedAt(index);
        RELEASE_ASSERT(cleared.numberOfLowestWords <= SavedFrame::maxClearedWords && cleared.numberOfLowestWords <= numberOfWordsKept + numberOfWordsKept % 2);
        suspensions[index]->m_numberOfFrameWordsToClearAtFirstSuspension = static_cast<uint8_t>(cleared.numberOfLowestWords);

        const bool clearsUnusedWord = cleared.wordAboveFirstPart && numberOfCalleeSaves % 2;
        if (!clearsUnusedWord && !cleared.others.size())
            continue;
        BitSpan wordsClearedInFront = BitSpan::create(arena, firstWordKept + numberOfWordsKept);
        if (clearsUnusedWord)
            wordsClearedInFront.set(numberOfCalleeSaves + 1);
        cleared.others.forEachSetBit([&](size_t word) {
            wordsClearedInFront.set(firstWordKept + word);
        });
        suspensions[index]->m_frameWordsClearedInFrontOfSuspension = wordsClearedInFront;
    }
}

SlotAssignment* assignSlots(Procedure& proc)
{
    SlotAssignment* slots = proc.arena().create<SlotAssignment>();
    // Most functions have nothing to place and nothing to say of a suspension: the assignment stays as it was made.
    const Allocation& allocation = proc.allocation();
    if (allocation.spills().empty() && proc.stackSlots().empty() && allocation.suspensions().empty())
        return slots;
    SlotAssigner(proc, *slots).run();
    return slots;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
