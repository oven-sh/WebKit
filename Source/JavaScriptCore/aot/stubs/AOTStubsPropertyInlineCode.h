/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT { namespace InlinePropertyCode {

// site holds the address of the Slot. The jump is taken unless the site holds a constant.
inline CCallHelpers::Jump loadConstantSlotAtSite(CCallHelpers& jit, GPRReg site, GPRReg header, GPRReg result)
{
    jit.load8(CCallHelpers::Address(site, Slot::offsetOfKind()), header);
    CCallHelpers::Jump isNotConstant = jit.branch32(CCallHelpers::NotEqual, header, CCallHelpers::TrustedImm32(static_cast<int32_t>(Slot::Kind::Constant)));
    jit.load64(CCallHelpers::Address(site, Slot::offsetOfPayload()), result);
    return isNotConstant;
}

} } } // namespace JSC::AOT::InlinePropertyCode

#endif // ENABLE(AOT)
