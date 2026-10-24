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

class ErrorInstance;
namespace AOT { struct ReturnAddressTrace; }
    
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
        DoNotCaptureStack
    };
    JS_EXPORT_PRIVATE static Exception* create(VM&, JSValue thrownValue, StackCaptureAction = StackCaptureAction::CaptureStack);
    // Walks no frame. Null unless the value is an Error that still holds the return addresses of its own stack.
    static Exception* tryCreateWithStackOfError(VM&, JSValue thrownValue);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_VISIT_CHILDREN;

    DECLARE_EXPORT_INFO;

    static constexpr ptrdiff_t valueOffset()
    {
        return OBJECT_OFFSETOF(Exception, m_value);
    }

    JSValue value() const { return m_value.get(); }
    const Vector<StackFrame>& stack() const LIFETIME_BOUND
    {
        if (returnAddresses()) [[unlikely]]
            const_cast<Exception*>(this)->materializeStack();
        return m_stack;
    }
    // The stack may be skipped when a user catch clause handles the throw, but vm.lastException()
    // outlives the catch and can still be reported, so fall back to the Error's own stack. That one
    // is freed whenever the Error materializes its stack property, so it is copied in, not borrowed.
    JS_EXPORT_PRIVATE const Vector<StackFrame>& stackForReporting(VM&) LIFETIME_BOUND;
    static size_t estimatedSize(JSCell*, VM&);

#if USE(BUN_JSC_ADDITIONS)
    // The async context (JSGlobalObject::m_asyncContextData field 0) this was first thrown in.
    // Empty: it never went through VM::throwException (never thrown, the termination exception),
    // or did before the embedder started tracking async contexts.
    JSValue asyncContext() const { return m_asyncContext.get(); }
    void setAsyncContext(VM& vm, JSValue asyncContext) { m_asyncContext.set(vm, this, asyncContext); }
#endif

    bool didNotifyInspectorOfThrow() const { return m_returnAddressesAndFlag & didNotifyInspectorOfThrowFlag; }
    void setDidNotifyInspectorOfThrow() { m_returnAddressesAndFlag |= didNotifyInspectorOfThrowFlag; }

#if ENABLE(WEBASSEMBLY)
    void tryUnwrapValueForJSTag(VM&);
    void wrapValueForJSTag(JSGlobalObject*);
#endif

    ~Exception();

private:
    Exception(VM&, JSValue thrownValue);
    void finishCreation(VM&, StackCaptureAction);
    bool tryRetainReturnAddresses(VM&);
    void retainReturnAddresses(VM&, AOT::ReturnAddressTrace*, Vector<StackFrame>&& otherFrames);
    static ErrorInstance* errorToTakeStackFrom(VM&, JSValue thrownValue);
    void takeStackFrom(VM&, ErrorInstance&);
    JS_EXPORT_PRIVATE void materializeStack();
    static void destroy(JSCell*);
    static constexpr uintptr_t didNotifyInspectorOfThrowFlag = 1;
    AOT::ReturnAddressTrace* returnAddresses() const { return std::bit_cast<AOT::ReturnAddressTrace*>(m_returnAddressesAndFlag & ~didNotifyInspectorOfThrowFlag); }
    void setReturnAddresses(AOT::ReturnAddressTrace* returnAddresses) { m_returnAddressesAndFlag = std::bit_cast<uintptr_t>(returnAddresses) | (m_returnAddressesAndFlag & didNotifyInspectorOfThrowFlag); }

    WriteBarrier<Unknown> m_value;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_asyncContext;
#endif
    Vector<StackFrame> m_stack;
    uintptr_t m_returnAddressesAndFlag { 0 };

    friend class LLIntOffsetsExtractor;
};

#if CPU(ADDRESS64)
static_assert(sizeof(Exception) <= 48);
#endif

} // namespace JSC
