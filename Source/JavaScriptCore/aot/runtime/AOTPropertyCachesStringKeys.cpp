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
#include "AOTVector.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

StringKeyCache::StringKeyCache(State& state)
    : m_instance(state.instance)
    , m_cageBase(state.instance.vm->heap.cage().base())
    , m_cageSize(state.instance.vm->heap.cage().size())
    , m_maxCachedKeyLength(runtimeLimitValue(*state.instance.vm, LimitID::MaxCachedKeyLength))
{
    state.caches->add(CacheKind::WeakByKey, "StringKeyCache"_s, *this);
    publishIntegerKeys();
    m_instance.realm.caseConversions = m_caseConversions.data();
}

void StringKeyCache::publishIntegerKeys()
{
    m_instance.realm.integerKeys = m_integerKeys.isEmpty() ? nullptr : m_integerKeys.span().data();
    m_instance.realm.integerKeyMask = m_integerKeys.isEmpty() ? 0 : m_integerKeys.size() - 1;
}

JSString* StringKeyCache::findOrAdd(VM& vm, std::span<const Latin1Character> characters)
{
    if (characters.size() < 2)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    if (characters.size() > m_maxCachedKeyLength) {
        countLimitHit(vm, LimitID::MaxCachedKeyLength);
        return jsNontrivialString(vm, String(characters));
    }
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (JSString* key = tryFindCachedKey(characters, hash))
        return key;
    return makeKey(vm, characters, hash);
}

NEVER_INLINE JSString* StringKeyCache::findAmongEqualHashes(const uint64_t* pair, std::span<const Latin1Character> characters) const
{
    if (JSString* key = matchingKey(pair[0], characters))
        return key;
    return matchingKey(pair[1], characters);
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

void StringKeyCache::cacheCaseConversion(VM& vm, const JSString* source, LetterCase letterCase, JSString* converted)
{
    if (!vm.heap.cage().contains(source) || !vm.heap.cage().contains(converted))
        return;
    uint32_t encoded = MarkedBlockCage::encode(m_cageBase, source);
    m_caseConversions[indexOfCaseConversion(encoded, letterCase)] = static_cast<uint64_t>(encoded) << 32 | MarkedBlockCage::encode(m_cageBase, converted);
    m_writtenSinceCollection.recordAll();
}

JSString* StringKeyCache::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    return makeKey(vm, characters, hash, { });
}

void StringKeyCache::growCachedKeys()
{
    Vector<uint64_t> former = std::exchange(m_cachedKeys, { });
    m_cachedKeys.fill(0, former.size() * 2);
    for (uint64_t formerEntry : former) {
        if (!formerEntry)
            continue;
        unsigned hash = formerEntry >> 32;
        size_t index = firstIndex(hash, m_cachedKeys.size());
        if (m_cachedKeys[index] && !m_cachedKeys[index + 1])
            ++index;
        else if (m_cachedKeys[index]) {
            index += hash >> 23 & 1;
            ++m_cachedKeyEvictionCount;
        }
        m_cachedKeys[index] = formerEntry;
    }
    m_cachedKeyEvictionCount = 0;
}

void StringKeyCache::cacheKeyByParts(const KeyByParts& parts, JSString* key)
{
    if (!canEncode(key))
        return;
    addKeyByParts(parts, key);
    m_writtenSinceCollection.recordAll();
}

void StringKeyCache::resizeIntegerKeys(size_t tableSize)
{
    m_integerKeyEvictionCount = 0;
    Vector<uint64_t> former = std::exchange(m_integerKeys, { });
    m_integerKeys.fill(0, tableSize);
    for (uint64_t formerEntry : former) {
        if (formerEntry)
            m_integerKeys[formerEntry >> 32 & (tableSize - 1)] = formerEntry;
    }
    publishIntegerKeys();
}

void StringKeyCache::cacheIntegerKey(VM& vm, uint32_t integer, JSString* key)
{
    if (!vm.heap.cage().contains(key))
        return;
    if (m_integerKeys.isEmpty()) [[unlikely]] {
        m_integerKeys.fill(0, minIntegerKeyTableSize);
        publishIntegerKeys();
    }
    size_t tableSize = m_integerKeys.size();
    uint64_t& entry = m_integerKeys[integer & (tableSize - 1)];
    bool overwrites = entry;
    entry = static_cast<uint64_t>(integer) << 32 | MarkedBlockCage::encode(m_cageBase, key);
    ++m_integerKeysEnteredSinceSweep;
    m_writtenSinceCollection.recordAll();
    if (!overwrites || ++m_integerKeyEvictionCount <= tableSize / 4 || tableSize >= maxIntegerKeyTableSize) [[likely]]
        return;
    resizeIntegerKeys(tableSize * 2);
}

JSString* StringKeyCache::makeIntegerKey(VM& vm, uint32_t integer, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    JSString* key = jsNontrivialString(vm, String(StringImpl::create(characters, hash)));
    cacheIntegerKey(vm, integer, key);
    return key;
}

void StringKeyCache::growKeysByParts()
{
    m_keysByPartsEvictionCount = 0;
    if (m_keysByParts.size() >= maxKeysByPartsTableSize)
        return;
    Vector<KeyByParts> former = std::exchange(m_keysByParts, { });
    m_keysByParts.fill({ }, former.size() * 2);
    for (const KeyByParts& entry : former) {
        if (!entry.countAndKinds)
            continue;
        uint32_t count = entry.countAndKinds & 0xff;
        uint64_t mixed = count;
        for (uint32_t i = 0; i < count; ++i)
            mixed = (mixed ^ entry.part(i)) * 0x9e3779b97f4a7c15ull;
        uint32_t hash = static_cast<uint32_t>((mixed ^ entry.countAndKinds >> 8) >> 32);
        size_t index = firstIndex(hash, m_keysByParts.size());
        if (m_keysByParts[index].countAndKinds && !m_keysByParts[index + 1].countAndKinds)
            ++index;
        else if (m_keysByParts[index].countAndKinds) {
            index += hash >> 31;
            ++m_keysByPartsEvictionCount;
        }
        m_keysByParts[index] = entry;
    }
}

size_t StringKeyCache::removeDeadKeysByParts(VM& vm)
{
    size_t removedCount = 0;
    for (KeyByParts& entry : m_keysByParts) {
        if (!entry.countAndKinds)
            continue;
        bool isAlive = vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, entry.key));
        for (uint32_t areCells = entry.countAndKinds >> 8, i = 0; areCells && isAlive; areCells >>= 1, ++i) {
            if (areCells & 1)
                isAlive = vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, entry.part(i)));
        }
        if (!isAlive) {
            entry = { };
            ++removedCount;
        }
    }
    return removedCount;
}

size_t StringKeyCache::removeDeadCaseConversions(VM& vm)
{
    size_t removedCount = 0;
    for (uint64_t& entry : m_caseConversions) {
        if (!entry)
            continue;
        if (vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry >> 32))) && vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry))))
            continue;
        entry = 0;
        ++removedCount;
    }
    return removedCount;
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

size_t StringKeyCache::removeDeadIntegerKeys(VM& vm)
{
    size_t removedCount = 0;
    size_t aliveCount = 0;
    for (uint64_t& entry : m_integerKeys) {
        if (!entry)
            continue;
        if (vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
            ++aliveCount;
            continue;
        }
        entry = 0;
        ++removedCount;
    }
    // A text that died is found no more, so the table has to hold what is alive and what will be entered until the next sweep, which is taken to be as much as since the last one.
    size_t wantedSize = std::max(minIntegerKeyTableSize, roundUpToPowerOfTwo((aliveCount + m_integerKeysEnteredSinceSweep) * 2));
    m_integerKeysEnteredSinceSweep = 0;
    if (wantedSize < m_integerKeys.size())
        resizeIntegerKeys(wantedSize);
    return removedCount;
}

size_t StringKeyCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t removedCount = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        removedCount += removeDeadEntries(vm, m_cachedKeys.mutableSpan());
        removedCount += removeDeadEntries(vm, m_caseConvertedStrings);
        removedCount += removeDeadCaseConversions(vm);
        removedCount += removeDeadIntegerKeys(vm);
        removedCount += removeDeadKeysByParts(vm);
    });
    return removedCount;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
