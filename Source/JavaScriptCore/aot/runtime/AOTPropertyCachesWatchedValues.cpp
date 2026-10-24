/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesWatchedValues.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"

namespace JSC { namespace AOT {

WatchedValues::WatchedValues(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::WeakByKey, "WatchedValues"_s, *this);
}

WatchedValues::~WatchedValues() = default;

bool WatchedValues::tryWatchConstant(VM& vm, JSObject* holder, UniquedStringImpl* uid, JSValue expected, SiteCache site)
{
    // A free-name site with a cached store compares no Structure and fires no watchpoint, and each later Structure of the holder starts with a valid replacement set.
    if (!site || m_namesWithCachedStore.contains(uid) || !holder->structure()->propertyAccessesAreCacheable())
        return false;
    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, uid);
    if (!condition || condition.requiredValue() != expected || !condition.isWatchable(PropertyCondition::EnsureWatchability))
        return false;

    SiteCaches& sites = m_state.propertyCaches->siteCaches();
    // Filling a site that holds a constant stops the watch of that site, which must not be the watch created here.
    if (site->get().kind == Slot::Kind::Constant)
        sites.clear(site);

    uint32_t siteIndex = sites.indexOf(site);
    auto watch = makeUnique<Watch>(*this, holder, siteIndex);
    watch->watchpoint.watch(vm, condition, didFire, watch.get());
    m_constantsBySite.set(siteIndex, WTF::move(watch));
    m_writtenSinceCollection.recordAll();
    return true;
}

void WatchedValues::didFire(VM&, void* context)
{
    // A watchpoint is not destroyed while it fires. The next watch of the site or the next sweep frees it.
    Watch& watch = *static_cast<Watch*>(context);
    WatchedValues& owner = watch.owner;
    watch.holder = nullptr;
    owner.m_writtenSinceCollection.recordAll();

    SiteCaches& sites = owner.m_state.propertyCaches->siteCaches();
    sites.clear(sites.at(watch.siteIndex));
}

void WatchedValues::stopWatchingConstant(SiteCache site)
{
    auto iterator = m_constantsBySite.find(m_state.propertyCaches->siteCaches().indexOf(site));
    if (iterator == m_constantsBySite.end() || iterator->value->hasFired())
        return;
    m_constantsBySite.remove(iterator);
}

void WatchedValues::didCacheStore(UniquedStringImpl* uid)
{
    m_namesWithCachedStore.add(uid);
}

size_t WatchedValues::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRemovedWatches = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_constantsBySite.removeIf([&](auto& entry) {
            const Watch& watch = *entry.value;
            if (!watch.hasFired() && vm.heap.isMarked(watch.holder))
                return false;
            ++numberOfRemovedWatches;
            return true;
        });
    });
    return numberOfRemovedWatches;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
