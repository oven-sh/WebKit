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

#include "config.h"
#include "DFGCodeOriginPool.h"

#if ENABLE(DFG_JIT)

namespace JSC { namespace DFG {

CodeOriginPool::CodeOriginPool()
{
    // Ensure that CallSiteIndex 0 => non-inlined-function BytecodeIndex(0).
    addCodeOrigin(CodeOrigin(BytecodeIndex(0)));
}

CallSiteIndex CodeOriginPool::addCodeOrigin(CodeOrigin codeOrigin)
{
    if (m_codeOrigins.isEmpty()
        || m_codeOrigins.last() != codeOrigin)
        appendEntry(codeOrigin);
    unsigned index = m_codeOrigins.size() - 1;
    ASSERT(m_codeOrigins[index] == codeOrigin);
    return CallSiteIndex(index);
}

CallSiteIndex CodeOriginPool::addUniqueCallSiteIndex(CodeOrigin codeOrigin)
{
    appendEntry(codeOrigin);
    unsigned index = m_codeOrigins.size() - 1;
    ASSERT(m_codeOrigins[index] == codeOrigin);
    return CallSiteIndex(index);
}

CallSiteIndex CodeOriginPool::lastCallSite() const
{
    RELEASE_ASSERT(m_codeOrigins.size());
    return CallSiteIndex(m_codeOrigins.size() - 1);
}

DisposableCallSiteIndex CodeOriginPool::addDisposableCallSiteIndex(CodeOrigin codeOrigin)
{
    if (!m_callSiteIndexFreeList.isEmpty()) {
        unsigned index = m_callSiteIndexFreeList.takeLast();
        m_codeOrigins[index] = codeOrigin;
        if (Options::useJSThreads()) [[unlikely]]
            m_published[index] = codeOrigin; // slot is free: no frame can name it (its routine died at a GC end)
        return DisposableCallSiteIndex(index);
    }

    appendEntry(codeOrigin);
    unsigned index = m_codeOrigins.size() - 1;
    ASSERT(m_codeOrigins[index] == codeOrigin);
    return DisposableCallSiteIndex(index);
}

void CodeOriginPool::removeDisposableCallSiteIndex(DisposableCallSiteIndex callSite)
{
    RELEASE_ASSERT(callSite.bits() < m_codeOrigins.size());
    m_callSiteIndexFreeList.append(callSite.bits());
    m_codeOrigins[callSite.bits()] = CodeOrigin();
    if (Options::useJSThreads()) [[unlikely]]
        m_published[callSite.bits()] = CodeOrigin();
}

void CodeOriginPool::shrinkToFit()
{
    m_codeOrigins.shrinkToFit();
    m_callSiteIndexFreeList.shrinkToFit();
    if (Options::useJSThreads()) [[unlikely]]
        republish(); // shrinkToFit reallocated the Vector; keep the published copy exact (called at link, before sharing)
}

void CodeOriginPool::appendEntry(CodeOrigin codeOrigin)
{
    m_codeOrigins.append(codeOrigin);
    if (!Options::useJSThreads()) [[likely]]
        return;
    // The published array always has room for m_codeOrigins.capacity()
    // entries; a growth of the Vector republishes (copy + retire), otherwise
    // the new entry is written in place (no reader can name its index yet).
    if (!m_published || m_codeOrigins.size() > m_publishedCapacity)
        republish();
    else
        m_published[m_codeOrigins.size() - 1] = codeOrigin;
}

void CodeOriginPool::republish()
{
    unsigned capacity = std::max<unsigned>(m_codeOrigins.capacity(), m_codeOrigins.size());
    auto fresh = makeUniqueArray<CodeOrigin>(capacity ? capacity : 1);
    for (unsigned i = 0; i < m_codeOrigins.size(); ++i)
        fresh[i] = m_codeOrigins[i];
    CodeOrigin* raw = fresh.get();
    if (m_published)
        m_retired.append(WTF::move(m_publishedOwner));
    m_publishedOwner = WTF::move(fresh);
    m_publishedCapacity = capacity ? capacity : 1;
    WTF::atomicStore(&m_published, raw, std::memory_order_release);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
