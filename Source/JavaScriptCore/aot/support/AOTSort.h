/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <utility>
#include <wtf/Compiler.h>
#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr std::ptrdiff_t largestRangeSortedByInsertion = 32;

inline constexpr std::ptrdiff_t longestRunThatMergingStartsFrom = 16;

inline constexpr std::ptrdiff_t minRadixSortLength = 2048;

template<typename Iterator, typename IsLess, typename Projection>
void insertEachWhereItBelongs(Iterator first, Iterator next, Iterator last, IsLess& isLess, Projection& projection)
{
    for (; next != last; ++next) {
        if (!std::invoke(isLess, std::invoke(projection, *next), std::invoke(projection, *std::ranges::prev(next))))
            continue;
        std::iter_value_t<Iterator> element = std::ranges::iter_move(next);
        auto place = next;
        do {
            *place = std::ranges::iter_move(std::ranges::prev(place));
            --place;
        } while (place != first && std::invoke(isLess, std::invoke(projection, element), std::invoke(projection, *std::ranges::prev(place))));
        *place = std::move(element);
    }
}

template<typename Element, typename IsLess, typename Projection>
void mergeNeighborsKeepingEqualElementsInOrder(Element* first, Element* middle, Element* last, Element* room, IsLess& isLess, Projection& projection)
{
    if (!std::invoke(isLess, std::invoke(projection, *middle), std::invoke(projection, middle[-1])))
        return;
    Element* endOfRoom = std::uninitialized_move(first, middle, room);
    Element* left = room;
    Element* right = middle;
    Element* place = first;
    while (left != endOfRoom && right != last) {
        if (std::invoke(isLess, std::invoke(projection, *right), std::invoke(projection, *left)))
            *place++ = std::move(*right++);
        else
            *place++ = std::move(*left++);
    }
    std::move(left, endOfRoom, place);
    std::destroy(room, endOfRoom);
}

template<typename Element, typename IsLess, typename Projection>
NEVER_INLINE void mergeSortKeepingEqualElementsInOrder(Element* first, Element* last, IsLess& isLess, Projection& projection)
{
    static_assert(alignof(Element) <= 16);
    std::ptrdiff_t size = last - first;
    std::ptrdiff_t lengthOfFirstRuns = size;
    while (lengthOfFirstRuns > longestRunThatMergingStartsFrom)
        lengthOfFirstRuns = (lengthOfFirstRuns + 1) / 2;
    for (std::ptrdiff_t low = 0; low < size; low += lengthOfFirstRuns) {
        Element* run = first + low;
        insertEachWhereItBelongs(run, run + 1, first + std::min(low + lengthOfFirstRuns, size), isLess, projection);
    }
    std::ptrdiff_t longestLeftRun = lengthOfFirstRuns;
    while (longestLeftRun * 2 < size)
        longestLeftRun *= 2;
    Element* room = static_cast<Element*>(fastMalloc(static_cast<size_t>(longestLeftRun) * sizeof(Element)));
    for (std::ptrdiff_t length = lengthOfFirstRuns; length < size; length *= 2) {
        for (std::ptrdiff_t low = 0; low + length < size; low += 2 * length)
            mergeNeighborsKeepingEqualElementsInOrder(first + low, first + low + length, first + std::min(low + 2 * length, size), room, isLess, projection);
    }
    fastFree(room);
}

template<std::unsigned_integral Integer>
NEVER_INLINE void radixSortIntegers(Integer* first, Integer* last)
{
    constexpr unsigned bitsInDigit = 11;
    constexpr size_t valuesOfDigit = 1 << bitsInDigit;
    size_t size = last - first;
    Integer bitsThatDiffer = 0;
    for (size_t index = 1; index < size; ++index)
        bitsThatDiffer |= first[index] ^ *first;
    Integer* room = static_cast<Integer*>(fastMalloc(size * sizeof(Integer)));
    Integer* from = first;
    Integer* to = room;
    while (bitsThatDiffer) {
        unsigned shift = std::countr_zero(bitsThatDiffer);
        bitsThatDiffer &= ~static_cast<Integer>(static_cast<Integer>(valuesOfDigit - 1) << shift);
        std::array<size_t, valuesOfDigit> places { };
        for (size_t index = 0; index < size; ++index)
            ++places[from[index] >> shift & (valuesOfDigit - 1)];
        size_t next = 0;
        for (size_t& place : places)
            next += std::exchange(place, next);
        for (size_t index = 0; index < size; ++index)
            to[places[from[index] >> shift & (valuesOfDigit - 1)]++] = from[index];
        std::swap(from, to);
    }
    if (from != first)
        memcpy(first, from, size * sizeof(Integer));
    fastFree(room);
}

template<std::ranges::random_access_range Range, typename IsLess, typename Projection>
void sortKeepingEqualElementsInOrder(Range&& range, IsLess isLess, Projection projection)
{
    auto first = std::ranges::begin(range);
    auto last = std::ranges::next(first, std::ranges::end(range));
    auto next = std::ranges::is_sorted_until(first, last, isLess, projection);
    if (next == last)
        return;
    if (last - first > largestRangeSortedByInsertion) {
        static_assert(std::contiguous_iterator<decltype(first)>);
        if constexpr (std::unsigned_integral<std::iter_value_t<decltype(first)>> && std::same_as<IsLess, std::ranges::less> && std::same_as<Projection, std::identity>) {
            if (last - first >= minRadixSortLength) {
                radixSortIntegers(std::to_address(first), std::to_address(last));
                return;
            }
        }
        mergeSortKeepingEqualElementsInOrder(std::to_address(first), std::to_address(first) + (last - first), isLess, projection);
        return;
    }
    insertEachWhereItBelongs(first, next, last, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void sort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void sortInLine(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void stableSort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void stableSortInLine(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
