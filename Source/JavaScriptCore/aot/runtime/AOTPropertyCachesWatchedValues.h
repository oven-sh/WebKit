/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeRecords.h"
#include "AOTWatchpoints.h"
#include "CollectionScope.h"
#include "Identifier.h"
#include "RememberedSlot.h"
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSObject;
class VM;

namespace AOT {

struct State;

class WatchedValues {
    WTF_MAKE_NONCOPYABLE(WatchedValues);
public:
    explicit WatchedValues(State&);
    ~WatchedValues();

    bool tryWatchConstant(VM&, JSObject* holder, UniquedStringImpl*, JSValue expected, SiteCache);
    void stopWatchingConstant(SiteCache);
    void didCacheStore(UniquedStringImpl*);

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    struct Watch {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Watch);

        Watch(WatchedValues& owner, JSObject* holder, uint32_t siteIndex)
            : owner(owner)
            , holder(holder)
            , siteIndex(siteIndex)
        {
        }

        bool hasFired() const { return !holder; }

        PropertyValueWatchpoint watchpoint;
        WatchedValues& owner;
        JSObject* holder;
        uint32_t siteIndex;
    };

    static void didFire(VM&, void* watch);

    State& m_state;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Watch>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantsBySite;
    IdentifierSet m_namesWithCachedStore;
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
