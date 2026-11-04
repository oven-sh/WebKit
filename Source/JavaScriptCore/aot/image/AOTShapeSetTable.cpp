/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTLimits.h"
#include "AOTPagedBitSet.h"
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

// PagedBitSet::locationOf() counts bytes from the first word of this table, and a page of a set is one word.
static_assert(sizeof(uint64_t) == 1u << PagedBitSet::logBytesPerWord && 8 * sizeof(uint64_t) == PagedBitSet::bitsPerPage);
// A table within the limit has no location that wraps or that reads as noBitSet.
static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::ARM64) <= noBitSet);
static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::X86_64) <= noBitSet);

void encodeShapeSetBits(TableEncoder& encoder)
{
    // The image builder takes the table off the end of the tables, so the table is all that this part holds.
    RELEASE_ASSERT(encoder.part.id() == TablePartID::ShapeSets && !encoder.part.size());

    // The rows of layoutSets are written already, and ShapeTestData::bitSetOffset of each is a location among these words.
    // No link can move such a location, so whoever gave out the locations also made the words: nothing is placed again here.
    std::span<const uint64_t> words;
    if (encoder.firstParts)
        words = encoder.firstParts->shapeSetWords.span();
    else
        encoder.fail("ShapeSetsNotHandedOver"_s);

    uint32_t offset = encoder.part.appendUnshared(asBytes(words), alignof(uint64_t));
    encoder.part.setTableRange(ProgramTableIndex::shapeSetBits, offset, static_cast<uint32_t>(words.size()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
