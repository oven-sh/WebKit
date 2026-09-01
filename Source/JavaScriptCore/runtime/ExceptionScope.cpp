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
#include "VMLite.h"
#include <wtf/DataLog.h>
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>

#if ASAN_ENABLED && __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#define EXCEPTION_SCOPE_CAN_PROBE_ASAN_POISON 1
#else
#define EXCEPTION_SCOPE_CAN_PROBE_ASAN_POISON 0
#endif

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
    , m_recursionDepth(0) // Computed below, AFTER the anchor is verified.
{
    auto& verificationState = m_vm.exceptionScopeVerificationState();
    m_verificationStateAtConstruction = &verificationState;
    m_liteAtConstruction = VMLite::currentIfExists();
    // Obligation-10 window-coherence, PUSH side (B1 hardening): the dtor's
    // straddle assert catches a scope whose OWN window moved, but a chain
    // anchor poisoned by an earlier flip (or by a foreign wild write) was
    // previously consumed unverified — the deferred garbage-
    // m_topExceptionScope ctor SEGV face. Every link of one chain must have
    // been pushed against THIS storage. The block below is deliberately the
    // FIRST dereference of m_previousScope in this object's lifetime (amend
    // round: the old member-init-list m_recursionDepth read consumed the
    // anchor before any check could run), so:
    //  - a readable-but-cross-window anchor fail-stops on the named
    //    RELEASE_ASSERT with the [B1-DIAG] (lite, state, thread) tuple;
    //  - an ASAN-poisoned anchor (f5f5 stack-after-return) emits the
    //    attribution tuple via the poison probe FIRST, then the very next
    //    read produces the full-provenance ASAN report;
    //  - an unmapped/garbage anchor in a non-ASAN assert build still SEGVs,
    //    but now AT this attributed B1 check line, not an incidental
    //    init-list line.
    // Legitimate GIL-on holder migration keeps storage identity (the VM
    // copy), so the assert is never-taken there; shipping builds compile
    // this class shape out entirely.
    if (m_previousScope) {
#if EXCEPTION_SCOPE_CAN_PROBE_ASAN_POISON
        if (__asan_region_is_poisoned(m_previousScope, sizeof(ExceptionScope))) [[unlikely]] {
            VMLite* currentLite = VMLite::currentIfExists();
            dataLogLn("[B1-DIAG] ExceptionScope push poisoned-anchor: this=", RawPointer(this),
                " state=", RawPointer(&verificationState),
                " prev=", RawPointer(m_previousScope),
                " lite=", RawPointer(currentLite),
                " liteTid=", currentLite ? currentLite->tid : 0,
                " vm=", RawPointer(&m_vm),
                " thread=", Thread::currentSingleton());
            // Fall through: the dereference below is now the first read of
            // the poisoned region, so ASAN reports there with provenance,
            // attributed by the line above.
        }
#endif
        if (m_previousScope->m_verificationStateAtConstruction != &verificationState) [[unlikely]] {
            VMLite* currentLite = VMLite::currentIfExists();
            dataLogLn("[B1-DIAG] ExceptionScope push window-mismatch: this=", RawPointer(this),
                " state=", RawPointer(&verificationState),
                " prev=", RawPointer(m_previousScope),
                " prevStateAtCtor=", RawPointer(m_previousScope->m_verificationStateAtConstruction),
                " prevLiteAtCtor=", RawPointer(m_previousScope->m_liteAtConstruction),
                " lite=", RawPointer(currentLite),
                " liteTid=", currentLite ? currentLite->tid : 0,
                " vm=", RawPointer(&m_vm),
                " thread=", Thread::currentSingleton());
        }
        RELEASE_ASSERT(m_previousScope->m_verificationStateAtConstruction == &verificationState);
        m_recursionDepth = m_previousScope->m_recursionDepth + 1;
    }
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
    if (&verificationState != m_verificationStateAtConstruction || verificationState.m_topExceptionScope != this) [[unlikely]] {
        // B1 diagnostic: name the moved (thread, lite) window before the
        // fail-stop below, so a routing flip is attributable from the log
        // (which lite each end resolved, and on which thread).
        VMLite* currentLite = VMLite::currentIfExists();
        dataLogLn("[B1-DIAG] ExceptionScope straddle: this=", RawPointer(this),
            " stateAtCtor=", RawPointer(m_verificationStateAtConstruction),
            " stateAtDtor=", RawPointer(&verificationState),
            " liteAtCtor=", RawPointer(m_liteAtConstruction),
            " liteAtDtor=", RawPointer(currentLite),
            " liteAtDtorTid=", currentLite ? currentLite->tid : 0,
            " liteAtDtorVM=", RawPointer(currentLite ? static_cast<void*>(currentLite->vm) : nullptr),
            " vm=", RawPointer(&m_vm),
            " top=", RawPointer(verificationState.m_topExceptionScope),
            " prev=", RawPointer(m_previousScope),
            " thread=", Thread::currentSingleton());
    }
    RELEASE_ASSERT(&verificationState == m_verificationStateAtConstruction);
    RELEASE_ASSERT(verificationState.m_topExceptionScope == this);
    verificationState.m_topExceptionScope = m_previousScope;
}

// Obligation-10 window-coherence, CONSUME side (B1 hardening follow-up):
// covers the one mid-lifetime dereference of m_previousScope outside this
// class (ThrowScope::~ThrowScope reading m_previousScope->stackPosition()),
// which the 2026-06-11g AMEND record left as the known residual consumption
// site. This is deliberately a SEPARATE function from the push-side check in
// the ctor: an unmapped-garbage anchor in a non-ASAN assert build SEGVs at
// THIS line's dereference, so the faulting PC alone distinguishes a
// consume-side face from a push-side one. Coverage mirrors the amended
// push-side statement:
//  - readable-but-cross-window anchor -> named RELEASE_ASSERT below with the
//    [B1-DIAG] consume (lite, state, thread) tuple;
//  - ASAN-poisoned anchor (f5f5 stack-after-return) -> attribution tuple via
//    the poison probe FIRST, then the very next read produces the
//    full-provenance ASAN report;
//  - unmapped/garbage anchor in a non-ASAN assert build -> SEGV at this
//    attributed consume-side line.
// A trample that reuses the anchor's memory for a coherent same-window scope
// remains undetectable here (same limitation as the push side). Legitimate
// GIL-on holder migration keeps storage identity (the VM copy), so the
// assert is never-taken there; shipping builds compile this class shape out
// entirely.
void ExceptionScope::verifyPreviousScopeWindowCoherenceBeforeConsume(const char* site)
{
    if (!m_previousScope)
        return;
    auto& verificationState = m_vm.exceptionScopeVerificationState();
#if EXCEPTION_SCOPE_CAN_PROBE_ASAN_POISON
    if (__asan_region_is_poisoned(m_previousScope, sizeof(ExceptionScope))) [[unlikely]] {
        VMLite* currentLite = VMLite::currentIfExists();
        dataLogLn("[B1-DIAG] ExceptionScope consume poisoned-anchor: site=", site,
            " this=", RawPointer(this),
            " state=", RawPointer(&verificationState),
            " stateAtCtor=", RawPointer(m_verificationStateAtConstruction),
            " prev=", RawPointer(m_previousScope),
            " liteAtCtor=", RawPointer(m_liteAtConstruction),
            " lite=", RawPointer(currentLite),
            " liteTid=", currentLite ? currentLite->tid : 0,
            " vm=", RawPointer(&m_vm),
            " thread=", Thread::currentSingleton());
        // Fall through: the dereference below is the first read of the
        // poisoned region on this path, so ASAN reports there with
        // provenance, attributed by the line above.
    }
#endif
    if (m_previousScope->m_verificationStateAtConstruction != &verificationState) [[unlikely]] {
        VMLite* currentLite = VMLite::currentIfExists();
        dataLogLn("[B1-DIAG] ExceptionScope consume window-mismatch: site=", site,
            " this=", RawPointer(this),
            " state=", RawPointer(&verificationState),
            " stateAtCtor=", RawPointer(m_verificationStateAtConstruction),
            " prev=", RawPointer(m_previousScope),
            " prevStateAtCtor=", RawPointer(m_previousScope->m_verificationStateAtConstruction),
            " prevLiteAtCtor=", RawPointer(m_previousScope->m_liteAtConstruction),
            " lite=", RawPointer(currentLite),
            " liteTid=", currentLite ? currentLite->tid : 0,
            " vm=", RawPointer(&m_vm),
            " thread=", Thread::currentSingleton());
    }
    RELEASE_ASSERT(m_previousScope->m_verificationStateAtConstruction == &verificationState);
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
