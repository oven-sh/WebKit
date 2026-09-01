/*
 * Copyright (C) 2017-2021 Apple Inc. All rights reserved.
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

#include "JITWorklist.h"

namespace JSC {

#if ENABLE(JIT)

template<typename Visitor>
void JITWorklist::iterateCodeBlocksForGC(Visitor& visitor, VM& vm, NOESCAPE const Function<void(CodeBlock*)>& func)
{
    if (!vm.numberOfActiveJITPlans())
        return;

    Locker locker { *m_lock };
    for (auto& entry : m_plans) {
        if (entry.value->vm() != &vm)
            continue;
        entry.value->iterateCodeBlocksForGC(visitor, func);
    }
    // UNGIL AB18-R1-B: plans claimed for finalize left m_plans, but their
    // CodeBlocks are still dereferenced by the finalizing mutator after its
    // in-finalize park points (GIL-off: the GILOffCompilationLocker contended
    // spin and the reallyAdd Class-A fire window both release heap access),
    // during which a sibling-conducted shared GC can run. Walk them
    // UNCONDITIONALLY (no isKnownToBeLiveDuringGC gate): the claim itself is
    // the root. GIL-on / flag-off the table is invariantly empty.
    for (auto& entry : m_finalizingPlans) {
        if (entry.value->vm() != &vm)
            continue;
        entry.value->iterateCodeBlocksForFinalizeRoots(func);
    }
}

#endif // ENABLE(JIT)

} // namespace JSC
