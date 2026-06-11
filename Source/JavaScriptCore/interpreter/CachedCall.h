/*
 * Copyright (C) 2009-2023 Apple Inc. All rights reserved.
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

#include "ArgList.h"
#include "CallLinkInfoBase.h"
#include "ExceptionHelpers.h"
#include "Interpreter.h"
#include "JSFunction.h"
#include "JSFunctionInlines.h"
#include "ProtoCallFrameInlines.h"
#include "VMEntryScope.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Scope.h>

namespace JSC {

class CachedCall : public CallLinkInfoBase {
    WTF_MAKE_NONCOPYABLE(CachedCall);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    JS_EXPORT_PRIVATE CachedCall(JSGlobalObject*, JSFunction*, int argumentCount);

    ~CachedCall()
    {
        // AB17e F4 (object-lifetime closure): delist FIRST, before the member
        // teardown below. A locked drain
        // (CodeBlock::unlinkOrUpgradeIncomingCalls) can be
        // mid-unlinkOrUpgradeImpl on this node (reading m_addressForCall /
        // m_protoCallFrame) when this stack object dies; removeOnDestruction
        // acquires the link lock unconditionally gilOff, so we either delist
        // before any drain observes the node or block until the drain loop
        // ends. ~CallLinkInfoBase's own gilOff delist would run only AFTER
        // this store — too late.
        if (g_jscConfig.gilOffProcess) [[unlikely]]
            removeOnDestruction();
        WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed); // THREADS: see unlinkOrUpgradeImpl.
    }

    ALWAYS_INLINE JSValue call()
    {
        ASSERT(m_valid);
        ASSERT(m_arguments.size() == static_cast<size_t>(m_protoCallFrame.argumentCount()));
        return m_vm.interpreter.executeCachedCall(*this);
    }

    JSFunction* function()
    {
        ASSERT(m_valid);
        return uncheckedDowncast<JSFunction>(m_protoCallFrame.calleeValue.unboxedCell());
    }
    FunctionExecutable* functionExecutable() { return m_functionExecutable; }
    JSScope* scope() { return m_scope; }

    void setThis(JSValue v) { m_protoCallFrame.setThisValue(v); }

    void clearArguments() { m_arguments.clear(); }
    void appendArgument(JSValue v) { m_arguments.append(v); }
    bool hasOverflowedArguments() { return m_arguments.hasOverflowed(); }

    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
    {
        // AB17c F4 (precondition 11): this node shares the incoming-calls
        // sentinel lists with the locked linkers; gilOff the removal must
        // serialize on the link lock (removeOnDestruction is the gated
        // locked-remove helper; the relink push below goes through
        // CodeBlock::linkIncomingCall, which locks its push gilOff).
        if (isOnList())
            removeOnDestruction();

        // THREADS: the owning thread reads these words lock-free in
        // executeCachedCall/tryCallWithArguments while the install drain on
        // another Thread rewrites them; write the codeBlock FIRST, then
        // release-publish the entry (reader acquires the entry, then reads the
        // codeBlock — never a new entry with a stale codeBlock).
        if (newCodeBlock && m_protoCallFrame.codeBlock() == oldCodeBlock) {
            newCodeBlock->m_shouldAlwaysBeInlined = false;
            m_protoCallFrame.setCodeBlock(newCodeBlock);
            WTF::atomicStore(&m_addressForCall, newCodeBlock->jitCode()->addressForCall(), std::memory_order_release);
            newCodeBlock->linkIncomingCall(nullptr, this);
            return;
        }
        WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed);
    }

    void relink();


    template<typename... Args> requires (std::is_convertible_v<Args, JSValue> && ...)
    JSValue callWithArguments(JSGlobalObject*, JSValue thisValue, Args...);

private:
    VM& m_vm;
    VMEntryScope m_entryScope;
    ProtoCallFrame m_protoCallFrame;
    MarkedArgumentBuffer m_arguments;

    FunctionExecutable* m_functionExecutable;
    JSScope* m_scope;
    void* m_addressForCall { nullptr };
    unsigned m_numParameters { 0 };
#if ASSERT_ENABLED
    bool m_valid { false };
#endif

    friend class Interpreter;
};

} // namespace JSC
