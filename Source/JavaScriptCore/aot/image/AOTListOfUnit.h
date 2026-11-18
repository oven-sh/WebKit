/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class BlocksOfUnits;

template<typename T>
inline constexpr bool holdsListsOfUnit = false;

template<typename T, size_t inlineCapacity>
class InlineRoomOfListOfUnit {
protected:
    T* inlineRoom() { return reinterpret_cast<T*>(m_room); }
    const T* inlineRoom() const { return reinterpret_cast<const T*>(m_room); }

private:
    alignas(T) std::byte m_room[inlineCapacity * sizeof(T)];
};

template<typename T>
class InlineRoomOfListOfUnit<T, 0> {
protected:
    T* inlineRoom() const { return nullptr; }
};

template<typename T, size_t inlineCapacity = 0>
class ListOfUnit : private InlineRoomOfListOfUnit<T, inlineCapacity> {
    using InlineRoom = InlineRoomOfListOfUnit<T, inlineCapacity>;
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    ListOfUnit() { }
    ListOfUnit(std::initializer_list<T> elements) { append(std::span<const T>(elements.begin(), elements.size())); }
    explicit ListOfUnit(std::span<const T> elements) { append(elements); }
    ListOfUnit(const ListOfUnit& other) { append(other.span()); }
    ListOfUnit(ListOfUnit&& other) { takeFrom(other); }
    template<size_t otherInlineCapacity>
    ListOfUnit(const Vector<T, otherInlineCapacity>& other) { append(other.span()); }
    template<size_t otherInlineCapacity>
    ListOfUnit(Vector<T, otherInlineCapacity>&& other)
    {
        size_t size = other.size();
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (size > inlineCapacity && other.capacity() > otherInlineCapacity) {
                RELEASE_ASSERT(size <= maxCapacity);
                m_buffer = takeBufferOf(other);
                m_size = m_capacity = static_cast<uint32_t>(size);
                return;
            }
        }
        reserveCapacity(size);
        for (T& element : other)
            new (NotNull, begin() + m_size++) T(WTF::move(element));
        other.clear();
    }
    ~ListOfUnit() { release(); }

    ListOfUnit& operator=(const ListOfUnit& other)
    {
        if (this != &other) {
            shrink(0);
            append(other.span());
        }
        return *this;
    }
    ALWAYS_INLINE ListOfUnit& operator=(ListOfUnit&& other)
    {
        if (this != &other) {
            release();
            takeFrom(other);
        }
        return *this;
    }

    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    size_t capacity() const { return m_buffer ? m_capacity & maxCapacity : inlineCapacity; }
    bool liesOnHeap() const { return m_buffer && !(m_capacity & liesInBlockBit); }
    bool liesInBlock() const { return m_buffer && m_capacity & liesInBlockBit; }

    T* begin() { return m_buffer ? m_buffer : InlineRoom::inlineRoom(); }
    T* end() { return begin() + m_size; }
    const T* begin() const { return m_buffer ? m_buffer : InlineRoom::inlineRoom(); }
    const T* end() const { return begin() + m_size; }
    std::span<const T> span() const { return { begin(), m_size }; }
    std::span<T> mutableSpan() { return { begin(), m_size }; }
    std::span<const T> subspan(size_t offset, size_t length = std::dynamic_extent) const { return span().subspan(offset, length); }

    T& at(size_t index)
    {
        RELEASE_ASSERT(index < m_size);
        return begin()[index];
    }
    const T& at(size_t index) const
    {
        RELEASE_ASSERT(index < m_size);
        return begin()[index];
    }
    T& operator[](size_t index) { return at(index); }
    const T& operator[](size_t index) const { return at(index); }
    T& first() { return at(0); }
    const T& first() const { return at(0); }
    T& last() { return at(size() - 1); }
    const T& last() const { return at(size() - 1); }

    size_t findIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const
    {
        for (size_t index = 0; index < m_size; ++index) {
            if (matches(begin()[index]))
                return index;
        }
        return notFound;
    }
    size_t find(const auto& value) const
    {
        return findIf([&](const T& element) {
            return element == value;
        });
    }
    bool contains(const auto& value) const { return find(value) != notFound; }
    bool containsIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const { return findIf(matches) != notFound; }

    void reserveCapacity(size_t newCapacity)
    {
        if (newCapacity > capacity())
            moveToHeap(newCapacity);
    }
    void reserveInitialCapacity(size_t newCapacity) { reserveCapacity(newCapacity); }

    void append(const T& value)
    {
        if (m_size < capacity()) {
            new (NotNull, end()) T(value);
            ++m_size;
            return;
        }
        append(T(value));
    }
    ALWAYS_INLINE void append(T&& value)
    {
        if (m_size == capacity()) [[unlikely]] {
            appendToFullList(WTF::move(value));
            return;
        }
        new (NotNull, end()) T(WTF::move(value));
        ++m_size;
    }
    template<typename... Arguments>
    ALWAYS_INLINE void appendWithinCapacity(Arguments&&... arguments)
    {
        RELEASE_ASSERT(m_size < capacity());
        new (NotNull, end()) T { std::forward<Arguments>(arguments)... };
        ++m_size;
    }
    template<typename U, size_t extent>
    void append(std::span<U, extent> elements)
    {
        if (elements.empty())
            return;
        makeRoomFor(size() + elements.size());
        T* place = end();
        if constexpr (std::is_same_v<std::remove_const_t<U>, T> && std::is_trivially_copy_constructible_v<T>)
            memcpy(static_cast<void*>(place), elements.data(), elements.size_bytes());
        else {
            for (const U& element : elements)
                new (NotNull, place++) T(element);
        }
        m_size += static_cast<uint32_t>(elements.size());
    }
    void appendList(std::initializer_list<T> elements) { append(std::span<const T>(elements.begin(), elements.size())); }
    void appendVector(const auto& other) { append(other.span()); }

    void shrink(size_t newSize)
    {
        RELEASE_ASSERT(newSize <= m_size);
        destroy(begin() + newSize, end());
        m_size = static_cast<uint32_t>(newSize);
    }
    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= m_size);
        makeRoomFor(newSize);
        if constexpr (!std::is_trivially_default_constructible_v<T>) {
            T* newEnd = begin() + newSize;
            for (T* place = end(); place != newEnd; ++place)
                new (NotNull, place) T();
        }
        m_size = static_cast<uint32_t>(newSize);
    }
    void resize(size_t newSize)
    {
        if (newSize <= m_size)
            shrink(newSize);
        else
            grow(newSize);
    }
    void clear() { release(); }

    void fill(const T& value, size_t newSize)
    {
        shrink(0);
        insertFill(0, value, newSize);
    }
    void insertFill(size_t position, const T& value, size_t count)
    {
        RELEASE_ASSERT(position <= m_size);
        T copy(value);
        makeRoomFor(size() + count);
        T* place = begin() + position;
        memmove(static_cast<void*>(place + count), place, (m_size - position) * sizeof(T));
        for (T* end = place + count; place != end; ++place)
            new (NotNull, place) T(copy);
        m_size += static_cast<uint32_t>(count);
    }

    void removeAt(size_t position, size_t count = 1)
    {
        RELEASE_ASSERT(position <= m_size && count <= m_size - position);
        T* place = begin() + position;
        destroy(place, place + count);
        memmove(static_cast<void*>(place), place + count, (m_size - position - count) * sizeof(T));
        m_size -= static_cast<uint32_t>(count);
    }
    void removeLast() { shrink(size() - 1); }
    T takeLast()
    {
        T result = WTF::move(last());
        removeLast();
        return result;
    }

    friend bool operator==(const ListOfUnit& a, const ListOfUnit& b)
    {
        if (a.m_size != b.m_size)
            return false;
        if (a.m_size > mostElementsComparedOneByOne)
            return std::ranges::equal(a.span(), b.span());
        const T* elementsOfA = a.begin();
        const T* elementsOfB = b.begin();
        for (uint32_t index = 0; index < a.m_size; ++index) {
            if (!(elementsOfA[index] == elementsOfB[index]))
                return false;
        }
        return true;
    }
    template<size_t otherInlineCapacity>
    friend bool operator==(const ListOfUnit& a, const Vector<T, otherInlineCapacity>& b) { return std::ranges::equal(a.span(), b.span()); }

private:
    friend class BlocksOfUnits;

    static_assert(sizeof(T) && alignof(T) <= alignof(uint64_t));

    static constexpr uint32_t liesInBlockBit = 1u << 31;
    static constexpr uint32_t maxCapacity = liesInBlockBit - 1;
    static constexpr size_t leastCapacityOnHeap = 16;
    static constexpr uint32_t mostElementsComparedOneByOne = 8;

    static void moveAsBytes(T* to, T* from, size_t count)
    {
        static_assert(holdsListsOfUnit<T> || (std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>));
        if (count)
            memcpy(static_cast<void*>(to), from, count * sizeof(T));
    }

    static void moveWholeInlineRoomAsBytes(T* to, T* from)
    {
        static_assert(holdsListsOfUnit<T> || (std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>));
        memcpy(static_cast<void*>(to), from, inlineCapacity * sizeof(T));
    }

    static void destroy(T* begin, T* end)
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (; begin != end; ++begin)
                begin->~T();
        }
    }

    template<size_t otherInlineCapacity>
    static T* takeBufferOf(Vector<T, otherInlineCapacity>& other)
    {
        using ReleasedBuffer = decltype(other.releaseBuffer());
        alignas(ReleasedBuffer) std::byte neverDestroyed[sizeof(ReleasedBuffer)];
        return (new (NotNull, neverDestroyed) ReleasedBuffer(other.releaseBuffer()))->leakSpan().data();
    }

    void takeFrom(ListOfUnit& other)
    {
        T* buffer = other.m_buffer;
        uint32_t size = other.m_size;
        uint32_t capacity = other.m_capacity;
        other.m_buffer = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
        m_buffer = buffer;
        m_size = size;
        m_capacity = capacity;
        if constexpr (!!inlineCapacity) {
            if (!buffer && size)
                moveWholeInlineRoomAsBytes(InlineRoom::inlineRoom(), other.InlineRoom::inlineRoom());
        }
    }

    NEVER_INLINE void appendToFullList(T&& value)
    {
        T moved(WTF::move(value));
        makeRoomFor(size() + 1);
        new (NotNull, end()) T(WTF::move(moved));
        ++m_size;
    }

    void release()
    {
        destroy(begin(), end());
        if (liesOnHeap())
            WTF::VectorBufferMalloc::free(m_buffer);
        m_buffer = nullptr;
        m_size = 0;
        m_capacity = 0;
    }

    ALWAYS_INLINE void makeRoomFor(size_t newSize)
    {
        if (newSize > capacity())
            moveToLargerRoomOnHeap(newSize);
    }

    NEVER_INLINE void moveToLargerRoomOnHeap(size_t newSize)
    {
        moveToHeap(std::max({ newSize, leastCapacityOnHeap, std::min<size_t>(2 * capacity(), maxCapacity) }));
    }

    void moveToHeap(size_t newCapacity)
    {
        RELEASE_ASSERT(newCapacity <= maxCapacity && newCapacity >= m_size);
        T* buffer = static_cast<T*>(WTF::VectorBufferMalloc::malloc(newCapacity * sizeof(T)));
        moveAsBytes(buffer, begin(), m_size);
        if (liesOnHeap())
            WTF::VectorBufferMalloc::free(m_buffer);
        m_buffer = buffer;
        m_capacity = static_cast<uint32_t>(newCapacity);
    }

    bool needsRoomInBlock() const { return liesOnHeap() && m_size > inlineCapacity; }

    void moveToBlock(T* room)
    {
        ASSERT(needsRoomInBlock());
        moveAsBytes(room, m_buffer, m_size);
        WTF::VectorBufferMalloc::free(m_buffer);
        m_buffer = room;
        m_capacity = m_size | liesInBlockBit;
    }

    T* leaveMemoryToBlocks()
    {
        ASSERT(liesOnHeap());
        m_capacity |= liesInBlockBit;
        return m_buffer;
    }

    void moveFromHeapIntoItself()
    {
        ASSERT(liesOnHeap() && !needsRoomInBlock());
        T* buffer = std::exchange(m_buffer, nullptr);
        m_capacity = 0;
        moveAsBytes(InlineRoom::inlineRoom(), buffer, m_size);
        WTF::VectorBufferMalloc::free(buffer);
    }

    T* m_buffer { nullptr };
    uint32_t m_size { 0 };
    uint32_t m_capacity { 0 };
};

template<typename T, size_t inlineCapacity>
inline constexpr bool holdsListsOfUnit<ListOfUnit<T, inlineCapacity>> = true;

template<typename Part>
class RarePartOfUnit {
public:
    RarePartOfUnit() = default;
    RarePartOfUnit(const RarePartOfUnit& other) { copyFrom(other); }
    RarePartOfUnit(RarePartOfUnit&& other)
        : m_bits(std::exchange(other.m_bits, 0))
    {
    }
    ALWAYS_INLINE ~RarePartOfUnit() { release(); }

    RarePartOfUnit& operator=(const RarePartOfUnit& other)
    {
        if (this != &other) {
            release();
            copyFrom(other);
        }
        return *this;
    }
    RarePartOfUnit& operator=(RarePartOfUnit&& other)
    {
        if (this != &other) {
            release();
            m_bits = std::exchange(other.m_bits, 0);
        }
        return *this;
    }

    bool isEmpty() const { return !m_bits; }
    bool liesOnHeap() const { return m_bits && !(m_bits & liesInBlockBit); }
    bool liesInBlock() const { return m_bits & liesInBlockBit; }

    const Part& operator*() const { return m_bits ? *part() : partOfUnitsWithoutOne(); }
    const Part* operator->() const { return &**this; }
    Part* ifExists() { return part(); }
    Part& ensure()
    {
        if (!m_bits)
            m_bits = reinterpret_cast<uintptr_t>(new (NotNull, WTF::VectorBufferMalloc::malloc(sizeof(Part))) Part);
        return *part();
    }

private:
    friend class BlocksOfUnits;

    static_assert(holdsListsOfUnit<Part> && alignof(Part) > 1 && alignof(Part) <= alignof(uint64_t));

    static constexpr uintptr_t liesInBlockBit = 1;

    static const Part& partOfUnitsWithoutOne()
    {
        static NeverDestroyed<Part> emptyPart;
        return emptyPart.get();
    }

    Part* part() const { return reinterpret_cast<Part*>(m_bits & ~liesInBlockBit); }

    void copyFrom(const RarePartOfUnit& other)
    {
        if (other.m_bits)
            m_bits = reinterpret_cast<uintptr_t>(new (NotNull, WTF::VectorBufferMalloc::malloc(sizeof(Part))) Part(*other.part()));
    }

    ALWAYS_INLINE void release()
    {
        if (!m_bits)
            return;
        part()->~Part();
        if (liesOnHeap())
            WTF::VectorBufferMalloc::free(part());
        m_bits = 0;
    }

    void moveToBlock(Part* room)
    {
        ASSERT(liesOnHeap());
        memcpy(static_cast<void*>(room), part(), sizeof(Part));
        WTF::VectorBufferMalloc::free(part());
        m_bits = reinterpret_cast<uintptr_t>(room) | liesInBlockBit;
    }

    uintptr_t m_bits { 0 };
};

class BlocksOfUnits {
    WTF_MAKE_NONCOPYABLE(BlocksOfUnits);
public:
    static constexpr size_t sizeOfBlock = MB;
    static constexpr size_t mostBytesBesideOthers = sizeOfBlock / 4;

    BlocksOfUnits() = default;
    BlocksOfUnits(BlocksOfUnits&& other) { takeFrom(other); }
    BlocksOfUnits& operator=(BlocksOfUnits&& other)
    {
        if (this != &other) {
            freeBlocks();
            takeFrom(other);
        }
        return *this;
    }
    ~BlocksOfUnits() { freeBlocks(); }

    template<typename Record>
    Record* keep(Record&& record)
    {
        static_assert(!std::is_reference_v<Record> && alignof(Record) <= alignment);
        Measurer measure { sizeof(Record) };
        forEachListOfUnitIn(record, measure);
        uint8_t* room = allocate(measure.m_offset);
        Record* kept = new (NotNull, room) Record(WTF::move(record));
        Mover move { *this, room, sizeof(Record) };
        forEachListOfUnitIn(*kept, move);
        RELEASE_ASSERT(move.m_offset == measure.m_offset);
        return kept;
    }

    size_t numberOfBlocks() const { return m_blocks.size(); }
    size_t sizeInBytes() const { return m_sizeInBytes; }

private:
    static constexpr size_t alignment = alignof(uint64_t);

    template<typename T>
    static size_t placeFor(size_t offset) { return WTF::roundUpToMultipleOf<alignof(T)>(offset); }

    template<typename T, size_t inlineCapacity>
    static bool staysWhereItLies(const ListOfUnit<T, inlineCapacity>& list)
    {
        size_t size = list.size() * sizeof(T);
        return size > mostBytesBesideOthers && list.liesOnHeap() && list.capacity() * sizeof(T) - size <= size / 8;
    }

    struct Measurer {
        template<typename T, size_t inlineCapacity>
        ALWAYS_INLINE void operator()(ListOfUnit<T, inlineCapacity>& list)
        {
            if (list.needsRoomInBlock() && !staysWhereItLies(list))
                m_offset = placeFor<T>(m_offset) + list.size() * sizeof(T);
            if constexpr (holdsListsOfUnit<T>) {
                for (T& element : list)
                    forEachListOfUnitIn(element, *this);
            }
        }

        template<typename Part>
        ALWAYS_INLINE void operator()(RarePartOfUnit<Part>& rarePart)
        {
            if (rarePart.isEmpty())
                return;
            if (rarePart.liesOnHeap())
                m_offset = placeFor<Part>(m_offset) + sizeof(Part);
            forEachListOfUnitIn(*rarePart.ifExists(), *this);
        }

        size_t m_offset;
    };

    struct Mover {
        template<typename T, size_t inlineCapacity>
        ALWAYS_INLINE void operator()(ListOfUnit<T, inlineCapacity>& list)
        {
            if (list.liesOnHeap())
                moveFromHeap(list);
            if constexpr (holdsListsOfUnit<T>) {
                for (T& element : list)
                    forEachListOfUnitIn(element, *this);
            }
        }

        template<typename Part>
        ALWAYS_INLINE void operator()(RarePartOfUnit<Part>& rarePart)
        {
            if (rarePart.isEmpty())
                return;
            if (rarePart.liesOnHeap()) {
                m_offset = placeFor<Part>(m_offset);
                rarePart.moveToBlock(reinterpret_cast<Part*>(m_room + m_offset));
                m_offset += sizeof(Part);
            }
            forEachListOfUnitIn(*rarePart.ifExists(), *this);
        }

        template<typename T, size_t inlineCapacity>
        void moveFromHeap(ListOfUnit<T, inlineCapacity>& list)
        {
            if (staysWhereItLies(list)) {
                size_t size = list.capacity() * sizeof(T);
                m_blocks.addBlock(reinterpret_cast<uint8_t*>(list.leaveMemoryToBlocks()), size);
            } else if (list.size() > inlineCapacity) {
                m_offset = placeFor<T>(m_offset);
                list.moveToBlock(reinterpret_cast<T*>(m_room + m_offset));
                m_offset += list.size() * sizeof(T);
            } else
                list.moveFromHeapIntoItself();
        }

        BlocksOfUnits& m_blocks;
        uint8_t* m_room;
        size_t m_offset;
    };

    uint8_t* allocate(size_t size)
    {
        size = WTF::roundUpToMultipleOf<alignment>(size);
        if (size > mostBytesBesideOthers)
            return allocateBlock(size);
        if (size > static_cast<size_t>(m_end - m_next)) {
            m_next = allocateBlock(sizeOfBlock);
            m_end = m_next + sizeOfBlock;
        }
        return std::exchange(m_next, m_next + size);
    }

    uint8_t* allocateBlock(size_t size)
    {
        uint8_t* block = static_cast<uint8_t*>(WTF::VectorBufferMalloc::malloc(size));
        addBlock(block, size);
        return block;
    }

    void addBlock(uint8_t* block, size_t size)
    {
        m_blocks.append(block);
        m_sizeInBytes += size;
    }

    void takeFrom(BlocksOfUnits& other)
    {
        m_blocks = std::exchange(other.m_blocks, { });
        m_next = std::exchange(other.m_next, nullptr);
        m_end = std::exchange(other.m_end, nullptr);
        m_sizeInBytes = std::exchange(other.m_sizeInBytes, 0);
    }

    void freeBlocks()
    {
        for (uint8_t* block : m_blocks)
            WTF::VectorBufferMalloc::free(block);
        m_blocks.clear();
    }

    Vector<uint8_t*> m_blocks;
    uint8_t* m_next { nullptr };
    uint8_t* m_end { nullptr };
    size_t m_sizeInBytes { 0 };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
