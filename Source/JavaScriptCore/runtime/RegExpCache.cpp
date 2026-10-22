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

#include "AOTRegExpRecords.h"
#include "AOTRemovableFeature.h"
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

    if (!regExp->isValid())
        return regExp;

#if ENABLE(AOT)
    if (vm.hasCompiledCode()) [[unlikely]] {
        const AOT::ImageRegExp* record = regExp->imageRecord();
        if (!record || record->form() == AOT::RegExpRecordForm::NoMetadata)
            keepParsedPattern(regExp);
    }
#endif

    {
        Locker locker { m_lock };
        weakAdd(m_weakCache, key, Weak<RegExp>(regExp, this));
        return regExp;
    }
}

#if ENABLE(AOT)
void RegExpCache::keepParsedPattern(RegExp* regExp)
{
    if (!m_parsedPatterns) {
        auto patterns = makeUniqueWithoutFastMallocCheck<ParsedPatterns>();
        patterns->fill(nullptr);
        WTF::storeStoreFence();
        m_parsedPatterns = WTF::move(patterns);
    }
    (*m_parsedPatterns)[m_nextParsedPattern] = regExp;
    m_nextParsedPattern = (m_nextParsedPattern + 1) % maxParsedPatternsKept;
}
#endif

NEVER_INLINE RegExp* RegExp::createFromCache(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags, unsigned numSubpatterns, String&& atom, Yarr::SpecificPattern specificPattern)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::createFromCache);
    return vm.regExpCache()->lookupOrCreate(vm, patternString, flags, [&] {
        RegExp* regExp = new (NotNull, allocateCell<RegExp>(vm)) RegExp(vm, patternString, flags);
        regExp->finishCreationFromCache(vm, numSubpatterns, WTF::move(atom), specificPattern);
        return regExp;
    });
}

NEVER_INLINE RegExp* RegExpCache::lookupOrCreate(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
{
    JSC_REMOVABLE_DEFINITION(RegExp, static_cast<RegExp* (RegExpCache::*)(VM&, const String&, OptionSet<Yarr::Flags>)>(&RegExpCache::lookupOrCreate));
    return lookupOrCreate(vm, patternString, flags, [&] { return RegExp::createWithoutCaching(vm, patternString, flags); });
}

NEVER_INLINE RegExp* RegExpCache::ensureEmptyRegExpSlow(VM& vm)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExpCache::ensureEmptyRegExpSlow);
    RegExp* regExp = RegExp::create(vm, emptyString(), { });
    m_emptyRegExp = regExp;
    return regExp;
}

NEVER_INLINE void RegExpCache::finalize(Handle<Unknown> handle, void*)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExpCache::finalize);
    Locker locker { m_lock };
    RegExp* regExp = static_cast<RegExp*>(handle.get().asCell());
    weakRemove(m_weakCache, regExp->key(), regExp);
}

void RegExpCache::addToStrongCache(RegExp* regExp)
{
    String pattern = regExp->pattern();
    if (pattern.length() > maxStrongCacheablePatternLength)
        return;

    m_strongCache[m_nextEntryInStrongCache] = regExp;
    m_nextEntryInStrongCache++;
    if (m_nextEntryInStrongCache == maxStrongCacheableEntries)
        m_nextEntryInStrongCache = 0;
}

void RegExpCache::deleteAllCode()
{
    m_strongCache.fill(nullptr);
    m_nextEntryInStrongCache = 0;
#if ENABLE(AOT)
    if (m_parsedPatterns)
        m_parsedPatterns->fill(nullptr);
    m_nextParsedPattern = 0;
#endif

    Locker locker { m_lock };
    for (auto& [key, weakHandle] : m_weakCache) {
        RegExp* regExp = weakHandle.get();
        if (!regExp) // Skip zombies.
            continue;
        regExp->deleteCode();
    }
}

// A RegExp that has matched since the last full collection began keeps its code; the others compile again when they
// next match.
void RegExpCache::deleteCodeNotUsedInCurrentFullCollectionCycle(VM& vm)
{
    Locker locker { m_lock };
    for (auto& [key, weakHandle] : m_weakCache) {
        RegExp* regExp = weakHandle.get();
        if (!regExp || regExp->wasUsedInCurrentFullCollectionCycle(vm))
            continue;
        regExp->deleteCode();
    }
}

template<typename Visitor>
void RegExpCache::visitAggregateImpl(Visitor& visitor)
{
    for (auto cell : m_strongCache)
        visitor.appendUnbarriered(cell);
#if ENABLE(AOT)
    if (ParsedPatterns* patterns = m_parsedPatterns.get()) {
        for (auto cell : *patterns)
            visitor.appendUnbarriered(cell);
    }
#endif
    visitor.appendUnbarriered(m_emptyRegExp);
}
DEFINE_VISIT_AGGREGATE(RegExpCache);

}
