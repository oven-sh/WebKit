/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <array>

namespace JSC { namespace AOT {

Vector<uint32_t> firstRowsOfGroups(const ParallelLoops& loops, uint32_t numberOfGroups, size_t numberOfRows, const Invocable<uint32_t(size_t)> auto& groupOfRow)
{
    RELEASE_ASSERT(numberOfGroups < UINT32_MAX && numberOfRows <= UINT32_MAX);
    Vector<uint32_t> firstRow;
    firstRow.grow(static_cast<size_t>(numberOfGroups) + 1);
    uint32_t firstGroupBehindRows = numberOfRows ? groupOfRow(numberOfRows - 1) + 1 : 0;
    RELEASE_ASSERT(firstGroupBehindRows <= numberOfGroups);
    forEachRangeOfIndicesInParallel(loops, numberOfRows, [&](size_t first, size_t end) {
        uint32_t nextGroup = first ? groupOfRow(first - 1) + 1 : 0;
        for (size_t row = first; row < end; ++row) {
            for (uint32_t group = groupOfRow(row); nextGroup <= group; ++nextGroup)
                firstRow[nextGroup] = static_cast<uint32_t>(row);
        }
    });
    for (uint32_t group = firstGroupBehindRows; group <= numberOfGroups; ++group)
        firstRow[group] = static_cast<uint32_t>(numberOfRows);
    return firstRow;
}

inline void sortRowsByGroup(const ParallelLoops& loops, Vector<uint64_t>& rows, uint32_t numberOfGroups)
{
    static constexpr unsigned bitsOfDigit = 11;
    static constexpr size_t numberOfDigits = static_cast<size_t>(1) << bitsOfDigit;
    static constexpr size_t fewestForPool = 1 << 13;
    static constexpr size_t fewestRowsOfPiece = 1 << 12;
    static constexpr size_t mostPieces = 256;

    size_t count = rows.size();
    RELEASE_ASSERT(count <= UINT32_MAX && (numberOfGroups || !count));
    if (count < fewestForPool) {
        AOT::sortInLine(rows, std::ranges::less { }, [](uint64_t row) {
            return static_cast<uint32_t>(row >> 32);
        });
        return;
    }

    const size_t numberOfPieces = std::min(mostPieces, count / fewestRowsOfPiece);
    auto pieceOf = [&](std::span<const uint64_t> all, size_t piece) {
        size_t first = count * piece / numberOfPieces;
        return all.subspan(first, count * (piece + 1) / numberOfPieces - first);
    };
    Vector<uint64_t> scratch;
    scratch.grow(count);
    Vector<uint32_t> firstPlace;
    firstPlace.grow(numberOfDigits * numberOfPieces);
    for (unsigned shift = 32; static_cast<uint64_t>(numberOfGroups - 1) >> (shift - 32); shift += bitsOfDigit) {
        auto digitOf = [&](uint64_t row) {
            return static_cast<size_t>(row >> shift) & (numberOfDigits - 1);
        };
        forEachIndexInParallel(loops, static_cast<unsigned>(numberOfPieces), [&](unsigned piece) {
            std::array<uint32_t, numberOfDigits> numberOfRows { };
            for (uint64_t row : pieceOf(rows.span(), piece))
                ++numberOfRows[digitOf(row)];
            for (size_t digit = 0; digit < numberOfDigits; ++digit)
                firstPlace[digit * numberOfPieces + piece] = numberOfRows[digit];
        });
        uint32_t next = 0;
        for (uint32_t& place : firstPlace)
            next += std::exchange(place, next);
        forEachIndexInParallel(loops, static_cast<unsigned>(numberOfPieces), [&](unsigned piece) {
            std::array<uint32_t, numberOfDigits> place;
            for (size_t digit = 0; digit < numberOfDigits; ++digit)
                place[digit] = firstPlace[digit * numberOfPieces + piece];
            for (uint64_t row : pieceOf(rows.span(), piece))
                scratch[place[digitOf(row)]++] = row;
        });
        std::swap(rows, scratch);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
