/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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

#include "CollectionScope.h"
#include <wtf/PrintStream.h>
#include <wtf/RefPtr.h>
#include <wtf/SharedTask.h>

namespace JSC {

struct GCRequest {
    // Identifies a request to the thread that asked for it, so it can wait for that request in
    // particular rather than for whatever collection happens to be running.
    using Ticket = uint64_t;

    GCRequest() { }
    
    GCRequest(CollectionScope scope)
        : scope(scope)
    {
    }
    
    GCRequest(std::optional<CollectionScope> scope)
        : scope(scope)
    {
    }
    
    bool NODELETE subsumedBy(const GCRequest& other) const;
    
    void dump(PrintStream&) const;
    
    std::optional<CollectionScope> scope;
    // Upstream removed this as unused (322293@main). Bun sets it (JSC__VM__collectAsyncIdle).
    RefPtr<SharedTask<void()>> didFinishEndPhase;
#if USE(BUN_JSC_ADDITIONS)
    // Set by an embedder on a collection it requests because the application has gone idle (as opposed to one paced by
    // allocation, forced by the program, or by memory pressure). Only such a collection may let optimized code that has
    // no execution counter of its own age out (CodeBlock::shouldJettisonDueToOldAge).
    bool isIdle { false };
#endif
};

} // namespace JSC

