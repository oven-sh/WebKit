/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTImage.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct TableInImage {
    struct Key {
        uint32_t string;
        unsigned hash;
        uint32_t stringConstant { 0 };
    };

    const uint8_t* index() const { return reinterpret_cast<const uint8_t*>(this + 1); }
    const uint32_t* strings() const { return reinterpret_cast<const uint32_t*>(index() + (static_cast<size_t>(2) << keysShift)); }
    const uint32_t* stringConstants() const { return strings() + capacity; }
    const uint64_t* values() const { return reinterpret_cast<const uint64_t*>(stringConstants() + capacity); }
    StringID stringAt(uint32_t entry) const { return StringID(strings()[entry]); }
    StringView textAt(const Image& image, uint32_t entry) const { return image.text().view(stringAt(entry)); }

    template<typename Matches> ALWAYS_INLINE std::optional<uint32_t> findWithHash(unsigned hash, NOESCAPE const Matches& matches) const
    {
        return CompactTable::probeIndex(CompactTable::slotKindFor(capacity), index(), indexBits & CompactTable::numberOfIndexBits, count, hash, matches);
    }
    ALWAYS_INLINE std::optional<uint32_t> find(const Image& image, StringView key, unsigned hash) const
    {
        return findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            return textAt(image, entry) == key;
        });
    }

    JS_EXPORT_PRIVATE static std::optional<Vector<uint8_t>> tryEncode(std::span<const Key>, std::span<const uint64_t> values);

    uint64_t unused;
    uint32_t count;
    uint32_t alive;
    uint32_t capacity;
    uint8_t indexBits;
    uint8_t keysShift;
    bool hasValues;
    uint8_t alsoUnused;
};
static_assert(sizeof(TableInImage) == sizeof(CompactTable) && OBJECT_OFFSETOF(TableInImage, count) == CompactTable::offsetOfUsed() && OBJECT_OFFSETOF(TableInImage, alive) == CompactTable::offsetOfAliveCount());
static_assert(OBJECT_OFFSETOF(TableInImage, capacity) == CompactTable::offsetOfCapacity() && OBJECT_OFFSETOF(TableInImage, indexBits) == CompactTable::offsetOfIndexBits() && OBJECT_OFFSETOF(TableInImage, keysShift) == CompactTable::offsetOfKeysShift());

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
