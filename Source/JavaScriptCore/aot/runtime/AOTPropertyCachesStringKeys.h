/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactTable.h"
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
#include <wtf/ASCIICType.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/RapidHash.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct Instance;
struct State;

// What compiled code passes as isLikelyKey to operationAOTConcatenate and operationAOTConcatenatePair.
enum class UseOfJoinedText : uint8_t {
    Unknown,
    LikelyKey,
    // A key of properties alone. What is made for it is a name at once, and is not made one by the first access.
    LikelyName,
};

// The short texts that a program joins to look something up with. Each is one flat string, found again by its parts, by its characters or by the integer that it spells.
// Nothing here keeps a string alive: an entry goes with the first of its cells that dies.
class StringKeyCache {
    WTF_MAKE_NONCOPYABLE(StringKeyCache);
    WTF_MAKE_TZONE_ALLOCATED(StringKeyCache);
public:
    static constexpr size_t maxCachedKeyLength = 64;

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
            } else if (std::optional<int32_t> integer = value.tryGetAsInt32()) {
                // A number that compiled code holds as a double is spelled as the integer that it is, -0 as 0.
                part = *integer;
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
            static_assert(count && count <= maxNumberOfParts);
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

        // No parts look like a place that holds nothing.
        bool tryTake(uintptr_t cageBase, std::span<const EncodedJSValue> values)
        {
            if (values.empty() || values.size() > maxNumberOfParts)
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

        // Two parts as they are encoded, for the table of pairs.
        static constexpr uint32_t pairMark = 1u << 31;
        void takePair(EncodedJSValue first, EncodedJSValue second)
        {
            countAndKinds = pairMark;
            partPairs = { static_cast<uint64_t>(first), static_cast<uint64_t>(second), 0 };
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

    // The key of two parts that were joined before. A part is compared as it is encoded: a cell by its address, any other value by its bits. The stub ConcatenateKeyOfPair looks it up.
    // Beside the key: what KeyedValuePlace says of the key and one Structure, so that a read by the two parts asks one table. The stub GetByValOfKeyOfPair copies it from there.
    // It holds for as long as the place it was copied from would: forgetPlacesOfPairs().
    struct KeyOfPair {
        // The two top bits of slotOfPlace say how the value is read, so that the usual reads compare nothing. x86-64 gets both from the flags of one shift.
        // Under the first three kinds lies half the offset of the word in the cell, which is even.
        enum class Read : uint32_t {
            // A field with the SlotRepresentation of that name.
            Int32,
            OptionalInt32,
            // A whole word that holds an encoded value: a field with SlotRepresentation::JSValue or Cell, or an own value that lies in the cell.
            // With isNarrowReference: a field with SlotRepresentation::Cell32 or OptionalCell32.
            EncodedValueOrNarrowReference,
            Other,
        };
        static constexpr uint32_t isNarrowReference = 1;
        // The next two bits of Read::Other, above 28 bits.
        enum class OtherRead : uint32_t {
            // Any other field that lies in the cell: its offset above the eight bits of its representation.
            Field,
            // Slot::wordIndexFor() of an own value out of line, which is below zero.
            ValueOutOfLine,
            // PropertyCacheWords::timesAbsencesWereForgotten as it was when nothing was found.
            Absence,
            // hiddenWordOffset above wordIndex of a KeyedValuePlace::ExtraProperty.
            ExtraProperty,
        };
        static constexpr unsigned bitsBelowRead = 30;
        static constexpr unsigned bitsBelowOtherRead = 28;
        static constexpr uint32_t maskBelowOtherRead = (1u << bitsBelowOtherRead) - 1;
        static constexpr unsigned bitsOfRepresentation = 8;
        static constexpr uint32_t maxOffsetOfOtherField = (1u << (bitsBelowOtherRead - bitsOfRepresentation)) - 1;
        static constexpr uint32_t markOf(Read read) { return static_cast<uint32_t>(read) << bitsBelowRead; }
        static constexpr uint32_t markOf(OtherRead read) { return markOf(Read::Other) | static_cast<uint32_t>(read) << bitsBelowOtherRead; }
        // The place of one Structure gives way at every eighth read of an object of another. Objects of two that are read in turns would else write over one another at every read.
        static constexpr int32_t stepOfMisses = (UINT8_MAX + 1) / 8;

        EncodedJSValue first { 0 };
        EncodedJSValue second { 0 };
        JSString* key { nullptr };
        uint32_t structureIDOfPlace { 0 };
        uint32_t slotOfPlace { 0 };
    };
    static constexpr size_t waysOfKeysOfPairs = 2;
    static constexpr unsigned keyOfPairSetShift = 6;
    static_assert(waysOfKeysOfPairs * sizeof(KeyOfPair) == 1 << keyOfPairSetShift);
    static constexpr uint32_t keyOfPairMultiplier = 0x9e3779b1u;
    static constexpr unsigned keyOfPairHashShift = 13;
    static constexpr uint64_t keyOfPairMaskFor(size_t tableSize) { return (tableSize / waysOfKeysOfPairs - 1) << keyOfPairSetShift; }
    // Under the mask: how many bytes behind the start of the table the set of the pair lies.
    static constexpr uint32_t hashOfPair(EncodedJSValue first, EncodedJSValue second)
    {
        uint32_t mixed = static_cast<uint32_t>(static_cast<uint64_t>(first) + (static_cast<uint64_t>(second) << 3));
        return mixed * keyOfPairMultiplier >> keyOfPairHashShift;
    }
    // One empty set, for an instance that has joined nothing yet.
    JS_EXPORT_PRIVATE static const KeyOfPair* noKeysOfPairs();
    void cacheKeyOfPair(EncodedJSValue first, EncodedJSValue second, JSString* key);
    void forgetPlacesOfPairs();
    // By the words of the instance. A stub sets pairsHavePlaces only where it has written into a table of the instance's own.
    JS_EXPORT_PRIVATE static void forgetPlacesOfPairs(Instance&);
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
    // Every road to the text of an integer asks for it by the integer first, so the text is entered in no table by its characters.
    JSString* textOfInt32(VM&, int32_t);

    // Null where there is no memory for the text.
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
    // Null where there is no memory for the text.
    JSString* makeKey(VM&, std::span<const Latin1Character>, unsigned hash);
    ALWAYS_INLINE JSString* makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash, const KeyByParts& parts, bool isName)
    {
        String text = isName ? nameWith(characters, hash) : String(StringImpl::tryCreate(characters, hash));
        if (text.isNull()) [[unlikely]]
            return nullptr;
        JSString* key = jsNontrivialString(vm, WTF::move(text));
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
    // The hash is set when the result is Absent.
    ALWAYS_INLINE Lookup findKeyForText(std::span<const Latin1Character> characters, JSString*& key, unsigned& hash) const
    {
        ASSERT(characters.size() >= 2 && characters.size() <= maxCachedKeyLength);
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

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static constexpr size_t minTableSize = 1 << 8;
    static constexpr size_t maxCachedKeyTableSize = 1 << 16;
    static constexpr size_t maxKeysByPartsTableSize = 1 << 14;
    static constexpr size_t maxIntegerKeyTableSize = 1 << 16;

    static size_t firstIndex(unsigned hash, size_t tableSize) { return hash & (tableSize - 1) & ~static_cast<size_t>(1); }
    static String nameWith(std::span<const Latin1Character> characters, unsigned hash)
    {
        WTF::HashTranslatorCharBuffer<Latin1Character> buffer { characters, hash };
        return String(AtomStringImpl::add(buffer));
    }
    bool canEncode(const JSCell* cell) const { return std::bit_cast<uintptr_t>(cell) - m_cageBase < m_cageSize; }
    uint32_t encode(const JSCell* cell) const { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(cell) >> MarkedBlockCage::shift); }
    ALWAYS_INLINE void addCachedKey(JSString* key, unsigned hash)
    {
        if (m_cachedKeys.isEmpty() && !tryResize(m_cachedKeys, minTableSize)) [[unlikely]]
            return;
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
        if (parts.countAndKinds == KeyByParts::pairMark) {
            cacheKeyOfPair(parts.partPairs[0], parts.partPairs[1], key);
            return;
        }
        if (m_keysByParts.isEmpty() && !tryResize(m_keysByParts, minTableSize)) [[unlikely]]
            return;
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
    // A key that has named a property holds the atom of its characters, which may be a 16-bit string.
    ALWAYS_INLINE JSString* matchingKey(uint64_t entry, std::span<const Latin1Character> characters) const
    {
        JSString* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)));
        const StringImpl& impl = *candidate->tryGetValueImpl();
        if (!impl.is8Bit()) [[unlikely]]
            return equal(&impl, characters) ? candidate : nullptr;
        return CompactTable::hasCharacters<Latin1Character>(impl, characters) ? candidate : nullptr;
    }
    JSString* findAmongEqualHashes(const uint64_t* pair, std::span<const Latin1Character>) const;
    // False where the table cannot name the key.
    bool cacheIntegerKey(uint32_t integer, JSString* key);
    size_t removeDeadCachedKeys(VM&);
    size_t removeDeadKeysByParts(VM&);
    size_t removeDeadKeysOfPairs(VM&);
    size_t removeDeadIntegerKeys(VM&);
    void resizeIntegerKeys(size_t tableSize);
    // The table becomes an empty one of that size and its former entries are handed back. Where there is no memory for it, the table stays as it is: what it does not hold is made again.
    template<typename T>
    static std::optional<Vector<T>> tryResize(Vector<T>& table, size_t size)
    {
        Vector<T> resized;
        if (!resized.tryReserveCapacity(size))
            return std::nullopt;
        resized.fill({ }, size);
        return std::exchange(table, WTF::move(resized));
    }
    void growCachedKeys();
    void growKeysByParts();
    bool addKeyOfPair(const KeyOfPair&);
    void growKeysOfPairs();
    void publishKeysOfPairs();
    void publishIntegerKeys();

    Instance& m_instance;
    uintptr_t m_cageBase;
    size_t m_cageSize;
    Vector<uint64_t> m_cachedKeys;
    size_t m_cachedKeyEvictionCount { 0 };
    Vector<uint64_t> m_integerKeys;
    size_t m_integerKeyEvictionCount { 0 };
    size_t m_integerKeysEnteredSinceSweep { 0 };
    Vector<KeyByParts> m_keysByParts;
    size_t m_keysByPartsEvictionCount { 0 };
    KeyByParts m_lastLookupByParts;
    Vector<KeyOfPair> m_keysOfPairs;
    size_t m_keyOfPairEvictionCount { 0 };
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
