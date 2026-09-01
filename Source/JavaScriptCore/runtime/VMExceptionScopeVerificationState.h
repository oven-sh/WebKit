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

#pragma once

#include <wtf/Platform.h>

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)

#include "ExceptionEventLocation.h"
#include <memory>
#include <wtf/RefPtr.h>
#include <wtf/StackTrace.h>
#include <wtf/Threading.h>

namespace JSC {

class ExceptionScope;

// UNGIL audit K4 table I / INTEGRATE-ungil.md obligation 10 (owner U-T8b):
// the EXCEPTION_SCOPE_VERIFICATION bookkeeping (the ExceptionScope linked
// chain anchor + simulated-throw state) is PER-LITE GIL-off — throw state is
// thread-local (SPEC-vmstate I15). The fields are bundled in this one struct
// so the storage can live in TWO places with a single mode-split selector
// (VM::exceptionScopeVerificationState(), the group3Primitives()-style
// accessor):
//   - GIL-on / flag-off / second-VM U0b: the VM member
//     (VM::m_exceptionScopeVerificationState) — bit-identical to the
//     pre-split single-mutator behavior;
//   - GIL-off: the CURRENT lite's copy (VMLite tail append — debug-only,
//     NOT part of the frozen VMLitePrimitives ABI; no generated-code
//     offsets involve it).
// Field names/types are EXACTLY the former VM members; the relocation is
// the rename that turns any missed raw site into a compile error.
//
// CAUTION (review-round requirement): unlike the group3Primitives
// precedent, the ExceptionScope chain write-back is NOT idempotent — a
// scope whose lifetime straddles a t_currentVMLite install/uninstall would
// resolve DIFFERENT storage in ctor vs dtor and corrupt both chains. Scopes
// must stay strictly inside a stable (thread, lite) window.
struct VMExceptionScopeVerificationState {
    ExceptionScope* m_topExceptionScope { nullptr };
    ExceptionEventLocation m_simulatedThrowPointLocation;
    unsigned m_simulatedThrowPointRecursionDepth { 0 };
    mutable bool m_needExceptionCheck { false };
    std::unique_ptr<WTF::StackTrace> m_nativeStackTraceOfLastThrow;
    std::unique_ptr<WTF::StackTrace> m_nativeStackTraceOfLastSimulatedThrow;
    RefPtr<WTF::Thread> m_throwingThread;
};

} // namespace JSC

#endif // ENABLE(EXCEPTION_SCOPE_VERIFICATION)
