/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableInImage.h"

#if ENABLE(AOT)

#include <algorithm>
#include <limits>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::optional<Vector<uint8_t>> TableInImage::tryEncode(std::span<const Key> keys, std::span<const uint64_t> values)
{
    if (keys.empty() || keys.size() > CompactTable::maxCapacity || (!values.empty() && values.size() != keys.size()))
        return std::nullopt;
    TableInImage header { };
    header.count = keys.size();
    header.alive = header.count;
    header.capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(header.count, 2));
    header.indexBits = CompactTable::indexBitsFor(header.capacity);
    header.keysShift = CompactTable::keysShiftFor(header.indexBits, header.capacity);
    header.hasValues = !values.empty();
    CompactTable::SlotKind slotKind = CompactTable::slotKindFor(header.capacity);
    size_t sizeOfIndex = static_cast<size_t>(2) << header.keysShift;
    size_t offsetOfStrings = sizeof(TableInImage) + sizeOfIndex;
    size_t offsetOfValues = offsetOfStrings + sizeof(uint32_t) * header.capacity;
    size_t size = offsetOfValues + (header.hasValues ? sizeof(uint64_t) * header.capacity : 0);
    if (size > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    Vector<uint8_t> bytes(FillWith { }, size, static_cast<uint8_t>(0));
    memcpy(bytes.mutableSpan().data(), &header, sizeof(header));
    uint8_t* index = bytes.mutableSpan().data() + sizeof(TableInImage);
    if (slotKind != CompactTable::SlotKind::Tag)
        memset(index, 0xff, sizeOfIndex);
    uint32_t* strings = reinterpret_cast<uint32_t*>(bytes.mutableSpan().data() + offsetOfStrings);
    if (header.hasValues)
        memcpy(bytes.mutableSpan().data() + offsetOfValues, values.data(), values.size_bytes());
    for (uint32_t entry = 0; entry < header.count; ++entry) {
        RELEASE_ASSERT(keys[entry].hash <= CompactTable::hashMask);
        strings[entry] = keys[entry].string;
        CompactTable::insertIntoIndex(slotKind, index, header.indexBits, entry, keys[entry].hash);
    }
    return bytes;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
