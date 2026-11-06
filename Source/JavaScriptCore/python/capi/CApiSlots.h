/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// The numbers of the slots of types and modules (Py_tp_new and so on), as the headers of 3.15 give them to what is compiled for abi3t. They are part of the ABI.
#define FOR_EACH_CAPI_SLOT(v) \
    v(slot_end, 0) \
    v(mp_subscript, 5) \
    v(nb_absolute, 6) \
    v(nb_add, 7) \
    v(nb_and, 8) \
    v(nb_bool, 9) \
    v(nb_divmod, 10) \
    v(nb_float, 11) \
    v(nb_floor_divide, 12) \
    v(nb_index, 13) \
    v(nb_inplace_add, 14) \
    v(nb_inplace_and, 15) \
    v(nb_inplace_floor_divide, 16) \
    v(nb_inplace_lshift, 17) \
    v(nb_inplace_multiply, 18) \
    v(nb_inplace_or, 19) \
    v(nb_inplace_power, 20) \
    v(nb_inplace_remainder, 21) \
    v(nb_inplace_rshift, 22) \
    v(nb_inplace_subtract, 23) \
    v(nb_inplace_true_divide, 24) \
    v(nb_inplace_xor, 25) \
    v(nb_int, 26) \
    v(nb_invert, 27) \
    v(nb_lshift, 28) \
    v(nb_multiply, 29) \
    v(nb_negative, 30) \
    v(nb_or, 31) \
    v(nb_positive, 32) \
    v(nb_power, 33) \
    v(nb_remainder, 34) \
    v(nb_rshift, 35) \
    v(nb_subtract, 36) \
    v(nb_true_divide, 37) \
    v(nb_xor, 38) \
    v(sq_ass_item, 39) \
    v(sq_concat, 40) \
    v(sq_contains, 41) \
    v(sq_inplace_concat, 42) \
    v(sq_inplace_repeat, 43) \
    v(sq_item, 44) \
    v(sq_length, 45) \
    v(sq_repeat, 46) \
    v(tp_alloc, 47) \
    v(tp_base, 48) \
    v(tp_bases, 49) \
    v(tp_call, 50) \
    v(tp_clear, 51) \
    v(tp_dealloc, 52) \
    v(tp_del, 53) \
    v(tp_descr_get, 54) \
    v(tp_descr_set, 55) \
    v(tp_doc, 56) \
    v(tp_getattr, 57) \
    v(tp_getattro, 58) \
    v(tp_hash, 59) \
    v(tp_init, 60) \
    v(tp_is_gc, 61) \
    v(tp_iter, 62) \
    v(tp_iternext, 63) \
    v(tp_methods, 64) \
    v(tp_new, 65) \
    v(tp_repr, 66) \
    v(tp_richcompare, 67) \
    v(tp_setattr, 68) \
    v(tp_setattro, 69) \
    v(tp_str, 70) \
    v(tp_traverse, 71) \
    v(tp_members, 72) \
    v(tp_getset, 73) \
    v(tp_free, 74) \
    v(nb_matrix_multiply, 75) \
    v(nb_inplace_matrix_multiply, 76) \
    v(am_await, 77) \
    v(am_aiter, 78) \
    v(am_anext, 79) \
    v(tp_finalize, 80) \
    v(am_send, 81) \
    v(tp_vectorcall, 82) \
    v(tp_token, 83) \
    v(mod_create, 84) \
    v(mod_exec, 85) \
    v(mod_multiple_interpreters, 86) \
    v(mod_gil, 87) \
    v(bf_getbuffer, 88) \
    v(bf_releasebuffer, 89) \
    v(mp_ass_subscript, 90) \
    v(mp_length, 91) \
    v(slot_subslots, 92) \
    v(tp_slots, 93) \
    v(mod_slots, 94) \
    v(tp_name, 95) \
    v(tp_basicsize, 96) \
    v(tp_extra_basicsize, 97) \
    v(tp_itemsize, 98) \
    v(tp_flags, 99) \
    v(mod_name, 100) \
    v(mod_doc, 101) \
    v(mod_state_size, 102) \
    v(mod_methods, 103) \
    v(mod_state_traverse, 104) \
    v(mod_state_clear, 105) \
    v(mod_state_free, 106) \
    v(tp_metaclass, 107) \
    v(tp_module, 108) \
    v(mod_abi, 109) \
    v(mod_token, 110)

namespace JSC {
namespace Python {
namespace CApi {

enum Slot : uint16_t {
#define DEFINE_SLOT(name, number) name = number,
    FOR_EACH_CAPI_SLOT(DEFINE_SLOT)
#undef DEFINE_SLOT
};
static constexpr unsigned numberOfSlots = 111;

} // namespace CApi
} // namespace Python
} // namespace JSC
