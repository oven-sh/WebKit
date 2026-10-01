/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include "config.h"
#include "MegamorphicCache.h"

#include "ProgramExecutable.h"

#include <wtf/TZoneMallocInlines.h>

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MegamorphicCache);
WTF_MAKE_TZONE_ALLOCATED_IMPL(MegamorphicCache);

bool MegamorphicCache::noteDependenceOnPrototypes(StructureID structureID, JSCell* upTo)
{
    for (Structure* structure = structureID.decode();;) {
        if (!structure->hasMonoProto())
            return false;
        JSValue prototype = structure->storedPrototype();
        if (!prototype.isObject())
            return true;
        JSObject* object = asObject(prototype);
        object->setIsPrototypeUsedByMegamorphicCache();
        if (object == upTo)
            return true;
        structure = object->structure();
    }
}

void MegamorphicCache::reconcileWeakReferencesAtGCEnd(VM& vm)
{
    Heap& heap = vm.heap;
    auto hasDied = [&](StructureID id) { return !heap.isMarked(id.decode()); };
    auto reconcileLoads = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch != m_epoch)
                continue;
            bool hasHolder = entry.m_holder && entry.m_holder != JSCell::seenMultipleCalleeObjects();
            if (hasDied(entry.m_structureID) || (hasHolder && !heap.isMarked(entry.m_holder)))
                entry.m_epoch = invalidEpoch;
        }
    };
    auto reconcileStores = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch == m_epoch && (hasDied(entry.m_oldStructureID) || hasDied(entry.m_newStructureID)))
                entry.m_epoch = invalidEpoch;
        }
    };
    auto reconcileHas = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch == m_epoch && hasDied(entry.m_structureID))
                entry.m_epoch = invalidEpoch;
        }
    };
    reconcileLoads(m_loadCachePrimaryEntries);
    reconcileLoads(m_loadCacheSecondaryEntries);
    reconcileLoads(m_getterCachePrimaryEntries);
    reconcileLoads(m_getterCacheSecondaryEntries);
    reconcileStores(m_storeCachePrimaryEntries);
    reconcileStores(m_storeCacheSecondaryEntries);
    reconcileHas(m_hasCachePrimaryEntries);
    reconcileHas(m_hasCacheSecondaryEntries);
    // (These also depend on something that is not a cell, so they are simply invalidated.)
    for (auto& entry : m_constructionEntries)
        entry.m_epoch = invalidEpoch;
    m_hasBeenReconciled = true;
}

void MegamorphicCache::age(CollectionScope collectionScope)
{
    if (std::exchange(m_hasBeenReconciled, false) && collectionScope == CollectionScope::Eden)
        return;
    ++m_epoch;
    if (collectionScope == CollectionScope::Full || m_epoch == invalidEpoch) {
        for (auto& entry : m_loadCachePrimaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_loadCacheSecondaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_storeCachePrimaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_storeCacheSecondaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_hasCachePrimaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_hasCacheSecondaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_getterCachePrimaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_getterCacheSecondaryEntries) {
            entry.m_uid = nullptr;
            entry.m_epoch = invalidEpoch;
        }
        for (auto& entry : m_constructionEntries)
            entry.m_epoch = invalidEpoch;
        if (m_epoch == invalidEpoch)
            m_epoch = 1;
    }
}

void MegamorphicCache::clearEntries()
{
    for (auto& entry : m_loadCachePrimaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_loadCacheSecondaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_storeCachePrimaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_storeCacheSecondaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_hasCachePrimaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_hasCacheSecondaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_getterCachePrimaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_getterCacheSecondaryEntries)
        entry.m_epoch = invalidEpoch;
    for (auto& entry : m_constructionEntries)
        entry.m_epoch = invalidEpoch;
    m_epoch = 1;
}

} // namespace JSC
