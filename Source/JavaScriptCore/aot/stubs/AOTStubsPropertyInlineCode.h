/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "CCallHelpers.h"
#include "RegisterSet.h"

namespace JSC { namespace AOT { namespace InlinePropertyCode {

// site holds the address of the Slot. The jump is taken unless the site holds a constant.
CCallHelpers::Jump loadConstantSlotAtSite(CCallHelpers&, GPRReg site, GPRReg header, GPRReg result);
// receiver is a cell, and neither it nor value is in registersOfFrontProbe(). The jumps go to getByIdAfterFrontProbe() or putByIdAfterFrontProbe(mode), whose cell operand is loaded.
CCallHelpers::JumpList loadOwnInlineValueByFront(CCallHelpers&, GPRReg receiver, uint32_t column, GPRReg result);
CCallHelpers::JumpList storeOwnInlineValueByFront(CCallHelpers&, GPRReg receiver, uint32_t column, GPRReg value, SlotAccess);
RegisterSet registersOfFrontProbe();

} } } // namespace JSC::AOT::InlinePropertyCode

#endif // ENABLE(AOT)
