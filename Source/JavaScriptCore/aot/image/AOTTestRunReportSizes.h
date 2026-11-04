/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTableParts.h"
#include "AOTVector.h"
#include <array>
#include <span>
#include <utility>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

// A name and a number for each cell of a line. The cells of `tables` add up to "table bytes" of `image`, and those of `programTables` to its "program table bytes":
// what no other cell of a line counts (a header, what is skipped in front of a section or a part) is its "other".
struct SizesOfImage {
    using Cells = Vector<std::pair<ASCIILiteral, uint64_t>>;

    Cells image;
    Cells tables;
    Cells programTables;
};

// `image` begins with its ImageHeader. `bytesOfTableParts`: BuiltImage::bytesOfTableParts.
SizesOfImage measureImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext&);
// LoggingOptions::logsPhaseTimes: a line for each of the three. Nothing of an image that was not written.
void logSizesOfImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
