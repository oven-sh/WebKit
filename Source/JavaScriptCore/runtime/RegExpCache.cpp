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
#include "JSThreadsSafepoint.h"
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

template<typename Create>
RegExp* RegExpCache::lookupOrCreate(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags, const Create& create)
{
    RegExpKey key(flags, patternString);
    {
        Locker locker { m_lock };
        if (RegExp* regExp = m_weakCache.get(key))
            return regExp;
    }

    RegExp* regExp = create();
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

RegExp* RegExp::createFromCache(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags, unsigned numSubpatterns, String&& atom, Yarr::SpecificPattern specificPattern)
{
    return vm.regExpCache()->lookupOrCreate(vm, patternString, flags, [&] {
        RegExp* regExp = new (NotNull, allocateCell<RegExp>(vm)) RegExp(vm, patternString, flags);
        regExp->finishCreationFromCache(vm, numSubpatterns, WTF::move(atom), specificPattern);
        return regExp;
    });
}

RegExp* RegExpCache::lookupOrCreate(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
{
    return lookupOrCreate(vm, patternString, flags, [&] { return RegExp::createWithoutCaching(vm, patternString, flags); });
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

void RegExpCache::deleteAllCode(VM& vm)
{
    auto clearCode = [&] {
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
        // process-wide order is cellLock -> m_lock). The snapshotted cells stay
        // alive: this thread holds heap access and reaches no safepoint before
        // the clear, and GIL-off the stop window forbids GC initiation.
        for (auto* regExp : liveRegExps)
            regExp->deleteCode();
    };
    if (vm.gilOff()) [[unlikely]] {
        // matchInline runs the Yarr code and the bytecode pattern lock-free,
        // and the idle decision in VM::whenIdle is not exclusive GIL-off (a
        // thread may enter right after it), so the clear runs with every other
        // mutator stopped: none is mid-match and none can enter until the
        // window closes. Nested inside the debugger's window it runs inline.
        JSThreadsSafepoint::ClassAStopWatchdogContext watchdogContext(this, "RegExpCache deleteAllCode");
        JSThreadsSafepoint::stopTheWorldAndRun(vm, ScopedLambda<void()>(clearCode));
        return;
    }
    clearCode();
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
