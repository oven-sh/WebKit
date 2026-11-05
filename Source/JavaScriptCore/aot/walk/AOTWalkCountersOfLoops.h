/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct CounterTestedInFrontOfLoop {
    uint32_t position { 0 };
    TypeID tested;
    bool isHeldWhereItComesRound { false };
};

Vector<CounterTestedInFrontOfLoop, 2> findCountersTestedInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
