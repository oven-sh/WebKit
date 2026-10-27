/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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

#if ENABLE(ASSEMBLER) && CPU(X86_64)
#include "MacroAssembler.h"

#include "OperationResult.h"
#include "ProbeContext.h"
#include <wtf/InlineASM.h>

#if OS(DARWIN)
#include <sys/sysctl.h>
#endif

namespace JSC {

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(ctiMasmProbeTrampoline, void, ());
JSC_ANNOTATE_JIT_OPERATION_PROBE(ctiMasmProbeTrampoline);
#if !OS(DARWIN)
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(ctiMasmProbeTrampolineAVX, void, ());
JSC_ANNOTATE_JIT_OPERATION_PROBE(ctiMasmProbeTrampolineAVX);
#endif

// The following are offsets for Probe::State fields accessed by the ctiMasmProbeTrampoline stub.

#define PTR_SIZE 8

#define PROBE_PROBE_FUNCTION_OFFSET (0 * PTR_SIZE)
#define PROBE_ARG_OFFSET (1 * PTR_SIZE)
#define PROBE_INIT_STACK_FUNCTION_OFFSET (2 * PTR_SIZE)
#define PROBE_INIT_STACK_ARG_OFFSET (3 * PTR_SIZE)

#define PROBE_FIRST_GPR_OFFSET (4 * PTR_SIZE)
#define PROBE_CPU_EAX_OFFSET (PROBE_FIRST_GPR_OFFSET + (0 * PTR_SIZE))
#define PROBE_CPU_ECX_OFFSET (PROBE_FIRST_GPR_OFFSET + (1 * PTR_SIZE))
#define PROBE_CPU_EDX_OFFSET (PROBE_FIRST_GPR_OFFSET + (2 * PTR_SIZE))
#define PROBE_CPU_EBX_OFFSET (PROBE_FIRST_GPR_OFFSET + (3 * PTR_SIZE))
#define PROBE_CPU_ESP_OFFSET (PROBE_FIRST_GPR_OFFSET + (4 * PTR_SIZE))
#define PROBE_CPU_EBP_OFFSET (PROBE_FIRST_GPR_OFFSET + (5 * PTR_SIZE))
#define PROBE_CPU_ESI_OFFSET (PROBE_FIRST_GPR_OFFSET + (6 * PTR_SIZE))
#define PROBE_CPU_EDI_OFFSET (PROBE_FIRST_GPR_OFFSET + (7 * PTR_SIZE))

#define PROBE_CPU_R8_OFFSET (PROBE_FIRST_GPR_OFFSET + (8 * PTR_SIZE))
#define PROBE_CPU_R9_OFFSET (PROBE_FIRST_GPR_OFFSET + (9 * PTR_SIZE))
#define PROBE_CPU_R10_OFFSET (PROBE_FIRST_GPR_OFFSET + (10 * PTR_SIZE))
#define PROBE_CPU_R11_OFFSET (PROBE_FIRST_GPR_OFFSET + (11 * PTR_SIZE))
#define PROBE_CPU_R12_OFFSET (PROBE_FIRST_GPR_OFFSET + (12 * PTR_SIZE))
#define PROBE_CPU_R13_OFFSET (PROBE_FIRST_GPR_OFFSET + (13 * PTR_SIZE))
#define PROBE_CPU_R14_OFFSET (PROBE_FIRST_GPR_OFFSET + (14 * PTR_SIZE))
#define PROBE_CPU_R15_OFFSET (PROBE_FIRST_GPR_OFFSET + (15 * PTR_SIZE))
#define PROBE_FIRST_SPR_OFFSET (PROBE_FIRST_GPR_OFFSET + (16 * PTR_SIZE))

#define PROBE_CPU_EIP_OFFSET (PROBE_FIRST_SPR_OFFSET + (0 * PTR_SIZE))
#define PROBE_CPU_EFLAGS_OFFSET (PROBE_FIRST_SPR_OFFSET + (1 * PTR_SIZE))
#define PROBE_FIRST_XMM_OFFSET (PROBE_FIRST_SPR_OFFSET + (2 * PTR_SIZE))

#define XMM_VECTOR_SIZE 16

// For the case when we want to save 128 bits for each XMM register, the layout is different.
#define PROBE_CPU_XMM0_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (0 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM1_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (1 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM2_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (2 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM3_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (3 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM4_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (4 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM5_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (5 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM6_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (6 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM7_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (7 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM8_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (8 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM9_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (9 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM10_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (10 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM11_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (11 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM12_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (12 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM13_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (13 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM14_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (14 * XMM_VECTOR_SIZE))
#define PROBE_CPU_XMM15_VECTOR_OFFSET (PROBE_FIRST_XMM_OFFSET + (15 * XMM_VECTOR_SIZE))

#define PROBE_SIZE (PROBE_FIRST_XMM_OFFSET + 16 * XMM_VECTOR_SIZE)

// The outgoing record to be popped off the stack at the end consists of:
// eflags, eax, ecx, ebp, eip.
#define OUT_SIZE        (5 * PTR_SIZE)

// These ASSERTs remind you that if you change the layout of Probe::State,
// you need to change ctiMasmProbeTrampoline offsets above to match.
#define PROBE_OFFSETOF(x) offsetof(struct Probe::State, x)
#define PROBE_OFFSETOF_REG(x, reg) offsetof(struct Probe::State, x) + reg * sizeof((reinterpret_cast<Probe::State*>(0))->x[reg])
static_assert(PROBE_OFFSETOF(probeFunction) == PROBE_PROBE_FUNCTION_OFFSET, "Probe::State::probeFunction's offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF(arg) == PROBE_ARG_OFFSET, "Probe::State::arg's offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF(initializeStackFunction) == PROBE_INIT_STACK_FUNCTION_OFFSET, "Probe::State::initializeStackFunction's offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF(initializeStackArg) == PROBE_INIT_STACK_ARG_OFFSET, "Probe::State::initializeStackArg's offset matches ctiMasmProbeTrampoline");

static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::eax) == PROBE_CPU_EAX_OFFSET, "Probe::State::cpu.gprs[eax]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::ecx) == PROBE_CPU_ECX_OFFSET, "Probe::State::cpu.gprs[ecx]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::edx) == PROBE_CPU_EDX_OFFSET, "Probe::State::cpu.gprs[edx]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::ebx) == PROBE_CPU_EBX_OFFSET, "Probe::State::cpu.gprs[ebx]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::esp) == PROBE_CPU_ESP_OFFSET, "Probe::State::cpu.gprs[esp]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::ebp) == PROBE_CPU_EBP_OFFSET, "Probe::State::cpu.gprs[ebp]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::esi) == PROBE_CPU_ESI_OFFSET, "Probe::State::cpu.gprs[esi]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::edi) == PROBE_CPU_EDI_OFFSET, "Probe::State::cpu.gprs[edi]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.sprs, X86Registers::eip) == PROBE_CPU_EIP_OFFSET, "Probe::State::cpu.gprs[eip]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.sprs, X86Registers::eflags) == PROBE_CPU_EFLAGS_OFFSET, "Probe::State::cpu.sprs[eflags]'s offset matches ctiMasmProbeTrampoline");

static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r8) == PROBE_CPU_R8_OFFSET, "Probe::State::cpu.gprs[r8]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r9) == PROBE_CPU_R9_OFFSET, "Probe::State::cpu.gprs[r9]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r10) == PROBE_CPU_R10_OFFSET, "Probe::State::cpu.gprs[r10]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r11) == PROBE_CPU_R11_OFFSET, "Probe::State::cpu.gprs[r11]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r12) == PROBE_CPU_R12_OFFSET, "Probe::State::cpu.gprs[r12]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r13) == PROBE_CPU_R13_OFFSET, "Probe::State::cpu.gprs[r13]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r14) == PROBE_CPU_R14_OFFSET, "Probe::State::cpu.gprs[r14]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.gprs, X86Registers::r15) == PROBE_CPU_R15_OFFSET, "Probe::State::cpu.gprs[r15]'s offset matches ctiMasmProbeTrampoline");

static_assert(!(PROBE_CPU_XMM0_VECTOR_OFFSET & 0xf), "Probe::State::cpu.fprs[xmm0]'s offset should be 16 byte aligned");

static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm0) == PROBE_CPU_XMM0_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm0]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm1) == PROBE_CPU_XMM1_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm1]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm2) == PROBE_CPU_XMM2_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm2]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm3) == PROBE_CPU_XMM3_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm3]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm4) == PROBE_CPU_XMM4_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm4]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm5) == PROBE_CPU_XMM5_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm5]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm6) == PROBE_CPU_XMM6_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm6]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm7) == PROBE_CPU_XMM7_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm7]'s offset matches ctiMasmProbeTrampoline");

static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm8) == PROBE_CPU_XMM8_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm8]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm9) == PROBE_CPU_XMM9_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm9]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm10) == PROBE_CPU_XMM10_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm10]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm11) == PROBE_CPU_XMM11_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm11]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm12) == PROBE_CPU_XMM12_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm12]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm13) == PROBE_CPU_XMM13_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm13]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm14) == PROBE_CPU_XMM14_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm14]'s offset matches ctiMasmProbeTrampoline");
static_assert(PROBE_OFFSETOF_REG(cpu.fprs.vectors, X86Registers::xmm15) == PROBE_CPU_XMM15_VECTOR_OFFSET, "Probe::State::cpu.fprs[xmm15]'s offset matches ctiMasmProbeTrampoline");

static_assert(sizeof(Probe::State) == PROBE_SIZE, "Probe::State::size's matches ctiMasmProbeTrampoline");

#undef PROBE_OFFSETOF

#define PROBE_XMM_SAVE_VMOVAPS \
    "vmovaps %xmm0,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM0_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm1,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM1_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm2,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM2_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm3,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM3_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm4,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM4_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm5,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM5_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm6,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM6_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm7,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM7_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm8,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM8_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm9,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM9_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm10, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM10_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm11, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM11_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm12, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM12_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm13, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM13_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm14, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM14_VECTOR_OFFSET) "(%rsp)" "\n" \
    "vmovaps %xmm15, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM15_VECTOR_OFFSET) "(%rsp)" "\n"

#define PROBE_XMM_RESTORE_VMOVAPS \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM0_VECTOR_OFFSET) "(%rbp), %xmm0" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM1_VECTOR_OFFSET) "(%rbp), %xmm1" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM2_VECTOR_OFFSET) "(%rbp), %xmm2" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM3_VECTOR_OFFSET) "(%rbp), %xmm3" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM4_VECTOR_OFFSET) "(%rbp), %xmm4" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM5_VECTOR_OFFSET) "(%rbp), %xmm5" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM6_VECTOR_OFFSET) "(%rbp), %xmm6" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM7_VECTOR_OFFSET) "(%rbp), %xmm7" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM8_VECTOR_OFFSET) "(%rbp), %xmm8" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM9_VECTOR_OFFSET) "(%rbp), %xmm9" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM10_VECTOR_OFFSET) "(%rbp), %xmm10" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM11_VECTOR_OFFSET) "(%rbp), %xmm11" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM12_VECTOR_OFFSET) "(%rbp), %xmm12" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM13_VECTOR_OFFSET) "(%rbp), %xmm13" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM14_VECTOR_OFFSET) "(%rbp), %xmm14" "\n" \
    "vmovaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM15_VECTOR_OFFSET) "(%rbp), %xmm15" "\n"

#if !OS(DARWIN)
#define PROBE_XMM_SAVE_MOVAPS \
    "movaps %xmm0,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM0_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm1,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM1_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm2,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM2_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm3,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM3_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm4,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM4_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm5,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM5_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm6,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM6_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm7,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM7_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm8,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM8_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm9,  " STRINGIZE_VALUE_OF(PROBE_CPU_XMM9_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm10, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM10_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm11, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM11_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm12, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM12_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm13, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM13_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm14, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM14_VECTOR_OFFSET) "(%rsp)" "\n" \
    "movaps %xmm15, " STRINGIZE_VALUE_OF(PROBE_CPU_XMM15_VECTOR_OFFSET) "(%rsp)" "\n"

#define PROBE_XMM_RESTORE_MOVAPS \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM0_VECTOR_OFFSET) "(%rbp), %xmm0" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM1_VECTOR_OFFSET) "(%rbp), %xmm1" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM2_VECTOR_OFFSET) "(%rbp), %xmm2" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM3_VECTOR_OFFSET) "(%rbp), %xmm3" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM4_VECTOR_OFFSET) "(%rbp), %xmm4" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM5_VECTOR_OFFSET) "(%rbp), %xmm5" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM6_VECTOR_OFFSET) "(%rbp), %xmm6" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM7_VECTOR_OFFSET) "(%rbp), %xmm7" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM8_VECTOR_OFFSET) "(%rbp), %xmm8" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM9_VECTOR_OFFSET) "(%rbp), %xmm9" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM10_VECTOR_OFFSET) "(%rbp), %xmm10" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM11_VECTOR_OFFSET) "(%rbp), %xmm11" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM12_VECTOR_OFFSET) "(%rbp), %xmm12" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM13_VECTOR_OFFSET) "(%rbp), %xmm13" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM14_VECTOR_OFFSET) "(%rbp), %xmm14" "\n" \
    "movaps " STRINGIZE_VALUE_OF(PROBE_CPU_XMM15_VECTOR_OFFSET) "(%rbp), %xmm15" "\n"
#endif // !OS(DARWIN)

#if COMPILER(MSVC)
#define ASM_PREVIOUS_SECTION
#else
#define ASM_PREVIOUS_SECTION ".previous" "\n"
#endif

#define DEFINE_PROBE_TRAMPOLINE(trampolineName, xmmSave, xmmRestore, copyLoopLabel, stateIsSafeLabel, restoreRegsLabel) \
__asm__( \
    ".text" "\n" \
    ".globl " SYMBOL_STRING(trampolineName) "\n" \
    HIDE_SYMBOL(trampolineName) "\n" \
    SYMBOL_STRING(trampolineName) ":" "\n" \
    \
    "pushq %rbp" "\n" \
    "movq  %rsp, %rbp" "\n" \
    \
    "pushfq" "\n" \
    \
    /* MacroAssemblerX86_64::probe() has already generated code to store some values. */ \
    /* Together with the rbp and rflags pushed above, the top of stack now looks like this: */ \
    /*     rbp[-1 * ptrSize]: rflags */ \
    /*     rbp[0 * ptrSize]: rbp / previousCallFrame */ \
    /*     rbp[1 * ptrSize]: return address / saved rip */ \
    /*     rbp[2 * ptrSize]: saved rbx */ \
    /*     rbp[3 * ptrSize]: saved rdx */ \
    /*     rbp[4 * ptrSize]: saved rax */ \
    /* */ \
    /* Incoming registers contain: */ \
    /*     rdx: probe function */ \
    /*     rbx: probe arg */ \
    /*     rax: scratch (was ctiMasmProbeTrampoline) */ \
    \
    "subq $" STRINGIZE_VALUE_OF(PROBE_SIZE + OUT_SIZE) ", %rsp" "\n" \
    \
    /* The X86_64 ABI specifies that the worse case stack alignment requirement is 32 bytes. */ \
    "andq $~0x1f, %rsp" "\n" \
    /* Since sp points to the Probe::State, we've ensured that it's protected from interrupts before we initialize it. */ \
    \
    "movq %rdx, " STRINGIZE_VALUE_OF(PROBE_PROBE_FUNCTION_OFFSET) "(%rsp)" "\n" \
    "movq %rbx, " STRINGIZE_VALUE_OF(PROBE_ARG_OFFSET) "(%rsp)" "\n" \
    "movq %rsi, " STRINGIZE_VALUE_OF(PROBE_CPU_ESI_OFFSET) "(%rsp)" "\n" \
    "movq %rdi, " STRINGIZE_VALUE_OF(PROBE_CPU_EDI_OFFSET) "(%rsp)" "\n" \
    \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_ECX_OFFSET) "(%rsp)" "\n" \
    \
    "movq -1 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EFLAGS_OFFSET) "(%rsp)" "\n" \
    "movq 0 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EBP_OFFSET) "(%rsp)" "\n" \
    "movq 1 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EIP_OFFSET) "(%rsp)" "\n" \
    "movq 2 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EBX_OFFSET) "(%rsp)" "\n" \
    "movq 3 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EDX_OFFSET) "(%rsp)" "\n" \
    "movq 4 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rbp), %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_EAX_OFFSET) "(%rsp)" "\n" \
    \
    "movq %rbp, %rcx" "\n" \
    "addq $" STRINGIZE_VALUE_OF(6 * PTR_SIZE) ", %rcx" "\n" \
    "movq %rcx, " STRINGIZE_VALUE_OF(PROBE_CPU_ESP_OFFSET) "(%rsp)" "\n" \
    \
    "movq %r8, " STRINGIZE_VALUE_OF(PROBE_CPU_R8_OFFSET) "(%rsp)" "\n" \
    "movq %r9, " STRINGIZE_VALUE_OF(PROBE_CPU_R9_OFFSET) "(%rsp)" "\n" \
    "movq %r10, " STRINGIZE_VALUE_OF(PROBE_CPU_R10_OFFSET) "(%rsp)" "\n" \
    "movq %r11, " STRINGIZE_VALUE_OF(PROBE_CPU_R11_OFFSET) "(%rsp)" "\n" \
    "movq %r12, " STRINGIZE_VALUE_OF(PROBE_CPU_R12_OFFSET) "(%rsp)" "\n" \
    "movq %r13, " STRINGIZE_VALUE_OF(PROBE_CPU_R13_OFFSET) "(%rsp)" "\n" \
    "movq %r14, " STRINGIZE_VALUE_OF(PROBE_CPU_R14_OFFSET) "(%rsp)" "\n" \
    "movq %r15, " STRINGIZE_VALUE_OF(PROBE_CPU_R15_OFFSET) "(%rsp)" "\n" \
    \
    xmmSave \
    \
    "movq %rsp, %rdi" "\n" /* the Probe::State* arg. */ \
    "call " SYMBOL_STRING(executeJSCJITProbe) "\n" \
    \
    /* Make sure the Probe::State is entirely below the result stack pointer so */ \
    /* that register values are still preserved when we call the initializeStack */ \
    /* function. */ \
    "movq %rsp, %rbp" "\n" \
    "movq $" STRINGIZE_VALUE_OF(PROBE_SIZE + OUT_SIZE) ", %rcx" "\n" \
    "movq %rsp, %rax" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_ESP_OFFSET) "(%rbp), %rdx" "\n" \
    "addq %rcx, %rax" "\n" \
    "cmpq %rax, %rdx" "\n" \
    "jge " LOCAL_LABEL_STRING(stateIsSafeLabel) "\n" \
    \
    /* Allocate a safe place on the stack below the result stack pointer to stash the Probe::State. */ \
    "subq %rcx, %rdx" "\n" \
    "andq $~0x1f, %rdx" "\n" /* Keep the stack pointer 32 bytes aligned. */ \
    "xorq %rax, %rax" "\n" \
    "movq %rdx, %rsp" "\n" \
    \
    "movq $" STRINGIZE_VALUE_OF(PROBE_SIZE) ", %rcx" "\n" \
    \
    /* Copy the Probe::State to the safe place. */ \
    LOCAL_LABEL_STRING(copyLoopLabel) ":" "\n" \
    "movq (%rbp, %rax), %rdx" "\n" \
    "movq %rdx, (%rsp, %rax)" "\n" \
    "addq $" STRINGIZE_VALUE_OF(PTR_SIZE) ", %rax" "\n" \
    "cmpq %rax, %rcx" "\n" \
    "jg " LOCAL_LABEL_STRING(copyLoopLabel) "\n" \
    \
    "movq %rsp, %rbp" "\n" \
    \
    /* Call initializeStackFunction if present. */ \
    LOCAL_LABEL_STRING(stateIsSafeLabel) ":" "\n" \
    "xorq %rcx, %rcx" "\n" \
    "addq " STRINGIZE_VALUE_OF(PROBE_INIT_STACK_FUNCTION_OFFSET) "(%rbp), %rcx" "\n" \
    "je " LOCAL_LABEL_STRING(restoreRegsLabel) "\n" \
    \
    "movq %rbp, %rdi" "\n" /* the Probe::State* arg. */ \
    "call *%rcx" "\n" \
    \
    LOCAL_LABEL_STRING(restoreRegsLabel) ":" "\n" \
    \
    /* To enable probes to modify register state, we copy all registers */ \
    /* out of the Probe::State before returning. */ \
    \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EDX_OFFSET) "(%rbp), %rdx" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EBX_OFFSET) "(%rbp), %rbx" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_ESI_OFFSET) "(%rbp), %rsi" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EDI_OFFSET) "(%rbp), %rdi" "\n" \
    \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R8_OFFSET) "(%rbp), %r8" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R9_OFFSET) "(%rbp), %r9" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R10_OFFSET) "(%rbp), %r10" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R11_OFFSET) "(%rbp), %r11" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R12_OFFSET) "(%rbp), %r12" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R13_OFFSET) "(%rbp), %r13" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R14_OFFSET) "(%rbp), %r14" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_R15_OFFSET) "(%rbp), %r15" "\n" \
    \
    xmmRestore \
    \
    /* There are 6 more registers left to restore: */ \
    /*     rax, rcx, rbp, rsp, rip, and rflags. */ \
    \
    /* The restoration process at ctiMasmProbeTrampolineEnd below works by popping */ \
    /* 5 words off the stack into rflags, rax, rcx, rbp, and rip. These 5 words need */ \
    /* to be pushed on top of the final esp value so that just by popping the 5 words, */ \
    /* we'll get the esp that the probe wants to set. Let's call this area (for storing */ \
    /* these 5 words) the restore area. */ \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_ESP_OFFSET) "(%rbp), %rcx" "\n" \
    "subq $5 * " STRINGIZE_VALUE_OF(PTR_SIZE) ", %rcx" "\n" \
    \
    /* rcx now points to the restore area. */ \
    \
    /* Copy remaining restore values from the Probe::State to the restore area. */ \
    /* Note: We already ensured above that the Probe::State is in a safe location before */ \
    /* calling the initializeStackFunction. The initializeStackFunction is not allowed to */ \
    /* change the stack pointer again. */ \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EFLAGS_OFFSET) "(%rbp), %rax" "\n" \
    "movq %rax, 0 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rcx)" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EAX_OFFSET) "(%rbp), %rax" "\n" \
    "movq %rax, 1 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rcx)" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_ECX_OFFSET) "(%rbp), %rax" "\n" \
    "movq %rax, 2 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rcx)" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EBP_OFFSET) "(%rbp), %rax" "\n" \
    "movq %rax, 3 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rcx)" "\n" \
    "movq " STRINGIZE_VALUE_OF(PROBE_CPU_EIP_OFFSET) "(%rbp), %rax" "\n" \
    "movq %rax, 4 * " STRINGIZE_VALUE_OF(PTR_SIZE) "(%rcx)" "\n" \
    "movq %rcx, %rsp" "\n" \
    \
    /* Do the remaining restoration by popping off the restore area. */ \
    "popfq" "\n" \
    "popq %rax" "\n" \
    "popq %rcx" "\n" \
    "popq %rbp" "\n" \
    "ret" "\n" \
    ASM_PREVIOUS_SECTION \
);

#if OS(DARWIN)
// On macOS, all x86_64 CPUs support AVX. Use vmovaps unconditionally.
DEFINE_PROBE_TRAMPOLINE(ctiMasmProbeTrampoline, PROBE_XMM_SAVE_VMOVAPS, PROBE_XMM_RESTORE_VMOVAPS,
    ctiMasmProbeTrampolineCopyLoop, ctiMasmProbeTrampolineProbeStateIsSafe, ctiMasmProbeTrampolineRestoreRegisters)
#else
// On Linux/Windows, AVX may not be available. Provide two trampolines:
// - ctiMasmProbeTrampoline: uses movaps (SSE, always available on x86_64)
// - ctiMasmProbeTrampolineAVX: uses vmovaps (requires AVX)
DEFINE_PROBE_TRAMPOLINE(ctiMasmProbeTrampoline, PROBE_XMM_SAVE_MOVAPS, PROBE_XMM_RESTORE_MOVAPS,
    ctiMasmProbeTrampolineCopyLoop, ctiMasmProbeTrampolineProbeStateIsSafe, ctiMasmProbeTrampolineRestoreRegisters)
DEFINE_PROBE_TRAMPOLINE(ctiMasmProbeTrampolineAVX, PROBE_XMM_SAVE_VMOVAPS, PROBE_XMM_RESTORE_VMOVAPS,
    ctiMasmProbeTrampolineAVXCopyLoop, ctiMasmProbeTrampolineAVXProbeStateIsSafe, ctiMasmProbeTrampolineAVXRestoreRegisters)
#endif

// What code is emitted for the probe?
// ==================================
// We want to keep the size of the emitted probe invocation code as compact as
// possible to minimize the perturbation to the JIT generated code. However,
// we also need to preserve the CPU registers and set up the Probe::State to be
// passed to the user probe function.
//
// Hence, we do only the minimum here to preserve a scratch register (i.e. rax
// in this case) and the stack pointer (i.e. rsp), and pass the probe arguments.
// We'll let the ctiMasmProbeTrampoline handle the rest of the probe invocation
// work i.e. saving the CPUState (and setting up the Probe::State), calling the
// user probe function, and restoring the CPUState before returning to JIT
// generated code.
//
// What registers need to be saved?
// ===============================
// The registers are saved for 2 reasons:
// 1. To preserve their state in the JITted code. This means that all registers
//    that are not callee saved needs to be saved. We also need to save the
//    condition code registers because the probe can be inserted between a test
//    and a branch.
// 2. To allow the probe to inspect the values of the registers for debugging
//    purposes. This means all registers need to be saved.
//
// In summary, save everything. But for reasons stated above, we should do the
// minimum here and let ctiMasmProbeTrampoline do the heavy lifting to save the
// full set.
//
// What values are in the saved registers?
// ======================================
// Conceptually, the saved registers should contain values as if the probe
// is not present in the JIT generated code. Hence, they should contain values
// that are expected at the start of the instruction immediately following the
// probe.
//
// Specifically, the saved stack pointer register will point to the stack
// position before we push the Probe::State frame. The saved rip will point to
// the address of the instruction immediately following the probe.

void MacroAssembler::probe(Probe::Function function, void* arg)
{
    // Extra push so that the total number of pushes pad out to 32-bytes, and the
    // stack pointer remains 32 byte aligned as required by the ABI.
    push(RegisterID::eax);
    push(RegisterID::eax);

#if OS(DARWIN)
    move(TrustedImmPtr(reinterpret_cast<void*>(ctiMasmProbeTrampoline)), RegisterID::eax);
#else
    if (supportsAVX())
        move(TrustedImmPtr(reinterpret_cast<void*>(ctiMasmProbeTrampolineAVX)), RegisterID::eax);
    else
        move(TrustedImmPtr(reinterpret_cast<void*>(ctiMasmProbeTrampoline)), RegisterID::eax);
#endif

    push(RegisterID::edx);
    move(TrustedImmPtr(reinterpret_cast<void*>(function)), RegisterID::edx);
    push(RegisterID::ebx);
    move(TrustedImmPtr(arg), RegisterID::ebx);
    call(RegisterID::eax, CFunctionPtrTag);
}

MacroAssemblerX86_64::CPUID MacroAssemblerX86_64::getCPUID(unsigned level)
{
    return getCPUIDEx(level, 0);
}

MacroAssemblerX86_64::CPUID MacroAssemblerX86_64::getCPUIDEx(unsigned level, unsigned count)
{
    CPUID result { };
    __asm__ (
        "cpuid\n"
        : "=a"(result[0]), "=b"(result[1]), "=c"(result[2]), "=d"(result[3])
        : "0"(level), "2"(count)
    );
    return result;
}

#if !OS(DARWIN)
static inline uint64_t readXCR0()
{
    uint32_t lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}
#endif

CPUFeatureSet MacroAssemblerX86_64::collectCPUFeatures()
{
    CPUFeatureSet features;
    auto add = [&](CPUFeature feature, bool isPresent) {
        if (isPresent)
            features = features.with(feature);
    };

#if !OS(DARWIN)
    bool hasAVX = false;
#endif
    {
        CPUID cpuid = getCPUID(0x1);
        add(CPUFeature::SSE3, cpuid[2] & (1 << 0));
        add(CPUFeature::SupplementalSSE3, cpuid[2] & (1 << 9));
        add(CPUFeature::SSE4_1, cpuid[2] & (1 << 19));
        add(CPUFeature::SSE4_2, cpuid[2] & (1 << 20));
        add(CPUFeature::Popcnt, cpuid[2] & (1 << 23));
#if OS(DARWIN)
        add(CPUFeature::AVX, true);
#else
        // Per the Intel SDM, AVX is usable only when:
        //   1. CPUID.1:ECX.AVX[bit 28] = 1      (CPU supports AVX)
        //   2. CPUID.1:ECX.OSXSAVE[bit 27] = 1  (OS has enabled XGETBV)
        //   3. XCR0[2:1] = 11b                  (OS saves XMM + YMM state)
        // Checking only bit 28 is insufficient: under many hypervisors
        // (Xen, KVM, Hyper-V, etc.) the AVX feature bit can be passed
        // through from the host CPU while XSAVE/YMM state is not enabled
        // in the guest, causing every VEX-encoded instruction to #UD.
        if ((cpuid[2] & (1 << 28)) && (cpuid[2] & (1 << 27))) {
            constexpr uint64_t ymmStateMask = 0x6; // XCR0.SSE | XCR0.AVX
            hasAVX = (readXCR0() & ymmStateMask) == ymmStateMask;
        }
        add(CPUFeature::AVX, hasAVX);
#endif
    }
#if OS(DARWIN)
    {
        uint32_t val = 0;
        size_t valSize = sizeof(val);
        int rc = sysctlbyname("hw.optional.bmi1", &val, &valSize, nullptr, 0);
        add(CPUFeature::BMI1, rc >= 0 && val);

        rc = sysctlbyname("hw.optional.avx2_0", &val, &valSize, nullptr, 0);
        add(CPUFeature::AVX2, rc >= 0 && val);
    }
#else
    {
        // CPUID leaf 7 sub-leaf 0: BMI1 = EBX[3], AVX2 = EBX[5].
        CPUID cpuid = getCPUID(0x7);
        add(CPUFeature::BMI1, cpuid[1] & (1 << 3));
        // AVX2 is VEX-encoded and therefore also requires the OS to have
        // enabled YMM state; gate it on the AVX result computed above.
        bool hasAVX2Feature = cpuid[1] & (1 << 5);
        add(CPUFeature::AVX2, hasAVX2Feature && hasAVX);
    }
#endif
    {
        CPUID cpuid = getCPUID(0x80000001);
        add(CPUFeature::LZCNT, cpuid[2] & (1 << 5));
    }
    return features;
}

void MacroAssemblerX86_64::emitVectorOperationWithoutAVX(VectorOperationWithoutAVX operation, SIMDLane lane, FPRegisterID left, FPRegisterID right, FPRegisterID dest)
{
    // Of two numbers that are both NaN the left one comes out, so only operations on integers and on bits may change sides.
    bool mayChangeSides = operation != VectorOperationWithoutAVX::Sub && operation != VectorOperationWithoutAVX::Div && operation != VectorOperationWithoutAVX::NarrowSigned
        && operation != VectorOperationWithoutAVX::NarrowUnsigned && operation != VectorOperationWithoutAVX::Swizzle && !scalarTypeIsFloatingPoint(lane);
    if (dest == right && mayChangeSides)
        std::swap(left, right);
    if (dest == right && dest != left) {
        moveVector(right, fpTempRegister);
        right = fpTempRegister;
    }
    if (dest != left)
        moveVector(left, dest);

    // The last byte of the instruction, and above it: 1 = behind the prefix 66, 2 = behind the escape 38. Zero: there is no such instruction.
    constexpr uint16_t with66 = 0x100;
    constexpr uint16_t with6638 = 0x300;
    // In the order of SIMDLane: i8x16, i16x8, i32x4, i64x2, f32x4, f64x2.
    static constexpr uint16_t arithmetic[10][6] = {
        { with66 | 0xfc, with66 | 0xfd, with66 | 0xfe, with66 | 0xd4, 0x58, with66 | 0x58 }, // paddb paddw paddd paddq addps addpd
        { with66 | 0xf8, with66 | 0xf9, with66 | 0xfa, with66 | 0xfb, 0x5c, with66 | 0x5c }, // psubb psubw psubd psubq subps subpd
        { 0, with66 | 0xd5, with6638 | 0x40, 0, 0x59, with66 | 0x59 }, // pmullw pmulld mulps mulpd
        { 0, 0, 0, 0, 0x5e, with66 | 0x5e }, // divps divpd
        { with6638 | 0x3c, with66 | 0xee, with6638 | 0x3d, 0, 0, 0 }, // pmaxsb pmaxsw pmaxsd
        { with6638 | 0x38, with66 | 0xea, with6638 | 0x39, 0, 0, 0 }, // pminsb pminsw pminsd
        { with66 | 0xde, with6638 | 0x3e, with6638 | 0x3f, 0, 0, 0 }, // pmaxub pmaxuw pmaxud
        { with66 | 0xda, with6638 | 0x3a, with6638 | 0x3b, 0, 0, 0 }, // pminub pminuw pminud
        { 0, with66 | 0x63, with66 | 0x6b, 0, 0, 0 }, // packsswb packssdw
        { 0, with66 | 0x67, with6638 | 0x2b, 0, 0, 0 }, // packuswb packusdw
    };
    static constexpr uint16_t onBits[4] = { 0x54, 0x56, 0x57, with6638 | 0x00 }; // andps orps xorps pshufb
    static_assert(static_cast<unsigned>(SIMDLane::i8x16) + 5 == static_cast<unsigned>(SIMDLane::f64x2));
    static_assert(static_cast<unsigned>(VectorOperationWithoutAVX::And) == 10 && static_cast<unsigned>(VectorOperationWithoutAVX::Swizzle) == 13);
    uint16_t instruction = 0;
    if (operation >= VectorOperationWithoutAVX::And)
        instruction = onBits[static_cast<unsigned>(operation) - 10];
    else {
        RELEASE_ASSERT(lane >= SIMDLane::i8x16 && lane <= SIMDLane::f64x2);
        instruction = arithmetic[static_cast<unsigned>(operation)][static_cast<unsigned>(lane) - static_cast<unsigned>(SIMDLane::i8x16)];
    }
    RELEASE_ASSERT(instruction);
    m_assembler.vectorOp_rr(instruction & 0x100, instruction & 0x200, static_cast<uint8_t>(instruction), right, dest);
}

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && CPU(X86_64)
