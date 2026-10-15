/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct CompiledCode;

Outlining chooseWindows(const Vector<const CompiledCode*>&, Vector<std::pair<uint32_t, uint32_t>>&& entriesInsideFunctions);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
