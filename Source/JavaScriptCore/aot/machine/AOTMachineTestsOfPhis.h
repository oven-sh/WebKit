/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineOrder.h"
#include <optional>

namespace JSC { namespace AOT { namespace Machine {

// `const ok = a ? check(b) : null; if (ok !== null) ..`: the way that brings the constant knows what the test will say.
// The join has one Phi, which one operation reads, no Upsilon, and a Branch for its end. All that stays in it is the Branch and integer operations that the Branch alone reads, made of the Phi and of constants.
bool holdsNothingButTestOfItsPhi(const Block* join, const Operation* phi);
// What `tested`, which is such an operation or the Phi, gives where the Phi holds `brought`. An Int32 is its sign extension. Nothing: some operation below it is none that this reckons.
JS_EXPORT_PRIVATE std::optional<int64_t> valueOfTestWherePhiHolds(const Operation* tested, const Operation* phi, int64_t brought);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
