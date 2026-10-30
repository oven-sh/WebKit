/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// What the tables of a body point to, written one part behind the other, for a body that is put away (ParkedBody).
using BodyWords = Vector<uint32_t, 2048>;

class BodyWordsReader {
public:
    explicit BodyWordsReader(std::span<const uint32_t> words)
        : m_words(words)
    {
    }

    uint32_t peek(size_t index) const { return m_words[index]; }
    std::span<const uint32_t> take(size_t numberOfWords)
    {
        auto taken = m_words.first(numberOfWords);
        m_words = m_words.subspan(numberOfWords);
        return taken;
    }
    bool isAtEnd() const { return m_words.empty(); }

private:
    std::span<const uint32_t> m_words;
};

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

    CompactList() { setStorage(emptyStorage()); }
    CompactList(std::initializer_list<T> list)
        : CompactList()
    {
        append(std::span<const T> { list.begin(), list.size() });
    }
    CompactList(std::span<const T> list)
        : CompactList()
    {
        append(list);
    }
    template<size_t inlineCapacity> CompactList(const Vector<T, inlineCapacity>& list)
        : CompactList()
    {
        append(list.span());
    }
    CompactList(const CompactList& other)
        : m_words(other.m_words)
    {
        if (other.isInline() || other.storage() == emptyStorage())
            return;
        setStorage(emptyStorage());
        append(other.span());
    }
    CompactList(CompactList&& other)
        : m_words(other.m_words)
    {
        other.setStorage(emptyStorage());
    }
    ~CompactList() { freeStorage(); }
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
            freeStorage();
            m_words = other.m_words;
            other.setStorage(emptyStorage());
        }
        return *this;
    }

    size_t size() const { return isInline() ? 1 : storage()->size; }
    size_t capacity() const { return isInline() ? 1 : storage()->capacity; }
    bool isEmpty() const { return !size(); }

    T* begin() LIFETIME_BOUND { return isInline() ? inlineElement() : storage()->elements(); }
    T* end() LIFETIME_BOUND { return begin() + size(); }
    const T* begin() const LIFETIME_BOUND { return const_cast<CompactList&>(*this).begin(); }
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
        if (newCapacity <= capacity() || (canStoreInline && newCapacity == 1))
            return;
        RELEASE_ASSERT(newCapacity <= UINT32_MAX);
        Storage* newStorage;
        if (isInline()) {
            newStorage = static_cast<Storage*>(fastMalloc(sizeof(Storage) + newCapacity * sizeof(T)));
            newStorage->size = 1;
            *newStorage->elements() = *inlineElement();
        } else {
            bool wasEmpty = storage() == emptyStorage();
            newStorage = static_cast<Storage*>(fastRealloc(wasEmpty ? nullptr : storage(), sizeof(Storage) + newCapacity * sizeof(T)));
            if (wasEmpty)
                newStorage->size = 0;
        }
        newStorage->capacity = newCapacity;
        setStorage(newStorage);
    }
    void reserveInitialCapacity(size_t initialCapacity) { reserveCapacity(initialCapacity); }
    void shrinkToFit()
    {
        if (isInline() || size() == capacity())
            return;
        if (!size()) {
            clear();
            return;
        }
        if (canStoreInline && size() == 1) {
            T element = first();
            fastFree(storage());
            setInline(element);
            return;
        }
        setStorage(static_cast<Storage*>(fastRealloc(storage(), sizeof(Storage) + size() * sizeof(T))));
        storage()->capacity = storage()->size;
    }

    void appendStorageTo(BodyWords& words) const
    {
        static_assert(sizeof(T) == sizeof(uint32_t));
        if (isInline() || storage() == emptyStorage())
            return;
        words.append(static_cast<uint32_t>(size()));
        words.append(std::span { reinterpret_cast<const uint32_t*>(begin()), size() });
    }
    // This is a copy, byte for byte, of a list that was written with appendStorageTo().
    void takeStorageFrom(BodyWordsReader& reader)
    {
        if (isInline() || storage() == emptyStorage())
            return;
        setStorage(emptyStorage());
        size_t size = reader.take(1)[0];
        append(std::span { reinterpret_cast<const T*>(reader.take(size).data()), size });
    }

    void append(T element)
    {
        if (canStoreInline && storage() == emptyStorage()) {
            setInline(element);
            return;
        }
        if (size() == capacity()) [[unlikely]]
            expandCapacity();
        Storage* storage = this->storage();
        storage->elements()[storage->size++] = element;
    }
    void append(std::span<const T> list)
    {
        if (list.empty())
            return;
        if (list.size() == 1) {
            append(list.front());
            return;
        }
        size_t before = size();
        if (list.data() >= begin() && list.data() < end()) {
            Vector<T, 8> copy(list);
            append(copy.span());
            return;
        }
        reserveCapacity(before + list.size());
        memcpy(storage()->elements() + before, list.data(), list.size_bytes());
        storage()->size = before + list.size();
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
        setSize(size() - length);
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
            setSize(newSize);
    }
    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size());
        if (newSize == size())
            return;
        if (canStoreInline && newSize == 1 && storage() == emptyStorage()) {
            setInline(T());
            return;
        }
        reserveCapacity(newSize);
        std::fill(end(), begin() + newSize, T());
        storage()->size = newSize;
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
        freeStorage();
        setStorage(emptyStorage());
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

    static_assert(sizeof(Storage*) == 2 * sizeof(uint32_t) && std::endian::native == std::endian::little);
    static constexpr bool canStoreInline = sizeof(T) == sizeof(uint32_t);
    static constexpr uint32_t inlineTag = 1;

    Storage* storage() const { return std::bit_cast<Storage*>(m_words); }
    void setStorage(Storage* storage) { m_words = std::bit_cast<std::array<uint32_t, 2>>(storage); }
    bool isInline() const { return canStoreInline && (m_words[0] & inlineTag); }
    T* inlineElement() { return reinterpret_cast<T*>(&m_words[1]); }
    void setInline(T element)
    {
        m_words[0] = inlineTag;
        *inlineElement() = element;
    }
    NEVER_INLINE void expandCapacity() { reserveCapacity(std::max<size_t>(2, capacity() + capacity() / 2 + 1)); }
    void setSize(size_t newSize)
    {
        if (!isInline())
            storage()->size = newSize;
        else if (!newSize)
            setStorage(emptyStorage());
    }
    void freeStorage()
    {
        if (!isInline() && storage() != emptyStorage())
            fastFree(storage());
    }

    std::array<uint32_t, 2> m_words;
};

using Arguments = CompactList<ValueID>;

template<typename T> inline constexpr bool canMoveWithMemcpy = std::is_trivially_copyable_v<T>;
template<typename T> inline constexpr bool canMoveWithMemcpy<CompactList<T>> = true;

template<typename T, unsigned inlineCapacity>
class InlineList {
    static_assert(canMoveWithMemcpy<T> && alignof(T) <= 8 && inlineCapacity);
public:
    using ValueType = T;
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    InlineList() = default;
    InlineList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    InlineList(std::span<const T> list) { append(list); }
    template<size_t otherInlineCapacity> InlineList(const Vector<T, otherInlineCapacity>& list) { append(list.span()); }
    template<size_t otherInlineCapacity> InlineList(Vector<T, otherInlineCapacity>&& list) { appendMoved(list.mutableSpan()); }
    InlineList(const InlineList& other) { copyFrom(other); }
    InlineList(InlineList&& other) { adopt(other); }
    ~InlineList() { destroy(); }
    InlineList& operator=(const InlineList& other)
    {
        if (this != &other) {
            if (m_isOutOfLine || other.m_isOutOfLine || !std::is_trivially_copyable_v<T>) {
                shrink(0);
                append(other.span());
            } else
                copyFrom(other);
        }
        return *this;
    }
    InlineList& operator=(InlineList&& other)
    {
        if (this != &other) {
            destroy();
            adopt(other);
        }
        return *this;
    }

    size_t size() const { return m_size; }
    size_t capacity() const { return m_isOutOfLine ? outOfLineCapacity() : inlineCapacity; }
    bool isEmpty() const { return !m_size; }

    T* begin() LIFETIME_BOUND { return m_isOutOfLine ? outOfLineElements() : inlineElements(); }
    T* end() LIFETIME_BOUND { return begin() + m_size; }
    const T* begin() const LIFETIME_BOUND { return const_cast<InlineList&>(*this).begin(); }
    const T* end() const LIFETIME_BOUND { return begin() + m_size; }
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
        RELEASE_ASSERT(newCapacity <= maxSize);
        T* elements;
        if (m_isOutOfLine)
            elements = static_cast<T*>(fastRealloc(outOfLineElements(), newCapacity * sizeof(T)));
        else {
            elements = static_cast<T*>(fastMalloc(newCapacity * sizeof(T)));
            relocate(elements, inlineElements(), m_size);
        }
        setOutOfLine(elements, newCapacity);
    }
    void reserveInitialCapacity(size_t initialCapacity) { reserveCapacity(initialCapacity); }
    void shrinkToFit()
    {
        if (!m_isOutOfLine || size() == outOfLineCapacity())
            return;
        T* elements = outOfLineElements();
        if (size() > inlineCapacity) {
            setOutOfLine(static_cast<T*>(fastRealloc(elements, size() * sizeof(T))), size());
            return;
        }
        m_isOutOfLine = false;
        relocate(inlineElements(), elements, m_size);
        fastFree(elements);
    }

    // Not what the elements point to.
    void appendStorageTo(BodyWords& words) const
    {
        static_assert(alignof(T) <= alignof(uint32_t) && !(sizeof(T) % sizeof(uint32_t)));
        if (m_isOutOfLine)
            words.append(std::span { reinterpret_cast<const uint32_t*>(outOfLineElements()), m_size * sizeof(T) / sizeof(uint32_t) });
    }
    // This is a copy, byte for byte, of a list that was written with appendStorageTo().
    void takeStorageFrom(BodyWordsReader& reader)
    {
        if (!m_isOutOfLine)
            return;
        const T* source = reinterpret_cast<const T*>(reader.take(m_size * sizeof(T) / sizeof(uint32_t)).data());
        if (m_size <= inlineCapacity) {
            m_isOutOfLine = false;
            relocate(inlineElements(), source, m_size);
            return;
        }
        T* elements = static_cast<T*>(fastMalloc(m_size * sizeof(T)));
        relocate(elements, source, m_size);
        setOutOfLine(elements, m_size);
    }

    void append(T element)
    {
        if (size() == capacity()) [[unlikely]]
            expandCapacity();
        new (NotNull, end()) T(WTF::move(element));
        ++m_size;
    }
    void append(std::span<const T> list)
    {
        if (list.empty())
            return;
        if (list.data() >= begin() && list.data() < end()) {
            Vector<T, 8> copy(list);
            append(copy.span());
            return;
        }
        reserveCapacity(size() + list.size());
        std::uninitialized_copy(list.begin(), list.end(), end());
        m_size += list.size();
    }
    template<typename... Arguments> void constructAndAppend(Arguments&&... arguments) { append(T(std::forward<Arguments>(arguments)...)); }
    void appendVector(const auto& other) { append(std::span<const T> { other.span() }); }
    void appendList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    template<typename Iterator> void appendRange(Iterator from, Iterator to)
    {
        for (; from != to; ++from)
            append(*from);
    }
    bool appendIfNotContains(const T& element)
    {
        if (contains(element))
            return false;
        append(element);
        return true;
    }
    void insert(size_t position, T element)
    {
        T* gap = openGap(position, 1);
        new (NotNull, gap) T(WTF::move(element));
    }
    void insertSpan(size_t position, std::span<const T> list)
    {
        if (list.empty())
            return;
        if (list.data() >= begin() && list.data() < end()) {
            Vector<T, 8> copy(list);
            insertSpan(position, copy.span());
            return;
        }
        std::uninitialized_copy(list.begin(), list.end(), openGap(position, list.size()));
    }
    void insertVector(size_t position, const auto& other) { insertSpan(position, std::span<const T> { other.span() }); }
    void removeAt(size_t position, size_t length = 1)
    {
        RELEASE_ASSERT(position <= size() && length <= size() - position);
        if (!length)
            return;
        T* removed = begin() + position;
        std::destroy_n(removed, length);
        relocate(removed, removed + length, size() - position - length);
        m_size -= length;
    }
    void removeLast()
    {
        RELEASE_ASSERT(!isEmpty());
        removeAt(size() - 1);
    }
    T takeLast()
    {
        T result = WTF::move(last());
        removeLast();
        return result;
    }
    void shrink(size_t newSize)
    {
        RELEASE_ASSERT(newSize <= size());
        std::destroy(begin() + newSize, end());
        m_size = newSize;
    }
    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size());
        reserveCapacity(newSize);
        std::uninitialized_value_construct(end(), begin() + newSize);
        m_size = newSize;
    }
    void resize(size_t newSize)
    {
        if (newSize <= size())
            shrink(newSize);
        else
            grow(newSize);
    }
    void fill(const T& element, size_t newSize)
    {
        resize(newSize);
        std::fill(begin(), end(), element);
    }
    void clear()
    {
        destroy();
        m_size = 0;
        m_isOutOfLine = false;
    }
    void reverse() { std::reverse(begin(), end()); }
    void swap(InlineList& other) { std::swap(*this, other); }

    size_t find(const T& element) const
    {
        return findIf([&](const T& other) {
            return other == element;
        });
    }
    bool contains(const T& element) const { return find(element) != notFound; }
    size_t findIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if (matches(begin()[index]))
                return index;
        }
        return notFound;
    }
    bool containsIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const { return findIf(matches) != notFound; }
    size_t reverseFindIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const
    {
        for (size_t index = size(); index--;) {
            if (matches(begin()[index]))
                return index;
        }
        return notFound;
    }
    size_t reverseFind(const T& element) const
    {
        return reverseFindIf([&](const T& other) {
            return other == element;
        });
    }
    bool removeFirstMatching(NOESCAPE const Invocable<bool(const T&)> auto& matches)
    {
        size_t index = findIf(matches);
        if (index == notFound)
            return false;
        removeAt(index);
        return true;
    }
    bool removeFirst(const T& element)
    {
        size_t index = find(element);
        if (index == notFound)
            return false;
        removeAt(index);
        return true;
    }
    size_t removeAllMatching(NOESCAPE const Invocable<bool(const T&)> auto& matches)
    {
        T* newEnd = std::remove_if(begin(), end(), matches);
        size_t removed = end() - newEnd;
        shrink(size() - removed);
        return removed;
    }
    size_t removeAll(const T& element)
    {
        return removeAllMatching([&](const T& other) {
            return other == element;
        });
    }
    auto map(NOESCAPE const auto& functor) const
    {
        Vector<std::decay_t<decltype(functor(*begin()))>> result;
        result.reserveInitialCapacity(size());
        for (const T& element : *this)
            result.append(functor(element));
        return result;
    }

    friend bool operator==(const InlineList& a, const InlineList& b) { return std::ranges::equal(a.span(), b.span()); }

private:
    static constexpr size_t maxSize = INT32_MAX;
    static constexpr size_t capacityOffset = sizeof(T*);
    static constexpr size_t storageSize = std::max(sizeof(T) * inlineCapacity, capacityOffset + sizeof(uint32_t));

    static void relocate(T* to, const T* from, size_t count) { memmove(static_cast<void*>(to), static_cast<const void*>(from), count * sizeof(T)); }

    T* inlineElements() { return reinterpret_cast<T*>(m_storage); }
    T* outOfLineElements() const
    {
        T* elements;
        memcpy(&elements, m_storage, sizeof(elements));
        return elements;
    }
    uint32_t outOfLineCapacity() const
    {
        uint32_t capacity;
        memcpy(&capacity, m_storage + capacityOffset, sizeof(capacity));
        return capacity;
    }
    void setOutOfLine(T* elements, uint32_t capacity)
    {
        memcpy(m_storage, &elements, sizeof(elements));
        memcpy(m_storage + capacityOffset, &capacity, sizeof(capacity));
        m_isOutOfLine = true;
    }

    NEVER_INLINE void expandCapacity() { reserveCapacity(capacity() + capacity() / 2 + 1); }
    void expandCapacityFor(size_t count)
    {
        size_t needed = size() + count;
        if (needed > capacity())
            reserveCapacity(std::max(needed, capacity() + capacity() / 2 + 1));
    }
    T* openGap(size_t position, size_t length)
    {
        RELEASE_ASSERT(position <= size());
        expandCapacityFor(length);
        T* gap = begin() + position;
        relocate(gap + length, gap, size() - position);
        m_size += length;
        return gap;
    }
    void appendMoved(std::span<T> list)
    {
        reserveCapacity(size() + list.size());
        std::uninitialized_move(list.begin(), list.end(), end());
        m_size += list.size();
    }
    void copyFrom(const InlineList& other)
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (!other.m_isOutOfLine) [[likely]] {
                memcpy(m_storage, other.m_storage, storageSize);
                m_size = other.m_size;
                return;
            }
        }
        if (!other.isEmpty())
            append(other.span());
    }
    void adopt(InlineList& other)
    {
        memcpy(m_storage, other.m_storage, storageSize);
        m_size = other.m_size;
        m_isOutOfLine = other.m_isOutOfLine;
        other.m_size = 0;
        other.m_isOutOfLine = false;
    }
    void destroy()
    {
        std::destroy(begin(), end());
        if (m_isOutOfLine)
            fastFree(outOfLineElements());
    }

    alignas(std::max(alignof(T), alignof(uint32_t))) std::byte m_storage[storageSize];
    uint32_t m_size : 31 { 0 };
    uint32_t m_isOutOfLine : 1 { false };
};

template<typename T, unsigned inlineCapacity> inline constexpr bool canMoveWithMemcpy<InlineList<T, inlineCapacity>> = true;

template<typename T>
class OutOfLineOptional {
public:
    OutOfLineOptional() = default;
    OutOfLineOptional(std::nullopt_t) { }
    OutOfLineOptional(const T& value) { set(create(value)); }
    OutOfLineOptional(T&& value) { set(create(WTF::move(value))); }
    OutOfLineOptional(const std::optional<T>& value)
    {
        if (value)
            set(create(*value));
    }
    OutOfLineOptional(std::optional<T>&& value)
    {
        if (value)
            set(create(WTF::move(*value)));
    }
    OutOfLineOptional(const OutOfLineOptional& other)
    {
        if (other)
            set(create(*other));
    }
    OutOfLineOptional(OutOfLineOptional&& other)
        : m_words(std::exchange(other.m_words, { }))
    {
    }
    ~OutOfLineOptional() { reset(); }
    OutOfLineOptional& operator=(const OutOfLineOptional& other)
    {
        if (this != &other)
            *this = OutOfLineOptional(other);
        return *this;
    }
    OutOfLineOptional& operator=(OutOfLineOptional&& other)
    {
        if (this != &other) {
            reset();
            m_words = std::exchange(other.m_words, { });
        }
        return *this;
    }

    explicit operator bool() const { return get(); }
    bool has_value() const { return get(); }
    T& operator*() LIFETIME_BOUND { return value(); }
    const T& operator*() const LIFETIME_BOUND { return value(); }
    T* operator->() LIFETIME_BOUND { return &value(); }
    const T* operator->() const LIFETIME_BOUND { return &value(); }
    T& value() LIFETIME_BOUND
    {
        RELEASE_ASSERT(get());
        return *get();
    }
    const T& value() const LIFETIME_BOUND
    {
        RELEASE_ASSERT(get());
        return *get();
    }
    operator std::optional<T>() const & { return get() ? std::optional<T>(*get()) : std::nullopt; }
    operator std::optional<T>() && { return get() ? std::optional<T>(WTF::move(*get())) : std::nullopt; }

    void reset()
    {
        T* value = get();
        if (!value)
            return;
        std::destroy_at(value);
        fastFree(value);
        m_words = { };
    }

    // Not what the value points to.
    void appendStorageTo(BodyWords& words) const
    {
        static_assert(alignof(T) <= alignof(uint32_t) && !(sizeof(T) % sizeof(uint32_t)) && canMoveWithMemcpy<T>);
        if (get())
            words.append(std::span { reinterpret_cast<const uint32_t*>(get()), sizeof(T) / sizeof(uint32_t) });
    }
    // This is a copy, byte for byte, of an optional that was written with appendStorageTo().
    void takeStorageFrom(BodyWordsReader& reader)
    {
        if (!get())
            return;
        T* value = static_cast<T*>(fastMalloc(sizeof(T)));
        memcpy(static_cast<void*>(value), reader.take(sizeof(T) / sizeof(uint32_t)).data(), sizeof(T));
        set(value);
    }

    friend bool operator==(const OutOfLineOptional& a, const OutOfLineOptional& b) { return a && b ? *a == *b : !a == !b; }
    friend bool operator==(const OutOfLineOptional& a, const std::optional<T>& b) { return a && b ? *a == *b : !a == !b; }
    friend bool operator==(const OutOfLineOptional& a, std::nullopt_t) { return !a; }

private:
    template<typename U> static T* create(U&& value) { return new (NotNull, fastMalloc(sizeof(T))) T(std::forward<U>(value)); }

    T* get() const { return std::bit_cast<T*>(m_words); }
    void set(T* value) { m_words = std::bit_cast<std::array<uint32_t, 2>>(value); }

    std::array<uint32_t, 2> m_words { };
};

template<typename T> inline constexpr bool canMoveWithMemcpy<OutOfLineOptional<T>> = true;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
