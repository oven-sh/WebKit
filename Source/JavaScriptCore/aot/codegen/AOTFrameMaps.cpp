/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameMaps.h"

#if ENABLE(AOT)

#include "AOTRepresentation.h"
#include "AOTSavedFrameMap.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/Hasher.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<Representation>, [](Representation representation) {
    return frameSlotKindOf(representation).has_value() == holdsCells(representation);
}));

// m_offsets is an open-addressed hash table whose size is a power of two. A bucket holds the offset in `words` of a map, or emptyFrameMapBucket.
// A map has at least one word, so two buckets for every word leave at least half of the buckets empty.
static constexpr uint32_t emptyFrameMapBucket = UINT32_MAX;

uint32_t FrameMapRecords::addMap(std::span<const uint32_t> map)
{
    std::optional<SavedFrameMap> parsed = SavedFrameMap::tryCreate(map, 0);
    RELEASE_ASSERT(parsed && parsed->numberOfWords() == map.size());

    // The header of a map gives its length, so no map is the start of another: words that start with a map are that map.
    auto bucketFor = [&](std::span<const uint32_t> sought) -> uint32_t& {
        size_t mask = m_offsets.size() - 1;
        for (size_t index = WTF::computeHash(sought) & mask;; index = (index + 1) & mask) {
            uint32_t& bucket = m_offsets[index];
            if (bucket == emptyFrameMapBucket || spanHasPrefix(words.subspan(bucket), sought))
                return bucket;
        }
    };

    if (size_t bucketsNeeded = 2 * (words.size() + map.size()); m_offsets.size() < bucketsNeeded) {
        Vector<uint32_t> previousBuckets = std::exchange(m_offsets, { });
        m_offsets.fill(emptyFrameMapBucket, std::bit_ceil(bucketsNeeded));
        for (uint32_t offset : previousBuckets) {
            if (offset != emptyFrameMapBucket)
                bucketFor(words.subspan(offset, SavedFrameMap::fromFirstWord(&words[offset]).numberOfWords())) = offset;
        }
    }

    uint32_t& bucket = bucketFor(map);
    if (bucket == emptyFrameMapBucket) {
        bucket = words.size();
        words.append(map);
    }
    return bucket;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
