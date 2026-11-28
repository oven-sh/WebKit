/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineOrder.h"
#include <optional>

namespace JSC { namespace AOT { namespace Machine {

bool holdsNothingButTestOfItsPhi(const Block* join, const Operation* phi);
bool testsOnlyPhi(const Block*, const Operation* phi);
JS_EXPORT_PRIVATE std::optional<int64_t> valueOfTestWherePhiHolds(const Operation* tested, const Operation* phi, int64_t brought);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
