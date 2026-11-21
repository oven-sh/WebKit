/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(DFG_JIT)

namespace JSC { namespace DFG {

class Graph;

// Turns a Phi of Decimals into Phis of their words and a NewDecimal, for allocation sinking to remove. A sum that is carried around a loop
// is then in registers.

bool performDecimalPhiUnboxing(Graph&);

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
