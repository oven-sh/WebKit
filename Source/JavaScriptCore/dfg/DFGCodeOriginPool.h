/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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

#include <wtf/Platform.h>

#if ENABLE(DFG_JIT)

#include "CallFrame.h"
#include "CodeOrigin.h"
#include "Options.h"
#include <wtf/Atomics.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace DFG {

class CodeOriginPool : public ThreadSafeRefCounted<CodeOriginPool> {
public:
    static Ref<CodeOriginPool> create()
    {
        return adoptRef(*new CodeOriginPool);
    }

    CallSiteIndex addCodeOrigin(CodeOrigin);
    CallSiteIndex addUniqueCallSiteIndex(CodeOrigin);
    CallSiteIndex NODELETE lastCallSite() const;
    DisposableCallSiteIndex addDisposableCallSiteIndex(CodeOrigin);
    void removeDisposableCallSiteIndex(DisposableCallSiteIndex);
    void shrinkToFit();

    // Readers are lock-free and, with useJSThreads, on other threads (stack
    // walks, unwinding, error stacks, the sampling profiler) while an inline
    // cache regeneration appends a disposable call site under the CodeBlock
    // lock. Flag-on, a growing append therefore never frees the array a
    // reader may be indexing: it copies into a larger one, release-publishes
    // it, and retires the old array until the pool dies (appends after link
    // are rare and bounded by the free list). Flag-off: the plain Vector.
    CodeOrigin get(unsigned index)
    {
        if (Options::useJSThreads()) [[unlikely]]
            return WTF::atomicLoad(&m_published, std::memory_order_acquire)[index];
        return m_codeOrigins[index];
    }
    unsigned size() const { return m_codeOrigins.size(); }

private:
    CodeOriginPool();
    void appendEntry(CodeOrigin);
    void republish();

    Vector<CodeOrigin, 0, UnsafeVectorOverflow> m_codeOrigins;
    Vector<unsigned> m_callSiteIndexFreeList;
    // Flag-on only: the array lock-free readers index (== m_codeOrigins' buffer
    // contents, republished after every growth) and the buffers it replaced.
    CodeOrigin* m_published { nullptr };
    unsigned m_publishedCapacity { 0 };
    UniqueArray<CodeOrigin> m_publishedOwner;
    Vector<UniqueArray<CodeOrigin>> m_retired;
};

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
