/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

template<typename T, unsigned shiftOfFirstSegment = 8>
class NonMovingVector {
    WTF_MAKE_NONCOPYABLE(NonMovingVector);
public:
    NonMovingVector() = default;

    ~NonMovingVector()
    {
        unsigned size = this->size();
        for (unsigned index = 0; index < size; ++index)
            std::destroy_at(slotAt(index));
        for (unsigned segment = 0; segment < numberOfSegments; ++segment) {
            if (m_segments[segment])
                fastFree(m_segments[segment] + (sizeOfFirstSegment << segment));
        }
    }

    unsigned size() const { return m_size.load(std::memory_order_relaxed); }
    bool isEmpty() const { return !size(); }

    const T& operator[](size_t index) const
    {
        ASSERT(index < size());
        return *slotAt(static_cast<unsigned>(index));
    }

    T& operator[](size_t index)
    {
        ASSERT(index < size());
        return *slotAt(static_cast<unsigned>(index));
    }

    template<typename... Arguments>
    T& constructAndAppend(Arguments&&... arguments)
    {
        unsigned index = size();
        T* slot = new (NotNull, ensureSlotAt(index)) T(std::forward<Arguments>(arguments)...);
        m_size.store(index + 1, std::memory_order_release);
        return *slot;
    }

    void append(T&& value) { constructAndAppend(WTF::move(value)); }
    void append(const T& value) { constructAndAppend(value); }

    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size() && newSize <= maxSize);
        for (unsigned index = size(); index < newSize; ++index)
            new (NotNull, ensureSlotAt(index)) T();
        m_size.store(static_cast<unsigned>(newSize), std::memory_order_release);
    }

    // The caller must call constructAt() once for every new index, on any thread, before the element is used.
    void growWithoutConstructing(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size() && newSize <= maxSize);
        if (newSize == size())
            return;
        for (unsigned segment = placeOf(size()).segment, last = placeOf(static_cast<unsigned>(newSize) - 1).segment; segment <= last; ++segment)
            ensureSlotAt((sizeOfFirstSegment << segment) - sizeOfFirstSegment);
        m_size.store(static_cast<unsigned>(newSize), std::memory_order_release);
    }

    template<typename... Arguments>
    void constructAt(unsigned index, Arguments&&... arguments)
    {
        ASSERT(index < size());
        new (NotNull, slotAt(index)) T(std::forward<Arguments>(arguments)...);
    }

private:
    static constexpr unsigned sizeOfFirstSegment = 1u << shiftOfFirstSegment;
    static constexpr unsigned numberOfSegments = 32 - shiftOfFirstSegment;
    static constexpr unsigned maxSize = UINT32_MAX - sizeOfFirstSegment;

    static_assert(alignof(T) <= 16);

    struct Place {
        unsigned segment;
        unsigned offset;
        unsigned sizeOfSegment;
    };

    static Place placeOf(unsigned index)
    {
        unsigned position = index + sizeOfFirstSegment;
        unsigned shift = std::bit_width(position) - 1;
        return { shift - shiftOfFirstSegment, position - (1u << shift), 1u << shift };
    }

    T* slotAt(unsigned index) const
    {
        // m_segments[n] points sizeOfFirstSegment << n elements before the segment, so that the position is the offset.
        unsigned position;
        if (__builtin_add_overflow(index, sizeOfFirstSegment, &position)) [[unlikely]]
            CRASH();
        return m_segments[31 - __builtin_clz(position) - shiftOfFirstSegment] + position;
    }

    T* ensureSlotAt(unsigned index)
    {
        RELEASE_ASSERT(index < maxSize);
        Place place = placeOf(index);
        T*& segment = m_segments[place.segment];
        if (!segment)
            segment = static_cast<T*>(fastMalloc(sizeof(T) * static_cast<size_t>(place.sizeOfSegment))) - place.sizeOfSegment;
        return segment + place.offset + place.sizeOfSegment;
    }

    std::array<T*, numberOfSegments> m_segments { };
    std::atomic<unsigned> m_size { 0 };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
