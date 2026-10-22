/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

// Why the closures that optimization left on the heap are objects.
JS_EXPORT_PRIVATE void dumpClosureObservers(PrintStream&, const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
