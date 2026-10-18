/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

template<typename T, auto... fields>
class MultiArrayList final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MultiArrayList);

    template<typename> struct MemberOf;
    template<typename Field> struct MemberOf<Field T::*> {
        using Type = Field;
    };
    template<auto field> using FieldType = typename MemberOf<decltype(field)>::Type;
    template<auto a, auto b> static constexpr bool isSameField = std::is_same_v<std::integral_constant<decltype(a), a>, std::integral_constant<decltype(b), b>>;

    static constexpr size_t numberOfColumns = sizeof...(fields);
    static constexpr std::array<size_t, numberOfColumns> sizes { sizeof(FieldType<fields>)... };
    static constexpr std::array<size_t, numberOfColumns> alignments { alignof(FieldType<fields>)... };

    static_assert(numberOfColumns > 0);
    static_assert((std::is_trivially_copyable_v<FieldType<fields>> && ...));
    static_assert((std::is_trivially_destructible_v<FieldType<fields>> && ...));
    static_assert(((alignof(FieldType<fields>) <= 16) && ...));

    template<auto field> static constexpr size_t columnOf()
    {
        constexpr std::array<bool, numberOfColumns> matches { isSameField<field, fields>... };
        size_t found = numberOfColumns;
        size_t count = 0;
        for (size_t i = 0; i < numberOfColumns; ++i) {
            if (matches[i]) {
                found = i;
                ++count;
            }
        }
        return count == 1 ? found : numberOfColumns;
    }

    static constexpr size_t rowBytesBefore(size_t column)
    {
        size_t bytes = 0;
        for (size_t other = 0; other < numberOfColumns; ++other) {
            if (alignments[other] > alignments[column] || (alignments[other] == alignments[column] && other < column))
                bytes += sizes[other];
        }
        return bytes;
    }

public:
    static constexpr size_t rowBytes = (sizeof(FieldType<fields>) + ...);

    MultiArrayList() = default;

    MultiArrayList(const MultiArrayList& other)
    {
        copyFrom(other);
    }

    MultiArrayList(MultiArrayList&& other)
        : m_bytes(std::exchange(other.m_bytes, nullptr))
        , m_size(std::exchange(other.m_size, 0))
        , m_capacity(std::exchange(other.m_capacity, 0))
    {
    }

    ~MultiArrayList()
    {
        fastFree(m_bytes);
    }

    MultiArrayList& operator=(const MultiArrayList& other)
    {
        if (this != &other) {
            m_size = 0;
            copyFrom(other);
        }
        return *this;
    }

    MultiArrayList& operator=(MultiArrayList&& other)
    {
        if (this != &other) {
            fastFree(m_bytes);
            m_bytes = std::exchange(other.m_bytes, nullptr);
            m_size = std::exchange(other.m_size, 0);
            m_capacity = std::exchange(other.m_capacity, 0);
        }
        return *this;
    }

    uint32_t size() const { return m_size; }
    uint32_t capacity() const { return m_capacity; }
    bool isEmpty() const { return !m_size; }
    size_t sizeInBytes() const { return rowBytes * m_capacity; }

    template<auto field> std::span<FieldType<field>> items() LIFETIME_BOUND
    {
        return unsafeMakeSpan(column<field>(m_bytes, m_capacity), m_size);
    }

    template<auto field> std::span<const FieldType<field>> items() const LIFETIME_BOUND
    {
        return unsafeMakeSpan(static_cast<const FieldType<field>*>(column<field>(m_bytes, m_capacity)), m_size);
    }

    template<auto field> FieldType<field>& at(uint32_t index) LIFETIME_BOUND
    {
        return items<field>()[checked(index)];
    }

    template<auto field> const FieldType<field>& at(uint32_t index) const LIFETIME_BOUND
    {
        return items<field>()[checked(index)];
    }

    T get(uint32_t index) const
    {
        checked(index);
        T row { };
        ((row.*fields = items<fields>()[index]), ...);
        return row;
    }

    void set(uint32_t index, const T& row)
    {
        checked(index);
        ((items<fields>()[index] = row.*fields), ...);
    }

    uint32_t append(const T& row)
    {
        RELEASE_ASSERT(m_size != UINT32_MAX);
        if (m_size == m_capacity) [[unlikely]] {
            T copy = row;
            expandCapacity(m_size + 1);
            return appendWithinCapacity(copy);
        }
        return appendWithinCapacity(row);
    }

    T takeLast()
    {
        T row = get(m_size - 1);
        --m_size;
        return row;
    }

    void swapRemove(uint32_t index)
    {
        checked(index);
        --m_size;
        if (index != m_size)
            ((column<fields>(m_bytes, m_capacity)[index] = column<fields>(m_bytes, m_capacity)[m_size]), ...);
    }

    void resize(uint32_t newSize)
    {
        if (newSize > m_size)
            grow(newSize);
        else
            shrink(newSize);
    }

    void grow(uint32_t newSize)
    {
        uint32_t oldSize = m_size;
        growWithoutInitializing(newSize);
        T row { };
        (std::ranges::fill(items<fields>().subspan(oldSize), row.*fields), ...);
    }

    void growWithoutInitializing(uint32_t newSize)
    {
        ASSERT(newSize >= m_size);
        if (newSize > m_capacity)
            expandCapacity(newSize);
        m_size = newSize;
    }

    template<auto field> void fill(const FieldType<field>& value)
    {
        std::ranges::fill(items<field>(), value);
    }

    void swap(MultiArrayList& other)
    {
        std::swap(m_bytes, other.m_bytes);
        std::swap(m_size, other.m_size);
        std::swap(m_capacity, other.m_capacity);
    }

    void shrink(uint32_t newSize)
    {
        ASSERT(newSize <= m_size);
        m_size = newSize;
    }

    void clear() { m_size = 0; }

    void clearAndFree()
    {
        fastFree(std::exchange(m_bytes, nullptr));
        m_size = 0;
        m_capacity = 0;
    }

    void reserveCapacity(uint32_t newCapacity)
    {
        if (newCapacity > m_capacity)
            setCapacity(newCapacity);
    }

    void shrinkToFit()
    {
        if (!m_size)
            clearAndFree();
        else if (m_size != m_capacity)
            setCapacity(m_size);
    }

private:
    uint32_t checked(uint32_t index) const
    {
        RELEASE_ASSERT(index < m_size);
        return index;
    }

    template<auto field> static FieldType<field>* column(std::byte* bytes, uint32_t capacity)
    {
        constexpr size_t index = columnOf<field>();
        static_assert(index < numberOfColumns, "the field must be listed exactly once among the columns");
        constexpr size_t ahead = rowBytesBefore(index);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        return reinterpret_cast<FieldType<field>*>(bytes + ahead * capacity);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }

    uint32_t appendWithinCapacity(const T& row)
    {
        uint32_t index = m_size++;
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        ((column<fields>(m_bytes, m_capacity)[index] = row.*fields), ...);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        return index;
    }

    void expandCapacity(uint32_t least)
    {
        uint64_t grown = static_cast<uint64_t>(m_capacity) + m_capacity / 2 + 8;
        setCapacity(static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(grown, least), UINT32_MAX)));
    }

    void setCapacity(uint32_t newCapacity)
    {
        ASSERT(newCapacity >= m_size);
        auto* newBytes = static_cast<std::byte*>(fastMalloc(rowBytes * newCapacity));
        if (m_size)
            (memcpy(column<fields>(newBytes, newCapacity), column<fields>(m_bytes, m_capacity), sizeof(FieldType<fields>) * m_size), ...);
        fastFree(m_bytes);
        m_bytes = newBytes;
        m_capacity = newCapacity;
    }

    void copyFrom(const MultiArrayList& other)
    {
        ASSERT(!m_size);
        reserveCapacity(other.m_size);
        m_size = other.m_size;
        if (m_size)
            (memcpy(column<fields>(m_bytes, m_capacity), column<fields>(other.m_bytes, other.m_capacity), sizeof(FieldType<fields>) * m_size), ...);
    }

    std::byte* m_bytes { nullptr };
    uint32_t m_size { 0 };
    uint32_t m_capacity { 0 };
};

} // namespace WTF

using WTF::MultiArrayList;
