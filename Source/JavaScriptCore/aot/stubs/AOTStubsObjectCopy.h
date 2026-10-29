/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubFrontEnds.h"

namespace JSC { namespace AOT {

PreludeExit emitCopyDataPropertiesPrelude(StubAssembler&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
