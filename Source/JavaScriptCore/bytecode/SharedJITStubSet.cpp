/*
 * Copyright (C) 2008-2024 Apple Inc. All rights reserved.
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
#include "SharedJITStubSet.h"

#include "BaselineJITRegisters.h"
#include "CacheableIdentifierInlines.h"
#include "DFGJITCode.h"
#include "InlineCacheCompiler.h"
#include "Repatch.h"

namespace JSC {

#if ENABLE(JIT)

// R2-2: all accessors take m_lock; see the rationale at the declaration block
// in SharedJITStubSet.h.

RefPtr<PolymorphicAccessJITStubRoutine> SharedJITStubSet::getStatelessStub(StatelessCacheKey key) const
{
    Locker locker { m_lock };
    return m_statelessStubs.get(key);
}

void SharedJITStubSet::setStatelessStub(StatelessCacheKey key, Ref<PolymorphicAccessJITStubRoutine> stub)
{
    Locker locker { m_lock };
    m_statelessStubs.add(key, WTF::move(stub));
}

MacroAssemblerCodeRef<JITStubRoutinePtrTag> SharedJITStubSet::getDOMJITCode(DOMJITCacheKey key) const
{
    Locker locker { m_lock };
    return m_domJITCodes.get(key);
}

void SharedJITStubSet::setDOMJITCode(DOMJITCacheKey key, MacroAssemblerCodeRef<JITStubRoutinePtrTag> code)
{
    Locker locker { m_lock };
    m_domJITCodes.add(key, WTF::move(code));
}

RefPtr<InlineCacheHandler> SharedJITStubSet::getSlowPathHandler(AccessType type) const
{
    Locker locker { m_lock };
    return m_slowPathHandlers[static_cast<unsigned>(type)];
}

void SharedJITStubSet::setSlowPathHandler(AccessType type, Ref<InlineCacheHandler> handler)
{
    // Drop any displaced handler OUTSIDE the lock: releasing the last ref to
    // a handler can deref its stub routine, whose observeZeroRefCountImpl
    // re-enters this set via remove(), and m_lock is not recursive.
    RefPtr<InlineCacheHandler> displaced;
    {
        Locker locker { m_lock };
        displaced = WTF::move(m_slowPathHandlers[static_cast<unsigned>(type)]);
        m_slowPathHandlers[static_cast<unsigned>(type)] = WTF::move(handler);
    }
}

#endif // ENABLE(JIT)

} // namespace JSC
