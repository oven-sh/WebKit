/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineLiveness.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

// What the scan does at every block. AOTMachineScan.cpp alone includes it, so it is part of the function that calls it. What the scan does at every call stands there, in noteScannedCall().

// `end`: Order::endOf() the block. The scan has been at every block further on in the order. An edge that leads back, or to the block itself, finds the word as it was filled.
inline void BackwardScan::noteWaysOnFrom(const Block* block, Position end)
{
    ArenaVector<Position>& ends = m_liveness.m_endsOfWhatWaysLeadTo;
    for (const Edge& edge : block->successors())
        end = std::max(end, ends[edge.block()->index()]);
    ends[block->index()] = end;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
