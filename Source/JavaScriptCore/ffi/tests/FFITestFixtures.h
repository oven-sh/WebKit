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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "JSExportMacros.h"
#include <cstdint>
#include <span>

typedef int64_t jsvalue_slot; // an encoded JSValue crossing the boundary as an int64

extern "C" {

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
JS_EXPORT_PRIVATE jsvalue_slot ffi_echo_jsvalue(jsvalue_slot);

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

JS_EXPORT_PRIVATE float ffi_ret_nan_f32(void); // NaN with a non-canonical payload; must be purified before boxing.
JS_EXPORT_PRIVATE double ffi_ret_impure_nan_f64(void); // 0x7ff0000000000001; must be purified before boxing.
JS_EXPORT_PRIVATE double ffi_ret_neg_zero_f64(void);
JS_EXPORT_PRIVATE float ffi_ret_denormal_f32(void); // smallest positive denormal (bits 0x00000001).
JS_EXPORT_PRIVATE double ffi_ret_inf_f64(void);

JS_EXPORT_PRIVATE int32_t ffi_add_i32(int32_t, int32_t);
JS_EXPORT_PRIVATE double ffi_add_f64(double, double);
JS_EXPORT_PRIVATE int64_t ffi_add_i64(int64_t, int64_t);
JS_EXPORT_PRIVATE uint64_t ffi_add_u64(uint64_t, uint64_t);
JS_EXPORT_PRIVATE float ffi_add_f32(float, float);

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

JS_EXPORT_PRIVATE double ffi_mix_1(int32_t, double, int64_t, float, void*, uint8_t, double, int16_t, double, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_2(float, int32_t, float, int32_t, float, int32_t, float, int32_t, float, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_3(double, double, double, double, double, double, double, double, int32_t);
JS_EXPORT_PRIVATE double ffi_mix_4(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, int64_t, double);
JS_EXPORT_PRIVATE double ffi_mix_5(uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, uint64_t, int64_t);
JS_EXPORT_PRIVATE double ffi_mix_6(bool, bool, int32_t, bool, double, bool, float, bool, bool, bool, bool, bool, bool);
JS_EXPORT_PRIVATE double ffi_mix_7(void*, signed char, void*, signed char, void*, signed char, void*, signed char, void*, signed char);
JS_EXPORT_PRIVATE double ffi_mix_8(float, double, float, double, float, double, float, double, float, double, float, double);

JS_EXPORT_PRIVATE void ffi_ptr_write_u32(uint32_t*, uint32_t);
JS_EXPORT_PRIVATE uint32_t ffi_ptr_read_u32(uint32_t*);
JS_EXPORT_PRIVATE uint64_t ffi_strlen(const char*);
JS_EXPORT_PRIVATE void* ffi_ptr_identity(void*);
JS_EXPORT_PRIVATE void* ffi_high_ptr(void); // returns (void*)0x00007fffdeadbee0
JS_EXPORT_PRIVATE uint64_t ffi_view_byte_length(const uint8_t* ptr, uint64_t length);
JS_EXPORT_PRIVATE int32_t ffi_view_last_byte(const uint8_t* ptr, uint64_t length);
JS_EXPORT_PRIVATE void ffi_bench_noop(void);
JS_EXPORT_PRIVATE const char* ffi_bench_string(void);
JS_EXPORT_PRIVATE uint32_t ffi_bench_hash(const uint8_t* ptr, uint32_t length);

JS_EXPORT_PRIVATE int32_t ffi_call_cb_i32(int32_t (*cb)(int32_t), int32_t x);
JS_EXPORT_PRIVATE double ffi_call_cb_f64_x8(double (*cb)(double, double, double, double, double, double, double, double), double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE double ffi_call_cb_mix(double (*cb)(int32_t, double, int64_t, float, void*), int32_t, double, int64_t, float, void*);
JS_EXPORT_PRIVATE void ffi_call_cb_void(void (*cb)(void));
JS_EXPORT_PRIVATE int32_t ffi_call_cb_reentrant(int32_t (*cb)(int32_t), int32_t depth); // returns sum of cb(i) for i in [0, depth)
JS_EXPORT_PRIVATE int64_t ffi_call_cb_i32_x9(int64_t (*cb)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t), int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
JS_EXPORT_PRIVATE double ffi_call_cb_f64_x9(double (*cb)(double, double, double, double, double, double, double, double, double), double, double, double, double, double, double, double, double, double);
JS_EXPORT_PRIVATE int64_t ffi_call_cb_u8_x10(int64_t (*cb)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t), uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
JS_EXPORT_PRIVATE int64_t ffi_sum_i32_x10(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
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

JS_EXPORT_PRIVATE int32_t ffi_canary_call(void (*cb)(void));

JS_EXPORT_PRIVATE double ffi_align_probe_0(void);
JS_EXPORT_PRIVATE double ffi_align_probe_9(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);

} // extern "C"

struct FFIFixtureEntry {
    const char* name;
    void* address;
};

JS_EXPORT_PRIVATE std::span<const FFIFixtureEntry> ffiTestFixtures();

JS_EXPORT_PRIVATE const FFIFixtureEntry* ffiTestFixtureNamed(const char* name);

#endif // USE(BUN_JSC_ADDITIONS)
