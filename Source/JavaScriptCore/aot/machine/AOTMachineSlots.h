/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"

namespace JSC { namespace AOT { namespace Machine {

struct SpillPlaceRange;

class SlotAssignment {
    WTF_MAKE_NONCOPYABLE(SlotAssignment);
public:
    uint32_t distanceOf(SpillIndex spill) const { return m_distanceOfSpill[spill]; }
    uint32_t distanceOf(const StackSlot* slot) const { return m_distanceOfStackSlot[slot->index()]; }

    uint32_t bytesKeptAtSuspensions() const { return m_bytesKeptAtSuspensions; }
    uint32_t bytesInAll() const { return m_bytesInAll; }

    uint32_t numberOfWordsKeptAtSuspensions() const { return m_bytesKeptAtSuspensions / sizeof(uint64_t); }

    std::span<const uint64_t> wordsDeadAcross(uint32_t suspension) const { return m_wordsDeadAcrossSuspension[suspension]; }

    uint32_t firstWordClearedAtSetup() const { return m_firstWordClearedAtSetup; }
    uint32_t endOfWordsClearedAtSetup() const { return m_endOfWordsClearedAtSetup; }

private:
    friend class Arena;
    friend class SlotAssigner;

    SlotAssignment() = default;

    ArenaVector<uint32_t> m_distanceOfSpill;
    ArenaVector<uint32_t> m_distanceOfStackSlot;
    ArenaVector<std::span<uint64_t>> m_wordsDeadAcrossSuspension;
    uint32_t m_firstWordClearedAtSetup { 0 };
    uint32_t m_endOfWordsClearedAtSetup { 0 };
    uint32_t m_bytesKeptAtSuspensions { 0 };
    uint32_t m_bytesInAll { 0 };
};

class SlotAssigner {
    WTF_MAKE_NONCOPYABLE(SlotAssigner);
public:
    SlotAssigner(Procedure&, SlotAssignment&);
    void run();

    static void publishDeadWords(Procedure&, unsigned numberOfCalleeSaves);

    enum Part : uint8_t { KeptAtSuspensions, DeadAtSuspensions };
    static constexpr unsigned numberOfParts = 2;
    enum Size : uint8_t { Half, Word, Pair };
    static constexpr unsigned numberOfSizes = 2;

    struct Holder {
        Position end;
        uint32_t word;
        SpillIndex spill;
        uint32_t wordOfStoredBits;
        uint64_t storedBits;
        bool isWrittenBehindDefinitionAlone;
    };

    struct FrameWords {
        uint32_t endOfReachOfFramePointer { 0 };
        uint32_t beginOfReachOfStackPointer { 0 };
        std::span<uint32_t> numberOfAccesses;
        std::span<uint32_t> numberOfAccessesToStackSlot;
        std::span<uint32_t> newWordOf;
    };

    void placeStackSlots();
    void sortSpillsByBegin();
    void releasePlacesFreeAt(Position);
    void releasePlacesOfNextSpillsFreeAt(Position);
    uint32_t takePlace(Part, Size, Position begin, Position endOfFirstPart, Position beginBehindGap, Position end);
    void joinParts();
    void findWordsDeadAcrossSuspensions();
    void bringBusyWordsNearEndsOfFrame();
    void countAccessesToFrame(FrameWords&);
    void arrangeWordsOfPart(FrameWords&, Part, uint32_t begin, uint32_t end);

    Procedure& m_proc;
    Arena& m_arena;
    const Allocation& m_allocation;
    SlotAssignment& m_slots;
    const uint64_t* m_spillsByBegin { nullptr };
    uint32_t m_numberOfSpills { 0 };
    const SpillPlaceRange* m_rangesOfSpillPlaces { nullptr };
    uint64_t* m_spillsByEndOfHold { nullptr };
    uint32_t m_nextSpillToLetGo { 0 };
    Position m_endOfNextHold { noPosition };
    uint32_t* m_placesOfSpills { nullptr };
    ArenaVector<Holder> m_holders;
    ArenaVector<uint32_t> m_freePlaces[numberOfParts][numberOfSizes];
    struct PositionsTakenBehindGaps {
        Position begin { noPosition };
        Position end { 0 };
    };
    ArenaVector<PositionsTakenBehindGaps> m_positionsTakenBehindGaps[numberOfParts];
    const ArenaVector<uint32_t>* m_placesSearchedLast { nullptr };
    Position m_beginOfLastSearch { noPosition };
    uint32_t m_endOfNextSearch { 0 };
    uint32_t m_bytesOfPart[numberOfParts] { 0, 0 };
    bool m_holdsPairs { false };
    ArenaVector<Part> m_partOfSpill;
    ArenaVector<Part> m_partOfStackSlot;
};

SlotAssignment* assignSlots(Procedure&);

Position positionOfStoreToSpillPlace(const Procedure&, const SpillFacts&);
// So where some path from an entrypoint leads without that store, nothing reads the place before the store has run.
bool spillPlaceIsWrittenBehindDefinitionAlone(const Procedure&, const SpillFacts&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
