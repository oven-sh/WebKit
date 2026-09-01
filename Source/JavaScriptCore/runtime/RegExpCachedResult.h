/*
 * Copyright (C) 2012-2021 Apple Inc. All rights reserved.
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

#include "MatchResult.h"
#include "SlotVisitorMacros.h"
#include "WriteBarrier.h"

namespace JSC {

class JSArray;
class JSString;
class RegExp;

// RegExpCachedResult is used to track the cached results of the last
// match, stores on the RegExp constructor (e.g. $&, $_, $1, $2 ...).
// These values will be lazily generated on demand, so the cached result
// may be in a lazy or reified state. A lazy state is indicated by a
// value of m_result indicating a successful match, and a reified state
// is indicated by setting m_result to MatchResult::failed().
// Following a successful match, m_result, m_lastInput and m_lastRegExp
// can be used to reify the results from the match, following reification
// m_reifiedResult and m_reifiedInput hold the cached results.
//
// =============================================================================
// UNGIL AUD1.K2 / annex N7 RESOLVED-7 / SD19 (BINDING; U-T8b) — per-lite
// carrier. This struct is a multi-word cache {m_result (2 words),
// m_lastInput, m_lastRegExp} rewritten on EVERY global-flag match — by
// DFG/FTL INLINE (RecordRegExpCachedResult consumes offsetOfResult/
// offsetOfLastInput/offsetOfLastRegExp below) — plus a lazy reify flip
// {m_reified + 4 reified barriers}. It is NOT lockable without putting a
// §LK acquisition on every successful match, so the ruling is §K.1 per-lite:
//
//   - GIL-OFF (RUNTIME RE-POINT LANDED — TSAN wave 1: every C++ consumer of
//     the match-result stream (RegExpConstructor.cpp, RegExpPrototype.cpp,
//     RegExpObject.cpp / RegExpObjectInlines.h, StringPrototype.cpp /
//     StringPrototypeInlines.h, RegExpSubstringGlobalAtomCache.cpp,
//     DFGOperations.cpp slow paths) now routes through
//     threadRegExpGlobalData(globalObject) (declared in JSGlobalObject.h),
//     so record()'s five plain stores + the reify flip are single-thread-
//     private GIL-off. vm.m_executingRegExp landed with the same wave:
//     YarrMatchingContextHolder ctor/dtor route to the CURRENT lite's
//     Group-4 slot (VMLite::executingRegExp) GIL-off.
//     JIT SIDE (gated; A16-ext jit slice still open for the OPTIMIZED
//     emission): gilOff DFG/FTL compilation can no longer write the SHARED
//     in-object stream. DFGStrengthReductionPhase refuses foldToConstant()
//     (which inserts RecordRegExpCachedResult) and convertTestToTestInline()
//     when gilOff, so the generic nodes lower to the re-pointed C++
//     operations; DFGSpeculativeJIT.cpp, DFGSpeculativeJIT64.cpp and
//     FTLLowerDFGToB3.cpp all carry fail-stop tripwires
//     (RecordRegExpCachedResult + RegExpTestInline) behind that gate.
//     CLASSIFICATION (why a gate, not a deprioritized residual): an
//     unguarded inline record interleaving with another thread's
//     record()/lastResult() cross-pairs (m_lastInput, m_result.start/end);
//     leftContext() then computes jsSubstring(0, m_result.start) with a
//     start past the input's length — MEMORY-SAFETY (OOB substring /
//     torn multi-word), NOT merely stale legacy statics, and invisible to
//     TSAN because JIT code is uninstrumented. The A16-ext lite-resident L2
//     slot re-point restores the inline fast path gilOff):
//     each entered thread owns a PRIVATE RegExpGlobalData stream
//     (carrying one RegExpCachedResult) — the per-lite side table in
//     JSGlobalObject.cpp (threadRegExpGlobalData), GC-rooted via the
//     global's visitChildren registry walk and freed by the lite-teardown
//     purge (~VMLite -> purgePerLiteRealmStateForLite). SEMANTICS (SD19,
//     GIL-off only): RegExp.$1-$9 / lastMatch / leftContext / rightContext /
//     input observe ONLY matches performed by the CURRENT thread.
//   - Every member access on a given copy is then single-thread-private:
//     the reify flip and all stores stay PLAIN (no atomics needed) — per
//     AUD1.K2's "the reify flip stays single-thread-private => plain
//     stores".
//   - TIERS: gilOff-mode compilation must emit
//     loadVMLite -> liteRegExpGlobalData -> field for every offsetOf*
//     consumer (AUD1.K4 / A16 ext; jit slice — see the activation checklist
//     in VMLite.cpp). FLAG-OFF/GIL-ON: the baked global-object-relative
//     address (offsetOfCachedResult chain) stays byte-identical; the
//     offsetOf* accessors below therefore MUST NOT change meaning or layout.
// =============================================================================
class RegExpCachedResult {
public:
    inline void record(VM&, JSObject* owner, RegExp*, JSString* input, MatchResult, bool oneCharacterMatch);

    JSArray* lastResult(JSGlobalObject*, JSObject* owner);
    void setInput(JSGlobalObject*, JSObject* owner, JSString*);

    JSString* leftContext(JSGlobalObject*, JSObject* owner);
    JSString* rightContext(JSGlobalObject*, JSObject* owner);

    JSString* input()
    {
        return m_reified ? m_reifiedInput.get() : m_lastInput.get();
    }

    DECLARE_VISIT_AGGREGATE;

    // m_lastRegExp would be nullptr when RegExpCachedResult is not reified.
    // If we find m_lastRegExp is nullptr, it means this should hold the empty RegExp.
    static constexpr ptrdiff_t offsetOfLastRegExp() { return OBJECT_OFFSETOF(RegExpCachedResult, m_lastRegExp); }
    static constexpr ptrdiff_t offsetOfLastInput() { return OBJECT_OFFSETOF(RegExpCachedResult, m_lastInput); }
    static constexpr ptrdiff_t offsetOfResult() { return OBJECT_OFFSETOF(RegExpCachedResult, m_result); }
    static constexpr ptrdiff_t offsetOfReified() { return OBJECT_OFFSETOF(RegExpCachedResult, m_reified); }
    static constexpr ptrdiff_t offsetOfOneCharacterMatch() { return OBJECT_OFFSETOF(RegExpCachedResult, m_oneCharacterMatch); }

    MatchResult result() const { return m_result; }

private:
    MatchResult m_result { 0, 0 };
    // m_reified / m_oneCharacterMatch stay plain bool (JIT consumes
    // offsetOfReified/offsetOfOneCharacterMatch as raw byte stores); the C++
    // sites that race with the concurrent GC visitor (record() vs
    // visitAggregateImpl) go through WTF::atomicLoad/Store relaxed instead.
    bool m_reified { false };
    bool m_oneCharacterMatch { false };
    WriteBarrier<JSString> m_lastInput;
    WriteBarrier<RegExp> m_lastRegExp;
    WriteBarrier<JSArray> m_reifiedResult;
    WriteBarrier<JSString> m_reifiedInput;
    WriteBarrier<JSString> m_reifiedLeftContext;
    WriteBarrier<JSString> m_reifiedRightContext;
};

} // namespace JSC
