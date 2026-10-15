/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include <array>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/ASCIILiteral.h>

#if ((CPU(X86_64) && OS(LINUX)) || (CPU(ARM64) && OS(DARWIN))) && !ASAN_ENABLED
#define B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS 1
#else
#define B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS 0
#endif

namespace JSC { namespace B3 {

class CompilationCohort {
    WTF_MAKE_NONCOPYABLE(CompilationCohort);
public:
    static constexpr unsigned maxSize = 16;
    static constexpr bool interleavesMembers = B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS;

    CompilationCohort() = default;
    JS_EXPORT_PRIVATE ~CompilationCohort();

    JS_EXPORT_PRIVATE void run(unsigned size, const ScopedLambda<void(unsigned member)>& compile);

    JS_EXPORT_PRIVATE static void yieldBeforePhase(ASCIILiteral phase);

#if B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS
private:
    static constexpr size_t stackSize = 1 * MB;

    static CompilationCohort*& current();
    static void runMember();

    void prepareStack(unsigned member);
    void yield(const char* phase);
    [[noreturn]] void finish();
    unsigned pickNextMember();
    void switchTo(unsigned member);

    std::array<const char*, maxSize> m_nextPhases { };
    std::array<unsigned, maxSize> m_numberOfYields { };
    std::array<void*, maxSize> m_stackPointers { };
    std::array<void*, maxSize> m_stacks { };
    void* m_stackPointerOfCaller { nullptr };
    const ScopedLambda<void(unsigned)>* m_compile { nullptr };
    const char* m_phase { nullptr };
    unsigned m_size { 0 };
    unsigned m_runningMember { 0 };
    unsigned m_numberOfUnfinishedMembers { 0 };
#endif
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
