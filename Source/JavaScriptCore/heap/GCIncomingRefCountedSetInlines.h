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

#include "GCIncomingRefCountedSet.h"
#include "VM.h"

namespace JSC {

template<typename T>
GCIncomingRefCountedSet<T>::GCIncomingRefCountedSet()
    : m_bytes { 0 }
{
}

template<typename T>
void GCIncomingRefCountedSet<T>::lastChanceToFinalize()
{
    Locker locker { m_lock };
    for (size_t i = m_vector.size(); i--;)
        m_vector[i]->filterIncomingReferences([] (JSCell*) { return false; });
}

template<typename T>
bool GCIncomingRefCountedSet<T>::addReference(JSCell* cell, T* object)
{
    // Serializes both the set's vector append and the object's own
    // incoming-reference storage mutation (addIncomingReference). Readers of
    // that per-object storage outside this set's API (ArrayBuffer detach /
    // wasm-grow walks) snapshot under this same lock via
    // Heap::arrayBufferIncomingReferencesLock() (see GCIncomingRefCounted.h).
    Locker locker { m_lock };
    if (!object->addIncomingReference(cell)) {
        ASSERT(object->isDeferred());
        ASSERT(object->numberOfIncomingReferences());
        return false;
    }
    m_vector.append(object);
    m_bytes.storeRelaxed(m_bytes.loadRelaxed() + object->gcSizeEstimateInBytes());
    ASSERT(object->isDeferred());
    ASSERT(object->numberOfIncomingReferences());
    return true;
}

template<typename T>
void GCIncomingRefCountedSet<T>::sweep(VM& vm, CollectionScope collectionScope)
{
    // GC end-phase runs stopped-world today, but take the lock anyway: it is
    // uncontended there, and it keeps the invariant "all access to m_vector
    // and per-object incoming-reference storage is under m_lock" auditable.
    Locker locker { m_lock };
    size_t preciseBytes = 0;
    m_vector.removeAllMatching([&](T* object) {
        size_t size = object->gcSizeEstimateInBytes();
        ASSERT(object->isDeferred());
        ASSERT(object->numberOfIncomingReferences());
        if (!object->filterIncomingReferences([&] (JSCell* cell) { return vm.heap.isMarked(cell); })) {
            preciseBytes += size;
            return false;
        }
        return true;
    });
    // Update m_bytes to the precise value when Full-GC happens since Eden-GC only expects that Eden region is collected.
    if (collectionScope == CollectionScope::Full)
        m_bytes.storeRelaxed(preciseBytes);
}

} // namespace JSC
