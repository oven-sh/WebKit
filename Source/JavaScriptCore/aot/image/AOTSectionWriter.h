/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<unsigned bitsInKey, typename T>
void radixSortKeepingEqualKeysInOrder(Vector<T>& list, const Invocable<uint64_t(const T&)> auto& keyOf)
{
    static_assert(std::is_trivially_copyable_v<T> && bitsInKey && bitsInKey <= 64);
    static constexpr size_t fewestSortedByDigits = 1 << 10;
    static constexpr unsigned bitsInDigit = 11;
    static constexpr size_t valuesOfDigit = 1 << bitsInDigit;
    static constexpr unsigned numberOfDigits = (bitsInKey + bitsInDigit - 1) / bitsInDigit;

    size_t count = list.size();
    if (count < fewestSortedByDigits) {
        AOT::stableSortInLine(list, { }, keyOf);
        return;
    }
    if (std::ranges::is_sorted(list, { }, keyOf))
        return;

    auto digitOf = [&](const T& element, unsigned digit) {
        return static_cast<size_t>(static_cast<uint64_t>(keyOf(element)) >> (digit * bitsInDigit)) & (valuesOfDigit - 1);
    };
    Vector<uint32_t> placesOfDigits(FillWith { }, numberOfDigits * valuesOfDigit, 0);
    uint32_t* places = placesOfDigits.mutableSpan().data();
    T* from = list.mutableSpan().data();
    for (size_t index = 0; index < count; ++index) {
        uint64_t key = keyOf(from[index]);
        for (unsigned digit = 0; digit < numberOfDigits; ++digit)
            ++places[digit * valuesOfDigit + (static_cast<size_t>(key >> (digit * bitsInDigit)) & (valuesOfDigit - 1))];
    }

    Vector<T> other;
    other.grow(count);
    T* to = other.mutableSpan().data();
    for (unsigned digit = 0; digit < numberOfDigits; ++digit) {
        uint32_t* placeOf = places + digit * valuesOfDigit;
        if (placeOf[digitOf(from[0], digit)] == count)
            continue;
        uint32_t place = 0;
        for (size_t value = 0; value < valuesOfDigit; ++value) {
            uint32_t numberWithValue = placeOf[value];
            placeOf[value] = place;
            place += numberWithValue;
        }
        for (size_t index = 0; index < count; ++index)
            to[placeOf[digitOf(from[index], digit)]++] = from[index];
        std::swap(from, to);
    }
    if (from != list.span().data())
        list.swap(other);
}

template<typename T>
void sortByWordOnPoolKeepingEqualElementsInOrder(const ParallelLoops& loops, std::span<T> list, const Invocable<uint64_t(const T&)> auto& wordOf)
{
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= alignof(std::max_align_t));
    static constexpr size_t fewestSortedByDigits = 1 << 14;
    static constexpr unsigned bitsInDigit = 8;
    static constexpr size_t valuesOfDigit = 1 << bitsInDigit;

    size_t count = list.size();
    if (count < fewestSortedByDigits) {
        AOT::stableSortInLine(list, { }, wordOf);
        return;
    }

    size_t numberOfRanges = numberOfRangesOfIndices(count);
    uint64_t wordOfFirst = wordOf(list[0]);
    Vector<uint64_t> bitsThatDifferInRanges(FillWith { }, numberOfRanges, 0);
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        uint64_t bitsThatDifferInRange = 0;
        for (const T& element : list.subspan(first, end - first))
            bitsThatDifferInRange |= wordOf(element) ^ wordOfFirst;
        bitsThatDifferInRanges[first / indicesInRangeOfLoop] = bitsThatDifferInRange;
    });
    uint64_t bitsThatDiffer = 0;
    for (uint64_t bitsThatDifferInRange : bitsThatDifferInRanges)
        bitsThatDiffer |= bitsThatDifferInRange;

    Vector<uint8_t> scratch;
    scratch.grow(count * sizeof(T));
    Vector<uint32_t> places;
    places.grow(valuesOfDigit * numberOfRanges);
    std::span<T> from = list;
    std::span<T> to(reinterpret_cast<T*>(scratch.mutableSpan().data()), count);
    while (bitsThatDiffer) {
        unsigned shift = std::countr_zero(bitsThatDiffer);
        bitsThatDiffer &= ~(static_cast<uint64_t>(valuesOfDigit - 1) << shift);
        auto digitOf = [&](const T& element) {
            return static_cast<size_t>(wordOf(element) >> shift) & (valuesOfDigit - 1);
        };
        forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
            std::array<uint32_t, valuesOfDigit> numbersInRange { };
            for (const T& element : from.subspan(first, end - first))
                ++numbersInRange[digitOf(element)];
            uint32_t* placeOfRange = places.mutableSpan().data() + first / indicesInRangeOfLoop;
            for (uint32_t number : numbersInRange) {
                *placeOfRange = number;
                placeOfRange += numberOfRanges;
            }
        });
        uint32_t next = 0;
        for (uint32_t& place : places)
            next += std::exchange(place, next);
        forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
            std::array<uint32_t, valuesOfDigit> placesOfRange;
            const uint32_t* placeOfRange = places.span().data() + first / indicesInRangeOfLoop;
            for (uint32_t& place : placesOfRange) {
                place = *placeOfRange;
                placeOfRange += numberOfRanges;
            }
            T* elements = to.data();
            for (const T& element : from.subspan(first, end - first))
                elements[placesOfRange[digitOf(element)]++] = element;
        });
        std::swap(from, to);
    }
    if (from.data() == list.data())
        return;
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        std::ranges::copy(from.subspan(first, end - first), list.subspan(first).begin());
    });
}

template<typename T>
void sortRunsOfEqualWordsOnPool(const ParallelLoops& loops, std::span<T> list, const Invocable<uint64_t(const T&)> auto& wordOf, const Invocable<bool(const T&, const T&)> auto& comesFirstInRun)
{
    size_t count = list.size();
    Vector<uint8_t> beginsRun;
    beginsRun.grow(count);
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place)
            beginsRun[place] = !place || wordOf(list[place]) != wordOf(list[place - 1]);
    });
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        size_t place = first;
        while (place < end && !beginsRun[place])
            ++place;
        while (place < end) {
            size_t endOfRun = place + 1;
            while (endOfRun < count && !beginsRun[endOfRun])
                ++endOfRun;
            if (endOfRun - place > 1)
                AOT::sortInLine(list.subspan(place, endOfRun - place), comesFirstInRun);
            place = endOfRun;
        }
    });
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename T>
Vector<uint8_t> bytesOf(const Vector<T>& values)
{
    static_assert(std::has_unique_object_representations_v<T>, "padding would make the bytes of the image differ from build to build");
    return Vector<uint8_t>(asBytes(values.span()));
}

[[nodiscard]] bool checkSectionSize(uint64_t size, BuildContext&);
[[nodiscard]] bool checkListSize(uint64_t count, size_t sizeOfElement, BuildContext&);
void growSectionTo(Vector<uint8_t>& bytes, size_t newSize);

Vector<uint32_t> findFirstEqualStreams(std::span<const Vector<uint16_t>> streams, const ParallelLoops&);

class SectionWriter {
    WTF_MAKE_NONCOPYABLE(SectionWriter);
public:
    explicit SectionWriter(BuildContext&);

    uint32_t append(std::span<const uint8_t>, size_t alignment);
    size_t size() const { return m_bytes.size(); }
    bool didOverflow() const { return m_didOverflow; }
    Vector<uint8_t> takeBytes();

private:
    bool canGrowTo(size_t newSize);

    BuildContext& m_context;
    Vector<uint8_t> m_bytes;
    bool m_didOverflow { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
