/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldMovePlanCache.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <algorithm>
#include <bit>

namespace JSC { namespace AOT {

FieldMovePlanCache::FieldMovePlanCache(State& state)
{
    state.caches->add(CacheKind::WeakByKey, "field move plans"_s, *this);
}

FieldMovePlanCache::~FieldMovePlanCache() = default;

unsigned FieldMovePlanCache::hashOf(const FieldMovePlanKey& key)
{
    uint64_t words = key.sourceWords * 0x9e3779b97f4a7c15ull ^ std::rotl(key.targetWords * 0xc2b2ae3d27d4eb4full, 31) ^ std::bit_cast<uintptr_t>(key.sourceStructure) ^ static_cast<uint64_t>(key.purpose) << 56;
    if (!key.replacedFields.isEmpty())
        words ^= key.replacedFields.hash();
    return static_cast<unsigned>(words >> 32 ^ words);
}

FieldMovePlanCache::Set& FieldMovePlanCache::setFor(const FieldMovePlanKey& key)
{
    ASSERT(m_sets);
    return (*m_sets)[hashOf(key) % numberOfSets];
}

const FieldMovePlan* FieldMovePlanCache::find(const FieldMovePlanKey& key)
{
    const Entry*& lastFound = m_lastFound[static_cast<size_t>(key.purpose)];
    if (lastFound && lastFound->key == key)
        return lastFound->plan.get();
    if (!m_sets)
        return nullptr;
    for (auto& way : setFor(key).ways) {
        if (way && way->key == key) {
            lastFound = way.get();
            return way->plan.get();
        }
    }
    return nullptr;
}

const FieldMovePlan& FieldMovePlanCache::add(VM& vm, FieldMovePlanKey&& key, std::unique_ptr<FieldMovePlan>&& plan)
{
    ASSERT(plan);
    if (!m_sets)
        m_sets = makeUniqueWithoutFastMallocCheck<Sets>();
    m_lastFound.fill(nullptr);
    Set& set = setFor(key);
    auto way = std::ranges::find_if(set.ways, [](auto& candidate) {
        return !candidate;
    });
    if (way == set.ways.end())
        way = set.ways.begin() + set.nextVictim++ % waysPerSet;
    *way = makeUnique<Entry>(WTF::move(key), WTF::move(plan));
    m_writes.recordAll();
    count(vm, RuntimeCounterID::FieldMovePlansMade);
    return *(*way)->plan;
}

size_t FieldMovePlanCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t removed = 0;
    auto isDead = [&](Structure* structureOrNull) {
        return structureOrNull && !vm.heap.isMarked(structureOrNull);
    };
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_lastFound.fill(nullptr);
        for (Set& set : *m_sets) {
            for (auto& way : set.ways) {
                if (!way || (!isDead(way->key.sourceStructure) && !isDead(way->plan->structure)))
                    continue;
                way = nullptr;
                ++removed;
            }
        }
    });
    return removed;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
