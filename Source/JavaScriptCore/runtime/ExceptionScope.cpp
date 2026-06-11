/*
 * Copyright (C) 2016-2017 Apple Inc. All rights reserved.
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
#include "ExceptionScope.h"

#include "ErrorInstance.h"
#include "Exception.h"
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>

namespace JSC {
    
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    
// UNGIL obligation 10 mode split: the chain anchor is per-lite GIL-off
// (VM::exceptionScopeVerificationState()), so a spawned thread's scope chain
// links only through its OWN frames — never into the carrier's stack (the
// deterministic GIL-off ExceptionScope::stackPosition()
// stack-use-after-return). GIL-on/flag-off: the VM copy, bit-identical.
// The linked-list write-back is NOT idempotent: ctor and dtor MUST resolve
// the same storage, so scopes live strictly inside a stable (thread, lite)
// window (see VMExceptionScopeVerificationState.h).
ExceptionScope::ExceptionScope(VM& vm, ExceptionEventLocation location)
    : m_vm(vm)
    , m_previousScope(vm.exceptionScopeVerificationState().m_topExceptionScope)
    , m_location(location)
    , m_recursionDepth(m_previousScope ? m_previousScope->m_recursionDepth + 1 : 0)
{
    auto& verificationState = m_vm.exceptionScopeVerificationState();
    m_verificationStateAtConstruction = &verificationState;
    verificationState.m_topExceptionScope = this;
}

ExceptionScope::~ExceptionScope()
{
    auto& verificationState = m_vm.exceptionScopeVerificationState();
    // Straddle enforcement (review round): the write-back is non-idempotent,
    // so a scope whose lifetime straddles a t_currentVMLite install/uninstall
    // would pop a DIFFERENT chain than the ctor pushed — cross-linking two
    // chains silently (the exact cross-stack-linkage / stackPosition() UAR
    // class obligation 10 closes). Assert storage identity (TopCallFrameSetter
    // precedent), and the strict LIFO invariant: this scope IS the top of the
    // chain it pushed onto.
    RELEASE_ASSERT(&verificationState == m_verificationStateAtConstruction);
    RELEASE_ASSERT(verificationState.m_topExceptionScope == this);
    verificationState.m_topExceptionScope = m_previousScope;
}

CString ExceptionScope::unexpectedExceptionMessage()
{
    StringPrintStream out;

    out.println("Unexpected exception observed on thread ", Thread::currentSingleton(), " at:");
    auto currentStack = StackTrace::captureStackTrace(Options::unexpectedExceptionStackTraceLimit(), 1);
    out.print(StackTracePrinter { *currentStack, "    " });

    if (!m_vm.nativeStackTraceOfLastThrow())
        return CString();
    
    out.println("The exception was thrown from thread ", *m_vm.throwingThread(), " at:");
    out.print(StackTracePrinter { *m_vm.nativeStackTraceOfLastThrow(), "    " });

    if (auto* error = dynamicDowncast<ErrorInstance>(exception()->value()))
        out.println("Error Exception: ", error->tryGetMessageForDebugging());
    else
        out.println("non-Error Exception: ", exception()->value());

    return out.toCString();
}

#endif // ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    
} // namespace JSC
