/*
 * Copyright (C) 2026 Peter Steinberger All rights reserved.
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

    .text
    .p2align 4
    .globl vmEntryRecordProbeInvoke
    .type vmEntryRecordProbeInvoke,@function
vmEntryRecordProbeInvoke:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $56, %rsp
    movq %rdi, %rbx
    movq 112(%rbx), %r12

    /* Six fixed-path stack arguments; the generic path simply ignores them. */
    movq 56(%rbx), %rax
    movq %rax, 0(%rsp)
    movq 64(%rbx), %rax
    movq %rax, 8(%rsp)
    movq 72(%rbx), %rax
    movq %rax, 16(%rsp)
    movq 80(%rbx), %rax
    movq %rax, 24(%rsp)
    movq 88(%rbx), %rax
    movq %rax, 32(%rsp)
    movq 96(%rbx), %rax
    movq %rax, 40(%rsp)

    /* call pushes PC, and the actual entry's prologue pushes rbp. */
    leaq -16(%rsp), %r10
    movq %r10, 24(%r12)
    addq 0(%r12), %r10
    movq 8(%r12), %rcx
    movq 16(%r12), %rax
.Lpoison:
    movq %rax, (%r10)
    addq $8, %r10
    decq %rcx
    jnz .Lpoison

    /* No C++ frame, helper call, or compiler-selected spill after poisoning. */
    movq 0(%rbx), %r11
    movq 8(%rbx), %rdi
    movq 16(%rbx), %rsi
    movq 24(%rbx), %rdx
    movq 32(%rbx), %rcx
    movq 40(%rbx), %r8
    movq 48(%rbx), %r9
    cmpq $0, 120(%rbx)
    je .Lcall
    movq 104(%rbx), %rdx
.Lcall:
    call *%r11
    addq $56, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size vmEntryRecordProbeInvoke, .-vmEntryRecordProbeInvoke

    .p2align 4
    .globl vmEntryRecordProbeCallee
    .type vmEntryRecordProbeCallee,@function
vmEntryRecordProbeCallee:
    /* First instruction of the callee; rbp is still the real entry frame. */
    movq vmEntryRecordProbeCurrent(%rip), %r11
    movq %rbp, 32(%r11)
    movq 0(%r11), %r10
    addq %rbp, %r10
    movq 8(%r11), %rcx
    leaq 40(%r11), %rdx
.Lcapture:
    movq (%r10), %rax
    movq %rax, (%rdx)
    addq $8, %r10
    addq $8, %rdx
    decq %rcx
    jnz .Lcapture
    /* Both native and JIT calls arrive with rsp == 8 mod 16 on this ABI. */
    subq $8, %rsp
    movq %rbp, %rdi
    call vmEntryRecordProbeFinish
    addq $8, %rsp
    ret
    .size vmEntryRecordProbeCallee, .-vmEntryRecordProbeCallee
    .section .note.GNU-stack,"",@progbits
