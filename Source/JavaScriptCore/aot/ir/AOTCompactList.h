/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <algorithm>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct CompactListHeader {
    uint32_t size;
    uint32_t capacity;
};
inline constexpr CompactListHeader emptyCompactListHeader { 0, 0 };

template<typename T>
class CompactList {
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
public:
    using ValueType = T;
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    CompactList() = default;
    CompactList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    CompactList(std::span<const T> list) { append(list); }
    template<size_t inlineCapacity> CompactList(const Vector<T, inlineCapacity>& list) { append(list.span()); }
    CompactList(const CompactList& other) { append(other.span()); }
    CompactList(CompactList&& other)
        : m_storage(std::exchange(other.m_storage, emptyStorage()))
    {
    }
    ~CompactList()
    {
        if (m_storage != emptyStorage())
            fastFree(m_storage);
    }
    CompactList& operator=(const CompactList& other)
    {
        if (this != &other) {
            shrink(0);
            append(other.span());
        }
        return *this;
    }
    CompactList& operator=(CompactList&& other)
    {
        if (this != &other) {
            if (m_storage != emptyStorage())
                fastFree(m_storage);
            m_storage = std::exchange(other.m_storage, emptyStorage());
        }
        return *this;
    }

    size_t size() const { return m_storage->size; }
    size_t capacity() const { return m_storage->capacity; }
    bool isEmpty() const { return !size(); }

    T* begin() LIFETIME_BOUND { return m_storage->elements(); }
    T* end() LIFETIME_BOUND { return begin() + size(); }
    const T* begin() const LIFETIME_BOUND { return m_storage->elements(); }
    const T* end() const LIFETIME_BOUND { return begin() + size(); }
    std::span<const T> span() const LIFETIME_BOUND { return { begin(), size() }; }
    std::span<T> mutableSpan() LIFETIME_BOUND { return { begin(), size() }; }
    std::span<const T> subspan(size_t offset, size_t length = std::dynamic_extent) const LIFETIME_BOUND { return span().subspan(offset, length); }

    T& at(size_t index) LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < size());
        return begin()[index];
    }
    const T& at(size_t index) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < size());
        return begin()[index];
    }
    T& operator[](size_t index) LIFETIME_BOUND { return at(index); }
    const T& operator[](size_t index) const LIFETIME_BOUND { return at(index); }
    T& first() LIFETIME_BOUND { return at(0); }
    const T& first() const LIFETIME_BOUND { return at(0); }
    T& last() LIFETIME_BOUND { return at(size() - 1); }
    const T& last() const LIFETIME_BOUND { return at(size() - 1); }

    void reserveCapacity(size_t newCapacity)
    {
        if (newCapacity <= capacity())
            return;
        RELEASE_ASSERT(newCapacity <= UINT32_MAX);
        bool wasEmpty = m_storage == emptyStorage();
        auto* storage = static_cast<Storage*>(fastRealloc(wasEmpty ? nullptr : m_storage, sizeof(Storage) + newCapacity * sizeof(T)));
        if (wasEmpty)
            storage->size = 0;
        storage->capacity = newCapacity;
        m_storage = storage;
    }
    void reserveInitialCapacity(size_t initialCapacity) { reserveCapacity(initialCapacity); }
    void shrinkToFit()
    {
        if (!size()) {
            clear();
            return;
        }
        if (size() == capacity())
            return;
        m_storage = static_cast<Storage*>(fastRealloc(m_storage, sizeof(Storage) + size() * sizeof(T)));
        m_storage->capacity = m_storage->size;
    }

    void append(T element)
    {
        if (size() == capacity())
            reserveCapacity(std::max<size_t>(2, capacity() + capacity() / 2 + 1));
        m_storage->elements()[m_storage->size++] = element;
    }
    void append(std::span<const T> list)
    {
        if (list.empty())
            return;
        size_t before = size();
        if (list.data() >= begin() && list.data() < end()) {
            Vector<T, 8> copy(list);
            append(copy.span());
            return;
        }
        reserveCapacity(before + list.size());
        memcpy(m_storage->elements() + before, list.data(), list.size_bytes());
        m_storage->size = before + list.size();
    }
    void appendVector(const CompactList& other) { append(other.span()); }
    template<size_t inlineCapacity> void appendVector(const Vector<T, inlineCapacity>& other) { append(other.span()); }
    void appendList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    template<typename Iterator> void appendRange(Iterator from, Iterator to)
    {
        for (; from != to; ++from)
            append(*from);
    }
    void insert(size_t position, T element)
    {
        RELEASE_ASSERT(position <= size());
        append(element);
        std::rotate(begin() + position, end() - 1, end());
    }
    void removeAt(size_t position, size_t length = 1)
    {
        RELEASE_ASSERT(position <= size() && length <= size() - position);
        if (!length)
            return;
        std::move(begin() + position + length, end(), begin() + position);
        m_storage->size -= length;
    }
    void removeLast()
    {
        RELEASE_ASSERT(!isEmpty());
        removeAt(size() - 1);
    }
    T takeLast()
    {
        T result = last();
        removeLast();
        return result;
    }
    void shrink(size_t newSize)
    {
        RELEASE_ASSERT(newSize <= size());
        if (newSize != size())
            m_storage->size = newSize;
    }
    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size());
        if (newSize == size())
            return;
        reserveCapacity(newSize);
        std::fill(end(), begin() + newSize, T());
        m_storage->size = newSize;
    }
    void resize(size_t newSize)
    {
        if (newSize <= size())
            shrink(newSize);
        else
            grow(newSize);
    }
    void fill(T element, size_t newSize)
    {
        resize(newSize);
        std::fill(begin(), end(), element);
    }
    void clear()
    {
        if (m_storage != emptyStorage())
            fastFree(m_storage);
        m_storage = emptyStorage();
    }
    void reverse() { std::reverse(begin(), end()); }

    size_t find(T element) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if (begin()[index] == element)
                return index;
        }
        return notFound;
    }
    bool contains(T element) const { return find(element) != notFound; }
    size_t findIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if (matches(begin()[index]))
                return index;
        }
        return notFound;
    }
    bool containsIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const { return findIf(matches) != notFound; }
    size_t removeAllMatching(NOESCAPE const Invocable<bool(const T&)> auto& matches)
    {
        auto* newEnd = std::remove_if(begin(), end(), matches);
        size_t removed = end() - newEnd;
        shrink(size() - removed);
        return removed;
    }
    size_t removeAll(T element)
    {
        return removeAllMatching([&](const T& other) {
            return other == element;
        });
    }
    auto map(NOESCAPE const auto& functor) const
    {
        Vector<std::decay_t<decltype(functor(T()))>> result;
        result.reserveInitialCapacity(size());
        for (T element : *this)
            result.append(functor(element));
        return result;
    }

    friend bool operator==(const CompactList& a, const CompactList& b) { return std::ranges::equal(a.span(), b.span()); }

private:
    struct Storage : CompactListHeader {
        T* elements() { return reinterpret_cast<T*>(this + 1); }
    };
    static Storage* emptyStorage() { return static_cast<Storage*>(const_cast<CompactListHeader*>(&emptyCompactListHeader)); }

    Storage* m_storage { emptyStorage() };
};

using Arguments = CompactList<ValueID>;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
