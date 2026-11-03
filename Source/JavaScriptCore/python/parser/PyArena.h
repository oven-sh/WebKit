/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// A sequence of the arena: fixed once made.
template<typename T>
class Seq {
public:
    Seq() = default;
    Seq(T* data, size_t size)
        : m_data(data)
        , m_size(static_cast<unsigned>(size))
    {
    }

    unsigned size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    T& operator[](size_t index) const
    {
        ASSERT(index < m_size);
        return m_data[index];
    }
    T& first() const { return (*this)[0]; }
    T& last() const { return (*this)[m_size - 1]; }
    T* begin() const { return m_data; }
    T* end() const { return m_data + m_size; }

private:
    T* m_data { nullptr };
    unsigned m_size { 0 };
};

// Where a parse puts its tokens' payloads and the syntax tree. Nothing in it is destroyed one by one.
class Arena {
    WTF_MAKE_NONCOPYABLE(Arena);
public:
    Arena() = default;

    ~Arena()
    {
        for (void* block : m_blocks)
            fastFree(block);
    }

    void* allocate(size_t size)
    {
        size = roundUpToMultipleOf<8>(size);
        if (static_cast<size_t>(m_limit - m_cursor) < size) [[unlikely]]
            return allocateSlow(size);
        void* result = m_cursor;
        m_cursor += size;
        return result;
    }

    template<typename T, typename... Arguments>
    T* create(Arguments&&... arguments)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        static_assert(alignof(T) <= 8);
        return new (NotNull, allocate(sizeof(T))) T(std::forward<Arguments>(arguments)...);
    }

    template<typename T>
    Seq<T> copy(std::span<const T> items)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (items.empty())
            return { };
        T* data = static_cast<T*>(allocate(items.size_bytes()));
        memcpy(static_cast<void*>(data), items.data(), items.size_bytes());
        return { data, items.size() };
    }

    template<typename T, size_t inlineCapacity>
    Seq<T> copy(const Vector<T, inlineCapacity>& items) { return copy(std::span<const T> { items.span() }); }

    template<typename T>
    Seq<T> single(T item) { return copy(std::span<const T> { &item, 1 }); }

    // The string lives as long as the arena.
    StringImpl* retain(const String& string)
    {
        ASSERT(!string.isNull());
        m_strings.append(string);
        return string.impl();
    }

private:
    static constexpr size_t blockSize = 64 * KB;

    NEVER_INLINE void* allocateSlow(size_t size)
    {
        if (size > blockSize / 4) {
            void* block = fastMalloc(size);
            m_blocks.append(block);
            return block;
        }
        m_cursor = static_cast<char*>(fastMalloc(blockSize));
        m_limit = m_cursor + blockSize;
        m_blocks.append(m_cursor);
        void* result = m_cursor;
        m_cursor += size;
        return result;
    }

    char* m_cursor { nullptr };
    char* m_limit { nullptr };
    Vector<void*> m_blocks;
    Vector<String> m_strings;
};

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
