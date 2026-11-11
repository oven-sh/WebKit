/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OverflowHandler.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

template<typename T>
class LargeList {
    WTF_MAKE_NONCOPYABLE(LargeList);
public:
    LargeList() = default;

    LargeList(LargeList&& other)
        : m_elements(std::exchange(other.m_elements, nullptr))
        , m_size(std::exchange(other.m_size, 0))
        , m_room(std::exchange(other.m_room, 0))
    {
    }

    LargeList& operator=(LargeList&& other)
    {
        LargeList taken(WTF::move(other));
        std::swap(m_elements, taken.m_elements);
        std::swap(m_size, taken.m_size);
        std::swap(m_room, taken.m_room);
        return *this;
    }

    ~LargeList()
    {
        std::ranges::destroy(mutableSpan());
        fastFree(m_elements);
    }

    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    size_t room() const { return m_room; }
    std::span<const T> span() const LIFETIME_BOUND { return unsafeMakeSpan(m_elements, m_size); }
    std::span<T> mutableSpan() LIFETIME_BOUND { return unsafeMakeSpan(m_elements, m_size); }
    auto begin() const LIFETIME_BOUND { return span().begin(); }
    auto end() const LIFETIME_BOUND { return span().end(); }
    auto begin() LIFETIME_BOUND { return mutableSpan().begin(); }
    auto end() LIFETIME_BOUND { return mutableSpan().end(); }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    T& operator[](size_t index) LIFETIME_BOUND { return m_elements[checked(index)]; }
    const T& operator[](size_t index) const LIFETIME_BOUND { return m_elements[checked(index)]; }
    ALWAYS_INLINE void prefetch(size_t index) const { __builtin_prefetch(m_elements + index); }
    template<typename Member> ALWAYS_INLINE void prefetch(size_t index, Member T::* member) const { __builtin_prefetch(&(m_elements[index].*member)); }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    void reserve(size_t room)
    {
        if (room <= m_room)
            return;
        if constexpr (movesAsBytes<T> || std::is_trivially_copyable_v<T>)
            m_elements = static_cast<T*>(fastRealloc(m_elements, room * sizeof(T)));
        else {
            T* elements = static_cast<T*>(fastMalloc(room * sizeof(T)));
            std::ranges::uninitialized_move(mutableSpan(), unsafeMakeSpan(elements, m_size));
            std::ranges::destroy(mutableSpan());
            fastFree(std::exchange(m_elements, elements));
        }
        m_room = room;
    }

    void grow(size_t size)
    {
        makeRoomFor(size);
        std::ranges::uninitialized_value_construct(roomAsSpan().subspan(m_size, size - m_size));
        m_size = size;
    }

    void grow(const ParallelLoops& loops, size_t size)
    {
        makeRoomFor(size);
        std::span<T> added = roomAsSpan().subspan(m_size, size - m_size);
        forEachRangeOfIndicesInParallel(loops, added.size(), [&](size_t first, size_t end) {
            std::ranges::uninitialized_value_construct(added.subspan(first, end - first));
        });
        m_size = size;
    }

    void append(T&& element)
    {
        makeRoomFor(m_size + 1);
        new (NotNull, &roomAsSpan()[m_size]) T(WTF::move(element));
        ++m_size;
    }

    void clear() { *this = { }; }

private:
    std::span<T> roomAsSpan() LIFETIME_BOUND { return unsafeMakeSpan(m_elements, m_room); }

    size_t checked(size_t index) const
    {
        if (index >= m_size) [[unlikely]]
            CrashOnOverflow::overflowed();
        return index;
    }

    void makeRoomFor(size_t size)
    {
        RELEASE_ASSERT(size >= m_size);
        if (size > m_room)
            reserve(std::max(size, m_room + m_room / 4 + 64));
    }

    T* m_elements { nullptr };
    size_t m_size { 0 };
    size_t m_room { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
