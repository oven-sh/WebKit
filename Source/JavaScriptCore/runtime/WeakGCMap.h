/*
 * Copyright (C) 2009, 2015-2016 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "DeferGC.h"
#include "Weak.h"
#include "WeakGCHashTable.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>

namespace JSC {

// A UncheckedKeyHashMap with Weak<JSCell> values, which automatically removes values once they're garbage collected.

// Per-VM WeakGCMap caches that are reachable from multiple JS threads (under
// Options::useJSThreads with the GIL off) opt into an internal lock that
// serializes every map operation. The lock is a leaf (SPEC-ungil §LK.7, §K
// class-2 cache lock): holders only perform HashMap storage operations
// (fastMalloc and Weak slot moves) and acquire no other lock, never wait, and
// never run user JS or allocate GC cells. Weak handle CREATION is hoisted
// outside the lock per SPEC-ungil §LK WS(i): callers of set() construct the
// Weak before the call, and ensureValue() runs its functor and constructs the
// Weak between the lookup and publish critical sections.
enum class WeakGCMapLocking : bool { No, Yes };

template<typename KeyArg, typename ValueArg, typename HashArg = DefaultHash<KeyArg>, typename KeyTraitsArg = HashTraits<KeyArg>>
class WeakGCMap final : public WeakGCHashTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WeakGCMap);
    WTF_MAKE_NONCOPYABLE(WeakGCMap);
    typedef Weak<ValueArg> ValueType;
    typedef UncheckedKeyHashMap<KeyArg, ValueType, HashArg, KeyTraitsArg> HashMapType;

public:
    typedef typename HashMapType::KeyType KeyType;
    typedef typename HashMapType::AddResult AddResult;
    typedef typename HashMapType::iterator iterator;
    typedef typename HashMapType::const_iterator const_iterator;

    explicit WeakGCMap(VM&, WeakGCMapLocking = WeakGCMapLocking::No);
    ~WeakGCMap() final;

    ValueArg* get(const KeyType& key) const
    {
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            return m_map.get(key);
        }
        return m_map.get(key);
    }

    AddResult set(const KeyType& key, ValueType value)
    {
        // The Weak value is constructed by the caller before the lock is
        // taken (SPEC-ungil §LK WS(i)). A displaced previous Weak is
        // destroyed under the lock; Weak destruction is a WeakSet free-list
        // splice, which is leaf-legal (§LK.8 "fastMalloc/list-splice-only").
        // NOTE: for locking instances, the returned AddResult's iterator must
        // not be dereferenced by the caller; existing callers ignore it.
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            return m_map.set(key, WTF::move(value));
        }
        return m_map.set(key, WTF::move(value));
    }

    // First-wins publish for pre-constructed values: installs `value` for
    // `key` unless a LIVE entry already exists, in which case the existing
    // value is canonical and is returned; `value`'s Weak dies after return.
    // A dead (GC'd-but-unpruned) entry is replaced like ensureValue does.
    // This is the canonicalization protocol for caches where the compound
    // get -> allocate -> publish must not let a racing loser clobber the
    // winner (e.g. VM::symbolImplToSymbolMap; see Symbol::create). The Weak
    // is constructed before the lock per SPEC-ungil §LK WS(i).
    ValueArg* addIfAbsent(const KeyType& key, ValueArg* value)
    {
        ValueType newEntry(value);
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            return addIfAbsentImpl(key, WTF::move(newEntry), value);
        }
        return addIfAbsentImpl(key, WTF::move(newEntry), value);
    }

    template<typename Functor>
    ValueArg* ensureValue(const KeyType& key, Functor&& functor)
    {
        // If functor invokes GC, GC can prune WeakGCMap, and manipulate UncheckedKeyHashMap while we are touching it in ensure function.
        // The functor must not invoke GC.
        AssertNoGC assertNoGC;
        if (m_locking == WeakGCMapLocking::No) {
            AddResult result = m_map.ensure(key, functor);
            ValueArg* value = result.iterator->value.get();
            if (!result.isNewEntry && !value) {
                value = functor();
                result.iterator->value = WTF::move(value);
            }
            return value;
        }

        {
            Locker locker { m_lock };
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                if (ValueArg* value = it->value.get())
                    return value;
            }
        }
        // Run the functor and construct the Weak outside the lock: the
        // functor may allocate cells (heap slow paths), and Weak creation
        // takes the WeakSet allocation path — neither is permitted while
        // holding a §LK leaf lock (SPEC-ungil §LK WS(i): hoist construction
        // before the lock, publish under it). No GC can intervene between
        // the two critical sections: this thread holds AssertNoGC and does
        // not reach a safepoint here, so the map cannot be pruned under us.
        ValueArg* value = functor();
        ValueType newEntry(value);
        {
            Locker locker { m_lock };
            AddResult result = m_map.add(key, ValueType());
            if (!result.isNewEntry) {
                if (ValueArg* existing = result.iterator->value.get()) {
                    // Another thread published a live value first; that value
                    // is canonical. Our newEntry Weak dies after unlock.
                    return existing;
                }
            }
            result.iterator->value = WTF::move(newEntry);
        }
        return value;
    }

    bool remove(const KeyType& key)
    {
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            return m_map.remove(key);
        }
        return m_map.remove(key);
    }

    void clear()
    {
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            m_map.clear();
            return;
        }
        m_map.clear();
    }

    bool isEmpty() const
    {
        const_iterator it = m_map.begin();
        const_iterator end = m_map.end();
        while (it != end) {
            if (it->value)
                return true;
        }
        return false;
    }

    // find() exposes an iterator into the underlying table and is therefore
    // NOT safe on WeakGCMapLocking::Yes instances unless the caller has
    // exclusive access (e.g. inside the GC stop-the-world window); no such
    // caller currently exists. contains() is safe on locking instances.
    inline iterator find(const KeyType& key);

    inline const_iterator find(const KeyType& key) const;

    inline bool contains(const KeyType& key) const;

    void pruneStaleEntries() final;

    // forEach() runs with the heap deferred (GC-side / exclusive contexts
    // only); it intentionally takes no lock because Func may allocate.
    template<typename Func>
    void forEach(Func);

private:
    ValueArg* addIfAbsentImpl(const KeyType& key, ValueType&& newEntry, ValueArg* value)
    {
        AddResult result = m_map.add(key, ValueType());
        if (!result.isNewEntry) {
            if (ValueArg* existing = result.iterator->value.get())
                return existing;
        }
        result.iterator->value = WTF::move(newEntry);
        return value;
    }

    HashMapType m_map;
    VM& m_vm;
    mutable Lock m_lock;
    const WeakGCMapLocking m_locking { WeakGCMapLocking::No };
};

} // namespace JSC
