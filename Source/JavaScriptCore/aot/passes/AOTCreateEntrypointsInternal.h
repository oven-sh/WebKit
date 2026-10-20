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
void emitReturnOfEntrypointCall(IRBuilder&, const FlowResults&, FunctionID callee, ValueID result, EntrypointResultTest = EntrypointResultTest::MadeByEntrypoint);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
