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
#include <memory>
#include <wtf/FastMalloc.h>

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
        BoxedAsInt32,
        BoxedAsInt64,
        BoxedAsDouble,
        BoxedAsNarrowString,
        BoxedAsJSValue,
        OtherForm,
        Tested,
    };
    enum class RunOfStub : uint8_t { HalfWords, NarrowedReferences, Words, Bytes, Int32sAsDoubles, BoxedInt32s, WidenedReferences };
    static constexpr unsigned numberOfRunsOfStub = 7;
    enum class Place : uint8_t { Cell, InlineProperties, OutsideProperties };
    static constexpr unsigned numberOfPlaces = 4;
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
        bool overwritesField { false };
        bool testsUndefinedAndNull { false };
        RunOfStub runOfStub { RunOfStub::HalfWords };
        Place placeInSource { Place::Cell };
        Place placeInTarget { Place::Cell };
        uint64_t firstBitsRefusedByStub { 0 };
        uint64_t numberOfBitsRefusedByStub { 0 };
        int32_t leastInt32 { 0 };
        int32_t greatestInt32 { -1 };
    };
    static_assert(sizeof(Move) == 48);
    struct WordMove {
        int32_t from;
        int32_t to;
        Place placeInSource;
        Place placeInTarget;
    };
    struct Run {
        const Move* end { nullptr };
        intptr_t negativeSize { 0 };
    };
    enum class State : uint8_t { IsUnknown, IsServed, IsNotServed };

    MappedCopy() = default;
    MappedCopy(MappedCopy&&) = default;
    MappedCopy& operator=(MappedCopy&&) = default;
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
    bool takesNoFrame { false };
    bool mayTakeNoFrame { false };
    bool extensionWordOfTargetIsWide { false };
    bool extensionWordOfSourceIsWide { false };
    bool readsOrdinaryObject { false };
    StructureID structureOfTarget;
    StructureID structureOfSource;
    StructureID structureOfSourceForStub;
    bool hasRunsBehindSecondForStub { false };
    bool hasRunsBehindFourthForStub { false };
    std::array<Run, numberOfRunsOfStub> runsOfStub { };
    uint32_t offsetOfExtensionWordInTarget { 0 };
    uint32_t offsetOfExtensionWordInSource { 0 };
    uint32_t sizeOfNewExtension { 0 };
    LocalAllocator* allocatorOfNewExtension { nullptr };
    const MappedCopy** placeOfOwnedCopy { nullptr };
    mutable uint8_t numberOfCopiesInVain { 0 };
    uint8_t numberOfTurnsLost { 0 };
    Vector<Move> moves;

    struct FollowedPlan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FollowedPlan);

        bool readsSecondObjectOfSource { false };
        bool secondObjectWordOfSourceIsWide { false };
        bool secondObjectWordOfTargetIsWide { false };
        uint64_t targetWords { 0 };
        uint32_t offsetOfSecondObjectWordInSource { 0 };
        uint32_t offsetOfSecondObjectWordInTarget { 0 };
        StructureID structureOfSecondObjectOfSource;
        StructureID structureOfTargetAfter;
        uint64_t headerOfNewSecondObject { 0 };
        Vector<WordMove> wordMoves;
        LocalAllocator* allocatorOfNewSecondObject { nullptr };
        uint32_t sizeOfNewSecondObject { 0 };
        LocalAllocator* allocatorOfNewStorage { nullptr };
        uint32_t sizeOfNewStorage { 0 };
        uint32_t bytesOfNewStorage { 0 };
        uint32_t bytesOfNewStorageLeftEmpty { 0 };
    };
    std::unique_ptr<FollowedPlan> followedPlan;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
