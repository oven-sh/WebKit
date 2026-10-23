/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubFrontEnds.h"
#include "AOTStubKey.h"

namespace JSC { namespace AOT {

enum class ConstructSyntax : uint8_t { NewExpression, SuperCall };

PreludeExit emitGetConstructPrototypePrelude(StubAssembler&);
PreludeExit emitGetSuperBasePrelude(StubAssembler&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
