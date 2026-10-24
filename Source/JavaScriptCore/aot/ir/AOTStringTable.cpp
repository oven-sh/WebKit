/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <unicode/utf16.h>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(StringTable::provisionalBit == ProvisionalStringID::provisionalBit);

static constexpr char32_t minCodePointForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };

Vector<char16_t> decodeProgramString(std::span<const uint8_t> bytes)
{
    Vector<char16_t> units;
    units.reserveInitialCapacity(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        uint8_t lead = bytes[i];
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
            units.append(static_cast<char16_t>(character));
        else {
            units.append(static_cast<char16_t>(0xd7c0 + (character >> 10)));
            units.append(static_cast<char16_t>(0xdc00 | (character & 0x3ff)));
        }
    }
    return units;
}

std::strong_ordering compareProgramStrings(std::span<const uint8_t> first, std::span<const uint8_t> second)
{
    constexpr uint8_t surrogateLeadByte = 0xED;
    auto [firstByte, secondByte] = std::ranges::mismatch(first, second);
    if (firstByte == first.end() || secondByte == second.end())
        return first.size() <=> second.size();
    // WTF-8 order is UTF-16 code unit order below U+D000.
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

static StringTable::Error checkStringAsRead(const BuiltinTable& builtins, const StringAsRead& string)
{
    if (!isCanonicalProgramString(string.bytes))
        return StringTable::Error::StringNotCanonical;
    if (classifyName(string.bytes) != string.nameClass || builtins.nameOf(string.bytes) != string.builtinName)
        return StringTable::Error::NameClassMismatch;
    return StringTable::Error::None;
}

StringTable::InitializationResult StringTable::initialize(const BuiltinTable& builtins, const ParallelLoops& loops, Vector<StringAsRead>&& strings, WhatIsSaidOfStrings whatIsSaid)
{
    RELEASE_ASSERT(!m_builtins && strings.size() < provisionalBit);
    m_builtins = &builtins;
    size_t numberOfStrings = strings.size();
    m_strings.grow(numberOfStrings);
    m_nameClasses.grow(numberOfStrings);
    m_builtinNames.grow(numberOfStrings);

    constexpr size_t stringsPerTask = 1024;
    Vector<uint64_t> hashes(numberOfStrings);
    Vector<Error> errors(numberOfStrings);
    forEachIndexInParallel(loops, static_cast<unsigned>((numberOfStrings + stringsPerTask - 1) / stringsPerTask), [&](unsigned task) {
        size_t end = std::min(numberOfStrings, (static_cast<size_t>(task) + 1) * stringsPerTask);
        for (size_t i = static_cast<size_t>(task) * stringsPerTask; i < end; ++i) {
            const StringAsRead& string = strings[i];
            m_strings[i] = string.bytes;
            m_nameClasses[i] = string.nameClass;
            m_builtinNames[i] = string.builtinName;
            hashes[i] = hashOf(string.bytes);
            errors[i] = whatIsSaid == WhatIsSaidOfStrings::IsChecked ? checkStringAsRead(builtins, string) : Error::None;
        }
    });
    size_t lowestInvalid = errors.findIf([](Error error) {
        return error != Error::None;
    });
    if (lowestInvalid != notFound)
        return { errors[lowestInvalid], StringID(lowestInvalid) };

    std::array<Vector<StringID>, numberOfShards> idsInShard;
    for (size_t i = 0; i < numberOfStrings; ++i)
        idsInShard[shardIndexOf(hashes[i])].append(StringID(i));
    std::array<StringID, numberOfShards> duplicateInShard;
    forEachIndexInParallel(loops, numberOfShards, [&](unsigned shardIndex) {
        Shard shard;
        reserve(shard, idsInShard[shardIndex].size());
        for (StringID id : idsInShard[shardIndex]) {
            uint64_t hash = hashes[id.index()];
            if (find(shard, hash, m_strings[id.index()])) {
                duplicateInShard[shardIndex] = id;
                return;
            }
            insert(shard, hash, id);
        }
        m_shards[shardIndex] = WTF::move(shard);
    });
    StringID lowestDuplicate;
    for (StringID duplicate : duplicateInShard) {
        if (duplicate && (!lowestDuplicate || duplicate < lowestDuplicate))
            lowestDuplicate = duplicate;
    }
    if (lowestDuplicate)
        return { Error::DuplicateString, lowestDuplicate };

    m_builtinNameStrings.fill(StringID(), builtins.nameCount());
    for (size_t i = 0; i < numberOfStrings; ++i)
        recordBuiltinName(StringID(i));
    m_emptyString = intern(std::span<const uint8_t> { });
    appendBuiltinNames();
    return { };
}

BuiltinName StringTable::builtinName(StringID id) const
{
    return isProvisional(id) ? m_builtins->nameOf(provisionalAt(id)) : m_builtinNames[id.index()];
}

bool StringTable::isAllASCII(StringID id) const
{
    return charactersAreAllASCII(byteCast<Latin1Character>((*this)[id]));
}

StringView StringTable::viewOf(StringID id, Vector<char16_t>& bufferForNonASCII) const
{
    if (id == m_emptyString)
        return emptyStringView();
    if (isAllASCII(id))
        return StringView(byteCast<Latin1Character>((*this)[id]));
    bufferForNonASCII = decodeProgramString((*this)[id]);
    return StringView(bufferForNonASCII.span());
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
    RELEASE_ASSERT(m_builtins && !m_openPhase);
    Vector<uint8_t> canonicalBytes;
    if (!isCanonicalProgramString(bytes)) {
        canonicalBytes = encodeProgramString(decodeProgramString(bytes).span());
        bytes = canonicalBytes.span();
    }
    uint64_t hash = hashOf(bytes);
    if (StringID id = find(m_shards[shardIndexOf(hash)], hash, bytes))
        return id;
    m_ownStrings.append(Vector<uint8_t>(bytes));
    return append(m_ownStrings.last().span(), hash);
}

StringID StringTable::intern(StringView view)
{
    return intern(encodeProgramString(view).span());
}

uint64_t StringTable::hashOf(std::span<const uint8_t> bytes)
{
    constexpr uint64_t fnvOffsetBasis = 0xcbf29ce484222325;
    constexpr uint64_t fnvPrime = 0x100000001b3;
    uint64_t hash = fnvOffsetBasis;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * fnvPrime;
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
    AOT_THRESHOLD(size_t, minNumberOfSlots, 8);
    if (shard.slots.size() >= 2 * count)
        return;
    size_t numberOfSlots = std::max(minNumberOfSlots, roundUpToPowerOfTwo(2 * count));
    Vector<StringID> oldSlots = std::exchange(shard.slots, Vector<StringID>(FillWith { }, numberOfSlots, StringID()));
    shard.count = 0;
    for (StringID id : oldSlots) {
        if (id)
            insert(shard, hashOf(m_strings[id.index()]), id);
    }
}

StringID StringTable::append(std::span<const uint8_t> bytes, uint64_t hash)
{
    RELEASE_ASSERT(m_strings.size() < provisionalBit);
    StringID id(m_strings.size());
    m_strings.constructAndAppend(bytes);
    m_nameClasses.append(classifyName(bytes));
    m_builtinNames.append(m_builtins->nameOf(bytes));
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
    unsigned nameCount = m_builtins->nameCount();
    for (unsigned index = 0; index < nameCount; ++index) {
        BuiltinName name(index);
        if (m_builtins->isSymbolName(name) || m_builtins->isRegisteredSymbolName(name) || stringOf(name))
            continue;
        StringID id = intern(m_builtins->textOf(name));
        RELEASE_ASSERT(stringOf(name) == id);
    }
}

std::span<const uint8_t> StringTable::provisionalAt(StringID id) const
{
    RELEASE_ASSERT(m_openPhase);
    return m_openPhase->textOf(id);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
