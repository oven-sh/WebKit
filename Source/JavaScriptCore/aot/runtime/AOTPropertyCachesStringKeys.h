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
#include <bit>
#include <optional>
#include <span>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/Int128.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/RapidHash.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct Instance;
struct State;

enum class UseOfJoinedText : uint8_t {
    Unknown,
    LikelyKey,
    LikelyName,
};

struct SharedPart {
    static constexpr uint32_t minLength = 2;
    static constexpr uint32_t maxLength = 16;
    static constexpr uint32_t unitsOfWord = sizeof(uint64_t) / sizeof(char16_t);
    static constexpr unsigned numberOfParts = 1024;
    static constexpr unsigned sizeShift = 6;
    static constexpr unsigned indexShift = 64 - WTF::fastLog2(numberOfParts);
    static constexpr unsigned tagShift = 22;
    static constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    static constexpr uint64_t bitsOfWideUnits = 0xff00ff00ff00ff00ull;

    SharedPart() = default;
    SharedPart(std::span<const char16_t> units)
        : length(units.size())
    {
        static_assert(std::endian::native == std::endian::little);
        ASSERT(length >= minLength && length <= maxLength);
        const char16_t* end = units.data() + length;
        if (length < unitsOfWord) {
            first = WTF::unalignedLoad<uint32_t>(units.data());
            last = WTF::unalignedLoad<uint32_t>(end - 2);
            return;
        }
        first = WTF::unalignedLoad<uint64_t>(units.data());
        last = WTF::unalignedLoad<uint64_t>(end - unitsOfWord);
        if (length <= 2 * unitsOfWord)
            return;
        second = WTF::unalignedLoad<uint64_t>(units.data() + unitsOfWord);
        third = WTF::unalignedLoad<uint64_t>(end - 2 * unitsOfWord);
    }
    bool isLatin1() const { return !((first | second | third | last) & bitsOfWideUnits); }
    static uint32_t narrow(uint64_t units)
    {
        units |= units >> 8;
        units &= 0x0000ffff0000ffffull;
        return static_cast<uint32_t>(units | units >> 16);
    }
    void copyLatin1Characters(std::span<Latin1Character> characters) const
    {
        ASSERT(isLatin1() && characters.size() == length);
        Latin1Character* begin = characters.data();
        Latin1Character* end = begin + length;
        if (length < unitsOfWord) {
            WTF::unalignedStore<uint16_t>(begin, narrow(first));
            WTF::unalignedStore<uint16_t>(end - 2, narrow(last));
            return;
        }
        if (length > 2 * unitsOfWord) {
            WTF::unalignedStore<uint32_t>(begin + unitsOfWord, narrow(second));
            WTF::unalignedStore<uint32_t>(end - 2 * unitsOfWord, narrow(third));
        }
        WTF::unalignedStore<uint32_t>(begin, narrow(first));
        WTF::unalignedStore<uint32_t>(end - unitsOfWord, narrow(last));
    }
    uint64_t hash() const { return (first + 2 * last + length) * multiplier; }
    unsigned index() const { return static_cast<unsigned>(hash() >> indexShift); }
    uint32_t tag() const { return static_cast<uint32_t>(hash() >> tagShift); }
    bool hasSameUnits(const SharedPart& other) const { return first == other.first && second == other.second && third == other.third && last == other.last && length == other.length; }

    uint64_t first { 0 };
    uint64_t second { 0 };
    uint64_t third { 0 };
    uint64_t last { 0 };
    uint64_t length { 0 };
    const JSString* string { nullptr };
    uint64_t unused[2] { };
};
static_assert(sizeof(SharedPart) == 1 << SharedPart::sizeShift);

struct SharedParts {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(SharedParts);

    std::array<uint32_t, SharedPart::numberOfParts> tags { };
    std::array<SharedPart, SharedPart::numberOfParts> parts { };
};

ALWAYS_INLINE std::optional<std::span<const Latin1Character>> latin1CharactersOf(StringView string, std::span<Latin1Character> buffer)
{
    if (string.is8Bit())
        return string.span8();
    std::span<const char16_t> units = string.span16();
    if (units.size() < SharedPart::minLength || units.size() > buffer.size() || !WTF::isLatin1(units[0]))
        return std::nullopt;
    std::span<Latin1Character> characters = buffer.first(units.size());
    if (units.size() > SharedPart::maxLength) {
        if (!WTF::charactersAreAllLatin1(units))
            return std::nullopt;
        StringImpl::copyCharacters(characters, units);
        return characters;
    }
    SharedPart part { units };
    if (!part.isLatin1())
        return std::nullopt;
    part.copyLatin1Characters(characters);
    return characters;
}

RefPtr<StringImpl> makeCharactersOfShortPart(Instance&, const JSRopeString&, std::span<const char16_t> units);

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

    struct KeyOfPair {
        enum class Read : uint32_t {
            Int32,
            OptionalInt32,
            EncodedValueOrNarrowReference,
            Other,
        };
        static constexpr uint32_t isNarrowReference = 1;
        enum class OtherRead : uint32_t {
            Field,
            ValueOutOfLine,
            Absence,
            ExtraProperty,
        };
        static constexpr unsigned bitsBelowRead = 30;
        static constexpr unsigned bitsBelowOtherRead = 28;
        static constexpr uint32_t maskBelowOtherRead = (1u << bitsBelowOtherRead) - 1;
        static constexpr unsigned bitsOfRepresentation = 8;
        static constexpr uint32_t maxOffsetOfOtherField = (1u << (bitsBelowOtherRead - bitsOfRepresentation)) - 1;
        static constexpr uint32_t markOf(Read read) { return static_cast<uint32_t>(read) << bitsBelowRead; }
        static constexpr uint32_t markOf(OtherRead read) { return markOf(Read::Other) | static_cast<uint32_t>(read) << bitsBelowOtherRead; }
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
    static constexpr uint32_t secondKeyMultiplier = 0x213bca7fu;
    static constexpr unsigned keyOfPairHashShift = 13;
    static constexpr uint64_t keyOfPairMaskFor(size_t tableSize) { return (tableSize / waysOfKeysOfPairs - 1) << keyOfPairSetShift; }
    static constexpr uint32_t hashOfPair(EncodedJSValue first, EncodedJSValue second)
    {
        uint32_t mixed = static_cast<uint32_t>(first) + static_cast<uint32_t>(second) * secondKeyMultiplier;
        return mixed * keyOfPairMultiplier >> keyOfPairHashShift;
    }
    JS_EXPORT_PRIVATE static const KeyOfPair* noKeysOfPairs();
    void cacheKeyOfPair(EncodedJSValue first, EncodedJSValue second, JSString* key);
    void forgetPlacesOfPairs();
    JS_EXPORT_PRIVATE static void forgetPlacesOfPairs(Instance&);
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
    JSString* textOfInt32(VM&, int32_t);

    struct FewCharacters {
        static constexpr uint32_t bytesOfWord = sizeof(uint64_t);
        static constexpr uint32_t mostCharacters = 2 * bytesOfWord;
        static constexpr uint32_t limitOfIntegers = 100000000;

        ALWAYS_INLINE bool tryAppend(uint64_t text, uint32_t size)
        {
            ASSERT(size && size <= sizeof(uint64_t));
            uint32_t end = length + size;
            if (end > mostCharacters)
                return false;
            if (length >= bytesOfWord)
                last |= text << (8 * (length - bytesOfWord));
            else {
                first |= text << (8 * length);
                if (end > bytesOfWord)
                    last = text >> (8 * (bytesOfWord - length));
            }
            length = end;
            return true;
        }

        ALWAYS_INLINE bool tryAppend(std::span<const Latin1Character> text)
        {
            const Latin1Character* from = text.data();
            size_t size = text.size();
            if (size == 1)
                return tryAppend(*from, 1);
            if (size > mostCharacters - length)
                return false;
            if (size >= sizeof(uint64_t)) {
                if (!tryAppend(WTF::unalignedLoad<uint64_t>(from), sizeof(uint64_t)))
                    return false;
                uint32_t rest = static_cast<uint32_t>(size - sizeof(uint64_t));
                return !rest || tryAppend(WTF::unalignedLoad<uint64_t>(from + rest) >> (8 * (sizeof(uint64_t) - rest)), rest);
            }
            if (size >= sizeof(uint32_t))
                return tryAppend(WTF::unalignedLoad<uint32_t>(from) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(from + size - sizeof(uint32_t))) << (8 * (size - sizeof(uint32_t))), static_cast<uint32_t>(size));
            return !size || tryAppend(from[0] | static_cast<uint64_t>(from[size / 2]) << (8 * (size / 2)) | static_cast<uint64_t>(from[size - 1]) << (8 * (size - 1)), static_cast<uint32_t>(size));
        }

        ALWAYS_INLINE bool tryAppendInteger(uint32_t integer)
        {
            constexpr uint32_t twoDigitLimit = 100;
            if (integer - twoDigitLimit >= limitOfIntegers - twoDigitLimit) {
                if (integer >= limitOfIntegers)
                    return false;
                uint32_t tens = integer * 103 >> 10;
                uint32_t digits = (tens | (integer - 10 * tens) << 8) | 0x3030;
                uint32_t leadingZeros = integer < 10;
                return tryAppend(digits >> (8 * leadingZeros), 2 - leadingZeros);
            }
            if (integer < 10000) {
                uint32_t pairs = integer / 100 | (integer % 100) << 16;
                uint32_t tens = pairs * 103 >> 10 & 0x000f000f;
                uint32_t digits = (tens | (pairs - 10 * tens) << 8) | 0x30303030;
                uint32_t leadingZeros = integer < 1000;
                return tryAppend(digits >> (8 * leadingZeros), 4 - leadingZeros);
            }
            uint64_t halves = integer / 10000 | static_cast<uint64_t>(integer % 10000) << 32;
            uint64_t hundreds = halves * 10486 >> 20 & 0x0000007f0000007full;
            uint64_t pairs = hundreds | (halves - 100 * hundreds) << 16;
            uint64_t tens = pairs * 103 >> 10 & 0x000f000f000f000full;
            uint64_t digits = tens | (pairs - 10 * tens) << 8;
            uint32_t leadingZeros = static_cast<uint32_t>(std::countr_zero(digits)) / 8;
            return tryAppend((digits | 0x3030303030303030ull) >> (8 * leadingZeros), 8 - leadingZeros);
        }

        uint64_t byteAt(uint32_t offset) const { return static_cast<uint8_t>(first >> (8 * offset)); }
        std::array<Latin1Character, mostCharacters> text() const { return std::bit_cast<std::array<Latin1Character, mostCharacters>>(std::array<uint64_t, 2> { first, last }); }
        ALWAYS_INLINE uint64_t lastEightBytes() const
        {
            ASSERT(length >= bytesOfWord);
            uint32_t bytesAfterFirst = length - bytesOfWord;
            return bytesAfterFirst == bytesOfWord ? last : first >> (8 * bytesAfterFirst) | (last << 1) << (8 * (bytesOfWord - bytesAfterFirst) - 1);
        }

        ALWAYS_INLINE unsigned hash() const
        {
            ASSERT(length && length <= mostCharacters);
            constexpr auto mix = [](uint64_t a, uint64_t b) ALWAYS_INLINE_LAMBDA {
                UInt128 product = static_cast<UInt128>(a) * b;
                return std::pair<uint64_t, uint64_t> { static_cast<uint64_t>(product), static_cast<uint64_t>(product >> 64) };
            };
            constexpr std::array<uint64_t, 3> secret = RapidHash::secret;
            constexpr auto seeds = mix(secret[0], secret[1]);
            uint64_t a;
            uint64_t b;
            if (length >= bytesOfWord) [[likely]] {
                uint64_t end = lastEightBytes();
                a = first << 32 | end >> 32;
                b = first >> 32 << 32 | static_cast<uint32_t>(end);
            } else if (length >= sizeof(uint32_t))
                a = b = first << 32 | static_cast<uint32_t>(first >> (8 * (length - sizeof(uint32_t))));
            else {
                a = byteAt(0) << 56 | byteAt(length / 2) << 32 | byteAt(length - 1);
                b = 0;
            }
            auto [low, high] = mix(a ^ secret[1], b ^ seeds.first ^ seeds.second ^ length);
            auto [lowOfHash, highOfHash] = mix(low ^ secret[0] ^ length, high ^ secret[1]);
            unsigned hash = static_cast<unsigned>(lowOfHash ^ highOfHash) & RapidHash::maskHash;
            return hash ? hash : 0x80000000 >> RapidHash::flagCount;
        }

        ALWAYS_INLINE bool isTextOf(const StringImpl& impl) const
        {
            ASSERT(length >= 2);
            if (impl.length() != length || !impl.is8Bit())
                return false;
            const Latin1Character* other = impl.span8().data();
            if (length >= bytesOfWord)
                return WTF::unalignedLoad<uint64_t>(other) == first && WTF::unalignedLoad<uint64_t>(other + length - bytesOfWord) == lastEightBytes();
            if (length >= sizeof(uint32_t))
                return WTF::unalignedLoad<uint32_t>(other) == static_cast<uint32_t>(first) && WTF::unalignedLoad<uint32_t>(other + length - sizeof(uint32_t)) == static_cast<uint32_t>(first >> (8 * (length - sizeof(uint32_t))));
            return other[0] == byteAt(0) && other[length / 2] == byteAt(length / 2) && other[length - 1] == byteAt(length - 1);
        }

        uint64_t first { 0 };
        uint64_t last { 0 };
        uint32_t length { 0 };
    };
    ALWAYS_INLINE JSString* tryFindCachedKey(const FewCharacters& characters, unsigned hash) const
    {
        size_t tableSize = m_cachedKeys.size();
        if (!tableSize) [[unlikely]]
            return nullptr;
        const uint64_t* pair = m_cachedKeys.span().data() + firstIndex(hash, tableSize);
        for (size_t i = 0; i < 2; ++i) {
            if (pair[i] >> 32 != hash)
                continue;
            JSString* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(pair[i])));
            if (characters.isTextOf(*candidate->tryGetValueImpl()))
                return candidate;
        }
        return nullptr;
    }

    JSString* findOrAdd(VM&, std::span<const Latin1Character>);
    RefPtr<StringImpl> makeCharactersOfShortPart(const JSRopeString&, std::span<const char16_t> units);
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
    ALWAYS_INLINE Lookup findKeyForText(std::span<const Latin1Character> characters, JSString*& key, unsigned& hash, bool mayBeIndex = true) const
    {
        ASSERT(characters.size() >= 2 && characters.size() <= maxCachedKeyLength);
        if (mayBeIndex && characters.size() <= maxIndexLength) {
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
    ALWAYS_INLINE JSString* matchingKey(uint64_t entry, std::span<const Latin1Character> characters) const
    {
        JSString* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)));
        const StringImpl& impl = *candidate->tryGetValueImpl();
        if (!impl.is8Bit()) [[unlikely]]
            return equal(&impl, characters) ? candidate : nullptr;
        return CompactTable::hasCharacters<Latin1Character>(impl, characters) ? candidate : nullptr;
    }
    JSString* findAmongEqualHashes(const uint64_t* pair, std::span<const Latin1Character>) const;
    bool cacheIntegerKey(uint32_t integer, JSString* key);
    size_t removeDeadCachedKeys(VM&);
    size_t removeDeadKeysByParts(VM&);
    size_t removeDeadKeysOfPairs(VM&);
    size_t removeDeadIntegerKeys(VM&);
    void resizeIntegerKeys(size_t tableSize);
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
    std::unique_ptr<SharedParts> m_sharedParts;
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
