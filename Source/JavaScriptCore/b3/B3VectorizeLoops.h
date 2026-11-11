/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 {

class Procedure;
class Value;

// Puts a loop in front of a small innermost counted loop that walks arrays element by element. One turn of it does the work of as many turns
// as there are elements in sixteen bytes. The loop itself stays behind for the turns that are left, and for arrays that lie over each other.
JS_EXPORT_PRIVATE bool vectorizeLoops(Procedure&);

// A patchpoint that does more than hold values alive, or a call, at which a vector is alive. Null: there is none.
JS_EXPORT_PRIVATE Value* findPatchpointWhereVectorIsAlive(Procedure&);

// Stops the compiler where there is one.
JS_EXPORT_PRIVATE void checkNoVectorLivesAcrossPatchpoint(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
