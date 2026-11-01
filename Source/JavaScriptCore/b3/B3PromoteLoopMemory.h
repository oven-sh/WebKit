/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 {

class Procedure;

// Promotes a memory location that a loop both loads and stores at a loop-invariant address, such as `a[i] += x` with an invariant i. The location is loaded once in the
// preheader and its value is kept in an SSA value across iterations.
// The stores are not moved, so memory holds the same contents at every point as before, and any other reader of the location sees no difference.
// Nothing else in the loop may write the location. That is checked for each replaced load using that load's own heap range. The range of an access that never executes
// proves nothing about the location.
// This relies on a location not changing its abstract heap while the loop runs. Code that converts a location from one heap to another in place must declare writes to both.
JS_EXPORT_PRIVATE bool promoteLoopMemory(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
