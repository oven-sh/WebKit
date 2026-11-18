/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesStringKeys.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONTextWriter.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include <algorithm>
#include <ranges>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/IntegerToStringConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(StringKeyCache);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(SharedParts);

StringKeyCache::StringKeyCache(State& state)
    : m_instance(state.instance)
    , m_cageBase(state.instance.vm->heap.cage().base())
    , m_cageSize(state.instance.vm->heap.cage().start() - state.instance.vm->heap.cage().base() + state.instance.vm->heap.cage().size())
{
    state.caches->add(CacheKind::WeakByKey, "StringKeyCache"_s, *this);
    publishIntegerKeys();
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
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (characters.size() > maxCachedKeyLength) {
        RefPtr text = StringImpl::tryCreate(characters, hash);
        return text ? jsNontrivialString(vm, String(WTF::move(text))) : nullptr;
    }
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

JSString* StringKeyCache::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    constexpr bool isName = false;
    return makeKey(vm, characters, hash, { }, isName);
}

void StringKeyCache::growCachedKeys()
{
    m_cachedKeyEvictionCount = 0;
    std::optional former = tryResize(m_cachedKeys, m_cachedKeys.size() * 2);
    if (!former)
        return;
    for (uint64_t formerEntry : *former) {
        if (!formerEntry)
            continue;
        unsigned hash = formerEntry >> 32;
        size_t index = firstIndex(hash, m_cachedKeys.size());
        if (m_cachedKeys[index])
            index += m_cachedKeys[index + 1] ? hash >> 23 & 1 : 1;
        m_cachedKeys[index] = formerEntry;
    }
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
    std::optional former = tryResize(m_integerKeys, tableSize);
    if (!former)
        return;
    for (uint64_t formerEntry : *former) {
        if (formerEntry)
            m_integerKeys[formerEntry >> 32 & (tableSize - 1)] = formerEntry;
    }
    publishIntegerKeys();
}

bool StringKeyCache::cacheIntegerKey(uint32_t integer, JSString* key)
{
    if (!canEncode(key))
        return false;
    if (m_integerKeys.isEmpty()) [[unlikely]] {
        if (!tryResize(m_integerKeys, minTableSize))
            return false;
        publishIntegerKeys();
    }
    size_t tableSize = m_integerKeys.size();
    uint64_t& entry = m_integerKeys[integer & (tableSize - 1)];
    bool overwrites = entry && entry >> 32 != integer;
    entry = static_cast<uint64_t>(integer) << 32 | MarkedBlockCage::encode(m_cageBase, key);
    ++m_integerKeysEnteredSinceSweep;
    m_writtenSinceCollection.recordAll();
    if (overwrites && ++m_integerKeyEvictionCount > tableSize / 4 && tableSize < maxIntegerKeyTableSize) [[unlikely]]
        resizeIntegerKeys(tableSize * 2);
    return true;
}

JSString* StringKeyCache::textOfInt32(VM& vm, int32_t integer)
{
    if (integer < static_cast<int32_t>(NumericStrings::cacheSize)) {
        JSString* text = int32ToString(vm, integer, 10);
        text->tryGetValueImpl()->hash();
        return text;
    }
    if (JSString* text = tryFindIntegerKey(static_cast<uint32_t>(integer)))
        return text;
    std::array<Latin1Character, maxJSONIntegerLength> buffer;
    const Latin1Character* end = appendJSONInteger(buffer.data(), integer);
    std::span<const Latin1Character> digits { buffer.data(), static_cast<size_t>(end - buffer.data()) };
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(digits);
    JSString* text = jsNontrivialString(vm, String(StringImpl::create(digits, hash)));
    if (!cacheIntegerKey(static_cast<uint32_t>(integer), text)) [[unlikely]]
        vm.numericStrings.setJSString(integer, text->tryGetValue(), text);
    return text;
}

const StringKeyCache::KeyOfPair* StringKeyCache::noKeysOfPairs()
{
    static constinit const std::array<KeyOfPair, waysOfKeysOfPairs> empty { };
    return empty.data();
}

void StringKeyCache::publishKeysOfPairs()
{
    m_instance.realm.keysOfPairs = m_keysOfPairs.span().data();
    m_instance.realm.keyOfPairMask = keyOfPairMaskFor(m_keysOfPairs.size());
}

bool StringKeyCache::addKeyOfPair(const KeyOfPair& added)
{
    KeyOfPair* set = m_keysOfPairs.mutableSpan().data() + (hashOfPair(added.first, added.second) & keyOfPairMaskFor(m_keysOfPairs.size())) / sizeof(KeyOfPair);
    bool evicts = set[0].key && set[1].key;
    if (set[0].key)
        set[1] = set[0];
    set[0] = added;
    return evicts;
}

void StringKeyCache::cacheKeyOfPair(EncodedJSValue first, EncodedJSValue second, JSString* key)
{
    if (m_keysOfPairs.isEmpty()) [[unlikely]] {
        if (!tryResize(m_keysOfPairs, minTableSize))
            return;
        publishKeysOfPairs();
    }
    if (addKeyOfPair({ first, second, key, 0, 0 }) && ++m_keyOfPairEvictionCount > m_keysOfPairs.size() / 4) [[unlikely]]
        growKeysOfPairs();
    m_writtenSinceCollection.recordAll();
}

void StringKeyCache::forgetPlacesOfPairs()
{
    forgetPlacesOfPairs(m_instance);
}

void StringKeyCache::forgetPlacesOfPairs(Instance& instance)
{
    if (!std::exchange(instance.propertyCaches.pairsHavePlaces, 0))
        return;
    auto* pairs = static_cast<KeyOfPair*>(const_cast<void*>(instance.realm.keysOfPairs));
    size_t numberOfPairs = ((instance.realm.keyOfPairMask >> keyOfPairSetShift) + 1) * waysOfKeysOfPairs;
    for (KeyOfPair& entry : std::span { pairs, numberOfPairs }) {
        entry.structureIDOfPlace = 0;
        entry.slotOfPlace = 0;
    }
}

void StringKeyCache::growKeysOfPairs()
{
    m_keyOfPairEvictionCount = 0;
    if (m_keysOfPairs.size() >= maxKeysByPartsTableSize)
        return;
    std::optional former = tryResize(m_keysOfPairs, m_keysOfPairs.size() * 2);
    if (!former)
        return;
    publishKeysOfPairs();
    for (const KeyOfPair& entry : *former | std::views::reverse) {
        if (entry.key)
            m_keyOfPairEvictionCount += addKeyOfPair(entry);
    }
}

void StringKeyCache::growKeysByParts()
{
    m_keysByPartsEvictionCount = 0;
    if (m_keysByParts.size() >= maxKeysByPartsTableSize)
        return;
    std::optional former = tryResize(m_keysByParts, m_keysByParts.size() * 2);
    if (!former)
        return;
    for (const KeyByParts& entry : *former) {
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

size_t StringKeyCache::removeDeadCachedKeys(VM& vm)
{
    size_t removedCount = 0;
    for (uint64_t& entry : m_cachedKeys) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
            entry = 0;
            ++removedCount;
        }
    }
    return removedCount;
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

size_t StringKeyCache::removeDeadKeysOfPairs(VM& vm)
{
    size_t removedCount = 0;
    auto isAlive = [&](EncodedJSValue part) {
        JSValue value = JSValue::decode(part);
        return !value.isCell() || vm.heap.isMarked(value.asCell());
    };
    for (KeyOfPair& entry : m_keysOfPairs) {
        if (!entry.key || (vm.heap.isMarked(entry.key) && isAlive(entry.first) && isAlive(entry.second)))
            continue;
        entry = { };
        ++removedCount;
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
    size_t wantedSize = std::max(minTableSize, roundUpToPowerOfTwo((aliveCount + m_integerKeysEnteredSinceSweep) * 2));
    m_integerKeysEnteredSinceSweep = 0;
    if (wantedSize < m_integerKeys.size())
        resizeIntegerKeys(wantedSize);
    return removedCount;
}

RefPtr<StringImpl> StringKeyCache::makeCharactersOfShortPart(const JSRopeString& string, std::span<const char16_t> units)
{
    auto copyWideCharacters = [&] ALWAYS_INLINE_LAMBDA {
        std::span<char16_t> characters;
        RefPtr impl = StringImpl::tryCreateUninitialized(units.size(), characters);
        if (impl)
            StringImpl::copyCharacters(characters, units);
        return impl;
    };
    if (!WTF::isLatin1(units[0]))
        return copyWideCharacters();
    SharedPart part { units };
    if (!part.isLatin1())
        return copyWideCharacters();
    if (!m_sharedParts) [[unlikely]]
        m_sharedParts = makeUnique<SharedParts>();
    unsigned index = part.index();
    bool wasSeen = std::exchange(m_sharedParts->tags[index], part.tag()) == part.tag();
    SharedPart& found = m_sharedParts->parts[index];
    if (wasSeen && found.string && found.hasSameUnits(part)) {
        StringImpl* impl = const_cast<StringImpl*>(found.string->tryGetValueImpl());
        ASSERT(impl && impl->length() == units.size());
        return impl;
    }
    std::span<Latin1Character> characters;
    RefPtr impl = StringImpl::tryCreateUninitialized(units.size(), characters);
    if (!impl) [[unlikely]]
        return nullptr;
    part.copyLatin1Characters(characters);
    if (!wasSeen || found.string)
        return impl;
    if (RefPtr<AtomStringImpl> atom = AtomStringImpl::lookUp(impl.get()))
        impl = WTF::move(atom);
    part.string = &string;
    found = part;
    return impl;
}

RefPtr<StringImpl> makeCharactersOfShortPart(Instance& instance, const JSRopeString& string, std::span<const char16_t> units)
{
    return instance.state->stringKeys->makeCharactersOfShortPart(string, units);
}

size_t StringKeyCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    forgetPlacesOfPairs();
    size_t removedCount = 0;
    if (m_sharedParts) {
        for (SharedPart& part : m_sharedParts->parts) {
            if (!part.string || vm.heap.isMarked(part.string))
                continue;
            part = { };
            ++removedCount;
        }
    }
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        removedCount += removeDeadCachedKeys(vm);
        removedCount += removeDeadIntegerKeys(vm);
        removedCount += removeDeadKeysByParts(vm);
        removedCount += removeDeadKeysOfPairs(vm);
    });
    return removedCount;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
