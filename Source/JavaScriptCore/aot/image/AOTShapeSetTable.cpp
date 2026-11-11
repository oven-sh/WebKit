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

static_assert(sizeof(uint64_t) == 1u << PagedBitSet::logBytesPerWord && 8 * sizeof(uint64_t) == PagedBitSet::bitsPerPage);
static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::ARM64) <= noBitSet);
static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::X86_64) <= noBitSet);

void encodeShapeSetBits(TableEncoder& encoder)
{
    RELEASE_ASSERT(encoder.part.id() == TablePartID::ShapeSets && !encoder.part.size());

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
