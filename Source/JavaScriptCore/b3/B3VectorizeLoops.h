/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 {

class Procedure;

// Puts a loop in front of a small innermost counted loop that walks arrays element by element. One turn of it does the work of as many turns
// as there are elements in sixteen bytes. The loop itself stays behind for the turns that are left, and for arrays that lie over each other.
JS_EXPORT_PRIVATE bool vectorizeLoops(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
