/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "Identifier.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "RememberedSlot.h"
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/RapidHash.h>

namespace JSC { namespace AOT {

struct Instance;
struct State;

class StringKeyCache {
    WTF_MAKE_NONCOPYABLE(StringKeyCache);
public:
    static constexpr size_t maxCaseConvertedStringLength = 64;

    struct KeyByParts {
        static constexpr uint32_t maxNumberOfParts = 6;

        ALWAYS_INLINE bool tryTakePart(uintptr_t cageBase, EncodedJSValue encodedValue, uint32_t index, uint32_t& areCells, uint64_t& mixed)
        {
            JSValue value = JSValue::decode(encodedValue);
            uint32_t part;
            if (value.isInt32())
                part = value.asInt32();
            else if (value.isCell()) {
                uintptr_t distance = std::bit_cast<uintptr_t>(value.asCell()) - cageBase;
                if (distance >= MarkedBlockCage::maxSize)
                    return false;
                part = static_cast<uint32_t>(distance >> MarkedBlockCage::shift);
                areCells |= 1u << index;
            } else
                return false;
            partPairs[index / 2] |= static_cast<uint64_t>(part) << (index % 2 * 32);
            mixed = (mixed ^ part) * 0x9e3779b97f4a7c15ull;
            return true;
        }

        ALWAYS_INLINE void finishTaking(uint32_t count, uint32_t areCells, uint64_t mixed)
        {
            countAndKinds = count | areCells << 8;
            hash = static_cast<uint32_t>((mixed ^ areCells) >> 32);
        }

        template<uint32_t count>
        ALWAYS_INLINE bool tryTake(uintptr_t cageBase, const EncodedJSValue* values)
        {
            static_assert(count <= maxNumberOfParts);
            uint32_t areCells = 0;
            uint64_t mixed = count;
            bool isTaken = [&]<size_t... index>(std::index_sequence<index...>) ALWAYS_INLINE_LAMBDA {
                return (tryTakePart(cageBase, values[index], index, areCells, mixed) && ...);
            }(std::make_index_sequence<count>());
            if (!isTaken)
                return false;
            finishTaking(count, areCells, mixed);
            return true;
        }

        bool tryTake(uintptr_t cageBase, std::span<const EncodedJSValue> values)
        {
            if (values.size() > maxNumberOfParts)
                return false;
            uint32_t count = static_cast<uint32_t>(values.size());
            uint32_t areCells = 0;
            uint64_t mixed = count;
            for (uint32_t index = 0; index < count; ++index) {
                if (!tryTakePart(cageBase, values[index], index, areCells, mixed))
                    return false;
            }
            finishTaking(count, areCells, mixed);
            return true;
        }

        ALWAYS_INLINE bool hasSameParts(const KeyByParts& other) const { return countAndKinds == other.countAndKinds && partPairs[0] == other.partPairs[0] && partPairs[1] == other.partPairs[1] && partPairs[2] == other.partPairs[2]; }
        uint32_t part(uint32_t index) const { return static_cast<uint32_t>(partPairs[index / 2] >> (index % 2 * 32)); }

        uint32_t countAndKinds { 0 };
        union {
            uint32_t key { 0 };
            uint32_t hash;
        };
        std::array<uint64_t, maxNumberOfParts / 2> partPairs { };
    };
    static_assert(sizeof(KeyByParts) == 32);

    explicit StringKeyCache(State&);

    uintptr_t cageBase() const { return m_cageBase; }
    ALWAYS_INLINE JSString* tryFindKeyByParts(const KeyByParts& taken) const
    {
        size_t numberOfPlaces = m_keysByParts.size();
        if (!numberOfPlaces) [[unlikely]]
            return nullptr;
        const KeyByParts* pair = m_keysByParts.span().data() + firstIndex(taken.hash, numberOfPlaces);
        for (size_t i = 0; i < 2; ++i) {
            if (pair[i].hasSameParts(taken))
                return uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, pair[i].key));
        }
        return nullptr;
    }
    void cacheKeyByParts(const KeyByParts&, JSString*);
    // What the last lookup by parts took and did not find, for whoever makes the key. No parts: countAndKinds is zero.
    KeyByParts& lastLookupByParts() { return m_lastLookupByParts; }

    ALWAYS_INLINE JSString* tryFindIntegerKey(uint32_t integer) const
    {
        size_t numberOfPlaces = m_integerKeys.size();
        if (!numberOfPlaces) [[unlikely]]
            return nullptr;
        uint64_t entry = m_integerKeys.span().data()[integer & (numberOfPlaces - 1)];
        if (entry >> 32 != integer || !static_cast<uint32_t>(entry))
            return nullptr;
        return uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)));
    }
    // Without a key: the text of the integer has been asked for once. Most integers that become text come once, and a key costs more than it saves until it is found again.
    void cacheIntegerKey(VM&, uint32_t integer, JSString* keyOrNull);
    bool wasAskedForIntegerKey(uint32_t integer) const { return !m_integerKeys.isEmpty() && m_integerKeys[integer & (m_integerKeys.size() - 1)] >> 32 == integer; }
    // Every road to the text of an integer asks for it by the integer first, so the text is entered in no table by its characters.
    JSString* makeIntegerKey(VM&, uint32_t integer, std::span<const Latin1Character>);

    size_t maxCachedKeyLength() const { return m_maxCachedKeyLength; }
    JSString* findOrAdd(VM&, std::span<const Latin1Character>);
    ALWAYS_INLINE JSString* tryFindCachedKey(std::span<const Latin1Character> characters, unsigned hash) const
    {
        size_t tableSize = m_cachedKeys.size();
        if (!tableSize) [[unlikely]]
            return nullptr;
        const uint64_t* pair = m_cachedKeys.span().data() + firstIndex(hash, tableSize);
        uint64_t entry = pair[0];
        if (entry >> 32 != hash) {
            entry = pair[1];
            if (entry >> 32 != hash)
                return nullptr;
        } else if (pair[1] >> 32 == hash) [[unlikely]]
            return findAmongEqualHashes(pair, characters);
        return matchingKey(entry, characters);
    }
    JSString* makeKey(VM&, std::span<const Latin1Character>, unsigned hash);
    ALWAYS_INLINE JSString* makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash, const KeyByParts& parts)
    {
        JSString* key = jsNontrivialString(vm, String(StringImpl::create(characters, hash)));
        if (!canEncode(key)) [[unlikely]]
            return key;
        addCachedKey(key, hash);
        if (parts.countAndKinds)
            addKeyByParts(parts, key);
        m_writtenSinceCollection.recordAll();
        return key;
    }

    static constexpr size_t maxIndexLength = 10;
    static constexpr size_t minReadableBytes = sizeof(uint64_t);
    // Reads minReadableBytes whatever the length is.
    static ALWAYS_INLINE std::optional<uint32_t> tryParseIndex(std::span<const Latin1Character> characters)
    {
        size_t length = characters.size();
        ASSERT(length && length <= maxIndexLength);
        if (length > sizeof(uint64_t)) [[unlikely]]
            return isASCIIDigit(characters.front()) && isASCIIDigit(characters.back()) ? parseIndex(characters) : std::nullopt;
        unsigned unusedBits = (sizeof(uint64_t) - length) * 8;
        uint64_t digits = (WTF::unalignedLoad<uint64_t>(characters.data()) ^ 0x3030303030303030ull) << unusedBits;
        if (((digits + 0x7676767676767676ull) | digits) & 0x8080808080808080ull)
            return std::nullopt;
        if (!(digits >> unusedBits & 0xff) && length > 1)
            return std::nullopt;
        uint64_t pairs = digits * 10 + (digits >> 8);
        constexpr uint64_t mask = 0x000000ff000000ffull;
        return static_cast<uint32_t>(((pairs & mask) * (100 + (1000000ull << 32)) + (pairs >> 16 & mask) * (1 + (10000ull << 32))) >> 32);
    }

    enum class Lookup : uint8_t { Found, Absent, AbsentIndex };
    // Integer keys are only cached by value. The hash is set when the result is Absent.
    ALWAYS_INLINE Lookup findKeyForText(std::span<const Latin1Character> characters, JSString*& key, unsigned& hash) const
    {
        ASSERT(characters.size() >= 2 && characters.size() <= m_maxCachedKeyLength);
        if (characters.size() <= maxIndexLength) {
            if (std::optional<uint32_t> index = tryParseIndex(characters)) {
                key = *index <= INT32_MAX ? tryFindIntegerKey(*index) : nullptr;
                return key ? Lookup::Found : Lookup::AbsentIndex;
            }
        }
        hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
        key = tryFindCachedKey(characters, hash);
        return key ? Lookup::Found : Lookup::Absent;
    }
    JSString* caseConvertedString(VM&, std::span<const Latin1Character>);
    // What the conversion of this very string gave the last time: a program lowers the same header names and method names over and over. A string never changes, so the answer holds while both live.
    enum class LetterCase : uint8_t { Lower, Upper };
    ALWAYS_INLINE JSString* tryFindCaseConversion(const JSString* source, LetterCase letterCase) const
    {
        uintptr_t distance = std::bit_cast<uintptr_t>(source) - m_cageBase;
        if (distance >= MarkedBlockCage::maxSize)
            return nullptr;
        uint32_t encoded = static_cast<uint32_t>(distance >> MarkedBlockCage::shift);
        uint64_t entry = m_caseConversions[indexOfCaseConversion(encoded, letterCase)];
        return entry >> 32 == encoded ? uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry))) : nullptr;
    }
    void cacheCaseConversion(VM&, const JSString* source, LetterCase, JSString* converted);
    // The two conversions of one string lie side by side.
    static constexpr uint32_t caseConversionMultiplier = 0x9e3779b1u;
    static constexpr unsigned caseConversionShift = 25;
    static constexpr size_t indexOfCaseConversion(uint32_t source, LetterCase letterCase) { return (source * caseConversionMultiplier >> caseConversionShift) << 1 | static_cast<size_t>(letterCase); }
    static constexpr size_t numberOfCaseConversions = 2u << (32 - caseConversionShift);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr size_t minTableSize = 1 << 8;
    AOT_CLASS_THRESHOLD(size_t, maxCachedKeyTableSize, 1 << 16);
    AOT_CLASS_THRESHOLD(size_t, maxKeysByPartsTableSize, 1 << 14);

    static size_t firstIndex(unsigned hash, size_t tableSize) { return hash & (tableSize - 1) & ~static_cast<size_t>(1); }
    bool canEncode(const JSCell* cell) const { return std::bit_cast<uintptr_t>(cell) - m_cageBase < m_cageSize; }
    uint32_t encode(const JSCell* cell) const { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(cell) >> MarkedBlockCage::shift); }
    ALWAYS_INLINE void addCachedKey(JSString* key, unsigned hash)
    {
        if (m_cachedKeys.isEmpty()) [[unlikely]]
            m_cachedKeys.fill(0, minTableSize);
        size_t tableSize = m_cachedKeys.size();
        uint64_t* entry = m_cachedKeys.mutableSpan().data() + firstIndex(hash, tableSize);
        bool evicts = false;
        if (*entry) {
            evicts = entry[1];
            entry += evicts ? hash >> 23 & 1 : 1;
        }
        *entry = static_cast<uint64_t>(hash) << 32 | encode(key);
        if (evicts && ++m_cachedKeyEvictionCount > tableSize / 4 && tableSize < maxCachedKeyTableSize) [[unlikely]]
            growCachedKeys();
    }
    ALWAYS_INLINE void addKeyByParts(const KeyByParts& parts, JSString* key)
    {
        if (m_keysByParts.isEmpty()) [[unlikely]]
            m_keysByParts.fill({ }, minTableSize);
        size_t tableSize = m_keysByParts.size();
        KeyByParts* entry = m_keysByParts.mutableSpan().data() + firstIndex(parts.hash, tableSize);
        bool evicts = false;
        if (entry->countAndKinds) {
            evicts = entry[1].countAndKinds;
            entry += evicts ? parts.hash >> 31 : 1;
        }
        *entry = parts;
        entry->key = encode(key);
        if (evicts && ++m_keysByPartsEvictionCount > tableSize / 4) [[unlikely]]
            growKeysByParts();
    }
    ALWAYS_INLINE JSString* matchingKey(uint64_t entry, std::span<const Latin1Character> characters) const
    {
        auto* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)));
        return CompactTable::hasCharacters<Latin1Character>(*candidate->tryGetValueImpl(), characters) ? candidate : nullptr;
    }
    JSString* findAmongEqualHashes(const uint64_t* pair, std::span<const Latin1Character>) const;
    size_t removeDeadEntries(VM&, std::span<uint64_t>);
    size_t removeDeadKeysByParts(VM&);
    size_t removeDeadCaseConversions(VM&);
    size_t removeDeadIntegerKeys(VM&);
    void resizeIntegerKeys(size_t tableSize);
    void growCachedKeys();
    void growKeysByParts();
    void publishIntegerKeys();

    Instance& m_instance;
    uintptr_t m_cageBase;
    size_t m_cageSize;
    size_t m_maxCachedKeyLength;
    Vector<uint64_t> m_cachedKeys;
    size_t m_cachedKeyEvictionCount { 0 };
    AOT_CLASS_THRESHOLD(size_t, minIntegerKeyTableSize, 1 << 8);
    AOT_CLASS_THRESHOLD(size_t, maxIntegerKeyTableSize, 1 << 16);
    Vector<uint64_t> m_integerKeys;
    size_t m_integerKeyEvictionCount { 0 };
    size_t m_integerKeysEnteredSinceSweep { 0 };
    Vector<KeyByParts> m_keysByParts;
    KeyByParts m_lastLookupByParts;
    size_t m_keysByPartsEvictionCount { 0 };
    std::array<uint64_t, 256> m_caseConvertedStrings { };
    std::array<uint64_t, numberOfCaseConversions> m_caseConversions { };
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
