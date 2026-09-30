/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
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

#include "JSDestructibleObject.h"
#include "StackFrame.h"
#include <wtf/Vector.h>

namespace JSC {
    
class Exception final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.exceptionSpace();
    }

    enum class StackCaptureAction {
        CaptureStack,
        DoNotCaptureStack,
        // It is thrown by code that makes nothing of where it was thrown from, and may well be caught by such code. The stack is captured if it comes to anything else, by captureStackIfPending(). That is while it
        // is unwinding, when every frame that it has been through is still there.
        CaptureStackWhenItIsSeen
    };
    JS_EXPORT_PRIVATE static Exception* create(VM&, JSValue thrownValue, StackCaptureAction = StackCaptureAction::CaptureStack);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_VISIT_CHILDREN;

    DECLARE_EXPORT_INFO;

    static constexpr ptrdiff_t valueOffset()
    {
        return OBJECT_OFFSETOF(Exception, m_value);
    }

    JSValue value() const { return m_value.get(); }
    const Vector<StackFrame>& stack() const LIFETIME_BOUND { return m_stack; }
    bool isStackCapturePending() const { return m_isStackCapturePending; }
    void captureStackIfPending(VM&);
    // What the stack is now, less the first `framesToSkip` of it, after `earlier`: frames that it went through and that are no longer there.
    void captureStackIfPending(VM&, Vector<StackFrame>&& earlier, size_t framesToSkip);
    static size_t estimatedSize(JSCell*, VM&);

#if USE(BUN_JSC_ADDITIONS)
    // The async context (JSGlobalObject::m_asyncContextData field 0) this was first thrown in.
    // Empty: it never went through VM::throwException (never thrown, the termination exception),
    // or did before the embedder started tracking async contexts.
    JSValue asyncContext() const { return m_asyncContext.get(); }
    void setAsyncContext(VM& vm, JSValue asyncContext) { m_asyncContext.set(vm, this, asyncContext); }
#endif

    // It has been thrown again by the code that caught it, as at the end of a `finally`, and has not begun to unwind since. It is this
    // object that is thrown then, and not what it holds.
    bool isBeingRethrown() const { return m_isBeingRethrown; }
    void setIsBeingRethrown(bool value) { m_isBeingRethrown = value; }

    bool didNotifyInspectorOfThrow() const { return m_didNotifyInspectorOfThrow; }
    void setDidNotifyInspectorOfThrow() { m_didNotifyInspectorOfThrow = true; }

#if ENABLE(WEBASSEMBLY)
    void tryUnwrapValueForJSTag(VM&);
    void wrapValueForJSTag(JSGlobalObject*);
#endif

    ~Exception();

private:
    Exception(VM&, JSValue thrownValue);
    void finishCreation(VM&, StackCaptureAction);
    static void destroy(JSCell*);

    WriteBarrier<Unknown> m_value;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_asyncContext;
#endif
    Vector<StackFrame> m_stack;
    bool m_didNotifyInspectorOfThrow { false };
    bool m_isBeingRethrown { false };
    bool m_isStackCapturePending { false };

    friend class LLIntOffsetsExtractor;
};

} // namespace JSC
