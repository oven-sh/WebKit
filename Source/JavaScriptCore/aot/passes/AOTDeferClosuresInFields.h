/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FunctionAnalyses;
class Heat;
class ProgramEditor;

void deferClosuresInFields(ProgramEditor&, const Heat&, BuildContext&);

// Whether the closure that an allocation's list or a store in `block` gives a field of `object` is one that the pass can leave unmade: a pass that would take the closure away from the allocation asks.
bool canLeaveClosureUnmade(FunctionAnalyses&, BlockID, ValueID object, ValueID given);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
