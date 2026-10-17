/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC { namespace AOT { namespace Typed {

enum class StringAppendPart : uint8_t {
    End,
    Value,
    PushStart,
    CheckLength,
    CheckLengthAndPop,
    CheckRegionLength,
    PopStart,
    HTMLEscapedValue,
    HTMLEscapedSlice,
    NumberInRadix,
};

static constexpr unsigned maxLengthEscapedInPlace = 1 << 24;
static constexpr unsigned stringAppendPartBits = 4;
static constexpr unsigned maxStringAppendParts = 14;
static constexpr unsigned stringAppendRegionShift = stringAppendPartBits * maxStringAppendParts;
static constexpr unsigned maxStringAppendRegion = 127;
static constexpr unsigned stringAppendBeginIsNested = 1;
static constexpr unsigned stringAppendBeginIsEager = 2;
static constexpr unsigned stringAppendBeginIsFirst = 4;
static constexpr unsigned stringAppendBeginRegionShift = 8;
static constexpr uint64_t stringAppendConstantTag = 1;
static constexpr uint64_t stringAppendControlTag = 3;
static constexpr uint64_t stringAppendTagMask = 15;
static constexpr unsigned stringAppendPayloadShift = 4;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
