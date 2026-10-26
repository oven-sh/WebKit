/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "StructureID.h"
#include <array>

namespace JSC {

class LocalAllocator;

namespace AOT {

struct MappedCopy {
    enum class MoveKind : uint8_t {
        WordUnlessZero,
        HalfWordUnlessZero,
        ByteUnlessZero,
        OptionalInt32,
        OptionalInt64,
        OptionalDouble,
        Word,
        HalfWord,
        QuarterWord,
        Byte,
        BoxedOptionalInt32,
        BoxedInt32,
        Int32AsOptionalInt32,
        OptionalInt32AsDouble,
        Int32AsDouble,
        WideNarrowReference,
        NarrowWideReference,
        OtherForm,
        Tested,
    };
    // The moves that the stub in front of the operation makes: in a run all are made by the same few instructions.
    enum class RunOfStub : uint8_t { HalfWords, NarrowedReferences, Words, Bytes };
    static constexpr unsigned numberOfRunsOfStub = 4;
    // An offset counts from the cell, or from the Extension with the granules that it has in such a copy.
    struct Move {
        uint32_t from;
        uint32_t to;
        MoveKind kind;
        uint8_t widthInTarget;
        bool readsExtension;
        bool writesExtension;
        SlotRepresentation representationOfSource;
        SlotRepresentation representationOfTarget;
        uint8_t acceptedKinds;
        uint16_t fieldOfTarget;
        RunOfStub runOfStub { RunOfStub::HalfWords };
        // The stub leaves the copy to the operation where the bits that it has read, less the first, are below the number.
        uint64_t firstBitsRefusedByStub { 0 };
        uint64_t numberOfBitsRefusedByStub { 0 };
    };
    struct Run {
        const Move* end { nullptr };
        intptr_t negativeSize { 0 };
    };
    enum class State : uint8_t { IsUnknown, IsServed, IsNotServed };

    bool isFor(uint64_t target, uint64_t source, uint32_t excludedRun) const { return state != State::IsUnknown && targetWords == target && sourceWords == source && excludedRunNumberPlusOne == excludedRun; }

    uint64_t targetWords { 0 };
    uint64_t sourceWords { 0 };
    uint64_t granuleBitsOfSource { 0 };
    uint64_t granuleBitsOfTargetBefore { 0 };
    uint64_t granuleBitsFilled { 0 };
    uint32_t excludedRunNumberPlusOne { 0 };
    uint32_t orderWordAfter { 0 };
    State state { State::IsUnknown };
    bool mayStoreCells { false };
    // The slots that the copy writes hold nothing yet, and it takes loads and stores alone and at the most a new Extension out of a free list.
    bool takesNoFrame { false };
    // Whether it does is asked when the copy is made for the second time: many a kind of copy is made once.
    bool mayTakeNoFrame { false };
    bool extensionWordOfTargetIsWide { false };
    bool extensionWordOfSourceIsWide { false };
    StructureID structureOfTarget;
    StructureID structureOfSource;
    // That of no cell where the stub leaves the moves to the operation. Else the moves lie in the order of the runs, so the operation begins a copy again that the stub has left.
    StructureID structureOfSourceForStub;
    bool hasWordsOrBytesForStub { false };
    std::array<Run, numberOfRunsOfStub> runsOfStub { };
    uint32_t offsetOfExtensionWordInTarget { 0 };
    uint32_t offsetOfExtensionWordInSource { 0 };
    uint32_t sizeOfNewExtension { 0 };
    LocalAllocator* allocatorOfNewExtension { nullptr };
    Vector<Move> moves;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
