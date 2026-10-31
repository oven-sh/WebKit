/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// The program table identifiersByHash.
class IdentifierTable {
public:
    struct Header {
        uint32_t numberOfSlots;
        uint32_t numberOfBuckets;
        uint32_t numberBits;
        uint32_t numberOfOthers;
    };
    struct Key {
        uint32_t hash;
        uint32_t number;
    };
    static constexpr size_t wordsInHeader = sizeof(Header) / sizeof(uint32_t);
    static constexpr unsigned firstHashBitInEntry = 9;
    static constexpr uint32_t maxDisplacement = UINT16_MAX;

    explicit IdentifierTable(std::span<const uint32_t> words)
        : m_words(words)
    {
    }

    bool isEmpty() const { return m_words.empty(); }
    const Header& header() const { return *reinterpret_cast<const Header*>(m_words.data()); }
    std::span<const uint32_t> slots() const { return m_words.subspan(wordsInHeader, header().numberOfSlots); }
    std::span<const uint16_t> displacements() const { return { reinterpret_cast<const uint16_t*>(slots().data() + header().numberOfSlots), header().numberOfBuckets }; }
    std::span<const Key> others() const { return { reinterpret_cast<const Key*>(m_words.data() + m_words.size() - 2 * static_cast<size_t>(header().numberOfOthers)), header().numberOfOthers }; }
    uint32_t numberIn(uint32_t entry) const { return (entry & ((1u << header().numberBits) - 1)) - 1; }

    static uint32_t slotFor(uint32_t hash, uint32_t displacement, uint32_t numberOfSlots)
    {
        uint32_t mixed = (hash ^ displacement * 0x9e3779b1u) * 0x85ebca6bu;
        return static_cast<uint32_t>(static_cast<uint64_t>(mixed) * numberOfSlots >> 32);
    }
    static uint32_t entryFor(uint32_t hash, uint32_t number, uint32_t numberBits) { return (number + 1) | static_cast<uint32_t>(static_cast<uint64_t>(hash >> firstHashBitInEntry) << numberBits); }

    // The one identifier in the slots that can have this hash. Without one, no identifier has it.
    ALWAYS_INLINE std::optional<uint32_t> inSlotFor(uint32_t hash) const
    {
        if (isEmpty())
            return std::nullopt;
        // slotFor() is below the number of slots and the bucket below the number of buckets, whatever the hash.
        const Header& header = this->header();
        const uint32_t* slots = m_words.data() + wordsInHeader;
        const uint16_t* displacements = reinterpret_cast<const uint16_t*>(slots + header.numberOfSlots);
        uint32_t entry = slots[slotFor(hash, displacements[hash & (header.numberOfBuckets - 1)], header.numberOfSlots)];
        uint32_t number = numberIn(entry);
        if (!entry || entry != entryFor(hash, number, header.numberBits))
            return std::nullopt;
        return number;
    }

    // isText(number) says whether the identifier has the text that is looked for. So does isTextOfOther(number), of the few identifiers that share their hash with another.
    ALWAYS_INLINE std::optional<uint32_t> find(uint32_t hash, const Invocable<bool(uint32_t)> auto& isText) const { return find(hash, isText, isText); }
    ALWAYS_INLINE std::optional<uint32_t> find(uint32_t hash, const Invocable<bool(uint32_t)> auto& isText, const Invocable<bool(uint32_t)> auto& isTextOfOther) const
    {
        std::optional<uint32_t> number = inSlotFor(hash);
        if (!number)
            return std::nullopt;
        if (isText(*number)) [[likely]]
            return number;
        if (!header().numberOfOthers) [[likely]]
            return std::nullopt;
        return findAmongOthers(hash, isTextOfOther);
    }

    static Vector<uint32_t> create(std::span<const Key>);

private:
    NEVER_INLINE std::optional<uint32_t> findAmongOthers(uint32_t hash, const Invocable<bool(uint32_t)> auto& isText) const
    {
        std::span<const Key> others = this->others();
        for (auto other = std::ranges::lower_bound(others, hash, { }, &Key::hash); other != others.end() && other->hash == hash; ++other) {
            if (isText(other->number))
                return other->number;
        }
        return std::nullopt;
    }

    std::span<const uint32_t> m_words;
};

inline Vector<uint32_t> IdentifierTable::create(std::span<const Key> allKeys)
{
    if (allKeys.empty())
        return { };
    Vector<Key> keys(allKeys);
    AOT::sort(keys, [](const Key& a, const Key& b) {
        return std::pair(a.hash, a.number) < std::pair(b.hash, b.number);
    });
    uint32_t maxNumber = 0;
    Vector<Key> others;
    Vector<Key> placed;
    for (const Key& key : keys) {
        maxNumber = std::max(maxNumber, key.number);
        (!placed.isEmpty() && placed.last().hash == key.hash ? others : placed).append(key);
    }
    uint32_t numberBits = std::bit_width(maxNumber + 1);
    uint32_t numberOfBuckets = WTF::roundUpToPowerOfTwo(std::max<uint32_t>(placed.size() / 4, 1));

    Vector<uint32_t> firstKeyOfBucket(FillWith { }, numberOfBuckets + 1, 0u);
    for (const Key& key : placed)
        ++firstKeyOfBucket[(key.hash & (numberOfBuckets - 1)) + 1];
    for (uint32_t bucket = 0; bucket < numberOfBuckets; ++bucket)
        firstKeyOfBucket[bucket + 1] += firstKeyOfBucket[bucket];
    Vector<Key> keysByBucket(placed.size());
    Vector<uint32_t> next(firstKeyOfBucket.span().first(numberOfBuckets));
    for (const Key& key : placed)
        keysByBucket[next[key.hash & (numberOfBuckets - 1)]++] = key;
    auto keysOf = [&](uint32_t bucket) {
        return keysByBucket.span().subspan(firstKeyOfBucket[bucket], firstKeyOfBucket[bucket + 1] - firstKeyOfBucket[bucket]);
    };
    // The largest buckets choose while the most slots are free.
    Vector<uint32_t> buckets(numberOfBuckets, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(buckets, [&](uint32_t a, uint32_t b) {
        return keysOf(a).size() > keysOf(b).size();
    });

    for (uint32_t numberOfSlots = std::max<uint32_t>(placed.size() + placed.size() / 6, 4);; numberOfSlots += numberOfSlots / 8 + 1) {
        Vector<uint32_t> slots(FillWith { }, numberOfSlots, 0u);
        Vector<uint16_t> displacements(FillWith { }, numberOfBuckets + numberOfBuckets % 2, static_cast<uint16_t>(0));
        Vector<uint32_t, 16> chosen;
        bool allHaveSlots = true;
        for (uint32_t bucket : buckets) {
            std::span<const Key> members = keysOf(bucket);
            if (members.empty())
                break;
            uint32_t displacement = 0;
            for (;; ++displacement) {
                if (displacement > maxDisplacement)
                    break;
                chosen.shrink(0);
                for (const Key& key : members) {
                    uint32_t slot = slotFor(key.hash, displacement, numberOfSlots);
                    if (slots[slot] || chosen.contains(slot))
                        break;
                    chosen.append(slot);
                }
                if (chosen.size() == members.size())
                    break;
            }
            if (displacement > maxDisplacement) {
                allHaveSlots = false;
                break;
            }
            displacements[bucket] = static_cast<uint16_t>(displacement);
            for (size_t index = 0; index < members.size(); ++index)
                slots[chosen[index]] = entryFor(members[index].hash, members[index].number, numberBits);
        }
        if (!allHaveSlots)
            continue;
        Vector<uint32_t> words;
        words.appendList({ numberOfSlots, numberOfBuckets, numberBits, static_cast<uint32_t>(others.size()) });
        words.appendVector(slots);
        words.append(std::span(reinterpret_cast<const uint32_t*>(displacements.span().data()), displacements.size() / 2));
        for (const Key& other : others)
            words.appendList({ other.hash, other.number });
        return words;
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
