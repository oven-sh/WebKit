/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <span>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE bool isPlainBaseURL(std::span<const Latin1Character>);

namespace Typed { namespace PlainURL {

inline constexpr unsigned bitsOfOffset = 12;
inline constexpr size_t maxOffset = (1 << bitsOfOffset) - 1;
inline constexpr size_t maxStartOfPath = (1 << (31 - 2 * bitsOfOffset)) - 1;
inline constexpr int32_t notPlain = -1;

inline constexpr int32_t absent = -1;
inline constexpr int32_t needsObject = -2;

constexpr int32_t packRange(size_t start, size_t end) { return static_cast<int32_t>(start << bitsOfOffset | end); }
constexpr int32_t packOffsets(size_t startOfPath, size_t endOfPath, size_t endOfQuery) { return static_cast<int32_t>(startOfPath << (2 * bitsOfOffset) | endOfPath << bitsOfOffset | endOfQuery); }
constexpr size_t endOfPath(int32_t offsets) { return offsets >> bitsOfOffset & maxOffset; }
constexpr size_t endOfQuery(int32_t offsets) { return offsets & maxOffset; }

inline constexpr unsigned shiftOfPairCount = bitsOfOffset + 1;

constexpr int32_t packQuery(size_t numberOfPairs, bool beginsWithQuestionMark, size_t length) { return static_cast<int32_t>(numberOfPairs << shiftOfPairCount | static_cast<size_t>(beginsWithQuestionMark) << bitsOfOffset | length); }
constexpr unsigned numberOfPairs(int32_t query) { return query >> shiftOfPairCount; }

} } // namespace Typed::PlainURL

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
