/*
 * Copyright (C) 2012-2025 Apple Inc. All rights reserved.
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

#include "InPlaceInterpreter.h"
#include "LLIntCommon.h"
#include "StructureID.h"
#include <wtf/Assertions.h>
#include <wtf/FastMalloc.h>

#if ENABLE(C_LOOP)
#define OFFLINE_ASM_C_LOOP 1
#define OFFLINE_ASM_ARMv7 0
#define OFFLINE_ASM_ARM64 0
#define OFFLINE_ASM_ARM64E 0
#define OFFLINE_ASM_X86_64 0
#define OFFLINE_ASM_ARMv7k 0
#define OFFLINE_ASM_ARMv7s 0
#define OFFLINE_ASM_RISCV64 0

#else // ENABLE(C_LOOP)

#define OFFLINE_ASM_C_LOOP 0

#ifdef __ARM_ARCH_7K__
#define OFFLINE_ASM_ARMv7k 1
#else
#define OFFLINE_ASM_ARMv7k 0
#endif

#ifdef __ARM_ARCH_7S__
#define OFFLINE_ASM_ARMv7s 1
#else
#define OFFLINE_ASM_ARMv7s 0
#endif

#if CPU(ARM_THUMB2)
#define OFFLINE_ASM_ARMv7 1
#else
#define OFFLINE_ASM_ARMv7 0
#endif

#if CPU(X86_64)
#define OFFLINE_ASM_X86_64 1
#else
#define OFFLINE_ASM_X86_64 0
#endif

#if CPU(ARM64)
#define OFFLINE_ASM_ARM64 1
#else
#define OFFLINE_ASM_ARM64 0
#endif

#if CPU(ARM64E)
#define OFFLINE_ASM_ARM64E 1
#undef OFFLINE_ASM_ARM64
#define OFFLINE_ASM_ARM64 0 // Pretend that ARM64 and ARM64E are mutually exclusive to please the offlineasm.
#else
#define OFFLINE_ASM_ARM64E 0
#endif

#if CPU(RISCV64)
#define OFFLINE_ASM_RISCV64 1
#else
#define OFFLINE_ASM_RISCV64 0
#endif

#endif // ENABLE(C_LOOP)

#if USE(JSVALUE64)
#define OFFLINE_ASM_JSVALUE64 1
#else
#define OFFLINE_ASM_JSVALUE64 0
#endif

#if USE(BIGINT32)
#define OFFLINE_ASM_BIGINT32 1
#else
#define OFFLINE_ASM_BIGINT32 0
#endif

#if USE(LARGE_TYPED_ARRAYS)
#define OFFLINE_ASM_LARGE_TYPED_ARRAYS 1
#else
#define OFFLINE_ASM_LARGE_TYPED_ARRAYS 0
#endif

#if CPU(ADDRESS64)
#define OFFLINE_ASM_ADDRESS64 1
#else
#define OFFLINE_ASM_ADDRESS64 0
#endif

#if ASSERT_ENABLED
#define OFFLINE_ASM_ASSERT_ENABLED 1
#else
#define OFFLINE_ASM_ASSERT_ENABLED 0
#endif

#if LLINT_TRACING
#define OFFLINE_ASM_TRACING 1
#else
#define OFFLINE_ASM_TRACING 0
#endif

#define OFFLINE_ASM_GIGACAGE_ENABLED GIGACAGE_ENABLED

#if ENABLE(JIT)
#define OFFLINE_ASM_JIT 1
#else
#define OFFLINE_ASM_JIT 0
#endif

#if ENABLE(JIT_CAGE)
#define OFFLINE_ASM_JIT_CAGE 1
#else
#define OFFLINE_ASM_JIT_CAGE 0
#endif

#if ENABLE(WEBASSEMBLY)
#define OFFLINE_ASM_WEBASSEMBLY 1
#else
#define OFFLINE_ASM_WEBASSEMBLY 0
#endif

#if ENABLE(WEBASSEMBLY_OMGJIT)
#define OFFLINE_ASM_WEBASSEMBLY_OMGJIT 1
#else
#define OFFLINE_ASM_WEBASSEMBLY_OMGJIT 0
#endif

#if ENABLE(WEBASSEMBLY_BBQJIT)
#define OFFLINE_ASM_WEBASSEMBLY_BBQJIT 1
#else
#define OFFLINE_ASM_WEBASSEMBLY_BBQJIT 0
#endif

#if HAVE(FAST_TLS)
#define OFFLINE_ASM_HAVE_FAST_TLS 1
#else
#define OFFLINE_ASM_HAVE_FAST_TLS 0
#endif

// SPEC-jit App. R5 (Task 8): ELF initial-exec TLS relocations for the
// per-thread butterfly TID tag (g_jscButterflyTIDTag) are emitted only on
// Linux; elsewhere (Darwin until the M4a JSCConfig key slot lands, Windows,
// C_LOOP) the LLInt threaded WRITE fast paths fall back to the slow path.
#if OS(LINUX)
#define OFFLINE_ASM_LINUX 1
#else
#define OFFLINE_ASM_LINUX 0
#endif

// UNGIL §A.1.3 / AB-1 (obligation 9b, U-T3): the LLInt Group-3
// storage-selection branches need an LLInt-visible TLS read of
// g_jscCurrentVMLite. Same per-OS surface as SPEC-jit App. R5
// (loadButterflyTIDTagToT6 / jit/AssemblyHelpers.cpp loadVMLite): ELF
// initial-exec relocations on Linux x86-64/arm64. The C_LOOP backend IS
// C++, so its "TLS loader" is a cloopDo call to VMLite::currentIfExists()
// — the same thread_local the IE-model symbol mirrors (VM.cpp CS3
// contract: VMLite::setCurrent is the sole writer, mirror store
// immediately after) — so JSVALUE64 CLoop qualifies on any OS. Elsewhere
// (Darwin hardware LLInt until the vmLiteTLSKey M4a slot lands, Windows
// hardware LLInt, 32-bit cloop) a SET gilOffProcess byte fail-stops in
// the LLInt (`break`) instead of silently reading VM-block Group-3 state
// — the in-LLInt tripwire AB-1 records as absent (mirrors the JIT tiers'
// Darwin RELEASE_ASSERT, AB-2).
#if (OS(LINUX) && (CPU(X86_64) || CPU(ARM64)) && !ENABLE(C_LOOP) && USE(JSVALUE64)) \
    || (ENABLE(C_LOOP) && USE(JSVALUE64))
#define OFFLINE_ASM_GILOFF_TLS 1
#else
#define OFFLINE_ASM_GILOFF_TLS 0
#endif
