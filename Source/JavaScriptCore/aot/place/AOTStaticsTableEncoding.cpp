/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct InitialWord {
    uint32_t offset;
    uint64_t bits;
};

} // anonymous namespace

static void encodeStaticsLayout(TableEncoder& encoder)
{
    const StaticsPlan& statics = encoder.placement.statics();
    Vector<StaticsLayout, 1> layout { StaticsLayout {
        .sizeFilledAtProgramEntry = statics.sizeFilledAtProgramEntry,
        .distanceFromArrayProfileToLastArray = statics.distanceFromArrayProfileToLastArray,
    } };
    encoder.setTable(ProgramTableIndex::staticsLayout, layout);
}

static Vector<InitialWord> collectInitialWords(const TableEncoder& encoder)
{
    Vector<InitialWord> initialWords;
    for (const PlacedVariable& placed : encoder.placement.statics().variables) {
        if (!placed.hasWordInStatics() || !placed.initialWord)
            continue;
        RELEASE_ASSERT(!(placed.offset % sizeof(uint64_t)));
        initialWords.append({ placed.offset, placed.initialWord });
    }
    std::ranges::sort(initialWords, { }, &InitialWord::offset);
    return initialWords;
}

static void encodeStaticsRuns(TableEncoder& encoder)
{
    Vector<InitialWord> initialWords = collectInitialWords(encoder);
    uint32_t sizeFilledAtProgramEntry = encoder.placement.statics().sizeFilledAtProgramEntry;
    Vector<StaticsRun> runs;
    for (unsigned first = 0; first < initialWords.size();) {
        unsigned end = first + 1;
        while (end < initialWords.size() && initialWords[end].bits == initialWords[first].bits && initialWords[end].offset == initialWords[end - 1].offset + sizeof(uint64_t) && initialWords[end].offset != sizeFilledAtProgramEntry)
            ++end;
        uint64_t bits = initialWords[first].bits;
        runs.append({ static_cast<uint32_t>(initialWords[first].offset / sizeof(uint64_t)), end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
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
    std::ranges::sort(functionVariables, { }, &FunctionVariableData::offset);
    encoder.setTable(ProgramTableIndex::functionVariables, functionVariables);
}

static void encodeStaticsRoots(TableEncoder& encoder)
{
    Vector<StaticsRange> ranges;
    for (uint32_t offset : encoder.placement.statics().roots) {
        if (!ranges.isEmpty() && ranges.last().end() == offset)
            ranges.last().size += sizeof(uint64_t);
        else
            ranges.append({ offset, sizeof(uint64_t) });
    }
    encoder.setTable(ProgramTableIndex::staticsRoots, ranges);
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
