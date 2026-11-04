/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCreateEntrypoints.h"
#include "AOTIRBuilder.h"

namespace JSC { namespace AOT { namespace Typed {

Function createDerivedHeader(const Function& target, FunctionID targetID, FunctionRole, uint32_t keyParameter);
// The call is the last operation of the open block.
void emitReturnOfEntrypointCall(IRBuilder&, ValueID call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
