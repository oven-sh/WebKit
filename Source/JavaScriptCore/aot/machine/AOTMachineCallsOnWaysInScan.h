/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineLiveness.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

inline void BackwardScan::noteWaysOnFrom(const Block* block, Position end)
{
    ArenaVector<Position>& ends = m_liveness.m_endsOfWhatWaysLeadTo;
    for (const Edge& edge : block->successors())
        end = std::max(end, ends[edge.block()->index()]);
    ends[block->index()] = end;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
