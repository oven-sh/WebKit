/*
 * Copyright (C) 2021-2024 Apple Inc. All rights reserved.
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
#include "WeakGCHashTable.h"
#include "WeakGCMap.h"
#include <wtf/HashSet.h>

namespace JSC {

// A HashSet holding JSCells weakly: an entry is removed once a collection proves it unreachable.
// Every entry an iterator hands out is therefore live.

// FIXME: This doesn't currently accept WeakHandleOwners by default... it's probably not hard to add but it's not exactly clear how to handle multiple different handle owners for the same value.
template<typename ValueArg, typename HashArg = DefaultHash<ValueArg*>, typename TraitsArg = HashTraits<ValueArg*>>
class WeakGCSet final : public WeakGCHashTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WeakGCSet);
    WTF_MAKE_NONCOPYABLE(WeakGCSet);
    using ValueType = ValueArg*;
    using HashSetType = UncheckedKeyHashSet<ValueType, HashArg, TraitsArg>;

public:
    using AddResult = typename HashSetType::AddResult;
    using iterator = typename HashSetType::iterator;
    using const_iterator = typename HashSetType::const_iterator;

    // With the flag on the set can be reached from several JS threads (the
    // global object's custom getter/setter function sets are filled on any
    // thread that reifies a custom accessor), so its operations take a leaf
    // lock, on the same terms as a locking WeakGCMap: holders only touch the
    // hash table (fastMalloc) and the heap's list of dirty tables, and
    // ensureValue() runs its functor, which allocates the cell, between the
    // lookup and the publish.
    explicit WeakGCSet(VM&, WeakGCMapLocking = WeakGCMapLocking::No);
    ~WeakGCSet() final;

    void clear()
    {
        if (m_locking == WeakGCMapLocking::Yes) {
            Locker locker { m_lock };
            m_set.clear();
            return;
        }
        m_set.clear();
    }

    AddResult add(ValueArg* value)
    {
        if (m_locking == WeakGCMapLocking::Yes) [[unlikely]] {
            // The returned iterator must not be dereferenced by the caller.
            Locker locker { m_lock };
            AddResult result = m_set.add(value);
            markDirty(m_vm);
            return result;
        }
        AddResult result = m_set.add(value);
        markDirty(m_vm);
        return result;
    }

    template<typename HashTranslator, typename T>
    ValueArg* ensureValue(T&& key, const Invocable<ValueType()> auto& functor)
    {
        // If functor invokes GC, GC can prune WeakGCSet, and manipulate HashSet while we are touching it in the ensure function.
        // The functor must not invoke GC.
        AssertNoGC assertNoGC;

        if (m_locking == WeakGCMapLocking::Yes) [[unlikely]] {
            {
                Locker locker { m_lock };
                auto it = m_set.template find<HashTranslator>(key);
                if (it != m_set.end())
                    return *it;
            }
            ValueArg* created = functor();
            Locker locker { m_lock };
            auto it = m_set.template find<HashTranslator>(key);
            if (it != m_set.end())
                return *it; // First wins.
            m_set.add(created);
            markDirty(m_vm);
            return created;
        }

        auto result = m_set.template ensure<HashTranslator>(std::forward<T>(key), functor);
        markDirty(m_vm);
        return *result.iterator;
    }

    // It's not safe to call into the VM or allocate an object while an iterator is open.
    inline iterator begin() { return m_set.begin(); }
    inline const_iterator begin() const { return m_set.begin(); }

    inline iterator end() { return m_set.end(); }
    inline const_iterator end() const { return m_set.end(); }

    // FIXME: Add support for find/contains/remove from a ValueArg* via a HashTranslator.

private:
    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope) final;

    HashSetType m_set;
    VM& m_vm;
    const WeakGCMapLocking m_locking;
    mutable Lock m_lock;
};

} // namespace JSC
