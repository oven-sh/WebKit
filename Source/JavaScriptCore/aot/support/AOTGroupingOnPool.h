/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <array>
#include <tuple>

namespace JSC { namespace AOT {

// What AOTGrouping.h does on one thread, on all of them. Whatever comes out is the same at any number of threads.

// The rows come ascending by group. Where the rows of each group begin, and one more for the end. A group may have no rows.
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

// Each index brings some items, and the items of all indices stand in one row from `firstPlace` on.
// Where the items of each range of forEachRangeOfIndicesInParallel() begin, and one more for the end.
Vector<size_t> firstItemsOfRanges(const ParallelLoops& loops, size_t count, size_t firstPlace, const Invocable<size_t(size_t)> auto& numberOfItemsAt)
{
    Vector<size_t> firstItem;
    firstItem.grow(numberOfRangesOfIndices(count) + 1);
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        size_t numberOfItems = 0;
        for (size_t index = first; index < end; ++index)
            numberOfItems += numberOfItemsAt(index);
        firstItem[first / indicesInRangeOfLoop] = numberOfItems;
    });
    firstItem.last() = 0;
    for (size_t& first : firstItem)
        firstPlace += std::exchange(first, firstPlace);
    return firstItem;
}

// A row has its group in the high half. The rows of a group stay in the order in which they came. A counting sort by eleven bits at a time, lowest first:
// each piece of the rows counts for itself, so nobody shares a counter and no group is one thread's.
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

    // By the count alone.
    const size_t numberOfPieces = std::min(mostPieces, count / fewestRowsOfPiece);
    auto pieceOf = [&](std::span<const uint64_t> all, size_t piece) {
        size_t first = count * piece / numberOfPieces;
        return all.subspan(first, count * (piece + 1) / numberOfPieces - first);
    };
    Vector<uint64_t> scratch;
    scratch.grow(count);
    // By digit, then by piece: the order in which the rows come to stand.
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

// A list of words that lies with whoever made it, who knows it by `source` and `number`. Equal lists have the same hash.
struct NamedList {
    uint32_t hash;
    uint32_t size;
    uint32_t source;
    uint32_t number;
};

// Puts equal lists side by side, those by source and number, and says where the first of each stands, ascending. No list is looked at more than a few times, however many are equal to it.
Vector<uint32_t> bringEqualListsTogether(const ParallelLoops& loops, std::span<NamedList> lists, const Invocable<std::span<const uint32_t>(const NamedList&)> auto& wordsOf)
{
    RELEASE_ASSERT(lists.size() <= UINT32_MAX);
    unsigned count = static_cast<unsigned>(lists.size());
    parallelSort(loops, lists, [](const NamedList& first, const NamedList& second) {
        return std::tie(first.hash, first.size, first.source, first.number) < std::tie(second.hash, second.size, second.source, second.number);
    });
    Vector<uint32_t> firstOfHash = findIndicesInParallel(loops, count, [&](unsigned index) {
        return !index || lists[index].hash != lists[index - 1].hash || lists[index].size != lists[index - 1].size;
    });

    Vector<uint8_t> isFirstOfEqualLists;
    isFirstOfEqualLists.grow(count);
    forEachIndexInParallel(loops, static_cast<unsigned>(firstOfHash.size()), [&](unsigned run) {
        size_t first = firstOfHash[run];
        size_t end = run + 1 < firstOfHash.size() ? firstOfHash[run + 1] : count;
        std::span<NamedList> ofOneHash = lists.subspan(first, end - first);
        auto differsFromListInFront = [&](size_t position) {
            return !std::ranges::equal(wordsOf(ofOneHash[position]), wordsOf(ofOneHash[position - 1]));
        };
        bool someDiffer = false;
        for (size_t position = 1; position < ofOneHash.size() && !someDiffer; ++position)
            someDiffer = differsFromListInFront(position);
        if (someDiffer) {
            AOT::sortInLine(ofOneHash, [&](const NamedList& one, const NamedList& other) {
                return std::ranges::lexicographical_compare(wordsOf(one), wordsOf(other));
            });
        }
        isFirstOfEqualLists[first] = true;
        for (size_t position = 1; position < ofOneHash.size(); ++position)
            isFirstOfEqualLists[first + position] = someDiffer && differsFromListInFront(position);
    });
    return findIndicesInParallel(loops, count, [&](unsigned index) {
        return !!isFirstOfEqualLists[index];
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
