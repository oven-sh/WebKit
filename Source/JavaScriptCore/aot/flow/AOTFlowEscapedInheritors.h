/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolver.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowEscapedInheritorState {
    Vector<BuiltinID> prototypes;
    BitVector isRequested;
};

// The node holds the objects in nativeCode that may inherit from the built-in prototype. refineEscapedInheritors() fills it.
FlowNodeID escapedInheritorsOf(FlowSolver&, BuiltinID prototype);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
