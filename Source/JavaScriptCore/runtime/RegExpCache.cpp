/*
 * Copyright (C) 2010 University of Szeged
 * Copyright (C) 2010 Renata Hodovan (hodovan@inf.u-szeged.hu)
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY UNIVERSITY OF SZEGED ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL UNIVERSITY OF SZEGED OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RegExpCache.h"

#include "HeapCellInlines.h"
#include "StrongInlines.h"
#include "WeakInlines.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpCache);

RegExp* RegExpCache::lookup(VM&, const WTF::String& patternString, OptionSet<Yarr::Flags> flags)
{
    Locker locker { m_lock };
    RegExpKey key(flags, patternString);
    return m_weakCache.get(key);
}

RegExp* RegExpCache::lookupOrCreate(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
{
    RegExpKey key(flags, patternString);
    {
        Locker locker { m_lock };
        if (RegExp* regExp = m_weakCache.get(key))
            return regExp;
    }

    RegExp* regExp = RegExp::createWithoutCaching(vm, patternString, flags);
#if ENABLE(REGEXP_TRACING)
    vm.addRegExpToTrace(regExp);
#endif

    {
        // Two threads can both miss the first lookup, both create a RegExp
        // for the same key, and race to insert. The miss-then-add is not
        // atomic (the lock is dropped across RegExp::createWithoutCaching
        // because allocation can GC and run finalizers that take m_lock).
        // Re-check under the lock and return the winner; the loser's freshly
        // created RegExp is unreferenced by the cache and is simply collected.
        // Note: m_weakCache.get() returns nullptr for zombie entries (a Weak
        // cleared by GC whose finalizer has not yet run), and weakAdd's
        // map.set() already tolerates overwriting a zombie, so that case
        // falls through to the existing add path unchanged.
        // WS1.2: construct the Weak BEFORE taking m_lock (no handle creation
        // under leaf locks); a losing duplicate is destroyed after release
        // (handle declared before the Locker, so the Locker unwinds first).
        Weak<RegExp> handle(regExp, this);
        Locker locker { m_lock };
        if (RegExp* winner = m_weakCache.get(key))
            return winner;
        weakAdd(m_weakCache, key, WTF::move(handle));
        return regExp;
    }
}

RegExp* RegExpCache::ensureEmptyRegExpSlow(VM& vm)
{
    RegExp* regExp = RegExp::create(vm, emptyString(), { });
    m_emptyRegExp = regExp;
    return regExp;
}

void RegExpCache::finalize(Handle<Unknown> handle, void*)
{
    Locker locker { m_lock };
    RegExp* regExp = static_cast<RegExp*>(handle.get().asCell());
    weakRemove(m_weakCache, regExp->key(), regExp);
}

void RegExpCache::addToStrongCache(RegExp* regExp)
{
    String pattern = regExp->pattern();
    if (pattern.length() > maxStrongCacheablePatternLength)
        return;

    Locker locker { m_lock };
    m_strongCache[m_nextEntryInStrongCache] = regExp;
    m_nextEntryInStrongCache++;
    if (m_nextEntryInStrongCache == maxStrongCacheableEntries)
        m_nextEntryInStrongCache = 0;
}

void RegExpCache::deleteAllCode()
{
    Vector<RegExp*> liveRegExps;
    {
        Locker locker { m_lock };
        m_strongCache.fill(nullptr);
        m_nextEntryInStrongCache = 0;
        liveRegExps.reserveInitialCapacity(m_weakCache.size());
        for (auto& [key, weakHandle] : m_weakCache) {
            if (RegExp* regExp = weakHandle.get())
                liveRegExps.append(regExp);
        }
    }
    // deleteCode() takes the RegExp cellLock; it must run OUTSIDE m_lock
    // (RegExp::compile holds cellLock when calling addToStrongCache, so the
    // process-wide order is cellLock -> m_lock; the caller runs from the
    // mutator with heap access, keeping the snapshotted cells alive).
    for (auto* regExp : liveRegExps)
        regExp->deleteCode();
}

template<typename Visitor>
void RegExpCache::visitAggregateImpl(Visitor& visitor)
{
    Locker locker { m_lock };
    for (auto cell : m_strongCache)
        visitor.appendUnbarriered(cell);
    visitor.appendUnbarriered(m_emptyRegExp);
}
DEFINE_VISIT_AGGREGATE(RegExpCache);

}
