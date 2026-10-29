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
        OptionalInt64UnlessMarker,
        OptionalDoubleUnlessMarker,
        OptionalBooleanUnlessMarker,
        JSValueUnlessMarker,
        Word,
        HalfWord,
        QuarterWord,
        Byte,
        BoxedOptionalInt32,
        BoxedInt32,
        Int32AsOptionalInt32,
        OptionalInt32AsDouble,
        Int32AsDouble,
        OptionalInt32AsOptionalInt64,
        Int32AsInt64,
        BoxedOptionalInt64,
        OptionalInt64AsOptionalInt32,
        OptionalInt64AsDouble,
        WideNarrowReference,
        NarrowWideReference,
        NarrowLiteralString,
        WideLiteralString,
        // Out of a property of an ordinary object. Each takes the other way for a value that its slot does not take as it is, and for undefined and null.
        BoxedAsInt32,
        BoxedAsInt64,
        BoxedAsDouble,
        BoxedAsNarrowString,
        BoxedAsJSValue,
        OtherForm,
        Tested,
    };
    // The moves that the stub in front of the operation makes: in a run all are made by the same few instructions.
    enum class RunOfStub : uint8_t { HalfWords, NarrowedReferences, Words, Bytes, Int32sAsDoubles, BoxedInt32s, WidenedReferences };
    static constexpr unsigned numberOfRunsOfStub = 7;
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
        // The target lists the field before the copy. Whoever makes the copy in another way, once a later move has failed, stores the same value there again.
        bool overwritesField { false };
        // The slot of the target does not take both. No kind moves them but by the other way.
        bool testsUndefinedAndNull { false };
        RunOfStub runOfStub { RunOfStub::HalfWords };
        // The stub leaves the copy to the operation where the bits that it has read, less the first, are below the number.
        uint64_t firstBitsRefusedByStub { 0 };
        uint64_t numberOfBitsRefusedByStub { 0 };
        // The int32s that a move out of an ordinary object takes.
        int32_t leastInt32 { 0 };
        int32_t greatestInt32 { -1 };
    };
    struct Run {
        const Move* end { nullptr };
        intptr_t negativeSize { 0 };
    };
    enum class State : uint8_t { IsUnknown, IsServed, IsNotServed };

    MappedCopy() = default;
    MappedCopy(const MappedCopy&) = default;
    MappedCopy& operator=(const MappedCopy&) = default;
    ~MappedCopy()
    {
        if (placeOfOwnedCopy && *placeOfOwnedCopy == this)
            *placeOfOwnedCopy = nullptr;
    }

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
    // It takes loads and stores alone and at the most a new Extension out of a free list.
    bool takesNoFrame { false };
    // Whether it does is asked when the copy is made for the second time: many a kind of copy is made once.
    bool mayTakeNoFrame { false };
    bool extensionWordOfTargetIsWide { false };
    bool extensionWordOfSourceIsWide { false };
    // The source is an ordinary object with all its properties in its cell. granuleBitsOfSource is not zero then, so that only a copy that looks at it asks.
    bool readsOrdinaryObject { false };
    StructureID structureOfTarget;
    StructureID structureOfSource;
    // That of no cell where the stub leaves the moves to the operation. Else the moves lie in the order of the runs, so the operation begins a copy again that the stub has left.
    StructureID structureOfSourceForStub;
    // Most copies have moves of the first two runs alone, and few have any behind the first four.
    bool hasRunsBehindSecondForStub { false };
    bool hasRunsBehindFourthForStub { false };
    std::array<Run, numberOfRunsOfStub> runsOfStub { };
    uint32_t offsetOfExtensionWordInTarget { 0 };
    uint32_t offsetOfExtensionWordInSource { 0 };
    uint32_t sizeOfNewExtension { 0 };
    LocalAllocator* allocatorOfNewExtension { nullptr };
    // Of a copy that a plan owns: where the instance keeps it for the stub. The plan may go at any time.
    const MappedCopy** placeOfOwnedCopy { nullptr };
    // Copies that took a frame though this one was kept at its place, since it was last put there.
    uint8_t numberOfCopiesInVain { 0 };
    Vector<Move> moves;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
