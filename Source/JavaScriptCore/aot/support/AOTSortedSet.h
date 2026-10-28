/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactList.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <concepts>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT {

// The values of a list that is so many times shorter than the other are looked for there by halving.
AOT_THRESHOLD(size_t, timesShorterToSearchByHalving, 16);

template<typename T>
bool sortedListsOverlap(std::span<const T> a, std::span<const T> b)
{
    if (a.size() > b.size())
        std::swap(a, b);
    if (a.size() * timesShorterToSearchByHalving < b.size()) {
        return std::ranges::any_of(a, [&](T value) {
            return std::ranges::binary_search(b, value);
        });
    }
    size_t indexInA = 0;
    size_t indexInB = 0;
    while (indexInA < a.size() && indexInB < b.size()) {
        if (a[indexInA] == b[indexInB])
            return true;
        if (a[indexInA] < b[indexInB])
            ++indexInA;
        else
            ++indexInB;
    }
    return false;
}

template<typename T>
bool sortedListIncludes(std::span<const T> list, std::span<const T> included)
{
    if (included.size() > list.size())
        return false;
    if (included.size() * timesShorterToSearchByHalving < list.size()) {
        return std::ranges::all_of(included, [&](T value) {
            return std::ranges::binary_search(list, value);
        });
    }
    return std::ranges::includes(list, included);
}

template<typename T, size_t inlineCapacity = 0>
class SortedSet {
    static_assert(std::is_trivially_copyable_v<T> && std::totally_ordered<T>);
public:
    SortedSet() = default;
    SortedSet(std::initializer_list<T> unsortedValues)
        : SortedSet(std::span<const T> { unsortedValues.begin(), unsortedValues.size() })
    {
    }
    explicit SortedSet(std::span<const T> unsortedValues);

    size_t size() const { return m_values.size(); }
    bool isEmpty() const { return m_values.isEmpty(); }
    const T* begin() const LIFETIME_BOUND { return m_values.begin(); }
    const T* end() const LIFETIME_BOUND { return m_values.end(); }
    T operator[](size_t index) const { return m_values[index]; }
    std::span<const T> span() const LIFETIME_BOUND { return m_values.span(); }

    bool contains(T value) const { return std::binary_search(begin(), end(), value); }
    bool isSubsetOf(const SortedSet&) const;
    bool overlaps(const SortedSet&) const;

    bool add(T);
    bool remove(T);
    bool unite(const SortedSet&);
    void intersect(const SortedSet&);
    void subtract(const SortedSet&);
    void clear() { m_values.clear(); }

    friend bool operator==(const SortedSet&, const SortedSet&) = default;

private:
    enum class CommonValues : bool { Remove, Retain };

    static constexpr unsigned numberOfValuesInPlace = std::max<size_t>(inlineCapacity, 12 / sizeof(T));
    using Values = std::conditional_t<!!numberOfValuesInPlace, Typed::InlineList<T, std::max(numberOfValuesInPlace, 1u)>, Vector<T>>;

    size_t lowerBound(T value) const { return std::lower_bound(begin(), end(), value) - begin(); }
    void filter(const SortedSet&, CommonValues);

    Values m_values;
};

template<typename T, size_t inlineCapacity>
SortedSet<T, inlineCapacity>::SortedSet(std::span<const T> unsortedValues)
    : m_values(unsortedValues)
{
    AOT::sort(m_values);
    m_values.shrink(std::unique(m_values.begin(), m_values.end()) - m_values.begin());
}

template<typename T, size_t inlineCapacity>
bool SortedSet<T, inlineCapacity>::isSubsetOf(const SortedSet& other) const
{
    return sortedListIncludes(other.span(), span());
}

template<typename T, size_t inlineCapacity>
bool SortedSet<T, inlineCapacity>::overlaps(const SortedSet& other) const
{
    return sortedListsOverlap(span(), other.span());
}

template<typename T, size_t inlineCapacity>
bool SortedSet<T, inlineCapacity>::add(T value)
{
    if (isEmpty() || m_values.last() < value) {
        m_values.append(value);
        return true;
    }
    size_t position = lowerBound(value);
    if (m_values[position] == value)
        return false;
    m_values.insert(position, value);
    return true;
}

template<typename T, size_t inlineCapacity>
bool SortedSet<T, inlineCapacity>::remove(T value)
{
    size_t position = lowerBound(value);
    if (position == size() || m_values[position] != value)
        return false;
    m_values.removeAt(position);
    return true;
}

template<typename T, size_t inlineCapacity>
bool SortedSet<T, inlineCapacity>::unite(const SortedSet& other)
{
    // A few values find their places by halving, and what stands behind them moves up: nothing is compared one by one, nothing copied.
    AOT_THRESHOLD(size_t, mostValuesAddedOneByOne, 4);
    if (other.size() <= mostValuesAddedOneByOne && &other != this) {
        bool hasGrown = false;
        for (T value : other)
            hasGrown |= add(value);
        return hasGrown;
    }
    if (other.isSubsetOf(*this))
        return false;
    if (isEmpty()) {
        m_values = other.m_values;
        return true;
    }
    Values united;
    united.grow(size() + other.size());
    united.shrink(std::set_union(begin(), end(), other.begin(), other.end(), united.begin()) - united.begin());
    m_values = WTF::move(united);
    return true;
}

template<typename T, size_t inlineCapacity>
void SortedSet<T, inlineCapacity>::intersect(const SortedSet& other)
{
    if (&other != this)
        filter(other, CommonValues::Retain);
}

template<typename T, size_t inlineCapacity>
void SortedSet<T, inlineCapacity>::subtract(const SortedSet& other)
{
    if (&other == this) {
        clear();
        return;
    }
    filter(other, CommonValues::Remove);
}

template<typename T, size_t inlineCapacity>
void SortedSet<T, inlineCapacity>::filter(const SortedSet& other, CommonValues commonValues)
{
    size_t writeIndex = 0;
    size_t otherIndex = 0;
    for (size_t readIndex = 0; readIndex < size(); ++readIndex) {
        T value = m_values[readIndex];
        while (otherIndex < other.size() && other.m_values[otherIndex] < value)
            ++otherIndex;
        bool isCommon = otherIndex < other.size() && other.m_values[otherIndex] == value;
        if (isCommon == (commonValues == CommonValues::Retain))
            m_values[writeIndex++] = value;
    }
    m_values.shrink(writeIndex);
}

template<typename T, size_t inlineCapacity>
void add(Hasher& hasher, const SortedSet<T, inlineCapacity>& set)
{
    add(hasher, static_cast<uint32_t>(set.size()));
    for (T value : set)
        add(hasher, value);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
