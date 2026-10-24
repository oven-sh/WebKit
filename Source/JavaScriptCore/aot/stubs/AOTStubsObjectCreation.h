/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubKey.h"
#include "GPRInfo.h"
#include "JSObject.h"

namespace JSC { namespace AOT {

class StubAssembler;

// base[offset + 8 * i] = argument register firstValueArgument + i for i < count, 0 for count <= i < end. Clobbers nothing.
void emitStoreValuesAndClear(StubAssembler&, unsigned firstValueArgument, unsigned count, unsigned end, GPRReg base, size_t offset);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
