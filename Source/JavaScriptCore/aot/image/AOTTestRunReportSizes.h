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

struct SizesOfImage {
    using Cells = Vector<std::pair<ASCIILiteral, uint64_t>>;

    Cells image;
    Cells tables;
    Cells programTables;
};

SizesOfImage measureImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext&);
void logSizesOfImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
