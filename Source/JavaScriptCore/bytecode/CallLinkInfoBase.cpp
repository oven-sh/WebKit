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

void CallLinkInfoBase::unlinkOrUpgrade(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    switch (callSiteType()) {
    case CallSiteType::CallLinkInfo:
        static_cast<CallLinkInfo*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        return;
    case CallSiteType::PolymorphicCallNode:
        static_cast<PolymorphicCallNode*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        return;
#if ENABLE(JIT)
    case CallSiteType::DirectCall:
        static_cast<DirectCallLinkInfo*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        return;
#endif
    case CallSiteType::CachedCall:
        static_cast<CachedCall*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        return;
    case CallSiteType::MicrotaskCall:
        static_cast<MicrotaskCall*>(this)->unlinkOrUpgradeImpl(vm, oldCodeBlock, newCodeBlock);
        return;
    }
#if USE(BUN_JSC_ADDITIONS)
    // The type byte is not one of ours: the storage behind this node was reused while the node was still
    // chained. Returning without removing the node would make CodeBlock::unlinkOrUpgradeIncomingCalls spin
    // on it forever, so report it here, together with the byte that now sits where the type was.
    RELEASE_ASSERT_NOT_REACHED(this, static_cast<unsigned>(callSiteType()), oldCodeBlock, newCodeBlock);
#endif
}

} // namespace JSC
