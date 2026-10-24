/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIR.h"
#include "AOTOpcode.h"
#include "AOTPassID.h"

namespace JSC { namespace AOT { namespace Typed {

using PassesByTrigger = EnumeratorBitSet<PassID, numberOfPasses>;

// False for a pass that may have something to do in any body.
bool hasTriggers(PassID);
// The passes that may have something to do in such a body.
PassesByTrigger passesThatMayFireIn(const BodyContents&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
