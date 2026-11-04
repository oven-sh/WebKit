/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "B3Origin.h"
#include "B3Policy.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

struct FrameAddressViolation {
    B3::Origin address;
    B3::Origin suspension;
};

// A block parameter whose type holds no object has been tested on every way into it, which the phase does not follow.
JS_EXPORT_PRIVATE B3::ClientPhase validateFrameAddresses(Vector<FrameAddressViolation>&, BitVector&& originsOfSuspensions, BitVector&& originsOfParametersWithoutObjects = { });
JS_EXPORT_PRIVATE B3::ClientPhase materializeSlotBasesLate();
JS_EXPORT_PRIVATE B3::ClientPhase dissolveBirthsOfFrameCells(const bool& hasBirths, bool verifies);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
