/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct InitialWord {
    uint32_t index;
    uint64_t bits;
};

} // anonymous namespace

static constexpr unsigned wordSize = sizeof(uint64_t);
static constexpr unsigned bitsPerByte = 8;

static void encodeStaticsLayout(TableEncoder& encoder)
{
    const StaticsPlan& statics = encoder.placement.statics();
    Vector<StaticsLayout, 1> layout { StaticsLayout {
        .sizeFilledAtProgramEntry = statics.sizeFilledAtProgramEntry,
        .distanceFromArrayProfileToLastArray = statics.distanceFromArrayProfileToLastArray,
    } };
    encoder.setTable(ProgramTableIndex::staticsLayout, layout);
}

// The run time fills whole words. Slots narrower than a word share one, and each gives the bits at its own place in it.
static Vector<InitialWord> collectInitialWords(const TableEncoder& encoder)
{
    Vector<InitialWord> parts;
    for (const PlacedVariable& placed : encoder.placement.statics().variables) {
        if (!placed.hasWordInStatics() || !placed.initialWord)
            continue;
        unsigned width = widthOf(placed.representation);
        RELEASE_ASSERT(!(placed.offset % width));
        RELEASE_ASSERT(width == wordSize || !(placed.initialWord >> (bitsPerByte * width)));
        parts.append({ placed.offset / wordSize, placed.initialWord << (bitsPerByte * (placed.offset % wordSize)) });
    }
    AOT::sort(parts, { }, &InitialWord::index);

    Vector<InitialWord> initialWords;
    for (const InitialWord& part : parts) {
        if (!initialWords.isEmpty() && initialWords.last().index == part.index)
            initialWords.last().bits |= part.bits;
        else
            initialWords.append(part);
    }
    return initialWords;
}

static void encodeStaticsRuns(TableEncoder& encoder)
{
    Vector<InitialWord> initialWords = collectInitialWords(encoder);
    // As ProgramStatics divides them: a word that lies on both sides is filled with the later ones, when the instance is created.
    uint32_t firstWordFilledAtInstanceCreation = encoder.placement.statics().sizeFilledAtProgramEntry / wordSize;
    Vector<StaticsRun> runs;
    for (unsigned first = 0; first < initialWords.size();) {
        unsigned end = first + 1;
        while (end < initialWords.size() && initialWords[end].bits == initialWords[first].bits && initialWords[end].index == initialWords[end - 1].index + 1 && initialWords[end].index != firstWordFilledAtInstanceCreation)
            ++end;
        uint64_t bits = initialWords[first].bits;
        runs.append({ initialWords[first].index, end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
        first = end;
    }
    encoder.setTable(ProgramTableIndex::staticsRuns, runs);
}

static void encodeFunctionVariables(TableEncoder& encoder)
{
    Vector<FunctionVariableData> functionVariables;
    for (const PlacedVariable& placed : encoder.placement.statics().variables) {
        if (!placed.hasWordInStatics() || placed.encoding != VariableEncoding::FunctionAsZero)
            continue;
        functionVariables.append({ placed.offset, encoder.entrypointOf(placed.function) });
    }
    AOT::sort(functionVariables, { }, &FunctionVariableData::offset);
    encoder.setTable(ProgramTableIndex::functionVariables, functionVariables);
}

static Vector<StaticsRange> joinAdjacentRoots(std::span<const uint32_t> ascendingOffsets, uint32_t width)
{
    Vector<StaticsRange> ranges;
    for (uint32_t offset : ascendingOffsets) {
        RELEASE_ASSERT(!(offset % width));
        if (!ranges.isEmpty() && ranges.last().end() == offset)
            ranges.last().size += width;
        else
            ranges.append({ offset, width });
    }
    return ranges;
}

// Only a variable holds a narrow reference, and its form says so. Every other root is a whole word.
static Vector<uint32_t> collectNarrowRoots(const StaticsPlan& statics)
{
    Vector<uint32_t> offsets;
    for (const PlacedVariable& placed : statics.variables) {
        if (placed.hasWordInStatics() && traitsOf(placed.representation).isNarrowReference)
            offsets.append(placed.offset);
    }
    AOT::sort(offsets);
    return offsets;
}

static void encodeStaticsRoots(TableEncoder& encoder)
{
    const StaticsPlan& statics = encoder.placement.statics();
    encoder.setTable(ProgramTableIndex::staticsRoots, joinAdjacentRoots(statics.roots.span(), wordSize));
    encoder.setTable(ProgramTableIndex::staticsNarrowRoots, joinAdjacentRoots(collectNarrowRoots(statics).span(), sizeof(uint32_t)));
}

void encodeStaticsTables(TableEncoder& encoder)
{
    const StaticsPlan& statics = encoder.placement.statics();
    encodeStaticsLayout(encoder);
    encodeStaticsRuns(encoder);
    encodeStaticsRoots(encoder);
    encoder.setTable(ProgramTableIndex::staticsClearedEachCollection, statics.clearedEachCollection);
    encodeFunctionVariables(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
