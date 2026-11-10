/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

class Arena {
    WTF_MAKE_NONCOPYABLE(Arena);
public:
    Arena() = default;
    JS_EXPORT_PRIVATE ~Arena();

    static constexpr size_t alignmentOfCursor = 8;
    static constexpr size_t smallestRoomLetGo = 4 * KB;

    static constexpr size_t bytesTakenBy(size_t bytes) { return (bytes + alignmentOfCursor - 1) & ~(alignmentOfCursor - 1); }

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t))
    {
        ASSERT(hasOneBitSet(alignment));
        uintptr_t start = alignment <= alignmentOfCursor ? m_cursor : (m_cursor + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
        ASSERT(start >= m_cursor);
        uintptr_t end = start + bytesTakenBy(bytes);
        if (end > m_end) [[unlikely]]
            return allocateSlow(bytes, alignment);
        m_cursor = end;
        return reinterpret_cast<void*>(start);
    }

    bool hasRoomFor(size_t bytes) const { return m_cursor + bytesTakenBy(bytes) <= m_end; }
    void* allocateWhereThereIsRoom(size_t bytes)
    {
        ASSERT(hasRoomFor(bytes));
        uintptr_t start = m_cursor;
        m_cursor = start + bytesTakenBy(bytes);
        return reinterpret_cast<void*>(start);
    }

    template<typename T, typename... Arguments>
    T* create(Arguments&&... arguments)
    {
        T* object = new (NotNull, allocate(sizeof(T), alignof(T))) T(std::forward<Arguments>(arguments)...);
        if constexpr (!std::is_trivially_destructible_v<T>)
            addFinalizer(object, [](void* pointer) { static_cast<T*>(pointer)->~T(); });
        return object;
    }

    template<typename T>
    T* allocateArray(size_t count)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        return static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
    }

    template<typename T>
    std::span<T> createSpan(size_t count)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        if (!count)
            return { };
        T* first = static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
        for (size_t index = 0; index < count; ++index)
            new (NotNull, first + index) T();
        return { first, count };
    }

    template<typename T>
    std::span<T> copySpan(std::span<const T> source)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        if (source.empty())
            return { };
        T* first = static_cast<T*>(allocate(source.size_bytes(), alignof(T)));
        std::uninitialized_copy_n(source.data(), source.size(), first);
        return { first, source.size() };
    }

    void letGo(void* room, size_t bytes)
    {
        if (bytes >= smallestRoomLetGo) [[unlikely]]
            letGoOfLargeRoom(reinterpret_cast<uintptr_t>(room), bytes);
    }

    template<typename T>
    void letGoOfArray(T* first, size_t count)
    {
        letGo(const_cast<std::remove_const_t<T>*>(first), bytesTakenBy(count * sizeof(T)));
    }

    JS_EXPORT_PRIVATE void reset();

    size_t bytesAllocatedSinceReset() const { return m_bytesInFullChunks + ((m_endOfChunk ? m_cursorInChunk : m_cursor) - m_begin); }

    template<typename T>
    T& objectKeptAcrossResets()
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        static constexpr char keyOfType = 0;
        for (KeptObject* kept = m_keptObjects; kept; kept = kept->next) {
            if (kept->keyOfType == &keyOfType)
                return *static_cast<T*>(kept->object);
        }
        T* object = new (NotNull, allocateKeptObject(sizeof(T), &keyOfType, [](void* pointer) { static_cast<T*>(pointer)->~T(); })) T();
        return *object;
    }

private:
    struct Chunk {
        Chunk* previous;
        size_t size;
    };
    struct Finalizer {
        Finalizer* next;
        void* object;
        void (*destroy)(void*);
    };
    struct KeptObject {
        KeptObject* next;
        const char* keyOfType;
        void* object;
        void (*destroy)(void*);
    };
    struct RoomLetGo {
        RoomLetGo* next;
        uintptr_t end;
    };

    static constexpr size_t smallestChunkSize = 64 * KB;
    static constexpr size_t largestChunkKept = MB;
    static constexpr size_t smallestRoomKept = 512;
    static_assert(smallestRoomKept >= sizeof(RoomLetGo) && smallestRoomKept <= smallestRoomLetGo);

    JS_EXPORT_PRIVATE void* allocateSlow(size_t bytes, size_t alignment);
    JS_EXPORT_PRIVATE void addFinalizer(void* object, void (*destroy)(void*));
    JS_EXPORT_PRIVATE void* allocateKeptObject(size_t bytes, const char* keyOfType, void (*destroy)(void*));
    JS_EXPORT_PRIVATE void letGoOfLargeRoom(uintptr_t begin, size_t bytes);
    bool holds(uintptr_t begin, uintptr_t end) const;
    void keepRoomLetGo(uintptr_t begin, uintptr_t end);
    void standInRoomLetGo();
    void* allocateOutsideRoomLetGo(size_t bytes, size_t alignment);
    void forgetRoomsLetGo();

    uintptr_t m_begin { 0 };
    uintptr_t m_cursor { 0 };
    uintptr_t m_end { 0 };
    size_t m_bytesInFullChunks { 0 };
    Chunk* m_chunk { nullptr };
    Finalizer* m_finalizers { nullptr };
    KeptObject* m_keptObjects { nullptr };
    RoomLetGo* m_roomsLetGo { nullptr };
    uintptr_t m_cursorInChunk { 0 };
    uintptr_t m_endOfChunk { 0 };
    bool m_forgetsRoomsLetGoAtReset { false };
};

template<typename T>
class ArenaVector {
    static_assert(std::is_trivially_destructible_v<T>);
public:
    ArenaVector() = default;

    uint32_t size() const { return m_size; }
    uint32_t capacity() const { return m_capacity; }
    bool isEmpty() const { return !m_size; }

    T& at(uint32_t index)
    {
        ASSERT(index < m_size);
        return m_data[index];
    }
    const T& at(uint32_t index) const
    {
        ASSERT(index < m_size);
        return m_data[index];
    }
    T& operator[](uint32_t index) { return at(index); }
    const T& operator[](uint32_t index) const { return at(index); }
    T& last() { return at(m_size - 1); }
    const T& last() const { return at(m_size - 1); }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }
    std::span<T> span() { return { m_data, m_size }; }
    std::span<const T> span() const { return { m_data, m_size }; }

    void append(Arena& arena, const T& value)
    {
        if (m_size == m_capacity) [[unlikely]]
            reserve(arena, m_capacity ? m_capacity * 2 : smallestCapacity);
        new (NotNull, m_data + m_size++) T(value);
    }

    void appendWithinCapacity(const T& value)
    {
        ASSERT(m_size < m_capacity);
        new (NotNull, m_data + m_size++) T(value);
    }

    T* appendUninitialized(Arena& arena)
    {
        if (m_size == m_capacity) [[unlikely]]
            reserve(arena, m_capacity ? m_capacity * 2 : smallestCapacity);
        return m_data + m_size++;
    }

    T* appendUninitialized(Arena& arena, uint32_t count)
    {
        if (m_capacity - m_size < count) [[unlikely]]
            reserve(arena, std::max(std::max(m_capacity * 2, m_size + count), smallestCapacity));
        T* first = m_data + m_size;
        m_size += count;
        return first;
    }

    void adoptStorage(T* data, uint32_t capacity)
    {
        ASSERT(!m_size && !m_capacity);
        m_data = data;
        m_capacity = capacity;
    }

    void grow(Arena& arena, uint32_t newSize)
    {
        ASSERT(newSize >= m_size);
        reserveForGrowth(arena, newSize);
        std::uninitialized_value_construct_n(m_data + m_size, newSize - m_size);
        m_size = newSize;
    }

    void growWithoutInitializing(Arena& arena, uint32_t newSize)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        ASSERT(newSize >= m_size);
        reserveForGrowth(arena, newSize);
        m_size = newSize;
    }

    void fill(Arena& arena, uint32_t newSize, const T& value)
    {
        m_size = 0;
        reserve(arena, newSize);
        std::uninitialized_fill_n(m_data, newSize, T(value));
        m_size = newSize;
    }

    void shrink(uint32_t newSize)
    {
        ASSERT(newSize <= m_size);
        m_size = newSize;
    }

    T takeLast()
    {
        T result = last();
        --m_size;
        return result;
    }

    void clear() { m_size = 0; }

    void reserve(Arena& arena, uint32_t capacity)
    {
        if (capacity <= m_capacity)
            return;
        T* data = static_cast<T*>(arena.allocate(static_cast<size_t>(capacity) * sizeof(T), alignof(T)));
        if (m_size)
            std::uninitialized_copy_n(m_data, m_size, data);
        arena.letGo(m_data, static_cast<size_t>(m_capacity) * sizeof(T));
        m_data = data;
        m_capacity = capacity;
    }

    void letGo(Arena& arena)
    {
        arena.letGo(m_data, static_cast<size_t>(m_capacity) * sizeof(T));
        m_data = nullptr;
        m_size = 0;
        m_capacity = 0;
    }

private:
    void reserveForGrowth(Arena& arena, uint32_t capacity)
    {
        if (capacity > m_capacity)
            reserve(arena, std::max(capacity, m_capacity * 2));
    }

    static constexpr uint32_t smallestCapacity = 4;

    T* m_data { nullptr };
    uint32_t m_size { 0 };
    uint32_t m_capacity { 0 };
};

class BitSpan {
public:
    BitSpan() = default;
    explicit BitSpan(std::span<uint64_t> words)
        : m_words(words)
    {
    }

    static BitSpan create(Arena& arena, size_t numberOfBits) { return BitSpan(arena.createSpan<uint64_t>((numberOfBits + 63) / 64)); }

    size_t size() const { return m_words.size() * 64; }
    bool get(size_t bit) const { return bit < size() && (m_words.data()[bit / 64] >> (bit % 64) & 1); }
    void set(size_t bit)
    {
        ASSERT(bit < size());
        m_words[bit / 64] |= uint64_t { 1 } << (bit % 64);
    }
    void clear(size_t bit)
    {
        ASSERT(bit < size());
        m_words[bit / 64] &= ~(uint64_t { 1 } << (bit % 64));
    }
    template<typename Functor>
    void forEachSetBit(const Functor& functor) const
    {
        for (size_t word = 0; word < m_words.size(); ++word) {
            for (uint64_t bits = m_words[word]; bits; bits &= bits - 1)
                functor(word * 64 + static_cast<size_t>(std::countr_zero(bits)));
        }
    }

private:
    std::span<uint64_t> m_words;
};

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
