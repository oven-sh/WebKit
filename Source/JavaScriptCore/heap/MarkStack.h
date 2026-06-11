/*
 * Copyright (C) 2009-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "GCSegmentedArray.h"
#include <wtf/Lock.h>

namespace JSC {

class JSCell;

class MarkStackArray : public GCSegmentedArray<const JSCell*> {
public:
    MarkStackArray();

    void transferTo(MarkStackArray&);
    size_t transferTo(MarkStackArray&, size_t limit); // Optimized for when `limit` is small.
    void donateSomeCellsTo(MarkStackArray&);
    void stealSomeCellsFrom(MarkStackArray&, size_t idleThreadCount);

    // GIL-off shared-GC-heap: a MarkStackArray reachable from more than one
    // mutator (Heap::m_mutatorMarkStack via the write-barrier slow path,
    // Heap::addToRememberedSet) must serialize append().
    // GCSegmentedArray<T>::postIncTop() is a non-atomic read-modify-write of
    // BOTH the cached m_top and the head segment's m_top, and append() can
    // also expand() the segment list at the capacity boundary. Opt-in per
    // instance so per-SlotVisitor stacks on the parallel-marking hot path
    // stay lock-free; the global shared stacks are already serialized by
    // Heap::m_markingMutex and the race stack by m_raceMarkStackLock.
    void setMultiProducerAccess() { m_multiProducerAccess = true; }

    ALWAYS_INLINE void append(const JSCell* cell)
    {
        if (m_multiProducerAccess) [[unlikely]] {
            Locker locker { m_appendLock };
            GCSegmentedArray<const JSCell*>::append(cell);
            return;
        }
        GCSegmentedArray<const JSCell*>::append(cell);
    }

    // The lock serializes producers only. Drains of a flagged instance must
    // run with all multi-producer mutators stopped (transferTo runs during
    // constraint solving, clear during Heap teardown/last-chance). Holding
    // m_appendLock across them is belt-and-suspenders: it is uncontended when
    // the world is stopped, and it turns a straggler-mutator append during a
    // drain from silent segment-list corruption into a bounded wait.
    ALWAYS_INLINE void clear()
    {
        if (m_multiProducerAccess) [[unlikely]] {
            Locker locker { m_appendLock };
            GCSegmentedArray<const JSCell*>::clear();
            return;
        }
        GCSegmentedArray<const JSCell*>::clear();
    }

private:
    void transferToImpl(MarkStackArray&);
    size_t transferToImpl(MarkStackArray&, size_t limit);

    Lock m_appendLock;
    bool m_multiProducerAccess { false };
};

} // namespace JSC
