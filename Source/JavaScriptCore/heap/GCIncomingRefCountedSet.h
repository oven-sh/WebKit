/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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

#include "CollectionScope.h"
#include "GCIncomingRefCounted.h"
#include <wtf/Atomics.h>
#include <wtf/Lock.h>

namespace JSC {

class VM;

// T = some subtype of GCIncomingRefCounted, must support a gcSizeEstimateInBytes()
// method.
template<typename T>
class GCIncomingRefCountedSet {
public:
    GCIncomingRefCountedSet();

    void lastChanceToFinalize();

    // Returns true if the native object is new to this set.
    bool addReference(JSCell*, T*);
    
    void sweep(VM&, CollectionScope);

    size_t size() const { return m_bytes.loadRelaxed(); };

    // The lock guarding both this set's vector AND every member object's
    // incoming-reference storage against concurrent mutators. Readers of that
    // storage outside this set's API (ArrayBuffer::notifyDetaching /
    // refreshAfterWasmMemoryGrow) snapshot the cell list under it — see
    // Heap::arrayBufferIncomingReferencesLock() and the GCIncomingRefCounted.h
    // invariant comment.
    Lock& lock() LIFETIME_BOUND { return m_lock; }

private:
    // GIL-off, N mutators reach addReference() concurrently (ArrayBuffer
    // wrapper construction) while others read a member object's
    // incoming-reference storage (detach / wasm-grow refresh); an
    // unsynchronized Vector append there races realloc against the readers.
    // So under g_jscConfig.gilOffProcess every mutator access — the append
    // and the readers' snapshot — holds m_lock. With the GIL, and flag-off,
    // one mutator runs at a time and the lock is never taken. GC end-phase
    // sweep and teardown walk the set with every mutator stopped and hold no
    // lock: deleting a member object from there runs ~ArrayBuffer, which
    // takes other locks and embedder destructors.
    // LOCK RANK: strict leaf — only a Vector append / copy (bmalloc) happens
    // under it; no other lock is acquired while it is held, and it never
    // safepoints.
    mutable Lock m_lock;
    Vector<T*> m_vector WTF_GUARDED_BY_LOCK(m_lock);
    // Byte count is read lock-free by Heap accounting (size()); writers update
    // it under m_lock, readers use a relaxed load (monotonic-ish advisory
    // counter for GC pacing; a momentarily stale value is harmless).
    Atomic<size_t> m_bytes;
};

} // namespace JSC
