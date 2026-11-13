/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <string.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct StringSwitchTable {
    static constexpr unsigned maxCases = 255;
    static constexpr unsigned maxLength = 255;
    static constexpr unsigned offsetOfLastBucket = 1;
    static constexpr unsigned offsetOfBuckets = 2;
    static constexpr unsigned offsetOfIndex = 1;
    static constexpr unsigned offsetOfCharacters = 2;
    static constexpr unsigned maxSize = UINT16_MAX;
    static constexpr unsigned shiftOfFirstCharacter = 2;

    static unsigned hash(std::span<const uint8_t> characters)
    {
        return (characters.front() << shiftOfFirstCharacter ^ characters.back()) + static_cast<unsigned>(characters.size());
    }

    static unsigned indexOf(const uint8_t* table, std::span<const uint8_t> characters)
    {
        const unsigned numberOfCases = table[0];
        const unsigned lastBucket = table[offsetOfLastBucket];
        const uint8_t* buckets = table + offsetOfBuckets;
        if (characters.empty())
            return buckets[2 * (lastBucket + 1)];
        const uint8_t* bucket = buckets + 2 * (hash(characters) & lastBucket);
        unsigned distance = bucket[0] | bucket[1] << 8;
        if (!distance)
            return numberOfCases;
        for (const uint8_t* string = table + distance; *string; string += offsetOfCharacters + *string) {
            if (*string == characters.size() && !memcmp(string + offsetOfCharacters, characters.data(), characters.size()))
                return string[offsetOfIndex];
        }
        return numberOfCases;
    }

    using Strings = Vector<std::span<const uint8_t>, 16>;

    static std::optional<Vector<uint8_t, 256>> build(const Strings& strings)
    {
        RELEASE_ASSERT(strings.size() && strings.size() <= maxCases);
        constexpr size_t maxStringsInOneBucket = 4;
        const size_t numberOfBuckets = strings.size() <= maxStringsInOneBucket ? 1 : std::bit_ceil(strings.size()) / 2;
        Vector<uint8_t, 256> table;
        table.append(static_cast<uint8_t>(strings.size()));
        table.append(static_cast<uint8_t>(numberOfBuckets - 1));
        table.grow(offsetOfBuckets + 2 * numberOfBuckets);
        std::ranges::fill(table.mutableSpan().subspan(offsetOfBuckets), 0);
        auto isEmpty = [](std::span<const uint8_t> string) {
            return string.empty();
        };
        table.append(static_cast<uint8_t>(std::ranges::find_if(strings, isEmpty) - strings.begin()));
        for (size_t bucket = 0; bucket < numberOfBuckets; ++bucket) {
            const size_t start = table.size();
            for (size_t index = 0; index < strings.size(); ++index) {
                std::span<const uint8_t> string = strings[index];
                RELEASE_ASSERT(string.size() <= maxLength);
                auto isSame = [&](std::span<const uint8_t> earlier) {
                    return std::ranges::equal(earlier, string);
                };
                if (string.empty() || (hash(string) & (numberOfBuckets - 1)) != bucket || std::ranges::any_of(strings.span().first(index), isSame))
                    continue;
                table.append(static_cast<uint8_t>(string.size()));
                table.append(static_cast<uint8_t>(index));
                table.append(string);
            }
            if (table.size() == start)
                continue;
            table.append(uint8_t { 0 });
            if (start > maxSize)
                return std::nullopt;
            table[offsetOfBuckets + 2 * bucket] = static_cast<uint8_t>(start);
            table[offsetOfBuckets + 2 * bucket + 1] = static_cast<uint8_t>(start >> 8);
        }
        return table;
    }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
