/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 {

class Procedure;

// Writes several turns of a small innermost counted loop one behind the other. Only one of them tests the counter, against a bound
// that leaves room for the others. The loop itself stays behind for the turns that are left.
JS_EXPORT_PRIVATE bool unrollLoops(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
