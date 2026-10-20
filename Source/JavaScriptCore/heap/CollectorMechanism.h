/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Options.h"

namespace JSC {

enum class CollectorMechanism : unsigned {
    LargerCells = 1 << 0,
    LazyMarkerHelpers = 1 << 2,
    EmptyEdenSkips = 1 << 3,
    IdlePollShortcut = 1 << 4,
    NarrowReferenceScan = 1 << 5,
    DeferredRootRegions = 1 << 6,
};

ALWAYS_INLINE bool isEnabled(CollectorMechanism mechanism)
{
    return !(Options::disabledCollectorMechanisms() & static_cast<unsigned>(mechanism));
}

} // namespace JSC
