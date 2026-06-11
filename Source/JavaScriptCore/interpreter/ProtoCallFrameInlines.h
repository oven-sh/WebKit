/*
 * Copyright (C) 2019-2023 Apple Inc. All rights reserved.
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

#include "ProtoCallFrame.h"
#include "RegisterInlines.h"
#include <wtf/Atomics.h>

namespace JSC {

inline void ProtoCallFrame::init(CodeBlock* codeBlock, JSGlobalObject* globalObject, JSObject* callee, JSValue thisValue, JSCell* context, int argCountIncludingThis, EncodedJSValue* otherArgs)
{
    this->args = otherArgs;
    this->context = context;
    this->setCodeBlock(codeBlock);
    this->setCallee(callee);
    this->setGlobalObject(globalObject);
    this->setArgumentCountIncludingThis(argCountIncludingThis);
    size_t paddedArgsCount = argCountIncludingThis;
    if (codeBlock && static_cast<unsigned>(argCountIncludingThis) < codeBlock->numParameters())
        paddedArgsCount = codeBlock->numParameters();
    paddedArgsCount = roundArgumentCountToAlignFrame(paddedArgsCount);
    this->setPaddedArgCount(paddedArgsCount);
    this->clearCurrentVPC();
    this->setThisValue(thisValue);
}

inline JSObject* ProtoCallFrame::callee() const
{
    return calleeValue.Register::object();
}

inline void ProtoCallFrame::setCallee(JSObject* callee)
{
    calleeValue = callee;
}

// THREADS (TSAN r11 report 32): a CachedCall/MicrotaskCall registered on a
// CodeBlock's incoming-call list has its proto frame's codeBlock slot READ
// AND REWRITTEN by the locked jettison drain on a sibling Thread
// (CachedCall::unlinkOrUpgradeImpl) while the owning thread initializes or
// reads it. Relaxed atomic accesses on the one Register word keep the
// pairing defined (identical codegen); ordering comes from the
// release-published entry address (CachedCall.h) and the drain lock.
inline CodeBlock* ProtoCallFrame::codeBlock() const
{
    static_assert(sizeof(codeBlockValue) == sizeof(CodeBlock*));
    return WTF::atomicLoad(const_cast<CodeBlock**>(std::bit_cast<CodeBlock* const*>(&codeBlockValue)), std::memory_order_relaxed);
}

inline void ProtoCallFrame::setCodeBlock(CodeBlock* codeBlock)
{
    WTF::atomicStore(std::bit_cast<CodeBlock**>(&codeBlockValue), codeBlock, std::memory_order_relaxed);
}

} // namespace JSC
