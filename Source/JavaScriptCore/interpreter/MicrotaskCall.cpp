/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "MicrotaskCall.h"

#include "CodeBlock.h"
#include "Interpreter.h"
#include "JSFunctionInlines.h"
#include "ThrowScope.h"

namespace JSC {

void MicrotaskCall::initialize(VM& vm, JSFunction* function)
{
    WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed); // THREADS: see unlinkOrUpgradeImpl.
    m_functionExecutable = function->jsExecutable();
    relink(vm, function);
}

void MicrotaskCall::relink(VM& vm, JSFunction* function)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Unlink from any prior CodeBlock before we reset state.
    // AB17c F4 (precondition 11), amended TSAN r11 (reports 30/31): call the
    // locked remove UNCONDITIONALLY — the previous unlocked isOnList()
    // pre-check read the node words while a sibling Thread's locked drain
    // (removeOnDestruction / linkIncomingCall) rewrote them; per the
    // CallLinkInfoBase::removeOnDestruction contract, only the under-lock
    // isOnList() re-check is authoritative. Not-on-list is a cheap no-op
    // under the lock (and stays the unlocked check GIL-on).
    removeOnDestruction();

    auto* newCodeBlock = vm.interpreter.prepareForMicrotaskCall(*this, function);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
    // codeBlock/numParameters already published by prepareForMicrotaskCall
    // before the entry address; restore them here too for the non-entry users.
    WTF::atomicStore(&m_codeBlock, newCodeBlock, std::memory_order_relaxed);
    WTF::atomicStore(&m_numParameters, newCodeBlock->numParameters(), std::memory_order_relaxed);
}

void MicrotaskCall::unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    // AB17c F4 (precondition 11): locked remove gilOff (shared sentinel list).
    if (isOnList())
        removeOnDestruction();

    // THREADS: another Thread's mutator can be reading these words lock-free in
    // tryCallWithArguments while the install drain rewrites them. Write the
    // codeBlock/numParameters FIRST, then release-publish the entry address —
    // a reader that acquires the new entry therefore sees a codeBlock at least
    // as new (a stale-null entry just makes the reader relink).
    if (newCodeBlock && WTF::atomicLoad(&m_codeBlock, std::memory_order_relaxed) == oldCodeBlock) {
        newCodeBlock->m_shouldAlwaysBeInlined = false;
        WTF::atomicStore(&m_codeBlock, newCodeBlock, std::memory_order_relaxed);
        WTF::atomicStore(&m_numParameters, newCodeBlock->numParameters(), std::memory_order_relaxed);
        WTF::atomicStore(&m_addressForCall, newCodeBlock->jitCode()->addressForCall(), std::memory_order_release);
        newCodeBlock->linkIncomingCall(nullptr, this);
        return;
    }
    WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed);
}

} // namespace JSC
