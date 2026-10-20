/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesStringKeys.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

StringKeyCache::StringKeyCache(State& state)
    : m_cageBase(state.instance.vm->heap.cage().base())
    , m_maxCachedKeyLength(runtimeLimitValue(*state.instance.vm, LimitID::MaxCachedKeyLength))
{
    state.caches->add(CacheKind::WeakByKey, "StringKeyCache"_s, *this);
}

JSString* StringKeyCache::findOrAdd(VM& vm, std::span<const Latin1Character> characters)
{
    if (characters.size() < 2)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    if (characters.size() > m_maxCachedKeyLength) {
        countLimitHit(vm, LimitID::MaxCachedKeyLength);
        return jsNontrivialString(vm, String(characters));
    }
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (JSString* key = tryFindCachedKey(characters, hash))
        return key;
    return makeKey(vm, characters, hash);
}

JSString* StringKeyCache::tryFindCachedKey(std::span<const Latin1Character> characters, unsigned hash) const
{
    size_t tableSize = m_cachedKeys.size();
    if (!tableSize) [[unlikely]]
        return nullptr;
    for (uint64_t entry : m_cachedKeys.span().subspan(firstIndex(hash, tableSize), 2)) {
        if (!entry || entry >> 32 != hash)
            continue;
        if (JSString* key = matchingKey(entry, characters))
            return key;
    }
    return nullptr;
}

JSString* StringKeyCache::matchingKey(uint64_t entry, std::span<const Latin1Character> characters) const
{
    auto* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)));
    auto* impl = candidate->tryGetValueImpl();
    return impl->is8Bit() && CompactTable::hasCharacters<Latin1Character>(*impl, characters) ? candidate : nullptr;
}

JSString* StringKeyCache::caseConvertedString(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    uint64_t& entry = m_caseConvertedStrings[hash % m_caseConvertedStrings.size()];
    if (entry && entry >> 32 == hash) {
        if (JSString* key = matchingKey(entry, characters))
            return key;
    }
    JSString* newString = jsNontrivialString(vm, String(characters));
    if (vm.heap.cage().contains(newString)) {
        entry = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(m_cageBase, newString);
        m_writtenSinceCollection.recordAll();
    }
    return newString;
}

JSString* StringKeyCache::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    static constexpr size_t minTableSize = 1 << 8;
    static constexpr size_t maxTableSize = 1 << 16;
    if (m_cachedKeys.isEmpty()) [[unlikely]]
        m_cachedKeys.fill(0, minTableSize);
    JSString* newKey = jsNontrivialString(vm, String(StringImpl::create(characters, hash)));
    if (!vm.heap.cage().contains(newKey))
        return newKey;
    auto put = [&](uint64_t entry) {
        unsigned entryHash = entry >> 32;
        size_t index = firstIndex(entryHash, m_cachedKeys.size());
        if (m_cachedKeys[index] && !m_cachedKeys[index + 1])
            ++index;
        else if (m_cachedKeys[index]) {
            index += entryHash >> 23 & 1;
            ++m_cachedKeyEvictionCount;
        }
        m_cachedKeys[index] = entry;
    };
    put(static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(m_cageBase, newKey));
    if (m_cachedKeyEvictionCount > m_cachedKeys.size() / 4 && m_cachedKeys.size() < maxTableSize) {
        Vector<uint64_t> former = std::exchange(m_cachedKeys, { });
        m_cachedKeys.fill(0, former.size() * 2);
        for (uint64_t entry : former) {
            if (entry)
                put(entry);
        }
        m_cachedKeyEvictionCount = 0;
    }
    m_writtenSinceCollection.recordAll();
    return newKey;
}

size_t StringKeyCache::removeDeadEntries(VM& vm, std::span<uint64_t> entries)
{
    size_t removedCount = 0;
    for (uint64_t& entry : entries) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
            entry = 0;
            ++removedCount;
        }
    }
    return removedCount;
}

size_t StringKeyCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t removedCount = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        removedCount += removeDeadEntries(vm, m_cachedKeys.mutableSpan());
        removedCount += removeDeadEntries(vm, m_caseConvertedStrings);
    });
    return removedCount;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
