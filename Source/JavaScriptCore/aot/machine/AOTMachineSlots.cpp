/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineSlots.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
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
    Position endOfFirstPart() const { return hasGap() ? beginOfGap - 1 : end; }

    Position begin;
    Position end;
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

static SpillPlaceRange rangeOfSpillPlace(const Procedure& proc, const SpillFacts& facts)
{
    SpillPlaceRange range { facts.begin, std::max(facts.begin, facts.end), facts.beginOfGap, facts.endOfGap };

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
    if (proc.liveness().isSunk(value))
        return facts.begin;
    const Block* block = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
    return std::max(facts.begin, proc.order().headOf(block));
}

bool spillPlaceIsWrittenBehindDefinitionAlone(const Procedure& proc, const SpillFacts& facts)
{
    const Operation* value = facts.value;
    if (facts.isSaveInRareRegion || !value)
        return false;
    return facts.isPointerBesideNarrowReference || value->opcode() != Phi || proc.allocation().registerAtDefinitionOf(value).isSet();
}

struct HalfWordsOfSpillPlace {
    uint32_t wordOfBits;
    uint64_t bits;
};

static HalfWordsOfSpillPlace halfWordsOfSpillPlace(uint32_t distance, SlotAssigner::Size size)
{
    const uint32_t lastHalfWord = distance / bytesPerPlaceHalf - 1;
    if (size == SlotAssigner::Half)
        return { lastHalfWord / 64, uint64_t { 1 } << (lastHalfWord % 64) };
    ASSERT(lastHalfWord % 2);
    return { lastHalfWord / 64, uint64_t { 3 } << ((lastHalfWord - 1) % 64) };
}

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

    if (!m_proc.stackSlots().empty())
        placeStackSlots();
    sortSpillsByBegin();
    m_partOfSpill.growWithoutInitializing(m_arena, numberOfSpills);
    m_slots.m_distanceOfSpill.growWithoutInitializing(m_arena, numberOfSpills);

    const SpillFacts* const factsOfSpills = spills.data();
    const SpillPlaceRange* const rangesOfSpillPlaces = m_rangesOfSpillPlaces;
    Part* const partOfSpill = m_partOfSpill.begin();
    uint32_t* const distanceOfSpill = m_slots.m_distanceOfSpill.begin();
    uint32_t* const placesOfSpills = m_placesOfSpills;
    const uint64_t* const spillsByBegin = m_spillsByBegin;
    for (uint32_t index = 0; index < numberOfSpills; ++index) {
        const SpillIndex spill = indexInPlaceSortKey(spillsByBegin[index]);
        const SpillFacts& facts = factsOfSpills[spill];
        const SpillPlaceRange range = rangesOfSpillPlaces[spill];
        releasePlacesFreeAt(range.begin);

        Part part = savesFrame && facts.livesAcrossSuspension ? KeptAtSuspensions : DeadAtSuspensions;
        Size size = sizeOfSpillPlace(facts);
        uint32_t distance = takePlace(part, size, range.begin, range.endOfFirstPart(), range.hasGap() ? range.endOfGap + 1 : noPosition, range.end);
        partOfSpill[spill] = part;
        distanceOfSpill[spill] = distance;
        placesOfSpills[spill] = makeHeldPlace(distance, part, size);
    }

    joinParts();
    if (!m_allocation.suspensions().empty())
        findWordsDeadAcrossSuspensions();
    if (isX86_64())
        bringBusyWordsNearEndsOfFrame();
}

static bool wordsLieInSeveralReaches(const SlotAssigner::FrameWords& words, uint32_t begin, uint32_t end)
{
    const uint32_t endOfHighWords = std::clamp(words.endOfReachOfFramePointer, begin, end);
    const uint32_t beginOfLowWords = std::clamp(words.beginOfReachOfStackPointer, endOfHighWords, end);
    return (begin < endOfHighWords) + (endOfHighWords < beginOfLowWords) + (beginOfLowWords < end) >= 2;
}

void SlotAssigner::bringBusyWordsNearEndsOfFrame()
{
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

    const uint32_t numberOfWordsKept = m_slots.numberOfWordsKeptAtSuspensions();
    const bool wordsKeptMayMove = m_slots.m_firstWordClearedAtSetup >= m_slots.m_endOfWordsClearedAtSetup;
    const uint32_t endOfWordsKeptThatMayMove = wordsKeptMayMove ? numberOfWordsKept : 0;
    const uint32_t firstWordDeadAtSuspensions = numberOfWords - m_bytesOfPart[DeadAtSuspensions] / bytesPerPlaceWord;
    if (wordsLieInSeveralReaches(words, 0, endOfWordsKeptThatMayMove) || wordsLieInSeveralReaches(words, firstWordDeadAtSuspensions, numberOfWords))
        countAccessesToFrame(words);
    if (wordsKeptMayMove)
        arrangeWordsOfPart(words, KeptAtSuspensions, 0, endOfWordsKeptThatMayMove);
    arrangeWordsOfPart(words, DeadAtSuspensions, firstWordDeadAtSuspensions, numberOfWords);

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
    auto moveBitsOfWordsKept = [&](const BitSpan& bits) {
        std::span<uint64_t> bitsOfMovedWords = m_arena.createSpan<uint64_t>((static_cast<size_t>(numberOfWordsKept) + 63) / 64);
        BitSpan moved(bitsOfMovedWords);
        bits.forEachSetBit([&](size_t word) {
            RELEASE_ASSERT(word < numberOfWordsKept);
            moved.set(words.newWordOf[word]);
        });
        return bitsOfMovedWords;
    };
    for (std::span<uint64_t>& deadWords : m_slots.m_wordsDeadAcrossSuspension)
        deadWords = moveBitsOfWordsKept(BitSpan(deadWords));
}

static_assert(sizeof(Location) == sizeof(uint32_t));
static_assert(offsetof(Step, sources) == offsetof(Step, destination) + sizeof(Location));
static_assert((1 + maxLeaves) * sizeof(Location) == 3 * sizeof(uint64_t));
static constexpr uint64_t bitOfLocationInFrame = std::bit_cast<uint32_t>(Location::spill(0));
static_assert(std::has_single_bit(bitOfLocationInFrame));
static_assert(std::bit_cast<uint32_t>(Location::slotBytes(0)) & bitOfLocationInFrame);
static_assert(!((std::bit_cast<uint32_t>(Location::spill(0)) | std::bit_cast<uint32_t>(Location::slotBytes(0))) & bitOfLocationInFrame << 1));
static_assert(std::bit_cast<uint32_t>(Location::immediate()) & bitOfLocationInFrame);
static constexpr uint64_t bitsOfPairWithLocationInFrame = bitOfLocationInFrame << 32 | bitOfLocationInFrame;

static constexpr uint64_t bitsOfPairWithoutBitAbove(uint64_t pair)
{
    return pair & ~(pair >> 1);
}

static ALWAYS_INLINE bool loadPairsThatMayHoldLocationInFrame(const Step& step, uint64_t (&pairs)[3])
{
    memcpy(pairs, reinterpret_cast<const char*>(&step) + offsetof(Step, destination), sizeof(pairs));
    return (pairs[0] | pairs[1] | pairs[2]) & bitsOfPairWithLocationInFrame;
}

template<typename Functor>
static ALWAYS_INLINE void forEachStepInFours(std::span<const Step> steps, const Functor& functor)
{
    const Step* step = steps.data();
    const Step* const end = step + steps.size();
    for (const Step* const endOfFours = step + (steps.size() & ~static_cast<size_t>(3)); step != endOfFours; step += 4) {
        functor(step[0]);
        functor(step[1]);
        functor(step[2]);
        functor(step[3]);
    }
    for (; step != end; ++step)
        functor(*step);
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

    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        const SpillFacts& facts = spills[spill];
        if (facts.value && !facts.isSaveInRareRegion && !facts.isPointerBesideNarrowReference && m_allocation.isStoredBehindDefinition(facts.value))
            note(Location::spill(spill));
    }

    auto noteLocationsIn = [&](const Step& step, const uint64_t (&pairs)[3]) ALWAYS_INLINE_LAMBDA {
        const uint64_t firstPair = bitsOfPairWithoutBitAbove(pairs[0]);
        const uint64_t secondPair = bitsOfPairWithoutBitAbove(pairs[1]);
        const uint64_t thirdPair = bitsOfPairWithoutBitAbove(pairs[2]);
        if (!((firstPair | secondPair | thirdPair) & bitsOfPairWithLocationInFrame))
            return;
        if (step.kind == StepKind::Execute && step.operation->opcode() == Extract)
            return;
        if (firstPair & bitsOfPairWithLocationInFrame) {
            note(step.destination);
            note(step.sources[0]);
        }
        if (secondPair & bitsOfPairWithLocationInFrame) {
            note(step.sources[1]);
            note(step.sources[2]);
        }
        if (thirdPair & bitsOfPairWithLocationInFrame) {
            note(step.sources[3]);
            note(step.sources[4]);
        }
    };

    std::span<const Step> steps = m_allocation.steps();
    if (m_proc.stackSlots().empty()) {
        forEachStepInFours(steps, [&](const Step& step) ALWAYS_INLINE_LAMBDA {
            uint64_t pairs[3];
            if (loadPairsThatMayHoldLocationInFrame(step, pairs))
                noteLocationsIn(step, pairs);
        });
    } else {
        forEachStepInFours(steps, [&](const Step& step) ALWAYS_INLINE_LAMBDA {
            ASSERT(step.kind != StepKind::Materialize || step.sources[0].isImmediate());
            uint64_t pairs[3];
            if (!loadPairsThatMayHoldLocationInFrame(step, pairs))
                return;
            if (step.kind != StepKind::Materialize) {
                noteLocationsIn(step, pairs);
                return;
            }
            const Operation* operation = step.operation;
            while (operation->opcode() == Add && operation->hasNoPosition())
                operation = operation->child(0);
            if (operation->opcode() == SlotBase)
                ++numberOfAccessesToStackSlot[operation->slot()->index()];
        });
    }

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

    for (uint32_t word : wordsEncodedAsIndex)
        numberOfAccesses[word] = UINT32_MAX;
}

void SlotAssigner::arrangeWordsOfPart(FrameWords& words, Part part, uint32_t begin, uint32_t end)
{
    RELEASE_ASSERT(begin <= end && !(begin % 2));
    if (!wordsLieInSeveralReaches(words, begin, end))
        return;
    const uint32_t endOfHighWords = std::clamp(words.endOfReachOfFramePointer, begin, end);
    const uint32_t beginOfLowWords = std::clamp(words.beginOfReachOfStackPointer, endOfHighWords, end);

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

        uint32_t mostWordsOfSlotsWithoutPlace = 1;
        for (const StackSlotInPart& slot : slots)
            mostWordsOfSlotsWithoutPlace += slot.numberOfWords + slot.endsAtEvenWord;

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
    Position previousBegin = 0;
    Position previousEndOfHold = 0;
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const SpillPlaceRange* range = new (NotNull, ranges + spill) SpillPlaceRange(rangeOfSpillPlace(m_proc, factsOfSpills[spill]));
        ASSERT(range->begin <= range->endOfFirstPart());
        const Position begin = range->begin;
        const Position endOfHold = range->endOfFirstPart();
        keys[spill] = makePlaceSortKey(begin, spill);
        keysRise &= previousBegin <= begin;
        previousBegin = begin;
        keysByEndOfHold[spill] = makePlaceSortKey(endOfHold, spill);
        keysByEndOfHoldRise &= previousEndOfHold <= endOfHold;
        previousEndOfHold = endOfHold;
    }
    m_rangesOfSpillPlaces = ranges;
    if (!keysRise)
        std::sort(keys, keys + numberOfSpills);
    if (!keysByEndOfHoldRise)
        std::sort(keysByEndOfHold, keysByEndOfHold + numberOfSpills);
    m_spillsByBegin = keys;
    m_numberOfSpills = numberOfSpills;
    m_spillsByEndOfHold = keysByEndOfHold;
    m_endOfNextHold = positionInPlaceSortKey(keysByEndOfHold[0]);
    m_placesOfSpills = m_arena.allocateArray<uint32_t>(numberOfSpills);
}

ALWAYS_INLINE_INTO_OTHER_FILES void SlotAssigner::releasePlacesFreeAt(Position position)
{
    if (m_endOfNextHold < position)
        releasePlacesOfNextSpillsFreeAt(position);
}

NEVER_INLINE void SlotAssigner::releasePlacesOfNextSpillsFreeAt(Position position)
{
    uint64_t* const keys = m_spillsByEndOfHold;
    const uint32_t* const placesOfSpills = m_placesOfSpills;
    const uint32_t numberOfSpills = m_numberOfSpills;
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
            for (uint32_t index = next; index < endOfEquals; ++index)
                keys[index] = placesOfSpills[indexInPlaceSortKey(keys[index])];
            std::sort(keys + next, keys + endOfEquals);
            for (uint32_t index = next; index < endOfEquals; ++index)
                release(static_cast<uint32_t>(keys[index]));
        }
        next = endOfEquals;
    } while (next < numberOfSpills && positionInPlaceSortKey(keys[next]) < position);
    m_nextSpillToLetGo = next;
    m_endOfNextHold = next < numberOfSpills ? positionInPlaceSortKey(keys[next]) : noPosition;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t SlotAssigner::takePlace(Part part, Size size, Position begin, Position endOfFirstPart, Position beginBehindGap, Position end)
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
    if (taken.isEmpty() && !freePlaces.isEmpty())
        return take(freePlaces.takeLast());
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
    std::span<const uint64_t> suspensions = sortSuspensionsForPlaces(m_proc);
    m_slots.m_wordsDeadAcrossSuspension.grow(m_arena, static_cast<uint32_t>(suspensions.size()));

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
    auto holdSpillsThatBeginInFrontOf = [&](Position position, Position early) ALWAYS_INLINE_LAMBDA {
        for (; nextSpill < m_numberOfSpills; ++nextSpill) {
            SpillIndex spill = indexInPlaceSortKey(m_spillsByBegin[nextSpill]);
            if (m_partOfSpill[spill] != KeptAtSuspensions)
                continue;
            const SpillPlaceRange range = m_rangesOfSpillPlaces[spill];
            if (range.begin >= position)
                break;
            if (range.end < early)
                continue;
            uint32_t distance = m_slots.m_distanceOfSpill[spill];
            uint32_t word = wordHoldingPlace(distance);
            HalfWordsOfSpillPlace halfWords = halfWordsOfSpillPlace(distance, sizeOfSpillPlace(spills[spill]));
            hold(word);
            m_holders.append(m_arena, Holder { range.end, word, spill, halfWords.wordOfBits, halfWords.bits, spillPlaceIsWrittenBehindDefinitionAlone(m_proc, spills[spill]) });
        }
    };

    std::span<uint64_t> bitsOfStoredHalfWords = m_arena.createSpan<uint64_t>(numberOfWords / 32 + 1);
    uint64_t* const storedHere = bitsOfStoredHalfWords.data();

    uint32_t firstWordClearedAtSetup = numberOfWords;
    uint32_t endOfWordsClearedAtSetup = 0;
    bool everyPathBeginsAtEntrypoint = true;
    ArenaVector<uint32_t> wordsNotHeldHere;
    auto visitSuspension = [&](uint64_t key) {
        const Position late = positionInPlaceSortKey(key);
        const Position early = late - 1;
        const BitSpan liveSpillsHere = m_allocation.liveSpillsAt(indexInPlaceSortKey(key));

        holdSpillsThatBeginInFrontOf(late, early);
        Holder* const holders = m_holders.begin();
        uint32_t numberOfSpillsHeld = m_holders.size();
        for (uint32_t index = 0; index < numberOfSpillsHeld;) {
            const Holder& holder = holders[index];
            const uint32_t word = holder.word;
            if (holder.end < early) {
                letGo(word);
                holders[index] = holders[--numberOfSpillsHeld];
                continue;
            }
            ++index;
            if (!liveSpillsHere.get(holder.spill)) {
                letGo(word);
                wordsNotHeldHere.append(m_arena, word);
                continue;
            }
            if ((storedHere[holder.wordOfStoredBits] & holder.storedBits) == holder.storedBits)
                continue;
            if (holder.isWrittenBehindDefinitionAlone && everyPathBeginsAtEntrypoint)
                continue;
            firstWordClearedAtSetup = std::min(firstWordClearedAtSetup, word);
            endOfWordsClearedAtSetup = std::max(endOfWordsClearedAtSetup, word + 1);
        }
        m_holders.shrink(numberOfSpillsHeld);

        holdSpillsThatBeginInFrontOf(late + 1, early);
        m_slots.m_wordsDeadAcrossSuspension[indexInPlaceSortKey(key)] = m_arena.copySpan<uint64_t>(bitsOfDeadWords);
        for (uint32_t wordHeldAgain : wordsNotHeldHere)
            hold(wordHeldAgain);
        wordsNotHeldHere.clear();
    };

    ArenaVector<uint64_t> stores;
    for (uint32_t index = 0; index < m_numberOfSpills; ++index) {
        SpillIndex spill = indexInPlaceSortKey(m_spillsByBegin[index]);
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
            HalfWordsOfSpillPlace halfWords = halfWordsOfSpillPlace(m_slots.m_distanceOfSpill[spill], sizeOfSpillPlace(spills[spill]));
            storedHere[halfWords.wordOfBits] |= halfWords.bits;
        }
    };

    const Order& order = m_proc.order();
    std::span<Block* const> blocks = order.blocksInOrder();
    const size_t numberOfWordsOfBits = bitsOfStoredHalfWords.size();
    auto copyBitsOfStoredHalfWords = [&](uint64_t* to, const uint64_t* from) {
        to[0] = from[0];
        for (size_t wordOfBits = 1; wordOfBits < numberOfWordsOfBits; ++wordOfBits)
            to[wordOfBits] = from[wordOfBits];
    };
    const uint64_t** const storedAtEndOfBlock = m_arena.createSpan<const uint64_t*>(m_proc.numberOfBlocks()).data();
    Block* const* const blocksInOrder = blocks.data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocks.size());
    const BlockPlace* const places = order.places();
    const uint64_t* const keysOfSuspensions = suspensions.data();
    const size_t numberOfSuspensions = suspensions.size();
    size_t nextSuspension = 0;
    auto findNextSuspensionOrStore = [&] {
        Position position = nextSuspension < numberOfSuspensions ? positionInPlaceSortKey(keysOfSuspensions[nextSuspension]) : noPosition;
        return nextStore < stores.size() ? std::min(position, positionInPlaceSortKey(stores[nextStore])) : position;
    };
    Position nextSuspensionOrStore = findNextSuspensionOrStore();
    for (uint32_t index = 0; nextSuspension < numberOfSuspensions; ++index) {
        for (;; ++index) {
            RELEASE_ASSERT(index < numberOfBlocks);
            const Block* plainBlock = blocksInOrder[index];
            const BlockPlace& placeOfPlainBlock = places[plainBlock->index()];
            if (placeOfPlainBlock.predecessors.size() != 1 || nextSuspensionOrStore < placeOfPlainBlock.head + 2 * (plainBlock->size() + 1))
                break;
            const uint64_t* storedAtEndOfPredecessor = storedAtEndOfBlock[(*placeOfPlainBlock.predecessors.data())->index()];
            if (!storedAtEndOfPredecessor)
                break;
            storedAtEndOfBlock[plainBlock->index()] = storedAtEndOfPredecessor;
        }

        const Block* block = blocksInOrder[index];
        const BlockPlace& place = places[block->index()];
        std::span<Block* const> predecessors = place.predecessors;
        const Position endOfBlock = place.head + 2 * (block->size() + 1);

        const uint64_t* storedAtEndOfFirstPredecessor = nullptr;
        for (const Block* predecessor : predecessors) {
            const uint64_t* storedAtEnd = storedAtEndOfBlock[predecessor->index()];
            if (!storedAtEnd)
                continue;
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
            storedAtEndOfBlock[block->index()] = storedAtEndOfFirstPredecessor;
            continue;
        }
        uint64_t* storedAtEnd = static_cast<uint64_t*>(m_arena.allocate(numberOfWordsOfBits * sizeof(uint64_t), alignof(uint64_t)));
        copyBitsOfStoredHalfWords(storedAtEnd, storedHere);
        storedAtEndOfBlock[block->index()] = storedAtEnd;
    }

    if (firstWordClearedAtSetup < endOfWordsClearedAtSetup) {
        m_slots.m_firstWordClearedAtSetup = firstWordClearedAtSetup;
        m_slots.m_endOfWordsClearedAtSetup = endOfWordsClearedAtSetup;
    }
}

static void setBitsMovedUp(std::span<uint64_t> to, std::span<const uint64_t> from, uint32_t distance)
{
    const size_t wordsOfBits = distance / 64;
    const uint32_t bitsWithinWord = distance % 64;
    for (size_t index = 0; index < from.size(); ++index) {
        const uint64_t bits = from[index];
        if (!bits)
            continue;
        to[index + wordsOfBits] |= bits << bitsWithinWord;
        if (bitsWithinWord && bits >> (64 - bitsWithinWord))
            to[index + wordsOfBits + 1] |= bits >> (64 - bitsWithinWord);
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

    std::span<uint64_t> bitsOfWordsAlwaysDead = arena.createSpan<uint64_t>(numberOfWordsInFrame / 64 + 1);
    BitSpan wordsAlwaysDead(bitsOfWordsAlwaysDead);
    if (numberOfCalleeSaves % 2)
        wordsAlwaysDead.set(numberOfCalleeSaves + 1);
    for (uint32_t word = firstWordKept + numberOfWordsKept; word <= numberOfWordsInFrame; ++word)
        wordsAlwaysDead.set(word);

    for (uint32_t index = 0; index < suspensions.size(); ++index) {
        std::span<uint64_t> bitsOfDeadWordsOfFrame = arena.copySpan<uint64_t>(bitsOfWordsAlwaysDead);
        setBitsMovedUp(bitsOfDeadWordsOfFrame, slots.wordsDeadAcross(index), firstWordKept);
        FactsOfSuspension& facts = suspensions[index]->ensureFactsOfSuspension();
        facts.frameWordsDeadAcrossSuspension = BitSpan(bitsOfDeadWordsOfFrame);
    }
}

SlotAssignment* assignSlots(Procedure& proc)
{
    SlotAssignment* slots = proc.arena().create<SlotAssignment>();
    const Allocation& allocation = proc.allocation();
    if (allocation.spills().empty() && proc.stackSlots().empty() && allocation.suspensions().empty())
        return slots;
    SlotAssigner(proc, *slots).run();
    return slots;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
