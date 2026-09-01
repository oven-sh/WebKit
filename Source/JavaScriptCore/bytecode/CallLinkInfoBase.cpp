/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "CallLinkInfoBase.h"

#include "CachedCall.h"
#include "CallLinkInfo.h"
#include "JSCJSValueInlines.h"
#include "JSFunctionInlines.h"
#include "MicrotaskCall.h"
#include "PolymorphicCallStubRoutine.h"

namespace JSC {

void CallLinkInfoBase::removeOnDestruction()
{
    // AB17c F4 (precondition 11), amended AB17e: see the declaration comment.
    // The recursive lock admits the sweep-from-inside-a-locked-linker path.
    // gilOff the lock is taken UNCONDITIONALLY (no unlocked isOnList()
    // pre-check): acquiring it blocks until any in-flight drain loop —
    // which holds the lock across its ENTIRE {takeFrom, begin,
    // unlinkOrUpgrade} traversal and keeps using a node after its in-loop
    // remove() — has finished with this object; only the under-lock
    // isOnList() re-check is authoritative. The mode gate is the inline
    // Config-page byte (g_jscConfig.gilOffProcess), not the 5-Options
    // VM::isGILOffProcess() re-derivation: this runs on every gilOff
    // call-link teardown.
    if (g_jscConfig.gilOffProcess) [[unlikely]] {
        Locker locker { CallLinkInfo::s_callLinkSerializationLock };
        if (isOnList())
            remove();
        return;
    }
    if (isOnList())
        remove();
}

void CallLinkInfoBase::unlinkOrUpgrade(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    switch (callSiteType()) {
    case CallSiteType::CallLinkInfo:
        static_cast<CallLinkInfo*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        break;
    case CallSiteType::PolymorphicCallNode:
        static_cast<PolymorphicCallNode*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        break;
#if ENABLE(JIT)
    case CallSiteType::DirectCall:
        static_cast<DirectCallLinkInfo*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        break;
#endif
    case CallSiteType::CachedCall:
        static_cast<CachedCall*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        break;
    case CallSiteType::MicrotaskCall:
        static_cast<MicrotaskCall*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        break;
    }
}

} // namespace JSC
