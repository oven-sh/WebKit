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

// Why the closures that optimization left on the heap are objects: for each NewClosure and each read of a variable that is born as a function, the first operation
// that does more with the closure than call it, counted apart for sites that run at most once and for closures without captures. Before the sums, one line per NewClosure
// and per NewObject with everything that is done with the cell: the census of a run counts closures by function and objects by layout, and the two together say how many cells
// of a run are only handed to calls.
JS_EXPORT_PRIVATE void dumpClosureObservers(PrintStream&, const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
