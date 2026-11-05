/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
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

class SectionWriter {
    WTF_MAKE_NONCOPYABLE(SectionWriter);
public:
    explicit SectionWriter(BuildContext&);

    uint32_t append(std::span<const uint8_t>, size_t alignment);
    Vector<uint32_t> appendAll(std::span<const Vector<uint8_t>> streams);
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
