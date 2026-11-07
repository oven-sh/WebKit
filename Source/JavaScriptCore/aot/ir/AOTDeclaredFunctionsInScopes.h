/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class BodyValue;
class Program;

FunctionID functionDeclaredInScopeSlot(const Program&, LayoutID environment, uint32_t field);
LayoutID scopeThatCaptureTakes(const Program&, FunctionID, uint32_t capture);
bool storeKeepsDeclaredFunctionOverItsOwnScope(const Program&, const BodyIndex&, const BodyValue& store);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
