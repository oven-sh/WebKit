/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <ranges>
#include <type_traits>
#include <wtf/Compiler.h>
#include <wtf/StdLibExtras.h>
#include <wtf/VectorTraits.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace Typed {
template<typename> class ID;
}
template<typename, typename> class BuiltinTableID;

// std::sort and std::stable_sort are 3 to 5 KB of code for every pair of an element type and an order, because the whole algorithm
// is compiled around each order. Here only the loops that compare are compiled for an order: an insertion sort, a partition, a
// merge. What decides which of them runs on which range is compiled once.
// sort() asks the questions and makes the moves of libstdc++'s std::sort, so equal elements end up where that leaves them, whichever
// standard library this is compiled with.

struct SortSteps {
    void (*insertionSort)(void* order, void* first, void* endOfGuarded, void* last);
    void* (*partition)(void* order, void* first, void* last);
    void (*merge)(void* order, void* first, void* endOfFirst, void* second, void* endOfSecond, void* output);
    bool (*isLess)(void* order, void* first, void* second);
    void* order;
    size_t elementSize;
};

inline constexpr size_t longestInsertionSort = 16;
inline constexpr size_t stableSortChunkSize = 7;

using SortedIndices = Vector<uint32_t, 32>;

inline NEVER_INLINE SortedIndices indicesInOrder(size_t size)
{
    return SortedIndices(size, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
}

inline NEVER_INLINE void heapSort(const SortSteps& steps, char* first, char* last)
{
    size_t elementSize = steps.elementSize;
    ptrdiff_t size = (last - first) / elementSize;
    SortedIndices heap = indicesInOrder(size);
    auto isLess = [&](uint32_t a, uint32_t b) {
        return steps.isLess(steps.order, first + a * elementSize, first + b * elementSize);
    };
    auto adjust = [&](ptrdiff_t hole, ptrdiff_t length, uint32_t value) {
        ptrdiff_t top = hole;
        ptrdiff_t child = hole;
        while (child < (length - 1) / 2) {
            child = 2 * (child + 1);
            if (isLess(heap[child], heap[child - 1]))
                --child;
            heap[hole] = heap[child];
            hole = child;
        }
        if (!(length & 1) && child == (length - 2) / 2) {
            child = 2 * (child + 1);
            heap[hole] = heap[child - 1];
            hole = child - 1;
        }
        for (ptrdiff_t parent = (hole - 1) / 2; hole > top && isLess(heap[parent], value); parent = (hole - 1) / 2) {
            heap[hole] = heap[parent];
            hole = parent;
        }
        heap[hole] = value;
    };
    for (ptrdiff_t parent = (size - 2) / 2; parent >= 0; --parent)
        adjust(parent, size, heap[parent]);
    for (ptrdiff_t end = size - 1; end > 0; --end) {
        uint32_t value = std::exchange(heap[end], heap[0]);
        adjust(0, end, value);
    }
    Vector<char> sorted(size * elementSize);
    for (ptrdiff_t index = 0; index < size; ++index)
        memcpy(sorted.mutableSpan().data() + index * elementSize, first + heap[index] * elementSize, elementSize);
    memcpy(first, sorted.span().data(), sorted.size());
}

inline NEVER_INLINE void partitionUntilShort(const SortSteps& steps, char* first, char* last, unsigned depthLimit)
{
    while (static_cast<size_t>(last - first) > longestInsertionSort * steps.elementSize) {
        if (!depthLimit) {
            heapSort(steps, first, last);
            return;
        }
        --depthLimit;
        char* cut = static_cast<char*>(steps.partition(steps.order, first, last));
        partitionUntilShort(steps, cut, last, depthLimit);
        last = cut;
    }
}

inline NEVER_INLINE void sortLongRange(const SortSteps& steps, char* first, size_t size)
{
    char* last = first + size * steps.elementSize;
    partitionUntilShort(steps, first, last, 2 * (std::bit_width(size) - 1));
    steps.insertionSort(steps.order, first, first + longestInsertionSort * steps.elementSize, last);
}

inline NEVER_INLINE void stableSortLongRange(const SortSteps& steps, char* first, size_t size)
{
    size_t elementSize = steps.elementSize;
    for (size_t begin = 0; begin < size; begin += stableSortChunkSize) {
        char* end = first + std::min(begin + stableSortChunkSize, size) * elementSize;
        steps.insertionSort(steps.order, first + begin * elementSize, end, end);
    }
    Vector<uint64_t, 128> buffer((size * elementSize + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    char* from = first;
    char* to = reinterpret_cast<char*>(buffer.mutableSpan().data());
    for (size_t width = stableSortChunkSize; width < size; width *= 2) {
        for (size_t begin = 0; begin < size; begin += 2 * width) {
            char* middle = from + std::min(begin + width, size) * elementSize;
            steps.merge(steps.order, from + begin * elementSize, middle, middle, from + std::min(begin + 2 * width, size) * elementSize, to + begin * elementSize);
        }
        std::swap(from, to);
    }
    if (from != first)
        memcpy(first, from, size * elementSize);
}

template<typename T>
inline constexpr bool movesAsBytes = alignof(T) <= alignof(uint64_t) && ((std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>) || VectorTraits<T>::canMoveWithMemcpy);

template<typename First, typename Second>
inline constexpr bool movesAsBytes<std::pair<First, Second>> = alignof(std::pair<First, Second>) <= alignof(uint64_t) && movesAsBytes<First> && movesAsBytes<Second>;

template<typename T, typename IsLess, typename Projection>
class OrderedSteps {
public:
    OrderedSteps(IsLess& isLess, Projection& projection)
        : m_isLess(isLess)
        , m_projection(projection)
    {
    }

    SortSteps stepsOfSort() { return { insertionSort, partition, nullptr, isLessOutOfLine, this, sizeof(T) }; }
    SortSteps stepsOfStableSort() { return { insertionSort, nullptr, merge, nullptr, this, sizeof(T) }; }
    SortSteps stepsOfSortAndMerge() { return { insertionSort, partition, merge, isLessOutOfLine, this, sizeof(T) }; }

    static NEVER_INLINE void insertionSort(void* erasedOrder, void* erasedFirst, void* erasedEndOfGuarded, void* erasedLast)
    {
        auto& order = *static_cast<OrderedSteps*>(erasedOrder);
        T* first = static_cast<T*>(erasedFirst);
        T* endOfGuarded = static_cast<T*>(erasedEndOfGuarded);
        T* last = static_cast<T*>(erasedLast);
        for (T* current = first + 1; current < last; ++current) {
            Held held(current);
            if (current < endOfGuarded && order.isLess(held.value(), *first)) {
                memmove(static_cast<void*>(first + 1), static_cast<void*>(first), reinterpret_cast<char*>(current) - reinterpret_cast<char*>(first));
                held.moveTo(first);
                continue;
            }
            T* hole = current;
            for (T* previous = hole - 1; order.isLess(held.value(), *previous); --previous) {
                move(hole, previous);
                hole = previous;
            }
            held.moveTo(hole);
        }
    }

private:
    class Held {
    public:
        explicit Held(T* from) { memcpy(m_bytes, static_cast<void*>(from), sizeof(T)); }
        T& value() { return *reinterpret_cast<T*>(m_bytes); }
        void moveTo(T* to) { memcpy(static_cast<void*>(to), m_bytes, sizeof(T)); }

    private:
        alignas(T) unsigned char m_bytes[sizeof(T)];
    };

    static void move(T* to, T* from) { memcpy(static_cast<void*>(to), static_cast<void*>(from), sizeof(T)); }

    static void swap(T* first, T* second)
    {
        Held held(first);
        move(first, second);
        held.moveTo(second);
    }

    ALWAYS_INLINE bool isLess(T& first, T& second) { return std::invoke(m_isLess, std::invoke(m_projection, first), std::invoke(m_projection, second)); }

    static bool isLessOutOfLine(void* order, void* first, void* second)
    {
        return static_cast<OrderedSteps*>(order)->isLess(*static_cast<T*>(first), *static_cast<T*>(second));
    }

    static void* partition(void* erasedOrder, void* erasedFirst, void* erasedLast)
    {
        auto& order = *static_cast<OrderedSteps*>(erasedOrder);
        T* first = static_cast<T*>(erasedFirst);
        T* last = static_cast<T*>(erasedLast);
        T* a = first + 1;
        T* b = first + (last - first) / 2;
        T* c = last - 1;
        T* median;
        if (isLessOutOfLine(erasedOrder, a, b))
            median = isLessOutOfLine(erasedOrder, b, c) ? b : isLessOutOfLine(erasedOrder, a, c) ? c : a;
        else
            median = isLessOutOfLine(erasedOrder, a, c) ? a : isLessOutOfLine(erasedOrder, b, c) ? c : b;
        swap(first, median);
        T* low = first + 1;
        T* high = last;
        while (true) {
            while (order.isLess(*low, *first))
                ++low;
            --high;
            while (order.isLess(*first, *high))
                --high;
            if (!(low < high))
                return low;
            swap(low, high);
            ++low;
        }
    }

    static void merge(void* erasedOrder, void* erasedFirst, void* erasedEndOfFirst, void* erasedSecond, void* erasedEndOfSecond, void* erasedOutput)
    {
        auto& order = *static_cast<OrderedSteps*>(erasedOrder);
        T* first = static_cast<T*>(erasedFirst);
        T* endOfFirst = static_cast<T*>(erasedEndOfFirst);
        T* second = static_cast<T*>(erasedSecond);
        T* endOfSecond = static_cast<T*>(erasedEndOfSecond);
        T* output = static_cast<T*>(erasedOutput);
        for (; first != endOfFirst && second != endOfSecond; ++output)
            move(output, order.isLess(*second, *first) ? second++ : first++);
        size_t restOfFirst = reinterpret_cast<char*>(endOfFirst) - reinterpret_cast<char*>(first);
        memcpy(static_cast<void*>(output), static_cast<void*>(first), restOfFirst);
        memcpy(reinterpret_cast<char*>(output) + restOfFirst, static_cast<void*>(second), reinterpret_cast<char*>(endOfSecond) - reinterpret_cast<char*>(second));
    }

    IsLess& m_isLess;
    Projection& m_projection;
};

template<typename Integer>
NEVER_INLINE void sortIntegers(Integer* begin, Integer* end)
{
    std::sort(begin, end);
}

template<typename T> struct IntegerOrderedLike { using Type = std::conditional_t<std::is_integral_v<T>, T, void>; };
template<typename Tag> struct IntegerOrderedLike<Typed::ID<Tag>> { using Type = uint32_t; };
template<typename Tag, typename Storage> struct IntegerOrderedLike<BuiltinTableID<Tag, Storage>> { using Type = Storage; };

template<typename IsLess, typename Projection, typename T>
inline constexpr bool isAscendingOrder = std::is_same_v<Projection, std::identity> && (std::is_same_v<IsLess, std::ranges::less> || std::is_same_v<IsLess, std::less<>> || std::is_same_v<IsLess, std::less<T>>);

template<typename T>
NEVER_INLINE void moveIntoOrder(T* elements, SortedIndices&& sources)
{
    for (uint32_t first = 0; first < sources.size(); ++first) {
        if (sources[first] == first)
            continue;
        T displaced = WTF::move(elements[first]);
        uint32_t target = first;
        for (uint32_t source = std::exchange(sources[target], target); source != first; source = std::exchange(sources[target], target)) {
            elements[target] = WTF::move(elements[source]);
            target = source;
        }
        elements[target] = WTF::move(displaced);
    }
}

enum class EqualElements : bool { InAnyOrder, KeepTheirOrder };

template<EqualElements equalElements, typename T, typename IsLess, typename Projection>
void sortElements(T* elements, size_t size, IsLess& isLess, Projection& projection)
{
    if (size < 2)
        return;
    if constexpr (!movesAsBytes<T>) {
        SortedIndices indices = indicesInOrder(size);
        auto isLessAt = [&](uint32_t first, uint32_t second) -> bool {
            return std::invoke(isLess, std::invoke(projection, elements[first]), std::invoke(projection, elements[second]));
        };
        std::identity identity;
        sortElements<equalElements>(indices.mutableSpan().data(), size, isLessAt, identity);
        moveIntoOrder(elements, WTF::move(indices));
    } else {
        using Steps = OrderedSteps<T, IsLess, Projection>;
        Steps order(isLess, projection);
        if (size <= longestInsertionSort)
            Steps::insertionSort(&order, elements, elements + size, elements + size);
        else if constexpr (equalElements == EqualElements::KeepTheirOrder)
            stableSortLongRange(order.stepsOfStableSort(), reinterpret_cast<char*>(elements), size);
        else
            sortLongRange(order.stepsOfSort(), reinterpret_cast<char*>(elements), size);
    }
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void sort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    using T = std::ranges::range_value_t<Range>;
    using Integer = typename IntegerOrderedLike<T>::Type;
    if constexpr (!std::ranges::contiguous_range<Range>)
        std::ranges::sort(range, WTF::move(isLess), WTF::move(projection));
    else if constexpr (!std::is_void_v<Integer> && isAscendingOrder<IsLess, Projection, T>) {
        static_assert(sizeof(Integer) == sizeof(T));
        if (size_t size = std::ranges::size(range); size > 1) {
            auto* begin = reinterpret_cast<Integer*>(std::ranges::data(range));
            sortIntegers(begin, begin + size);
        }
    } else
        sortElements<EqualElements::InAnyOrder>(std::ranges::data(range), std::ranges::size(range), isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void stableSort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    if constexpr (!std::ranges::contiguous_range<Range>)
        std::ranges::stable_sort(range, WTF::move(isLess), WTF::move(projection));
    else
        sortElements<EqualElements::KeepTheirOrder>(std::ranges::data(range), std::ranges::size(range), isLess, projection);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
