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

void MegamorphicCache::takeOutDead(Heap& heap)
{
    auto isDead = [&](StructureID structureID) {
        return structureID && !heap.isMarked(structureID.decode());
    };
    auto takeOutLoads = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch == invalidEpoch)
                continue;
            JSCell* holder = entry.m_holder;
            bool isOwn = holder == JSCell::seenMultipleCalleeObjects();
            if (!isLiveLoad(entry) || isDead(entry.m_structureID) || (holder && !isOwn && !heap.isMarked(holder))) {
                entry.m_uid = nullptr;
                entry.m_epoch = invalidEpoch;
                continue;
            }
            if (Options::validateAOTTypedFacts() && isOwn) [[unlikely]]
                RELEASE_ASSERT_WITH_MESSAGE(entry.m_structureID.decode()->getConcurrently(entry.m_uid.get()) == static_cast<PropertyOffset>(entry.m_offset), "AOT: the megamorphic cache says where every object of a Structure has a property itself, and the Structure says otherwise");
        }
    };
    takeOutLoads(m_loadCachePrimaryEntries);
    takeOutLoads(m_loadCacheSecondaryEntries);
    takeOutLoads(m_getterCachePrimaryEntries);
    takeOutLoads(m_getterCacheSecondaryEntries);
    auto takeOutStores = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch == invalidEpoch)
                continue;
            if (entry.m_epoch != m_epoch || isDead(entry.m_oldStructureID) || isDead(entry.m_newStructureID)) {
                entry.m_uid = nullptr;
                entry.m_epoch = invalidEpoch;
            }
        }
    };
    takeOutStores(m_storeCachePrimaryEntries);
    takeOutStores(m_storeCacheSecondaryEntries);
    auto takeOutAnswers = [&](auto& entries) {
        for (auto& entry : entries) {
            if (entry.m_epoch == invalidEpoch)
                continue;
            if (entry.m_epoch != m_epoch || isDead(entry.m_structureID)) {
                entry.m_uid = nullptr;
                entry.m_epoch = invalidEpoch;
            }
        }
    };
    takeOutAnswers(m_hasCachePrimaryEntries);
    takeOutAnswers(m_hasCacheSecondaryEntries);
}

void MegamorphicCache::age(CollectionScope collectionScope)
{
    ++m_epoch;
    // Nobody has looked whether their Structures are alive.
    if (std::exchange(m_hasPermanentEntries, false))
        collectionScope = CollectionScope::Full;
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
        if (m_epoch == invalidEpoch)
            m_epoch = firstEpoch;
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
    m_epoch = firstEpoch;
    m_hasPermanentEntries = false;
}

} // namespace JSC
