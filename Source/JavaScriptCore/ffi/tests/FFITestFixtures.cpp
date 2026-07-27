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
#include "FFITestFixtures.h"

#if USE(BUN_JSC_ADDITIONS)

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <wtf/Threading.h>
#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

extern "C" {

signed char ffi_echo_char(signed char x) { return x; }
int8_t ffi_echo_i8(int8_t x) { return x; }
uint8_t ffi_echo_u8(uint8_t x) { return x; }
int16_t ffi_echo_i16(int16_t x) { return x; }
uint16_t ffi_echo_u16(uint16_t x) { return x; }
int32_t ffi_echo_i32(int32_t x) { return x; }
uint32_t ffi_echo_u32(uint32_t x) { return x; }
int64_t ffi_echo_i64(int64_t x) { return x; }
uint64_t ffi_echo_u64(uint64_t x) { return x; }
float ffi_echo_f32(float x) { return x; }
double ffi_echo_f64(double x) { return x; }
bool ffi_echo_bool(bool x) { return x; }
void* ffi_echo_ptr(void* x) { return x; }
const char* ffi_echo_cstring(const char* x) { return x; }
jsvalue_slot ffi_echo_jsvalue(jsvalue_slot x) { return x; }

int64_t ffi_widen_char(signed char x) { return x; }
int64_t ffi_widen_i8(int8_t x) { return x; }
int64_t ffi_widen_u8(uint8_t x) { return x; }
int64_t ffi_widen_i16(int16_t x) { return x; }
int64_t ffi_widen_u16(uint16_t x) { return x; }
void* ffi_ret_null_ptr(void) { return nullptr; }
int8_t ffi_ret_two_as_bool(void) { return 2; }
int8_t ffi_ret_neg_one_i8(void) { return -1; }
int16_t ffi_ret_neg_one_i16(void) { return -1; }
int32_t ffi_ret_neg_one_i32(void) { return -1; }
int64_t ffi_ret_neg_one_i64(void) { return -1; }
uint8_t ffi_ret_neg_one_u8(void) { return static_cast<uint8_t>(-1); }
uint16_t ffi_ret_neg_one_u16(void) { return static_cast<uint16_t>(-1); }
uint32_t ffi_ret_neg_one_u32(void) { return static_cast<uint32_t>(-1); }
uint64_t ffi_ret_neg_one_u64(void) { return static_cast<uint64_t>(-1); }

float ffi_ret_nan_f32(void) { return std::bit_cast<float>(0x7fc00001u); }
double ffi_ret_impure_nan_f64(void) { return std::bit_cast<double>(0x7ff0000000000001ull); }
double ffi_ret_neg_zero_f64(void) { return -0.0; }
float ffi_ret_denormal_f32(void) { return std::bit_cast<float>(0x00000001u); }
double ffi_ret_inf_f64(void) { return std::bit_cast<double>(0x7ff0000000000000ull); }

int32_t ffi_add_i32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
double ffi_add_f64(double a, double b) { return a + b; }
int64_t ffi_add_i64(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); }
uint64_t ffi_add_u64(uint64_t a, uint64_t b) { return a + b; }
float ffi_add_f32(float a, float b) { return a + b; }

int64_t ffi_sum_i32_0(void) { return 0; }
int64_t ffi_sum_i32_1(int32_t a0) { return int64_t(a0); }
int64_t ffi_sum_i32_2(int32_t a0, int32_t a1) { return int64_t(a0) + int64_t(a1); }
int64_t ffi_sum_i32_4(int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3);
}
int64_t ffi_sum_i32_6(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5);
}
int64_t ffi_sum_i32_7(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6);
}
int64_t ffi_sum_i32_8(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7);
}
int64_t ffi_sum_i32_9(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8);
}
int64_t ffi_sum_i32_12(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8, int32_t a9, int32_t a10, int32_t a11)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7)
        + int64_t(a8) + int64_t(a9) + int64_t(a10) + int64_t(a11);
}
int64_t ffi_sum_i32_16(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8, int32_t a9, int32_t a10, int32_t a11, int32_t a12, int32_t a13, int32_t a14, int32_t a15)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7)
        + int64_t(a8) + int64_t(a9) + int64_t(a10) + int64_t(a11) + int64_t(a12) + int64_t(a13) + int64_t(a14) + int64_t(a15);
}

double ffi_sum_f64_1(double a0) { return a0; }
double ffi_sum_f64_2(double a0, double a1) { return a0 + a1; }
double ffi_sum_f64_7(double a0, double a1, double a2, double a3, double a4, double a5, double a6)
{
    return a0 + a1 + a2 + a3 + a4 + a5 + a6;
}
double ffi_sum_f64_8(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7)
{
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
double ffi_sum_f64_9(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8)
{
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8;
}
double ffi_sum_f64_12(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8, double a9, double a10, double a11)
{
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11;
}

int64_t ffi_sum_u8_10(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7, uint8_t a8, uint8_t a9)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8) + int64_t(a9);
}
int64_t ffi_sum_u8_12(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7, uint8_t a8, uint8_t a9, uint8_t a10, uint8_t a11)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8) + int64_t(a9)
        + int64_t(a10) + int64_t(a11);
}
int64_t ffi_sum_i16_10(int16_t a0, int16_t a1, int16_t a2, int16_t a3, int16_t a4, int16_t a5, int16_t a6, int16_t a7, int16_t a8, int16_t a9)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8) + int64_t(a9);
}
int64_t ffi_sum_i16_12(int16_t a0, int16_t a1, int16_t a2, int16_t a3, int16_t a4, int16_t a5, int16_t a6, int16_t a7, int16_t a8, int16_t a9, int16_t a10, int16_t a11)
{
    return int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8) + int64_t(a9)
        + int64_t(a10) + int64_t(a11);
}

static inline double ffiPtrToDouble(void* p)
{
    return static_cast<double>(reinterpret_cast<uintptr_t>(p));
}

double ffi_mix_1(int32_t a0, double a1, int64_t a2, float a3, void* a4, uint8_t a5, double a6, int16_t a7, double a8, int32_t a9)
{
    return 1.0 * static_cast<double>(a0)
        + 2.0 * a1
        + 3.0 * static_cast<double>(a2)
        + 4.0 * static_cast<double>(a3)
        + 5.0 * ffiPtrToDouble(a4)
        + 6.0 * static_cast<double>(a5)
        + 7.0 * a6
        + 8.0 * static_cast<double>(a7)
        + 9.0 * a8
        + 10.0 * static_cast<double>(a9);
}

double ffi_mix_2(float a0, int32_t a1, float a2, int32_t a3, float a4, int32_t a5, float a6, int32_t a7, float a8, int32_t a9)
{
    return 1.0 * static_cast<double>(a0)
        + 2.0 * static_cast<double>(a1)
        + 3.0 * static_cast<double>(a2)
        + 4.0 * static_cast<double>(a3)
        + 5.0 * static_cast<double>(a4)
        + 6.0 * static_cast<double>(a5)
        + 7.0 * static_cast<double>(a6)
        + 8.0 * static_cast<double>(a7)
        + 9.0 * static_cast<double>(a8)
        + 10.0 * static_cast<double>(a9);
}

double ffi_mix_3(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, int32_t a8)
{
    return 1.0 * a0 + 2.0 * a1 + 3.0 * a2 + 4.0 * a3 + 5.0 * a4 + 6.0 * a5 + 7.0 * a6 + 8.0 * a7 + 9.0 * static_cast<double>(a8);
}

double ffi_mix_4(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, double a6, int64_t a7, double a8)
{
    return 1.0 * static_cast<double>(a0)
        + 2.0 * static_cast<double>(a1)
        + 3.0 * static_cast<double>(a2)
        + 4.0 * static_cast<double>(a3)
        + 5.0 * static_cast<double>(a4)
        + 6.0 * static_cast<double>(a5)
        + 7.0 * a6
        + 8.0 * static_cast<double>(a7)
        + 9.0 * a8;
}

double ffi_mix_5(uint8_t a0, int8_t a1, uint16_t a2, int16_t a3, uint32_t a4, int32_t a5, uint64_t a6, int64_t a7)
{
    return 1.0 * static_cast<double>(a0)
        + 2.0 * static_cast<double>(a1)
        + 3.0 * static_cast<double>(a2)
        + 4.0 * static_cast<double>(a3)
        + 5.0 * static_cast<double>(a4)
        + 6.0 * static_cast<double>(a5)
        + 7.0 * static_cast<double>(a6)
        + 8.0 * static_cast<double>(a7);
}

double ffi_mix_6(bool a0, bool a1, int32_t a2, bool a3, double a4, bool a5, float a6, bool a7, bool a8, bool a9, bool a10, bool a11, bool a12)
{
    return 1.0 * (a0 ? 1.0 : 0.0)
        + 2.0 * (a1 ? 1.0 : 0.0)
        + 3.0 * static_cast<double>(a2)
        + 4.0 * (a3 ? 1.0 : 0.0)
        + 5.0 * a4
        + 6.0 * (a5 ? 1.0 : 0.0)
        + 7.0 * static_cast<double>(a6)
        + 8.0 * (a7 ? 1.0 : 0.0)
        + 9.0 * (a8 ? 1.0 : 0.0)
        + 10.0 * (a9 ? 1.0 : 0.0)
        + 11.0 * (a10 ? 1.0 : 0.0)
        + 12.0 * (a11 ? 1.0 : 0.0)
        + 13.0 * (a12 ? 1.0 : 0.0);
}

double ffi_mix_7(void* a0, signed char a1, void* a2, signed char a3, void* a4, signed char a5, void* a6, signed char a7, void* a8, signed char a9)
{
    return 1.0 * ffiPtrToDouble(a0)
        + 2.0 * static_cast<double>(a1)
        + 3.0 * ffiPtrToDouble(a2)
        + 4.0 * static_cast<double>(a3)
        + 5.0 * ffiPtrToDouble(a4)
        + 6.0 * static_cast<double>(a5)
        + 7.0 * ffiPtrToDouble(a6)
        + 8.0 * static_cast<double>(a7)
        + 9.0 * ffiPtrToDouble(a8)
        + 10.0 * static_cast<double>(a9);
}

double ffi_mix_8(float a0, double a1, float a2, double a3, float a4, double a5, float a6, double a7, float a8, double a9, float a10, double a11)
{
    return 1.0 * static_cast<double>(a0)
        + 2.0 * a1
        + 3.0 * static_cast<double>(a2)
        + 4.0 * a3
        + 5.0 * static_cast<double>(a4)
        + 6.0 * a5
        + 7.0 * static_cast<double>(a6)
        + 8.0 * a7
        + 9.0 * static_cast<double>(a8)
        + 10.0 * a9
        + 11.0 * static_cast<double>(a10)
        + 12.0 * a11;
}

void ffi_ptr_write_u32(uint32_t* p, uint32_t v) { *p = v; }
uint32_t ffi_ptr_read_u32(uint32_t* p) { return *p; }
uint64_t ffi_strlen(const char* s) { return static_cast<uint64_t>(strlen(s)); }
void* ffi_ptr_identity(void* p) { return p; }
void* ffi_high_ptr(void) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x00007fffdeadbee0ull)); }
uint64_t ffi_view_byte_length(const uint8_t*, uint64_t length) { return length; }
int32_t ffi_view_last_byte(const uint8_t* ptr, uint64_t length) { return length ? static_cast<int32_t>(ptr[length - 1]) : -1; }

void ffi_bench_noop(void) { }
const char* ffi_bench_string(void) { return "Hello, world!"; }
uint32_t ffi_bench_hash(const uint8_t* ptr, uint32_t length)
{
    uint32_t hash = 0;
    for (uint32_t i = 0; i < length; ++i)
        hash = hash * 0x10001000u + ptr[i];
    return hash;
}

int32_t ffi_call_cb_i32(int32_t (*cb)(int32_t), int32_t x) { return cb(x); }

double ffi_call_cb_f64_x8(double (*cb)(double, double, double, double, double, double, double, double), double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7)
{
    return cb(a0, a1, a2, a3, a4, a5, a6, a7);
}

double ffi_call_cb_mix(double (*cb)(int32_t, double, int64_t, float, void*), int32_t a0, double a1, int64_t a2, float a3, void* a4)
{
    return cb(a0, a1, a2, a3, a4);
}

void ffi_call_cb_void(void (*cb)(void)) { cb(); }

int32_t ffi_call_cb_reentrant(int32_t (*cb)(int32_t), int32_t depth)
{
    int32_t sum = 0;
    for (int32_t i = 0; i < depth; ++i)
        sum += cb(i);
    return sum;
}

int64_t ffi_call_cb_i32_x9(int64_t (*cb)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t), int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8)
{
    return cb(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

double ffi_call_cb_f64_x9(double (*cb)(double, double, double, double, double, double, double, double, double), double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8)
{
    return cb(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

int64_t ffi_call_cb_u8_x10(int64_t (*cb)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t), uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7, uint8_t a8, uint8_t a9)
{
    return cb(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

int64_t ffi_sum_i32_x10(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8, int32_t a9)
{
    return static_cast<int64_t>(a0) * 1 + static_cast<int64_t>(a1) * 2 + static_cast<int64_t>(a2) * 3
        + static_cast<int64_t>(a3) * 4 + static_cast<int64_t>(a4) * 5 + static_cast<int64_t>(a5) * 6
        + static_cast<int64_t>(a6) * 7 + static_cast<int64_t>(a7) * 8 + static_cast<int64_t>(a8) * 9
        + static_cast<int64_t>(a9) * 10;
}

void ffi_call_cb_from_thread(void (*cb)(int32_t, int64_t, uint64_t, double), int32_t a, int64_t b, uint64_t c, double d)
{
    auto thread = Thread::create("ffi-cb-from-thread"_s, [cb, a, b, c, d] {
        cb(a, b, c, d);
    });
    thread->waitForCompletion();
}

int64_t ffi_call_cb_ret_i8(int8_t (*cb)(void)) { return static_cast<int64_t>(cb()); }
int64_t ffi_call_cb_ret_u8(uint8_t (*cb)(void)) { return static_cast<int64_t>(cb()); }
int64_t ffi_call_cb_ret_i64(int64_t (*cb)(void)) { return cb(); }
uint64_t ffi_call_cb_ret_u64(uint64_t (*cb)(void)) { return cb(); }
int32_t ffi_call_cb_ret_bool(bool (*cb)(void)) { return cb() ? 10 : 20; }
float ffi_call_cb_ret_f32(float (*cb)(void)) { return cb(); }
double ffi_call_cb_ret_f64(double (*cb)(void)) { return cb(); }
void* ffi_call_cb_ret_ptr(void* (*cb)(void)) { return cb(); }

uint32_t ffi_call_cb_then_read_u32(uint32_t (*cb)(void), uint32_t* p)
{
    cb();
    return *p;
}

#if CPU(X86_64)

struct FFICanaryStateX64 {
    uint64_t original[7]; // rbx, r12, r13, r14, r15, rsi, rdi
    uint64_t sentinel[7];
    uint64_t observed[7];
    unsigned char originalXMM[10][16]; // xmm6 - xmm15 (Win64 nonvolatile only)
    unsigned char sentinelXMM[10][16];
    unsigned char observedXMM[10][16];
    void (*callback)(void);
};
static_assert(offsetof(FFICanaryStateX64, original) == 0);
static_assert(offsetof(FFICanaryStateX64, sentinel) == 56);
static_assert(offsetof(FFICanaryStateX64, observed) == 112);
static_assert(offsetof(FFICanaryStateX64, originalXMM) == 168);
static_assert(offsetof(FFICanaryStateX64, sentinelXMM) == 328);
static_assert(offsetof(FFICanaryStateX64, observedXMM) == 488);
static_assert(offsetof(FFICanaryStateX64, callback) == 648);

int32_t ffi_canary_call(void (*cb)(void))
{
    FFICanaryStateX64 state;
    memset(&state, 0, sizeof(state));
    state.callback = cb;
    for (unsigned i = 0; i < 7; ++i)
        state.sentinel[i] = 0xC0FFEE00CA11EE00ull + (uint64_t(i + 1) << 8) + (i + 1);
    for (unsigned i = 0; i < 10; ++i) {
        for (unsigned j = 0; j < 16; ++j)
            state.sentinelXMM[i][j] = static_cast<unsigned char>(0xA0 + i * 16 + j);
    }

#if OS(WINDOWS)
    asm volatile(
        "movq %[state], %%rax\n\t"
        "movq %%rbx, 0(%%rax)\n\t"
        "movq %%r12, 8(%%rax)\n\t"
        "movq %%r13, 16(%%rax)\n\t"
        "movq %%r14, 24(%%rax)\n\t"
        "movq %%r15, 32(%%rax)\n\t"
        "movq %%rsi, 40(%%rax)\n\t"
        "movq %%rdi, 48(%%rax)\n\t"
        "movdqu %%xmm6, 168(%%rax)\n\t"
        "movdqu %%xmm7, 184(%%rax)\n\t"
        "movdqu %%xmm8, 200(%%rax)\n\t"
        "movdqu %%xmm9, 216(%%rax)\n\t"
        "movdqu %%xmm10, 232(%%rax)\n\t"
        "movdqu %%xmm11, 248(%%rax)\n\t"
        "movdqu %%xmm12, 264(%%rax)\n\t"
        "movdqu %%xmm13, 280(%%rax)\n\t"
        "movdqu %%xmm14, 296(%%rax)\n\t"
        "movdqu %%xmm15, 312(%%rax)\n\t"
        "movq 56(%%rax), %%rbx\n\t"
        "movq 64(%%rax), %%r12\n\t"
        "movq 72(%%rax), %%r13\n\t"
        "movq 80(%%rax), %%r14\n\t"
        "movq 88(%%rax), %%r15\n\t"
        "movq 96(%%rax), %%rsi\n\t"
        "movq 104(%%rax), %%rdi\n\t"
        "movdqu 328(%%rax), %%xmm6\n\t"
        "movdqu 344(%%rax), %%xmm7\n\t"
        "movdqu 360(%%rax), %%xmm8\n\t"
        "movdqu 376(%%rax), %%xmm9\n\t"
        "movdqu 392(%%rax), %%xmm10\n\t"
        "movdqu 408(%%rax), %%xmm11\n\t"
        "movdqu 424(%%rax), %%xmm12\n\t"
        "movdqu 440(%%rax), %%xmm13\n\t"
        "movdqu 456(%%rax), %%xmm14\n\t"
        "movdqu 472(%%rax), %%xmm15\n\t"
        "movq %%rsp, %%r11\n\t"
        "subq $272, %%rsp\n\t"
        "andq $-16, %%rsp\n\t"
        "pushq %%r11\n\t"
        "pushq %%rax\n\t"
        "movq 648(%%rax), %%r10\n\t"
        "subq $32, %%rsp\n\t"
        "callq *%%r10\n\t"
        "addq $32, %%rsp\n\t"
        "popq %%rax\n\t"
        "popq %%r11\n\t"
        "movq %%r11, %%rsp\n\t"
        "movq %%rbx, 112(%%rax)\n\t"
        "movq %%r12, 120(%%rax)\n\t"
        "movq %%r13, 128(%%rax)\n\t"
        "movq %%r14, 136(%%rax)\n\t"
        "movq %%r15, 144(%%rax)\n\t"
        "movq %%rsi, 152(%%rax)\n\t"
        "movq %%rdi, 160(%%rax)\n\t"
        "movdqu %%xmm6, 488(%%rax)\n\t"
        "movdqu %%xmm7, 504(%%rax)\n\t"
        "movdqu %%xmm8, 520(%%rax)\n\t"
        "movdqu %%xmm9, 536(%%rax)\n\t"
        "movdqu %%xmm10, 552(%%rax)\n\t"
        "movdqu %%xmm11, 568(%%rax)\n\t"
        "movdqu %%xmm12, 584(%%rax)\n\t"
        "movdqu %%xmm13, 600(%%rax)\n\t"
        "movdqu %%xmm14, 616(%%rax)\n\t"
        "movdqu %%xmm15, 632(%%rax)\n\t"
        "movq 0(%%rax), %%rbx\n\t"
        "movq 8(%%rax), %%r12\n\t"
        "movq 16(%%rax), %%r13\n\t"
        "movq 24(%%rax), %%r14\n\t"
        "movq 32(%%rax), %%r15\n\t"
        "movq 40(%%rax), %%rsi\n\t"
        "movq 48(%%rax), %%rdi\n\t"
        "movdqu 168(%%rax), %%xmm6\n\t"
        "movdqu 184(%%rax), %%xmm7\n\t"
        "movdqu 200(%%rax), %%xmm8\n\t"
        "movdqu 216(%%rax), %%xmm9\n\t"
        "movdqu 232(%%rax), %%xmm10\n\t"
        "movdqu 248(%%rax), %%xmm11\n\t"
        "movdqu 264(%%rax), %%xmm12\n\t"
        "movdqu 280(%%rax), %%xmm13\n\t"
        "movdqu 296(%%rax), %%xmm14\n\t"
        "movdqu 312(%%rax), %%xmm15\n\t"
        :
        : [state] "r"(&state)
        : "memory", "cc", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
#else // !OS(WINDOWS): System V AMD64
    asm volatile(
        "movq %[state], %%rax\n\t"
        "movq %%rbx, 0(%%rax)\n\t"
        "movq %%r12, 8(%%rax)\n\t"
        "movq %%r13, 16(%%rax)\n\t"
        "movq %%r14, 24(%%rax)\n\t"
        "movq %%r15, 32(%%rax)\n\t"
        "movq 56(%%rax), %%rbx\n\t"
        "movq 64(%%rax), %%r12\n\t"
        "movq 72(%%rax), %%r13\n\t"
        "movq 80(%%rax), %%r14\n\t"
        "movq 88(%%rax), %%r15\n\t"
        "movq %%rsp, %%r11\n\t"
        "subq $256, %%rsp\n\t"
        "andq $-16, %%rsp\n\t"
        "pushq %%r11\n\t"
        "pushq %%rax\n\t"
        "movq 648(%%rax), %%r10\n\t"
        "callq *%%r10\n\t"
        "popq %%rax\n\t"
        "popq %%r11\n\t"
        "movq %%r11, %%rsp\n\t"
        "movq %%rbx, 112(%%rax)\n\t"
        "movq %%r12, 120(%%rax)\n\t"
        "movq %%r13, 128(%%rax)\n\t"
        "movq %%r14, 136(%%rax)\n\t"
        "movq %%r15, 144(%%rax)\n\t"
        "movq 0(%%rax), %%rbx\n\t"
        "movq 8(%%rax), %%r12\n\t"
        "movq 16(%%rax), %%r13\n\t"
        "movq 24(%%rax), %%r14\n\t"
        "movq 32(%%rax), %%r15\n\t"
        :
        : [state] "r"(&state)
        : "memory", "cc", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif

    uint32_t mask = 0;
#if OS(WINDOWS)
    for (unsigned i = 0; i < 7; ++i) {
        if (state.observed[i] != state.sentinel[i])
            mask |= 1u << i;
    }
    for (unsigned i = 0; i < 10; ++i) {
        if (memcmp(state.observedXMM[i], state.sentinelXMM[i], 16))
            mask |= 1u << (7 + i);
    }
#else
    for (unsigned i = 0; i < 5; ++i) {
        if (state.observed[i] != state.sentinel[i])
            mask |= 1u << i;
    }
#endif
    return static_cast<int32_t>(mask);
}

#elif CPU(ARM64)

#if OS(LINUX) || OS(FREEBSD)
#define FFI_CANARY_CLOBBER_X18 "x18",
#else
#define FFI_CANARY_CLOBBER_X18
#endif

struct FFICanaryStateARM64 {
    uint64_t originalX[10]; // x19 - x28
    uint64_t sentinelX[10];
    uint64_t observedX[10];
    unsigned char originalQ[8][16]; // q8 - q15
    unsigned char sentinelQ[8][16];
    unsigned char observedQ[8][16];
    void (*callback)(void);
};
static_assert(offsetof(FFICanaryStateARM64, originalX) == 0);
static_assert(offsetof(FFICanaryStateARM64, sentinelX) == 80);
static_assert(offsetof(FFICanaryStateARM64, observedX) == 160);
static_assert(offsetof(FFICanaryStateARM64, originalQ) == 240);
static_assert(offsetof(FFICanaryStateARM64, sentinelQ) == 368);
static_assert(offsetof(FFICanaryStateARM64, observedQ) == 496);
static_assert(offsetof(FFICanaryStateARM64, callback) == 624);

int32_t ffi_canary_call(void (*cb)(void))
{
    FFICanaryStateARM64 state;
    memset(&state, 0, sizeof(state));
    state.callback = cb;
    for (unsigned i = 0; i < 10; ++i)
        state.sentinelX[i] = 0xC0FFEE00CA11EE00ull + (uint64_t(i + 1) << 8) + (i + 1);
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 16; ++j)
            state.sentinelQ[i][j] = static_cast<unsigned char>(0x60 + i * 16 + j);
    }

    asm volatile(
        "mov x9, %[state]\n\t"
        "stp x19, x20, [x9, #0]\n\t"
        "stp x21, x22, [x9, #16]\n\t"
        "stp x23, x24, [x9, #32]\n\t"
        "stp x25, x26, [x9, #48]\n\t"
        "stp x27, x28, [x9, #64]\n\t"
        "stp q8, q9, [x9, #240]\n\t"
        "stp q10, q11, [x9, #272]\n\t"
        "stp q12, q13, [x9, #304]\n\t"
        "stp q14, q15, [x9, #336]\n\t"
        "ldp x19, x20, [x9, #80]\n\t"
        "ldp x21, x22, [x9, #96]\n\t"
        "ldp x23, x24, [x9, #112]\n\t"
        "ldp x25, x26, [x9, #128]\n\t"
        "ldp x27, x28, [x9, #144]\n\t"
        "ldp q8, q9, [x9, #368]\n\t"
        "ldp q10, q11, [x9, #400]\n\t"
        "ldp q12, q13, [x9, #432]\n\t"
        "ldp q14, q15, [x9, #464]\n\t"
        "str x9, [sp, #-16]!\n\t"
        "ldr x10, [x9, #624]\n\t"
        "blr x10\n\t"
        "ldr x9, [sp], #16\n\t"
        "stp x19, x20, [x9, #160]\n\t"
        "stp x21, x22, [x9, #176]\n\t"
        "stp x23, x24, [x9, #192]\n\t"
        "stp x25, x26, [x9, #208]\n\t"
        "stp x27, x28, [x9, #224]\n\t"
        "stp q8, q9, [x9, #496]\n\t"
        "stp q10, q11, [x9, #528]\n\t"
        "stp q12, q13, [x9, #560]\n\t"
        "stp q14, q15, [x9, #592]\n\t"
        "ldp x19, x20, [x9, #0]\n\t"
        "ldp x21, x22, [x9, #16]\n\t"
        "ldp x23, x24, [x9, #32]\n\t"
        "ldp x25, x26, [x9, #48]\n\t"
        "ldp x27, x28, [x9, #64]\n\t"
        "ldp q8, q9, [x9, #240]\n\t"
        "ldp q10, q11, [x9, #272]\n\t"
        "ldp q12, q13, [x9, #304]\n\t"
        "ldp q14, q15, [x9, #336]\n\t"
        :
        : [state] "r"(&state)
        : "memory", "cc", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
          "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", FFI_CANARY_CLOBBER_X18 "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");

    uint32_t mask = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (state.observedX[i] != state.sentinelX[i])
            mask |= 1u << i;
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (memcmp(state.observedQ[i], state.sentinelQ[i], 8))
            mask |= 1u << (10 + i);
    }
    return static_cast<int32_t>(mask);
}

#else

int32_t ffi_canary_call(void (*cb)(void))
{
    cb();
    return 0;
}

#endif

static inline double ffiAlignedVectorProbe()
{
    alignas(16) double buffer[2];
    buffer[0] = 1.0;
    buffer[1] = 0.0;
#if CPU(X86_64)
    asm volatile(
        "movaps (%[buf]), %%xmm0\n\t"
        "movaps %%xmm0, (%[buf])\n\t"
        :
        : [buf] "r"(&buffer[0])
        : "xmm0", "memory");
#elif CPU(ARM64)
    asm volatile(
        "str q0, [sp, #-16]!\n\t"
        "ld1 { v0.4s }, [%[buf]]\n\t"
        "st1 { v0.4s }, [%[buf]]\n\t"
        "ldr q0, [sp], #16\n\t"
        :
        : [buf] "r"(&buffer[0])
        : "v0", "memory");
#endif
    return buffer[0];
}

double ffi_align_probe_0(void)
{
    return ffiAlignedVectorProbe();
}

double ffi_align_probe_9(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8)
{
    double folded = ffiAlignedVectorProbe();
    int64_t junk = int64_t(a0) + int64_t(a1) + int64_t(a2) + int64_t(a3) + int64_t(a4) + int64_t(a5) + int64_t(a6) + int64_t(a7) + int64_t(a8);
    return folded + (static_cast<double>(junk) * 0.0);
}

} // extern "C"

#define FFI_FIXTURE(name) { #name, reinterpret_cast<void*>(&name) }

std::span<const FFIFixtureEntry> ffiTestFixtures()
{
    static const FFIFixtureEntry fixtures[] = {
        FFI_FIXTURE(ffi_echo_char),
        FFI_FIXTURE(ffi_echo_i8),
        FFI_FIXTURE(ffi_echo_u8),
        FFI_FIXTURE(ffi_echo_i16),
        FFI_FIXTURE(ffi_echo_u16),
        FFI_FIXTURE(ffi_echo_i32),
        FFI_FIXTURE(ffi_echo_u32),
        FFI_FIXTURE(ffi_echo_i64),
        FFI_FIXTURE(ffi_echo_u64),
        FFI_FIXTURE(ffi_echo_f32),
        FFI_FIXTURE(ffi_echo_f64),
        FFI_FIXTURE(ffi_echo_bool),
        FFI_FIXTURE(ffi_echo_ptr),
        FFI_FIXTURE(ffi_echo_cstring),
        FFI_FIXTURE(ffi_echo_jsvalue),
        FFI_FIXTURE(ffi_widen_char),
        FFI_FIXTURE(ffi_widen_i8),
        FFI_FIXTURE(ffi_widen_u8),
        FFI_FIXTURE(ffi_widen_i16),
        FFI_FIXTURE(ffi_widen_u16),
        FFI_FIXTURE(ffi_ret_null_ptr),
        FFI_FIXTURE(ffi_ret_two_as_bool),
        FFI_FIXTURE(ffi_ret_neg_one_i8),
        FFI_FIXTURE(ffi_ret_neg_one_i16),
        FFI_FIXTURE(ffi_ret_neg_one_i32),
        FFI_FIXTURE(ffi_ret_neg_one_i64),
        FFI_FIXTURE(ffi_ret_neg_one_u8),
        FFI_FIXTURE(ffi_ret_neg_one_u16),
        FFI_FIXTURE(ffi_ret_neg_one_u32),
        FFI_FIXTURE(ffi_ret_neg_one_u64),
        FFI_FIXTURE(ffi_ret_nan_f32),
        FFI_FIXTURE(ffi_ret_impure_nan_f64),
        FFI_FIXTURE(ffi_ret_neg_zero_f64),
        FFI_FIXTURE(ffi_ret_denormal_f32),
        FFI_FIXTURE(ffi_ret_inf_f64),
        FFI_FIXTURE(ffi_add_i32),
        FFI_FIXTURE(ffi_add_f64),
        FFI_FIXTURE(ffi_add_i64),
        FFI_FIXTURE(ffi_add_u64),
        FFI_FIXTURE(ffi_add_f32),
        FFI_FIXTURE(ffi_sum_i32_0),
        FFI_FIXTURE(ffi_sum_i32_1),
        FFI_FIXTURE(ffi_sum_i32_2),
        FFI_FIXTURE(ffi_sum_i32_4),
        FFI_FIXTURE(ffi_sum_i32_6),
        FFI_FIXTURE(ffi_sum_i32_7),
        FFI_FIXTURE(ffi_sum_i32_8),
        FFI_FIXTURE(ffi_sum_i32_9),
        FFI_FIXTURE(ffi_sum_i32_12),
        FFI_FIXTURE(ffi_sum_i32_16),
        FFI_FIXTURE(ffi_sum_f64_1),
        FFI_FIXTURE(ffi_sum_f64_2),
        FFI_FIXTURE(ffi_sum_f64_7),
        FFI_FIXTURE(ffi_sum_f64_8),
        FFI_FIXTURE(ffi_sum_f64_9),
        FFI_FIXTURE(ffi_sum_f64_12),
        FFI_FIXTURE(ffi_sum_u8_10),
        FFI_FIXTURE(ffi_sum_u8_12),
        FFI_FIXTURE(ffi_sum_i16_10),
        FFI_FIXTURE(ffi_sum_i16_12),
        FFI_FIXTURE(ffi_mix_1),
        FFI_FIXTURE(ffi_mix_2),
        FFI_FIXTURE(ffi_mix_3),
        FFI_FIXTURE(ffi_mix_4),
        FFI_FIXTURE(ffi_mix_5),
        FFI_FIXTURE(ffi_mix_6),
        FFI_FIXTURE(ffi_mix_7),
        FFI_FIXTURE(ffi_mix_8),
        FFI_FIXTURE(ffi_ptr_write_u32),
        FFI_FIXTURE(ffi_ptr_read_u32),
        FFI_FIXTURE(ffi_strlen),
        FFI_FIXTURE(ffi_ptr_identity),
        FFI_FIXTURE(ffi_high_ptr),
        FFI_FIXTURE(ffi_view_byte_length),
        FFI_FIXTURE(ffi_view_last_byte),
        FFI_FIXTURE(ffi_bench_noop),
        FFI_FIXTURE(ffi_bench_string),
        FFI_FIXTURE(ffi_bench_hash),
        FFI_FIXTURE(ffi_call_cb_i32),
        FFI_FIXTURE(ffi_call_cb_f64_x8),
        FFI_FIXTURE(ffi_call_cb_mix),
        FFI_FIXTURE(ffi_call_cb_void),
        FFI_FIXTURE(ffi_call_cb_reentrant),
        FFI_FIXTURE(ffi_call_cb_i32_x9),
        FFI_FIXTURE(ffi_call_cb_f64_x9),
        FFI_FIXTURE(ffi_call_cb_u8_x10),
        FFI_FIXTURE(ffi_call_cb_ret_i8),
        FFI_FIXTURE(ffi_call_cb_ret_u8),
        FFI_FIXTURE(ffi_call_cb_ret_i64),
        FFI_FIXTURE(ffi_call_cb_ret_u64),
        FFI_FIXTURE(ffi_call_cb_ret_bool),
        FFI_FIXTURE(ffi_call_cb_ret_f32),
        FFI_FIXTURE(ffi_call_cb_ret_f64),
        FFI_FIXTURE(ffi_call_cb_ret_ptr),
        FFI_FIXTURE(ffi_call_cb_then_read_u32),
        FFI_FIXTURE(ffi_canary_call),
        FFI_FIXTURE(ffi_sum_i32_x10),
        FFI_FIXTURE(ffi_call_cb_from_thread),
        FFI_FIXTURE(ffi_align_probe_0),
        FFI_FIXTURE(ffi_align_probe_9),
    };
    return std::span<const FFIFixtureEntry>(fixtures);
}

#undef FFI_FIXTURE

const FFIFixtureEntry* ffiTestFixtureNamed(const char* name)
{
    for (const auto& entry : ffiTestFixtures()) {
        if (!strcmp(entry.name, name))
            return &entry;
    }
    return nullptr;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)
