/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#include "JSExportMacros.h"
#include <cstdint>
#include <span>

// Native fixture functions for the bun:ffi test suite (SPEC section 11.1).
//
// These are plain extern "C" functions compiled into the JavaScriptCore
// library itself (this .cpp is listed in Sources.txt) so that both the
// testFFI executable and $vm.ffiFixture(name) in tools/JSDollarVM.cpp can
// resolve them. Every name below is a contract with the JS stress tests
// (JSTests/stress/ffi-*.js) and with testFFI.cpp; do not rename.

// The engine treats jsvalue (historically napi_value) as a raw EncodedJSValue
// pass-through, so the fixtures only need pointer/int64 stand-ins.
typedef void* napi_env;
typedef int64_t napi_value;

extern "C" {

// ---------------------------------------------------------------------------
// Echo family: ffi_echo_<t>(x) returns x unchanged, one per non-void,
// non-napi FFI type. FFI Type::Char is a SIGNED 8-bit integer on every target
// (SPEC section 2), so the char fixtures are declared with `signed char`, not
// the platform-dependent plain `char`, to lock that rule down.
// ---------------------------------------------------------------------------
JS_EXPORT_PRIVATE signed char ffi_echo_char(signed char);
JS_EXPORT_PRIVATE int8_t ffi_echo_i8(int8_t);
JS_EXPORT_PRIVATE uint8_t ffi_echo_u8(uint8_t);
JS_EXPORT_PRIVATE int16_t ffi_echo_i16(int16_t);
JS_EXPORT_PRIVATE uint16_t ffi_echo_u16(uint16_t);
JS_EXPORT_PRIVATE int32_t ffi_echo_i32(int32_t);
JS_EXPORT_PRIVATE uint32_t ffi_echo_u32(uint32_t);
JS_EXPORT_PRIVATE int64_t ffi_echo_i64(int64_t);
JS_EXPORT_PRIVATE uint64_t ffi_echo_u64(uint64_t);
JS_EXPORT_PRIVATE float ffi_echo_f32(float);
JS_EXPORT_PRIVATE double ffi_echo_f64(double);
JS_EXPORT_PRIVATE bool ffi_echo_bool(bool);
JS_EXPORT_PRIVATE void* ffi_echo_ptr(void*);
JS_EXPORT_PRIVATE const char* ffi_echo_cstring(const char*);
JS_EXPORT_PRIVATE napi_value ffi_echo_napi_value(napi_value);
JS_EXPORT_PRIVATE void* ffi_recv_napi_env(napi_env);

// ---------------------------------------------------------------------------
// Sub-word signedness probes (missing caller-side extension) and return
// normalization probes (missing callee-side extension). ffi_widen_char is the
// char-signedness lock (SPEC section 2); ffi_ret_two_as_bool returns int8 2
// but is called through a signature declaring the return `bool`, which must
// normalize to `true` (SPEC sections 4 and 7.2).
// ---------------------------------------------------------------------------
JS_EXPORT_PRIVATE int64_t ffi_widen_char(signed char);
JS_EXPORT_PRIVATE int64_t ffi_widen_i8(int8_t);
JS_EXPORT_PRIVATE int64_t ffi_widen_u8(uint8_t);
JS_EXPORT_PRIVATE int64_t ffi_widen_i16(int16_t);
JS_EXPORT_PRIVATE int64_t ffi_widen_u16(uint16_t);
JS_EXPORT_PRIVATE void* ffi_ret_null_ptr(void);
JS_EXPORT_PRIVATE int8_t ffi_ret_two_as_bool(void);
JS_EXPORT_PRIVATE int8_t ffi_ret_neg_one_i8(void);
JS_EXPORT_PRIVATE int16_t ffi_ret_neg_one_i16(void);
JS_EXPORT_PRIVATE int32_t ffi_ret_neg_one_i32(void);
JS_EXPORT_PRIVATE int64_t ffi_ret_neg_one_i64(void);
JS_EXPORT_PRIVATE uint8_t ffi_ret_neg_one_u8(void);
JS_EXPORT_PRIVATE uint16_t ffi_ret_neg_one_u16(void);
JS_EXPORT_PRIVATE uint32_t ffi_ret_neg_one_u32(void);
JS_EXPORT_PRIVATE uint64_t ffi_ret_neg_one_u64(void);

// Floating-point edge probes.
// FFI-SPEC-GAP: ffi_ret_impure_nan_f64 is an addition to the SPEC's list so
// the f64 return path (not just f32) is proven to purifyNaN a non-canonical
// payload before boxing.
JS_EXPORT_PRIVATE float ffi_ret_nan_f32(void); // NaN with a non-canonical payload; must be purified before boxing.
JS_EXPORT_PRIVATE double ffi_ret_impure_nan_f64(void); // 0x7ff0000000000001; must be purified before boxing.
JS_EXPORT_PRIVATE double ffi_ret_neg_zero_f64(void);
JS_EXPORT_PRIVATE float ffi_ret_denormal_f32(void); // smallest positive denormal (bits 0x00000001).
JS_EXPORT_PRIVATE double ffi_ret_inf_f64(void);

// Two-argument adders.
JS_EXPORT_PRIVATE int32_t ffi_add_i32(int32_t, int32_t);
JS_EXPORT_PRIVATE double ffi_add_f64(double, double);
JS_EXPORT_PRIVATE int64_t ffi_add_i64(int64_t, int64_t);
JS_EXPORT_PRIVATE uint64_t ffi_add_u64(uint64_t, uint64_t);
JS_EXPORT_PRIVATE float ffi_add_f32(float, float);

// ---------------------------------------------------------------------------
// Arity ladders straddling every register->stack boundary of the supported
// ABIs (SysV64: 6 GPR / 8 FPR; AAPCS64: 8 GPR / 8 FPR; Win64: 4 positional).
// ffi_sum_i32_<n> returns the int64 sum of its n int32 arguments;
// ffi_sum_f64_<n> the double sum of its n double arguments; the sub-8-byte
// ladders return int64 sums of their zero/sign-extended arguments and exist
// to exercise Apple arm64 stack packing (SPEC section 7.1.1).
// FFI-SPEC-GAP: the spec does not give a return type for ffi_sum_u8_<n> /
// ffi_sum_i16_<n>; int64_t (a BigInt in JS) is used so the sum is exact.
// ---------------------------------------------------------------------------
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_0(void);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_1(int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_2(int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_4(int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_6(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_7(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_8(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_9(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_12(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_16(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE double ffi_sum_f64_1(double);
JS_EXPORT_PRIVATE double ffi_sum_f64_2(double, double);
JS_EXPORT_PRIVATE double ffi_sum_f64_7(double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE double ffi_sum_f64_8(double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE double ffi_sum_f64_9(double, double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE double ffi_sum_f64_12(double, double, double, double, double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE int64_t ffi_sum_u8_10(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_u8_12(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i16_10(int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i16_12(int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t);

// ---------------------------------------------------------------------------
// Interleaved mixes. Each returns the double checksum
//     checksum = sum over k (0-based argument index) of (k + 1) * cast(arg_k)
// where cast() is: integers -> their exact double value (uint8_t/int16_t
// after C conversion), float -> (double)f, bool -> b ? 1 : 0, and
// void* -> (double)(uintptr_t)p. The position weight (k + 1) makes any
// argument-order or register/stack-slot swap change the checksum.
//   ffi_mix_1(int32_t, double, int64_t, float, void*, uint8_t, double, int16_t, double, int32_t)
//   ffi_mix_2(float, int32_t) x5           - independent int/fp counters vs Win64 positional slots
//   ffi_mix_3(double x8, int32_t)          - int arriving after all FPRs are consumed
//   ffi_mix_4(int64_t x6, double, int64_t, double) - GPR overflow interleaved with FPRs
//   ffi_mix_5(uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, uint64_t, int64_t) - every integer width
//   ffi_mix_6(bool, bool, int32_t, bool, double, bool, float, bool x6) - bool ladders onto the stack
//   ffi_mix_7(void*, signed char) x5      - pointer/char alternation
//   ffi_mix_8(float, double) x6          - f32/f64 alternation past the FPR limit (f32 stack args)
// ---------------------------------------------------------------------------
JS_EXPORT_PRIVATE double ffi_mix_1(int32_t, double, int64_t, float, void*, uint8_t, double, int16_t, double, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_2(float, int32_t, float, int32_t, float, int32_t, float, int32_t, float, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_3(double, double, double, double, double, double, double, double, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_4(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, int64_t, double);
JS_EXPORT_PRIVATE double ffi_mix_5(uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, uint64_t, int64_t);
JS_EXPORT_PRIVATE double ffi_mix_6(bool, bool, int32_t, bool, double, bool, float, bool, bool, bool, bool, bool, bool);
JS_EXPORT_PRIVATE double ffi_mix_7(void*, signed char, void*, signed char, void*, signed char, void*, signed char, void*, signed char);
JS_EXPORT_PRIVATE double ffi_mix_8(float, double, float, double, float, double, float, double, float, double, float, double);

// Pointer probes.
JS_EXPORT_PRIVATE void ffi_ptr_write_u32(uint32_t*, uint32_t);
JS_EXPORT_PRIVATE uint32_t ffi_ptr_read_u32(uint32_t*);
JS_EXPORT_PRIVATE uint64_t ffi_strlen(const char*);
JS_EXPORT_PRIVATE void* ffi_ptr_identity(void*);
JS_EXPORT_PRIVATE void* ffi_high_ptr(void); // returns (void*)0x00007fffdeadbee0
// Perf-gate fixtures mirroring Bun's bench/ffi native library.
JS_EXPORT_PRIVATE void ffi_bench_noop(void);
JS_EXPORT_PRIVATE const char* ffi_bench_string(void);
JS_EXPORT_PRIVATE uint32_t ffi_bench_hash(const uint8_t* ptr, uint32_t length);

// ---------------------------------------------------------------------------
// Callback fixtures (native -> JS direction).
// FFI-SPEC-GAP: the x9/x10/ret_*/then_read entries below go beyond the SPEC's
// required list; they exist so the callback thunk's incoming-stack loads
// (integer and floating stack arguments, sub-8-byte packed stack arguments
// on Darwin arm64), its per-type return normalization, and "GC inside a
// callback while a pointer argument is outstanding" are each covered by a
// dedicated fixture.
// ---------------------------------------------------------------------------
JS_EXPORT_PRIVATE int32_t ffi_call_cb_i32(int32_t (*cb)(int32_t), int32_t x);
JS_EXPORT_PRIVATE double ffi_call_cb_f64_x8(double (*cb)(double, double, double, double, double, double, double, double), double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE double ffi_call_cb_mix(double (*cb)(int32_t, double, int64_t, float, void*), int32_t, double, int64_t, float, void*);
JS_EXPORT_PRIVATE void ffi_call_cb_void(void (*cb)(void));
JS_EXPORT_PRIVATE int32_t ffi_call_cb_reentrant(int32_t (*cb)(int32_t), int32_t depth); // returns sum of cb(i) for i in [0, depth)
JS_EXPORT_PRIVATE int64_t ffi_call_cb_i32_x9(int64_t (*cb)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t), int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE double ffi_call_cb_f64_x9(double (*cb)(double, double, double, double, double, double, double, double, double), double, double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE int64_t ffi_call_cb_u8_x10(int64_t (*cb)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t), uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
// Threadsafe-callback fixtures: spawn a FOREIGN OS thread that invokes cb with the given values,
// join it, and return. A threadsafe callback must not run inline (it is queued for the JS thread).
JS_EXPORT_PRIVATE void ffi_call_cb_from_thread(void (*cb)(int32_t, int64_t, uint64_t, double), int32_t a, int64_t b, uint64_t c, double d);
JS_EXPORT_PRIVATE int64_t ffi_call_cb_ret_i8(int8_t (*cb)(void)); // returns (int64_t) of the callback result
JS_EXPORT_PRIVATE int64_t ffi_call_cb_ret_u8(uint8_t (*cb)(void));
JS_EXPORT_PRIVATE int64_t ffi_call_cb_ret_i64(int64_t (*cb)(void));
JS_EXPORT_PRIVATE uint64_t ffi_call_cb_ret_u64(uint64_t (*cb)(void));
JS_EXPORT_PRIVATE int32_t ffi_call_cb_ret_bool(bool (*cb)(void)); // returns 10 if the callback returned true, else 20
JS_EXPORT_PRIVATE float ffi_call_cb_ret_f32(float (*cb)(void));
JS_EXPORT_PRIVATE double ffi_call_cb_ret_f64(double (*cb)(void));
JS_EXPORT_PRIVATE void* ffi_call_cb_ret_ptr(void* (*cb)(void));
JS_EXPORT_PRIVATE uint32_t ffi_call_cb_then_read_u32(uint32_t (*cb)(void), uint32_t* p); // calls cb() (which may GC), then returns *p

// Callee-saved register canary: loads sentinels into the host ABI's
// callee-saved GPR/FPR set, calls cb, and returns 0 or a bitmask of registers
// the callee failed to preserve. x86-64: bit0 rbx, bit1 r12, bit2 r13, bit3
// r14, bit4 r15 (+ on Windows: bit5 rsi, bit6 rdi, bits 7-16 xmm6-xmm15).
// arm64: bits 0-9 x19-x28, bits 10-17 d8-d15.
JS_EXPORT_PRIVATE int32_t ffi_canary_call(void (*cb)(void));

// Stack-alignment probes: perform an alignment-checked 16-byte access (movaps
// on a 16-byte-aligned local on x86-64; sp-based q-register accesses under
// the hardware SP-alignment check on arm64), so a misaligned incoming stack
// faults, and return 1.0.
// FFI-SPEC-GAP: the spec fixes only the argument counts (0 and 9); nine
// int32 arguments are used so every ABI passes at least one on the stack.
JS_EXPORT_PRIVATE double ffi_align_probe_0(void);
JS_EXPORT_PRIVATE double ffi_align_probe_9(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);

} // extern "C"

struct FFIFixtureEntry {
    const char* name;
    void* address;
};

// The complete fixture table (used by $vm.ffiFixture(name) / $vm.ffiFixtures()).
JS_EXPORT_PRIVATE std::span<const FFIFixtureEntry> ffiTestFixtures();

// Convenience lookup over ffiTestFixtures(); returns nullptr for an unknown name.
JS_EXPORT_PRIVATE const FFIFixtureEntry* ffiTestFixtureNamed(const char* name);

#endif // USE(BUN_JSC_ADDITIONS)
