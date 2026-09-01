/*
 * Copyright (C) 2017-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Atomics.h>
#include <wtf/ConcurrentBuffer.h>
#include <wtf/Noncopyable.h>

namespace WTF {

// An iterator for ConcurrentVector. It supports only the pre ++ operator
template <typename T, size_t SegmentSize = 8> class ConcurrentVector;
template <typename T, size_t SegmentSize = 8> class ConcurrentVectorIterator {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ConcurrentVectorIterator);
private:
    friend class ConcurrentVector<T, SegmentSize>;
public:
    typedef ConcurrentVectorIterator<T, SegmentSize> Iterator;

    ~ConcurrentVectorIterator() { }

    T& operator*() const { return m_vector.at(m_index); }
    T* operator->() const { return &m_vector.at(m_index); }

    // Only prefix ++ operator supported
    Iterator& operator++()
    {
        m_index++;
        return *this;
    }

    bool operator==(const Iterator& other) const
    {
        return m_index == other.m_index && &m_vector == &other.m_vector;
    }

    ConcurrentVectorIterator& operator=(const ConcurrentVectorIterator<T, SegmentSize>& other)
    {
        m_vector = other.m_vector;
        m_index = other.m_index;
        return *this;
    }

private:
    ConcurrentVectorIterator(ConcurrentVector<T, SegmentSize>& vector, size_t index)
        : m_vector(vector)
        , m_index(index)
    {
    }

    ConcurrentVector<T, SegmentSize>& m_vector;
    size_t m_index;
};

// ConcurrentVector is like SegmentedVector, but suitable for scenarios where one thread appends
// elements and another thread continues to access elements at lower indices. Only one thread can
// append at a time, so that activity still needs locking. size() and last() are racy with append(),
// in the sense that last() may crash if an append() is running concurrently because size()-1 does yet
// have a segment. If you want size() to be safe with additions use appendConcurrently() but note
// appendConcurrently() is not multi-writer safe.
//
// Typical users of ConcurrentVector already have some way of ensuring that by the time someone is
// trying to use an index, some synchronization has happened to ensure that this index contains fully
// initialized data. Thereafter, the keeper of that index is allowed to use it on this vector without
// any locking other than what is needed to protect the integrity of the element at that index. This
// works because we guarantee shrinking the vector is impossible and that growing the vector doesn't
// delete old vector spines.
template <typename T, size_t SegmentSize>
class ConcurrentVector final {
    friend class ConcurrentVectorIterator<T, SegmentSize>;
    WTF_MAKE_NONCOPYABLE(ConcurrentVector);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ConcurrentVector);

public:
    typedef ConcurrentVectorIterator<T, SegmentSize> Iterator;

    ConcurrentVector() = default;

    // This may return a size that is bigger than the underlying storage, since this does not fence
    // manipulations of size. So if you access at size()-1, you may crash because this hasn't
    // allocated storage for that index yet.
    // THREADS/TSAN: m_size, m_numSegments and the segment-pointer slots are
    // accessed with relaxed/release atomics — this container's contract is
    // single writer + concurrent lock-free readers (appendConcurrently), so the
    // races are by design; the atomics make them defined and TSAN-visible.
    // Codegen on x86-64/arm64 is unchanged for the loads/plain stores.
    // TSAN r13: acquire under TSAN ONLY — pairs with appendConcurrently's
    // release store so the element-construction writes (placement-new /
    // memset) become HB-visible to readers; production keeps relaxed (the
    // contract is dependency/fence-ordered, which TSAN cannot model). Same
    // gate shape as JSString::fiberConcurrently (TSAN-TRIAGE §13.4).
#if TSAN_ENABLED
    size_t size() const { return WTF::atomicLoad(const_cast<size_t*>(&m_size), std::memory_order_acquire); }
#else
    size_t size() const { return WTF::atomicLoad(const_cast<size_t*>(&m_size), std::memory_order_relaxed); }
#endif

    bool isEmpty() const { return !size(); }

    T& at(size_t index) LIFETIME_BOUND
    {
        ASSERT_WITH_SECURITY_IMPLICATION(index < size());
        return segmentFor(index)->entries[subscriptFor(index)];
    }

    const T& at(size_t index) const LIFETIME_BOUND
    {
        return const_cast<ConcurrentVector<T, SegmentSize>*>(this)->at(index);
    }

    T& operator[](size_t index) LIFETIME_BOUND
    {
        return at(index);
    }

    const T& operator[](size_t index) const LIFETIME_BOUND
    {
        return at(index);
    }

    T& first() LIFETIME_BOUND
    {
        ASSERT_WITH_SECURITY_IMPLICATION(!isEmpty());
        return at(0);
    }
    const T& first() const LIFETIME_BOUND
    {
        ASSERT_WITH_SECURITY_IMPLICATION(!isEmpty());
        return at(0);
    }
    
    // This may crash if run concurrently to append(). If you want to accurately track the size of
    // this vector, use appendConcurrently().
    T& last() LIFETIME_BOUND
    {
        ASSERT_WITH_SECURITY_IMPLICATION(!isEmpty());
        return at(size() - 1);
    }
    const T& last() const LIFETIME_BOUND
    {
        ASSERT_WITH_SECURITY_IMPLICATION(!isEmpty());
        return at(size() - 1);
    }

    T takeLast()
    {
        ASSERT_WITH_SECURITY_IMPLICATION(!isEmpty());
        T result = WTF::move(last());
        WTF::atomicStore(&m_size, size() - 1, std::memory_order_relaxed);
        return result;
    }

    template<typename... Args>
    void append(Args&&... args)
    {
        size_t newSize = size() + 1;
        WTF::atomicStore(&m_size, newSize, std::memory_order_relaxed);
        if (!segmentExistsFor(newSize - 1))
            allocateSegment();
        new (NotNull, &last()) T(std::forward<Args>(args)...);
    }

    template<typename... Args>
    T& alloc(Args&&... args) LIFETIME_BOUND
    {
        append(std::forward<Args>(args)...);
        return last();
    }

    // Note, appendConcurrently() assumes only one thread can append at a time and is not safe with removeLast()/takeLast().
    template<typename... Args>
    void appendConcurrently(Args&&... args)
    {
        size_t oldSize = size();
        if (!segmentExistsFor(oldSize))
            allocateSegment();
        T* slot = &segmentFor(oldSize)->entries[subscriptFor(oldSize)];
        new (NotNull, slot) T(std::forward<Args>(args)...);
        // Release store publishes the constructed element to concurrent readers
        // (subsumes the storeStoreFence the plain ++ used).
        WTF::atomicStore(&m_size, oldSize + 1, std::memory_order_release);
    }

    void removeLast()
    {
        last().~T();
        WTF::atomicStore(&m_size, size() - 1, std::memory_order_relaxed);
    }

    void grow(size_t newSize)
    {
        size_t oldSize = size();
        if (newSize == oldSize)
            return;
        ASSERT(newSize > oldSize);
        ensureSegmentsFor(newSize);
        WTF::atomicStore(&m_size, newSize, std::memory_order_relaxed);
        for (size_t i = oldSize; i < newSize; ++i)
            new (NotNull, &at(i)) T();
    }

    Iterator begin() LIFETIME_BOUND
    {
        return Iterator(*this, 0);
    }

    Iterator end() LIFETIME_BOUND
    {
        return Iterator(*this, size());
    }

private:
    struct Segment {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Segment);
            
        std::array<T, SegmentSize> entries;
    };

    bool segmentExistsFor(size_t index)
    {
        return index / SegmentSize < WTF::atomicLoad(&m_numSegments, std::memory_order_relaxed);
    }

    Segment* segmentFor(size_t index)
    {
        // Relaxed atomic read of the unique_ptr slot — allocateSegment() on the
        // writer thread publishes it with a release store before bumping
        // m_numSegments/m_size.
        // TSAN r13: acquire under TSAN ONLY (see size() above) so the fresh
        // segment's pre-publication writes get an HB edge in TSAN's model.
#if TSAN_ENABLED
        return WTF::atomicLoad(std::bit_cast<Segment**>(&m_segments[index / SegmentSize]), std::memory_order_acquire);
#else
        return WTF::atomicLoad(std::bit_cast<Segment**>(&m_segments[index / SegmentSize]), std::memory_order_relaxed);
#endif
    }

    size_t subscriptFor(size_t index)
    {
        return index % SegmentSize;
    }

    void ensureSegmentsFor(size_t size)
    {
        size_t segmentCount = (this->size() + SegmentSize - 1) / SegmentSize;
        size_t neededSegmentCount = (size + SegmentSize - 1) / SegmentSize;

        for (size_t i = segmentCount ? segmentCount - 1 : 0; i < neededSegmentCount; ++i)
            ensureSegment(i);
    }

    void ensureSegment(size_t segmentIndex)
    {
        size_t numSegments = WTF::atomicLoad(&m_numSegments, std::memory_order_relaxed);
        ASSERT_WITH_SECURITY_IMPLICATION(segmentIndex <= numSegments);
        if (segmentIndex == numSegments)
            allocateSegment();
    }

    void allocateSegment()
    {
        size_t numSegments = WTF::atomicLoad(&m_numSegments, std::memory_order_relaxed);
        m_segments.grow(numSegments + 1);
        // Release-publish the fresh segment pointer, then the count, so a
        // concurrent reader that passes segmentExistsFor() sees the pointer.
        WTF::atomicStore(std::bit_cast<Segment**>(&m_segments[numSegments]), makeUnique<Segment>().release(), std::memory_order_release);
        WTF::atomicStore(&m_numSegments, numSegments + 1, std::memory_order_release);
    }

    size_t m_size { 0 };
    ConcurrentBuffer<std::unique_ptr<Segment>> m_segments;
    size_t m_numSegments { 0 };
};

} // namespace WTF

using WTF::ConcurrentVector;
