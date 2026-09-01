/*
 * Copyright (C) 2009-2021 Apple Inc. All rights reserved.
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

#include "VM.h"
#include "Yarr.h"
#include "YarrJIT.h"

namespace JSC {

class VM;
class ExecutablePool;
class RegExp;

namespace Yarr {

class MatchingContextHolder {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    MatchingContextHolder(VM&, RegExp*, MatchFrom);
    ~MatchingContextHolder();

    static constexpr ptrdiff_t offsetOfStackLimit() { return OBJECT_OFFSETOF(MatchingContextHolder, m_stackLimit); }
    static constexpr ptrdiff_t offsetOfFreeList() { return OBJECT_OFFSETOF(MatchingContextHolder, m_freeList); }

    void* stackLimit() const { return m_stackLimit; }
    void* freeList() const { return m_freeList; }

private:
    void* m_stackLimit;
    void* m_freeList { nullptr };
    RegExp** m_executingRegExpSlot { nullptr };
    MatchFrom m_matchFrom;
};

inline MatchingContextHolder::MatchingContextHolder(VM& vm, RegExp* regExp, MatchFrom matchFrom)
    : m_matchFrom(matchFrom)
{
    if (matchFrom == MatchFrom::VMThread) {
        m_stackLimit = vm.softStackLimitForCurrentThreadSlow(); // UNGIL §A.2.2 (AB-17): per-thread limit GIL-off.
        // UNGIL AUD1.K2 sub-item / SamplingProfiler.h Group-4 row: GIL-off,
        // "executing RegExp" is per-thread state and lives in the CURRENT
        // lite's Group-4 slot (VMLite::executingRegExp) — N mutators writing
        // the single VM member was a write-write race (TSAN regexp-shared
        // family). The slot is resolved ONCE here and cached so the dtor's
        // clear targets the same word by construction (lite installation is
        // stable across a match; un-install happens only at VM exit).
        // Flag-off/GIL-on keeps the VM member byte-identically (the profiler
        // GIL-on reader and VMLite.h's "deliberately NOT in VMLitePrimitives"
        // note both name the VM member as the GIL-on storage side); the
        // GIL-off profiler reader half (SamplingProfiler.cpp lite-resolved
        // reads) is the PENDING U-T8d wiring recorded in SamplingProfiler.h.
        m_executingRegExpSlot = &vm.m_executingRegExp;
        if (vm.gilOffWithProcessGate()) [[unlikely]] {
            if (VMLite* lite = VMLite::currentIfExists(); lite && lite->vm == &vm)
                m_executingRegExpSlot = &lite->executingRegExp;
        }
        *m_executingRegExpSlot = regExp;
    } else {
        StackBounds stack = Thread::currentSingleton().stack();
        m_stackLimit = stack.recursionLimit(Options::reservedZoneSize());
    }
}

inline MatchingContextHolder::~MatchingContextHolder()
{
    if (m_matchFrom == MatchFrom::VMThread)
        *m_executingRegExpSlot = nullptr;
}

} } // namespace JSC::Yarr
