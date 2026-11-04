/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"

namespace JSC { namespace AOT { namespace Machine {

struct SpillPlaceRange;

// A word that nothing has stored to since the frame was set up holds what an earlier call left at its address. A suspension would keep it, and every resumption would put it back on the stack.
// A function that returns ahead of its first suspension pays nothing for that: such a word is cleared where the function may be suspended for the first time, which is at any suspension.
// The words are those that no spill and no StackSlot holds where the suspension begins, and that some path to it may not have stored to. What the suspension defines is written behind it.
// A range goes by the order, so it may reach over a suspension that does not hold the spill: Allocation::spillsHeldAt(). Nothing reads the spill from there on, so it holds nothing there.
// The same goes for a suspension that a path gets to without the definition of the value, whatever step may read the spill behind it.
struct WordsClearedAtSuspension {
    // Whoever suspends the function for the first time clears the lowest words that suspensions keep, this many: SavedFrame::siteWord().
    // They keep whole pairs, so below a first part with an odd number of words the next word is the lowest.
    uint32_t numberOfLowestWords { 0 };
    // Stores in front of the suspension clear the others. Bit w: word w of the first part. Without a bit where there are none.
    BitSpan others;
    // And the word above the first part, which nothing uses where the number of callee saves is odd.
    bool wordAboveFirstPart { false };
};

// Step 5. The part of the frame between the callee saves and the outgoing words, from the top down:
//
//     the spills and StackSlots that a suspension keeps
//     a word that nothing uses, where those are an odd number of words: suspensions keep whole pairs
//     the spills and StackSlots that no suspension keeps
//
// A function without suspensions has the second part alone. Distances here count bytes down from the top of the first part. planFrame() turns them into offsets from the frame pointer.
class SlotAssignment {
    WTF_MAKE_NONCOPYABLE(SlotAssignment);
public:
    // The lowest byte of the place lies this far below the top: a multiple of the size of the place, which is 4 or 8 for a spill.
    uint32_t distanceOf(SpillIndex spill) const { return m_distanceOfSpill[spill]; }
    // A multiple of 8, or of 16 for a slot of 16 bytes or more.
    uint32_t distanceOf(const StackSlot* slot) const { return m_distanceOfStackSlot[slot->index()]; }

    // Both are multiples of 8.
    uint32_t bytesKeptAtSuspensions() const { return m_bytesKeptAtSuspensions; }
    uint32_t bytesInAll() const { return m_bytesInAll; }

    // The words of the first part.
    uint32_t numberOfWordsKeptAtSuspensions() const { return m_bytesKeptAtSuspensions / sizeof(uint64_t); }

    // By the order in which Allocation lists the suspensions: Allocation::suspensions(). Bit w: word w of the first part, counted from 0 at the top, holds nothing that is read behind the suspension.
    const BitSpan& wordsDeadAcross(uint32_t suspension) const { return m_wordsDeadAcrossSuspension[suspension]; }

    const WordsClearedAtSuspension& wordsClearedAt(uint32_t suspension) const { return m_wordsClearedAtSuspension[suspension]; }

    // Words of the first part: [first, end). They hold spills that several steps write, for which spillPlaceIsWrittenBehindDefinitionAlone() does not hold, at a suspension that a path may reach without any of them.
    // There the word can be neither relied on nor cleared, so every setup of the frame clears it.
    uint32_t firstWordClearedAtSetup() const { return m_firstWordClearedAtSetup; }
    uint32_t endOfWordsClearedAtSetup() const { return m_endOfWordsClearedAtSetup; }

private:
    friend class Arena;
    friend class SlotAssigner;

    SlotAssignment() = default;

    ArenaVector<uint32_t> m_distanceOfSpill;
    ArenaVector<uint32_t> m_distanceOfStackSlot;
    ArenaVector<BitSpan> m_wordsDeadAcrossSuspension;
    ArenaVector<WordsClearedAtSuspension> m_wordsClearedAtSuspension;
    uint32_t m_firstWordClearedAtSetup { 0 };
    uint32_t m_endOfWordsClearedAtSetup { 0 };
    uint32_t m_bytesKeptAtSuspensions { 0 };
    uint32_t m_bytesInAll { 0 };
};

// One scan over the spills in rising order of SpillFacts::begin. A place is free again behind SpillFacts::end of its holder, and in the gap of its holder.
// A free place of the same size and the same part is taken before a new one is made, the one that became free last first. Two free halves of one word do not join.
// The order of the scan and of the free lists depends on positions alone, so equal procedures get equal frames.
class SlotAssigner {
    WTF_MAKE_NONCOPYABLE(SlotAssigner);
public:
    SlotAssigner(Procedure&, SlotAssignment&);
    void run();

    // Sets what PatchOperation::frameWordsDeadAcrossSuspension(), numberOfFrameWordsToClearAtFirstSuspension() and frameWordsClearedInFrontOfSuspension() answer,
    // in words counted down from the frame pointer, first word 1. planFrame() calls it: the callee saves lie in between.
    // None of their words is dead. The unused word below an odd number of them is.
    static void publishDeadWords(Procedure&, unsigned numberOfCalleeSaves);

    enum Part : uint8_t { KeptAtSuspensions, DeadAtSuspensions };
    static constexpr unsigned numberOfParts = 2;
    enum Size : uint8_t { Half, Word };
    static constexpr unsigned numberOfSizes = 2;

    // Of findWordsDeadAcrossSuspensions().
    struct Holder {
        Position end;
        uint32_t distanceWithinPart;
        Part part;
        Size size;
        bool isWrittenBehindDefinitionAlone { false };
        SpillIndex spill { noSpill };
    };

    // The words that SlotAssignment lays out, counted from 0 at the top of the first part.
    struct FrameWords {
        // One byte of offset reaches the words in front of the first from the frame pointer, and those from the second on from the stack pointer.
        uint32_t endOfReachOfFramePointer { 0 };
        uint32_t beginOfReachOfStackPointer { 0 };
        // How many instructions name a spill that lies in the word. UINT32_MAX: PatchOperation::operandEncodedAsFrameWordIndex().
        std::span<uint32_t> numberOfAccesses;
        // By StackSlot::index().
        std::span<uint32_t> numberOfAccessesToStackSlot;
        std::span<uint32_t> newWordOf;
    };

    void placeStackSlots();
    void sortSpillsByBegin();
    void releasePlacesFreeAt(Position);
    void releasePlacesOfNextSpillsFreeAt(Position);
    uint32_t takePlace(Part, Size, Position begin, Position endOfFirstPart, Position beginBehindGap, Position end);
    // Moves the second part below the first, once the size of the first is known.
    void joinParts();
    // And the words to clear: both from the spills of the first part in the order of the scan, with the suspensions beside them.
    // What every path has stored to goes from block to block in the order, each block once. It looks at no operation.
    void findWordsDeadAcrossSuspensions();
    // x86-64: one byte of offset reaches 128 bytes from either end of the frame, and within its part a word may lie anywhere. So the words and StackSlots with the most accesses to a word lie near the frame pointer, and the next near the stack pointer.
    // An access is an instruction that names the place. No instruction changes but in its length: places that share a word move together, a StackSlot moves as a whole, and among words that are equally near the order of the scan stays.
    // The lowest words of the first part that whoever suspends the function first clears stay, and the whole first part stays where a setup clears words of it.
    void bringBusyWordsNearEndsOfFrame();
    void countAccessesToFrame(FrameWords&);
    // The words from begin to end belong to that part and hold all of its StackSlots.
    void arrangeWordsOfPart(FrameWords&, Part, uint32_t begin, uint32_t end);

    Procedure& m_proc;
    Arena& m_arena;
    const Allocation& m_allocation;
    SlotAssignment& m_slots;
    ArenaVector<SpillIndex> m_spillsByBegin;
    // By SpillIndex, from sortSpillsByBegin() on: from the first write to the place to the last read of it.
    const SpillPlaceRange* m_rangesOfSpillPlaces { nullptr };
    // From sortSpillsByBegin() on, one key for each spill, rising: the position behind which its place is free again, above the spill.
    // A range does not end ahead of its beginning, so by the time the scan has passed that position the spill has its place.
    uint64_t* m_spillsByEndOfHold { nullptr };
    // In m_spillsByEndOfHold: the places of those in front of it have gone back to the free places.
    uint32_t m_nextSpillToLetGo { 0 };
    // By SpillIndex, once the scan has given the spill its place: the distance within its part, and below that the part and the size.
    uint32_t* m_placesOfSpills { nullptr };
    // A heap by Holder::end.
    ArenaVector<Holder> m_holders;
    ArenaVector<uint32_t> m_freePlaces[numberOfParts][numberOfSizes];
    // A place is free again where the gap of its holder begins, and is the holder's once more behind the gap. By the distance of the place in half words: from the first to the last position that holders so far read behind their gaps.
    // Whoever takes the place next lies apart from all of that. Nothing is taken while end lies ahead of the scan.
    struct PositionsTakenBehindGaps {
        Position begin { noPosition };
        Position end { 0 };
    };
    ArenaVector<PositionsTakenBehindGaps> m_positionsTakenBehindGaps[numberOfParts];
    uint32_t m_bytesOfPart[numberOfParts] { 0, 0 };
    ArenaVector<Part> m_partOfSpill;
    ArenaVector<Part> m_partOfStackSlot;
};

SlotAssignment* assignSlots(Procedure&);

// Whatever runs through this position of its block has written the place of the spill. noPosition where Saves write it, wherever the value loses its register.
Position positionOfStoreToSpillPlace(const Procedure&, const SpillFacts&);
// One store writes the place, at that position: right behind the definition of the value, which lies on every path to a read of it.
// So where some path from an entrypoint leads without that store, nothing reads the place before the store has run.
bool spillPlaceIsWrittenBehindDefinitionAlone(const Procedure&, const SpillFacts&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
