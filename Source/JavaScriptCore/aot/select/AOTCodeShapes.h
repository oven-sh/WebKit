/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"

namespace JSC { namespace AOT { namespace Typed {

struct CodeShapeModel {
    double compactBelow { 1.0 / 16 };
    double fastFrom { 10 };
};

inline constexpr CodeShapeModel codeShapeModel { };

// Compact code calls. Balanced code has in line what is short and pays at once. Fast code has the long forms in line too: an append, equality, the test of a barrier.
constexpr CodeShape codeShapeFor(Expected<double> heat, bool runsAtMostOnce, Expected<BlockFrequency> recordedFrequency, Expected<double> fastFrom = codeShapeModel.fastFrom)
{
    if (runsAtMostOnce || recordedFrequency.predicted() == BlockFrequency::Rare || heat.predicted() < codeShapeModel.compactBelow)
        return CodeShape::Compact;
    if (heat.predicted() >= fastFrom.predicted())
        return CodeShape::Fast;
    return CodeShape::Balanced;
}

static_assert(codeShapeFor(1, false, BlockFrequency::Normal) == CodeShape::Balanced);
static_assert(codeShapeFor(1, true, BlockFrequency::Normal) == CodeShape::Compact);
static_assert(codeShapeFor(1e6, false, BlockFrequency::Rare) == CodeShape::Compact);
static_assert(codeShapeFor(10, false, BlockFrequency::Unlikely) == CodeShape::Fast);

enum class ChosenShapes : uint8_t { KeepChosen, ChooseAgain };

class CFG;

// Sums of shares of the frequency with which a function is entered come out above it by no more than this.
inline constexpr double roundingOfFrequencySums = 1 + 1e-6;

// Where no block is entered again, what enters the function is divided among the edges, so no block runs more often than the function is entered.
bool runsNoBlockMoreOftenThanEntry(const CFG&);

PassOutcome chooseCodeShapes(FunctionPassContext&, ChosenShapes);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
