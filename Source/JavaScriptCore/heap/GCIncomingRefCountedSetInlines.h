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
void GCIncomingRefCountedSet<T>::lastChanceToFinalize() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // Heap teardown: no mutator is left, so the set is walked unlocked.
    // filterIncomingReferences deletes the object once its incoming references
    // are gone, and that destructor takes locks of its own (see m_lock).
    for (size_t i = m_vector.size(); i--;)
        m_vector[i]->filterIncomingReferences([] (JSCell*) { return false; });
}

template<typename T>
bool GCIncomingRefCountedSet<T>::addReference(JSCell* cell, T* object) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // Only GIL-off has mutators appending here concurrently with each other
    // and with the detach / wasm-grow snapshot readers (see
    // Heap::arrayBufferIncomingReferencesLock()). With the GIL, and flag-off,
    // exactly one mutator runs at a time and the upstream lock-free shape is kept.
    std::optional<Locker<Lock>> locker;
    if (g_jscConfig.gilOffProcess) [[unlikely]]
        locker.emplace(m_lock);
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
void GCIncomingRefCountedSet<T>::sweep(VM& vm, CollectionScope collectionScope) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // GC end phase: every mutator is stopped, so none is inside addReference
    // or a snapshot and the set is walked unlocked. m_lock must not be held
    // here anyway: filterIncomingReferences deletes an object whose last
    // incoming reference died, and ~ArrayBuffer takes other locks and runs
    // embedder destructors.
    ASSERT(vm.heap.worldIsStopped());
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
