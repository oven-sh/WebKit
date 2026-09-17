/*
 * Copyright (C) 2026 the WebKit project authors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(DFG_JIT)

namespace JSC { namespace DFG {

class Graph;

// SPEC-jit I21 / history §50. GIL off, a poll (CheckTraps) must keep another
// thread's plain writes visible to whatever decides control flow in the loop
// it sits in, and nothing else. This analysis, run on FTL plans in SSA form
// right before each global CSE and before LICM, attaches to every in-loop
// poll the value heaps read by the backward slices of the loop's Branch and
// Switch conditions; DFGClobberize.h's CheckTraps case writes exactly those.
// A poll with no attachment (DFG plans, polls outside loops, loops the
// analysis gives up on) writes the interim set of AUDIT-checktraps §7.1.
// Does nothing flag off and GIL on.

bool performPollVisibilityAnalysis(Graph&);

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
