/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "SmallStrings.h"
#include <algorithm>
#include <bit>
#include <unicode/utf16.h>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr char32_t minCodePointForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };

bool hasCellOfVM(std::span<const uint8_t> bytes)
{
    constexpr size_t maxEncodedSizeOfLatin1Character = 2;
    if (bytes.size() > maxEncodedSizeOfLatin1Character)
        return false;
    Vector<char16_t> units = decodeProgramString(bytes);
    return units.isEmpty() || (units.size() == 1 && units[0] <= maxSingleCharacterString);
}

Vector<char16_t> decodeProgramString(std::span<const uint8_t> bytes)
{
    Vector<char16_t> units;
    units.grow(bytes.size());
    std::span<char16_t> room = units.mutableSpan();
    size_t numberOfUnits = 0;
    for (size_t i = 0; i < bytes.size();) {
        uint8_t lead = bytes[i];
        if (lead < 0x80) {
            room[numberOfUnits++] = lead;
            ++i;
            continue;
        }
        unsigned length = lead < 0xc0 || lead >= 0xf8 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        char32_t character = length == 1 ? lead : lead & (0x7f >> length);
        bool isWhole = i + length <= bytes.size();
        for (unsigned k = 1; k < length && isWhole; ++k) {
            isWhole = (bytes[i + k] & 0xc0) == 0x80;
            character = character << 6 | (bytes[i + k] & 0x3f);
        }
        if (!isWhole || character < minCodePointForLength[length] || character > 0x10ffff) {
            character = 0xfffd;
            length = 1;
        }
        i += length;
        if (character < 0x10000)
            room[numberOfUnits++] = static_cast<char16_t>(character);
        else {
            room[numberOfUnits++] = static_cast<char16_t>(0xd7c0 + (character >> 10));
            room[numberOfUnits++] = static_cast<char16_t>(0xdc00 | (character & 0x3ff));
        }
    }
    units.shrink(numberOfUnits);
    return units;
}

std::strong_ordering compareProgramStrings(std::span<const uint8_t> first, std::span<const uint8_t> second)
{
    constexpr uint8_t surrogateLeadByte = 0xED;
    auto [firstByte, secondByte] = std::ranges::mismatch(first, second);
    if (firstByte == first.end() || secondByte == second.end())
        return first.size() <=> second.size();
    if (*firstByte < surrogateLeadByte && *secondByte < surrogateLeadByte)
        return *firstByte <=> *secondByte;
    auto firstUnits = decodeProgramString(first);
    auto secondUnits = decodeProgramString(second);
    return std::lexicographical_compare_three_way(firstUnits.begin(), firstUnits.end(), secondUnits.begin(), secondUnits.end());
}

Vector<uint8_t> encodeProgramString(std::span<const char16_t> units)
{
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(units.size());
    auto appendContinuationBytes = [&](char32_t character, unsigned count) {
        while (count--)
            bytes.append(static_cast<uint8_t>(0x80 | (character >> (6 * count) & 0x3f)));
    };
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t character = units[i];
        if (U16_IS_LEAD(character) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1])) {
            character = static_cast<char32_t>(U16_GET_SUPPLEMENTARY(character, units[i + 1]));
            ++i;
        }
        if (character < 0x80)
            bytes.append(static_cast<uint8_t>(character));
        else if (character < 0x800) {
            bytes.append(static_cast<uint8_t>(0xc0 | character >> 6));
            appendContinuationBytes(character, 1);
        } else if (character < 0x10000) {
            bytes.append(static_cast<uint8_t>(0xe0 | character >> 12));
            appendContinuationBytes(character, 2);
        } else {
            bytes.append(static_cast<uint8_t>(0xf0 | character >> 18));
            appendContinuationBytes(character, 3);
        }
    }
    return bytes;
}

Vector<uint8_t> encodeProgramString(StringView view)
{
    if (view.is8Bit() && charactersAreAllASCII(view.span8()))
        return Vector<uint8_t>(byteCast<uint8_t>(view.span8()));
    return encodeProgramString(view.upconvertedCharacters().span());
}

bool isCanonicalProgramString(std::span<const uint8_t> bytes)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return true;
    bool isAfterLeadSurrogate = false;
    for (size_t i = 0; i < bytes.size();) {
        uint8_t lead = bytes[i];
        if (lead < 0x80) {
            isAfterLeadSurrogate = false;
            ++i;
            continue;
        }
        if ((lead >= 0x80 && lead < 0xc2) || lead > 0xf4)
            return false;
        unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (length > bytes.size() - i)
            return false;
        char32_t character = length == 1 ? lead : lead & (0x7f >> length);
        for (unsigned k = 1; k < length; ++k) {
            if ((bytes[i + k] & 0xc0) != 0x80)
                return false;
            character = character << 6 | (bytes[i + k] & 0x3f);
        }
        if (character < minCodePointForLength[length] || character > 0x10ffff)
            return false;
        if (isAfterLeadSurrogate && U_IS_TRAIL(character))
            return false;
        isAfterLeadSurrogate = U_IS_LEAD(character);
        i += length;
    }
    return true;
}

void StringTable::initialize(const BuiltinTable& builtins, const ParallelLoops& loops, Vector<StringAsRead>&& strings)
{
    RELEASE_ASSERT(!m_builtins);
    m_builtins = &builtins;
    size_t numberOfStrings = strings.size();
    m_numberOfStringsAsRead = numberOfStrings;
    size_t largestSizeWhenInitialized = numberOfStrings + 6 + builtins.nameCount();
    m_strings.reserveInitialCapacity(largestSizeWhenInitialized);
    m_nameClasses.reserveInitialCapacity(largestSizeWhenInitialized);
    m_builtinNames.reserveInitialCapacity(largestSizeWhenInitialized);
    m_strings.grow(numberOfStrings);
    m_nameClasses.grow(numberOfStrings);
    m_builtinNames.grow(numberOfStrings);

    constexpr size_t stringsPerTask = 1024;
    Vector<uint64_t> hashes(numberOfStrings);
    Vector<uint32_t> stringsInOrderOfShards(numberOfStrings);
    Vector<std::array<uint16_t, numberOfShards + 1>> firstsOfShardsByTask((numberOfStrings + stringsPerTask - 1) / stringsPerTask);
    CodeUnitSet codeUnits { };
    Lock codeUnitsLock;
    forEachIndexInParallel(loops, static_cast<unsigned>((numberOfStrings + stringsPerTask - 1) / stringsPerTask), [&](unsigned task) {
        CodeUnitSet codeUnitsOfTask { };
        ASCIIUnits asciiUnitsOfTask { };
        size_t end = std::min(numberOfStrings, (static_cast<size_t>(task) + 1) * stringsPerTask);
        for (size_t i = static_cast<size_t>(task) * stringsPerTask; i < end; ++i) {
            const StringAsRead& string = strings[i];
            m_strings[i] = string.bytes;
            m_nameClasses[i] = string.nameClass;
            m_builtinNames[i] = string.builtinName;
            hashes[i] = hashOf(string.bytes);
            addCodeUnitsOf(string.bytes, asciiUnitsOfTask, codeUnitsOfTask);
        }
        addASCIIUnits(asciiUnitsOfTask, codeUnitsOfTask);
        size_t begin = static_cast<size_t>(task) * stringsPerTask;
        auto& firstsOfShards = firstsOfShardsByTask[task];
        firstsOfShards.fill(0);
        for (size_t i = begin; i < end; ++i)
            ++firstsOfShards[shardIndexOf(hashes[i]) + 1];
        for (unsigned shardIndex = 0; shardIndex < numberOfShards; ++shardIndex)
            firstsOfShards[shardIndex + 1] += firstsOfShards[shardIndex];
        auto nextsOfShards = firstsOfShards;
        for (size_t i = begin; i < end; ++i)
            stringsInOrderOfShards[begin + nextsOfShards[shardIndexOf(hashes[i])]++] = static_cast<uint32_t>(i);
        Locker locker { codeUnitsLock };
        for (size_t word = 0; word < codeUnits.size(); ++word)
            codeUnits[word] |= codeUnitsOfTask[word];
    });

    forEachIndexInParallel(loops, numberOfShards, [&](unsigned shardIndex) {
        size_t numberInShard = 0;
        for (const auto& firstsOfShards : firstsOfShardsByTask)
            numberInShard += firstsOfShards[shardIndex + 1] - firstsOfShards[shardIndex];
        Shard shard;
        reserve(shard, numberInShard);
        for (size_t task = 0; task < firstsOfShardsByTask.size(); ++task) {
            for (size_t place = firstsOfShardsByTask[task][shardIndex]; place < firstsOfShardsByTask[task][shardIndex + 1]; ++place) {
                uint32_t string = stringsInOrderOfShards[task * stringsPerTask + place];
                insert(shard, hashes[string], StringID(string));
            }
        }
        m_shards[shardIndex] = WTF::move(shard);
    });

    m_builtinNameStrings.fill(StringID(), builtins.nameCount());
    for (size_t i = 0; i < numberOfStrings; ++i)
        recordBuiltinName(StringID(i));
    m_emptyString = intern(std::span<const uint8_t> { });
    m_comma = intern(StringView(","_s));
    m_textOfUndefined = intern(StringView("undefined"_s));
    m_textOfNull = intern(StringView("null"_s));
    m_textOfTrue = intern(StringView("true"_s));
    m_textOfFalse = intern(StringView("false"_s));
    appendBuiltinNames();
    ASCIIUnits asciiUnits { };
    for (size_t i = numberOfStrings; i < m_strings.size(); ++i)
        addCodeUnitsOf(m_strings[i], asciiUnits, codeUnits);
    addASCIIUnits(asciiUnits, codeUnits);
    internCodeUnitsIn(codeUnits);
}

void StringTable::addCodeUnitsOf(std::span<const uint8_t> bytes, ASCIIUnits& asciiUnits, CodeUnitSet& codeUnits)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes))) {
        for (uint8_t byte : bytes)
            asciiUnits[byte] = 1;
        return;
    }
    for (char16_t unit : decodeProgramString(bytes))
        codeUnits[unit / 64] |= 1ull << (unit % 64);
}

void StringTable::addASCIIUnits(const ASCIIUnits& asciiUnits, CodeUnitSet& codeUnits)
{
    for (unsigned unit = 0; unit < asciiUnits.size(); ++unit)
        codeUnits[unit / 64] |= static_cast<uint64_t>(asciiUnits[unit]) << (unit % 64);
}

StringID StringTable::internCodeUnit(char16_t unit)
{
    m_internedCodeUnits[unit / 64] |= 1ull << (unit % 64);
    std::span<const char16_t> text = singleElementSpan(unit);
    return intern(encodeProgramString(text).span());
}

void StringTable::internCodeUnitsIn(const CodeUnitSet& codeUnits)
{
    for (size_t word = 0; word < codeUnits.size(); ++word) {
        for (uint64_t bits = codeUnits[word]; bits; bits &= bits - 1)
            internCodeUnit(static_cast<char16_t>(word * 64 + std::countr_zero(bits)));
    }
}

void StringTable::internCodeUnitsOf(std::span<const uint8_t> bytes, size_t numberOfBytesLookedAt)
{
    auto internIfNew = [&](char16_t unit) {
        if (!(m_internedCodeUnits[unit / 64] >> (unit % 64) & 1))
            internCodeUnit(unit);
    };
    std::span<const uint8_t> rest = bytes.subspan(numberOfBytesLookedAt);
    if (charactersAreAllASCII(byteCast<Latin1Character>(rest))) {
        if (bytes.size() < 2)
            return;
        for (uint8_t byte : rest)
            internIfNew(byte);
        return;
    }
    Vector<char16_t> units = decodeProgramString(rest);
    if (units.size() < 2 && !numberOfBytesLookedAt)
        return;
    for (char16_t unit : units)
        internIfNew(unit);
}

StringID StringTable::codeUnitAt(StringID id, uint64_t position) const
{
    std::span<const uint8_t> bytes = (*this)[id];
    StringID codeUnit;
    if (isAllASCII(id)) {
        if (position >= bytes.size())
            return { };
        codeUnit = find(bytes.subspan(position, 1));
    } else {
        Vector<char16_t> units = decodeProgramString(bytes);
        if (position >= units.size())
            return { };
        std::span<const char16_t> text = units.span().subspan(position, 1);
        codeUnit = find(encodeProgramString(text).span());
    }
    RELEASE_ASSERT(codeUnit);
    return codeUnit;
}

bool StringTable::isAllASCII(StringID id) const
{
    return charactersAreAllASCII(byteCast<Latin1Character>((*this)[id]));
}

StringView viewOfProgramString(std::span<const uint8_t> bytes, Vector<char16_t>& bufferForNonASCII)
{
    if (bytes.empty())
        return emptyStringView();
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return StringView(byteCast<Latin1Character>(bytes));
    bufferForNonASCII = decodeProgramString(bytes);
    return StringView(bufferForNonASCII.span());
}

StringView StringTable::viewOf(StringID id, Vector<char16_t>& bufferForNonASCII) const
{
    return viewOfProgramString((*this)[id], bufferForNonASCII);
}

String StringTable::toWTFString(StringID id) const
{
    if (id == m_emptyString)
        return WTF::emptyString();
    if (isAllASCII(id))
        return String(byteCast<Latin1Character>((*this)[id]));
    return String(decodeProgramString((*this)[id]).span());
}

StringID StringTable::find(std::span<const uint8_t> canonicalProgramString) const
{
    ASSERT(isCanonicalProgramString(canonicalProgramString));
    uint64_t hash = hashOf(canonicalProgramString);
    return find(m_shards[shardIndexOf(hash)], hash, canonicalProgramString);
}

StringID StringTable::intern(std::span<const uint8_t> bytes)
{
    RELEASE_ASSERT(m_builtins);
    Vector<uint8_t> canonicalBytes;
    if (!isCanonicalProgramString(bytes)) {
        canonicalBytes = encodeProgramString(decodeProgramString(bytes).span());
        bytes = canonicalBytes.span();
    }
    uint64_t hash = hashOf(bytes);
    if (StringID id = find(m_shards[shardIndexOf(hash)], hash, bytes))
        return id;
    m_ownStrings.append(Vector<uint8_t>(bytes));
    StringID id = append(m_ownStrings.last().span(), hash);
    internCodeUnitsOf(bytes);
    return id;
}

StringID StringTable::intern(StringView view)
{
    return intern(encodeProgramString(view).span());
}

std::span<const uint8_t> StringTable::keep(std::span<const uint8_t> bytes)
{
    m_ownStrings.append(Vector<uint8_t>(bytes));
    return m_ownStrings.last().span();
}

StringID StringTable::internKept(std::span<const uint8_t> bytes, FrontsOfKeptTexts& fronts)
{
    RELEASE_ASSERT(m_builtins);
    ASSERT(isCanonicalProgramString(bytes));
    FrontOfKeptTexts& front = fronts.add(bytes.data(), FrontOfKeptTexts { }).iterator->value;
    uint64_t hash = hashGoingOnFrom(front, bytes);
    StringID id = find(m_shards[shardIndexOf(hash)], hash, bytes);
    size_t numberOfBytesLookedAt = std::min(std::exchange(front.numberOfBytesLookedAt, std::max(front.numberOfBytesLookedAt, bytes.size())), bytes.size());
    if (id)
        return id;
    id = append(bytes, hash);
    internCodeUnitsOf(bytes, numberOfBytesLookedAt);
    return id;
}

uint64_t StringTable::hashOf(std::span<const uint8_t> bytes)
{
    FrontOfKeptTexts nothing;
    return hashGoingOnFrom(nothing, bytes);
}

uint64_t StringTable::hashOf(StringID id) const
{
    return id.index() < m_numberOfStringsAsRead ? hashOf(m_strings[id.index()]) : m_hashesOfInternedStrings[id.index() - m_numberOfStringsAsRead];
}

uint64_t StringTable::hashGoingOnFrom(FrontOfKeptTexts& front, std::span<const uint8_t> bytes)
{
    constexpr uint64_t multiplier = 0x9e3779b97f4a7c15;
    auto add = [](uint64_t hash, uint64_t word) {
        hash = (hash ^ word) * multiplier;
        return hash ^ hash >> 32;
    };
    size_t numberOfBytes = bytes.size();
    size_t numberOfWords = numberOfBytes / sizeof(uint64_t);
    if (front.numberOfWords > numberOfWords) {
        front.hashOfWords = 0;
        front.numberOfWords = 0;
    }
    uint64_t hash = front.hashOfWords;
    bytes = bytes.subspan(front.numberOfWords * sizeof(uint64_t));
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t))) {
        uint64_t word;
        memcpySpan(asMutableByteSpan(word), bytes.first(sizeof(uint64_t)));
        hash = add(hash, word);
    }
    front.hashOfWords = hash;
    front.numberOfWords = numberOfWords;
    uint64_t rest = 0;
    for (size_t index = 0; index < bytes.size(); ++index)
        rest |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    hash = add(add(hash, rest), numberOfBytes);
    hash ^= hash >> 29;
    hash *= 0xbf58476d1ce4e5b9;
    hash ^= hash >> 32;
    return hash;
}

StringID StringTable::find(const Shard& shard, uint64_t hash, std::span<const uint8_t> bytes) const
{
    if (shard.slots.isEmpty())
        return { };
    size_t slot = firstSlotOf(shard, hash);
    while (StringID id = shard.slots[slot]) {
        if (equalSpans(m_strings[id.index()], bytes))
            return id;
        slot = nextSlotOf(shard, slot);
    }
    return { };
}

void StringTable::insert(Shard& shard, uint64_t hash, StringID id)
{
    ASSERT(shard.slots.size() >= 2 * (static_cast<size_t>(shard.count) + 1));
    size_t slot = firstSlotOf(shard, hash);
    while (shard.slots[slot])
        slot = nextSlotOf(shard, slot);
    shard.slots[slot] = id;
    ++shard.count;
}

void StringTable::reserve(Shard& shard, size_t count) const
{
    static constexpr size_t minNumberOfSlots = 8;
    if (shard.slots.size() >= 2 * count)
        return;
    size_t numberOfSlots = std::max(minNumberOfSlots, roundUpToPowerOfTwo(2 * count));
    Vector<StringID> oldSlots = std::exchange(shard.slots, Vector<StringID>(FillWith { }, numberOfSlots, StringID()));
    shard.count = 0;
    for (StringID id : oldSlots) {
        if (id)
            insert(shard, hashOf(id), id);
    }
}

StringID StringTable::append(std::span<const uint8_t> bytes, uint64_t hash)
{
    return append(bytes, hash, m_builtins->nameOf(bytes));
}

StringID StringTable::append(std::span<const uint8_t> bytes, uint64_t hash, BuiltinName name)
{
    StringID id(m_strings.size());
    m_strings.constructAndAppend(bytes);
    m_nameClasses.append(classifyName(bytes));
    m_builtinNames.append(name);
    m_hashesOfInternedStrings.append(hash);
    recordBuiltinName(id);
    Shard& shard = m_shards[shardIndexOf(hash)];
    reserve(shard, static_cast<size_t>(shard.count) + 1);
    insert(shard, hash, id);
    return id;
}

void StringTable::recordBuiltinName(StringID id)
{
    BuiltinName name = builtinName(id);
    if (name && !stringOf(name))
        m_builtinNameStrings[name.index()] = id;
}

void StringTable::appendBuiltinNames()
{
    struct NameToAppend {
        BuiltinName name;
        uint64_t hash;
    };
    unsigned nameCount = m_builtins->nameCount();
    Vector<NameToAppend> names;
    names.reserveInitialCapacity(nameCount);
    std::array<uint32_t, numberOfShards> countInShard { };
    for (unsigned index = 0; index < nameCount; ++index) {
        BuiltinName name(index);
        if (m_builtins->isSymbolName(name) || m_builtins->isRegisteredSymbolName(name) || stringOf(name))
            continue;
        uint64_t hash = hashOf(m_builtins->textOf(name));
        names.append({ name, hash });
        ++countInShard[shardIndexOf(hash)];
    }
    for (unsigned shardIndex = 0; shardIndex < numberOfShards; ++shardIndex)
        reserve(m_shards[shardIndex], static_cast<size_t>(m_shards[shardIndex].count) + countInShard[shardIndex]);
    for (auto [name, hash] : names) {
        std::span<const uint8_t> text = m_builtins->textOf(name);
        ASSERT(isCanonicalProgramString(text));
        RELEASE_ASSERT(!find(m_shards[shardIndexOf(hash)], hash, text));
        StringID id = append(text, hash, name);
        RELEASE_ASSERT(stringOf(name) == id);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
