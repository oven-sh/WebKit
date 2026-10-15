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

#include "config.h"
#include "B3CompilationCohort.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include <wtf/InlineASM.h>
#include <wtf/OSAllocator.h>

#if B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS

extern "C" void b3SwitchStacks(void** savedStackPointer, void* stackPointer);

#if CPU(X86_64)
asm (
    ".text" "\n"
    ".globl " SYMBOL_STRING(b3SwitchStacks) "\n"
    HIDE_SYMBOL(b3SwitchStacks) "\n"
    SYMBOL_STRING(b3SwitchStacks) ":" "\n"
    "pushq %rbp" "\n"
    "pushq %rbx" "\n"
    "pushq %r12" "\n"
    "pushq %r13" "\n"
    "pushq %r14" "\n"
    "pushq %r15" "\n"
    "movq %rsp, (%rdi)" "\n"
    "movq %rsi, %rsp" "\n"
    "popq %r15" "\n"
    "popq %r14" "\n"
    "popq %r13" "\n"
    "popq %r12" "\n"
    "popq %rbx" "\n"
    "popq %rbp" "\n"
    "ret" "\n"
);
#elif CPU(ARM64)
asm (
    ".text" "\n"
    ".balign 16" "\n"
    ".globl " SYMBOL_STRING(b3SwitchStacks) "\n"
    HIDE_SYMBOL(b3SwitchStacks) "\n"
    SYMBOL_STRING(b3SwitchStacks) ":" "\n"
    "sub sp, sp, #160" "\n"
    "stp x19, x20, [sp, #0]" "\n"
    "stp x21, x22, [sp, #16]" "\n"
    "stp x23, x24, [sp, #32]" "\n"
    "stp x25, x26, [sp, #48]" "\n"
    "stp x27, x28, [sp, #64]" "\n"
    "stp x29, x30, [sp, #80]" "\n"
    "stp d8, d9, [sp, #96]" "\n"
    "stp d10, d11, [sp, #112]" "\n"
    "stp d12, d13, [sp, #128]" "\n"
    "stp d14, d15, [sp, #144]" "\n"
    "mov x9, sp" "\n"
    "str x9, [x0]" "\n"
    "mov sp, x1" "\n"
    "ldp x19, x20, [sp, #0]" "\n"
    "ldp x21, x22, [sp, #16]" "\n"
    "ldp x23, x24, [sp, #32]" "\n"
    "ldp x25, x26, [sp, #48]" "\n"
    "ldp x27, x28, [sp, #64]" "\n"
    "ldp x29, x30, [sp, #80]" "\n"
    "ldp d8, d9, [sp, #96]" "\n"
    "ldp d10, d11, [sp, #112]" "\n"
    "ldp d12, d13, [sp, #128]" "\n"
    "ldp d14, d15, [sp, #144]" "\n"
    "add sp, sp, #160" "\n"
    "ret" "\n"
);
#endif

#endif // B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS

namespace JSC { namespace B3 {

#if B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS

#if CPU(X86_64)
static constexpr unsigned numberOfWordsInSwitchFrame = 8;
static constexpr unsigned indexOfReturnAddressInSwitchFrame = 6;
#elif CPU(ARM64)
static constexpr unsigned numberOfWordsInSwitchFrame = 20;
static constexpr unsigned indexOfReturnAddressInSwitchFrame = 11;
#endif

static constexpr unsigned numberOfGuardPages = 1;

CompilationCohort*& CompilationCohort::current()
{
    static thread_local CompilationCohort* cohort;
    return cohort;
}

CompilationCohort::~CompilationCohort()
{
    for (void* stack : m_stacks) {
        if (stack)
            OSAllocator::releaseDecommitted(stack, stackSize, numberOfGuardPages);
    }
}

void CompilationCohort::prepareStack(unsigned member)
{
    if (!m_stacks[member])
        m_stacks[member] = OSAllocator::reserveAndCommit(stackSize, OSAllocator::UnknownUsage, nullptr, true, false, false, numberOfGuardPages);
    std::span frame { reinterpret_cast<void**>(static_cast<char*>(m_stacks[member]) + stackSize) - numberOfWordsInSwitchFrame, numberOfWordsInSwitchFrame };
    std::ranges::fill(frame, nullptr);
    frame[indexOfReturnAddressInSwitchFrame] = reinterpret_cast<void*>(runMember);
    m_stackPointers[member] = frame.data();
}

void CompilationCohort::run(unsigned size, const ScopedLambda<void(unsigned)>& compile)
{
    RELEASE_ASSERT(size && size <= maxSize && !current());
    static constexpr const char* start = "";
    for (unsigned member = 0; member < size; ++member) {
        prepareStack(member);
        m_nextPhases[member] = start;
        m_numberOfYields[member] = 0;
    }
    m_size = size;
    m_numberOfUnfinishedMembers = size;
    m_compile = &compile;
    m_phase = start;
    m_runningMember = 0;
    current() = this;
    b3SwitchStacks(&m_stackPointerOfCaller, m_stackPointers[0]);
    current() = nullptr;
}

void CompilationCohort::runMember()
{
    CompilationCohort& cohort = *current();
    (*cohort.m_compile)(cohort.m_runningMember);
    cohort.finish();
}

void CompilationCohort::finish()
{
    m_nextPhases[m_runningMember] = nullptr;
    if (--m_numberOfUnfinishedMembers)
        switchTo(pickNextMember());
    else
        b3SwitchStacks(&m_stackPointers[m_runningMember], m_stackPointerOfCaller);
    RELEASE_ASSERT_NOT_REACHED();
}

void CompilationCohort::yieldBeforePhase(ASCIILiteral phase)
{
    if (CompilationCohort* cohort = current()) [[unlikely]]
        cohort->yield(phase.characters());
}

void CompilationCohort::yield(const char* phase)
{
    m_nextPhases[m_runningMember] = phase;
    ++m_numberOfYields[m_runningMember];
    switchTo(pickNextMember());
}

unsigned CompilationCohort::pickNextMember()
{
    for (unsigned offset = 1; offset <= m_size; ++offset) {
        unsigned member = m_runningMember + offset;
        if (member >= m_size)
            member -= m_size;
        if (m_nextPhases[member] == m_phase)
            return member;
    }
    unsigned result = 0;
    unsigned leastNumberOfYields = std::numeric_limits<unsigned>::max();
    for (unsigned member = 0; member < m_size; ++member) {
        if (m_nextPhases[member] && m_numberOfYields[member] < leastNumberOfYields) {
            leastNumberOfYields = m_numberOfYields[member];
            result = member;
        }
    }
    m_phase = m_nextPhases[result];
    return result;
}

void CompilationCohort::switchTo(unsigned member)
{
    if (member == m_runningMember)
        return;
    unsigned previous = std::exchange(m_runningMember, member);
    b3SwitchStacks(&m_stackPointers[previous], m_stackPointers[member]);
}

#else // B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS

CompilationCohort::~CompilationCohort() = default;

void CompilationCohort::run(unsigned size, const ScopedLambda<void(unsigned)>& compile)
{
    for (unsigned member = 0; member < size; ++member)
        compile(member);
}

void CompilationCohort::yieldBeforePhase(ASCIILiteral)
{
}

#endif // B3_COMPILATION_COHORT_INTERLEAVES_MEMBERS

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
