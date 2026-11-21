/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTVector.h"
#include <limits>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

inline constexpr uint32_t noStartRank = std::numeric_limits<uint32_t>::max();

struct LayoutRecordToPlace {
    LayoutID id;
    uint32_t shape;
};

void sortKeys(std::span<uint64_t>);
Vector<uint32_t> rankFunctionsByListedUnits(std::span<const uint32_t> positionsOfListedUnits, const LinkedFunctions&, size_t numberOfFunctions);
Vector<uint32_t> listedNumbersFirst(std::span<const uint32_t> listed, size_t count);
Vector<uint32_t> listSitesInStartOrder(const NumberingInputs&, std::span<const uint32_t> startRanks, const NumberedKeys& sites);
void forEachTextOfListedData(const NumberingInputs&, std::span<const uint32_t> startRanks, NOESCAPE const ScopedLambda<void(StringID, FunctionID)>&);
Vector<LayoutRecordToPlace> sortLayoutRecords(Vector<LayoutRecordToPlace>&&, const ImageNumbers&, size_t numberOfShapes, size_t numberOfLayouts);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
