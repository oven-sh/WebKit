/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

// The slow paths the interpreter calls, by name, and the table through which it calls them.
//
// Each of these is compiled once per threads mode (ThreadsModePage.h). The interpreter does not call the function
// that has the slow path's name, which would test the mode on every call: it calls through LLInt::SlowPathTable,
// which LLInt::initialize() fills with the copy for the process's mode before the opcode config is frozen
// (callSlowPath in LowLevelInterpreter64.asm). The function that has the name is what everything else calls.
//
// A slow path defined with LLINT_SLOW_PATH_DECL (LLIntSlowPaths.cpp) or JSC_DEFINE_COMMON_SLOW_PATH
// (CommonSlowPaths.cpp) has to be listed here: the definition does not compile otherwise.

#define FOR_EACH_LLINT_SLOW_PATH(macro) \
    macro(llint_trace_prologue) \
    macro(llint_trace_prologue_function_for_call) \
    macro(llint_trace_prologue_function_for_construct) \
    macro(llint_trace_arityCheck_for_call) \
    macro(llint_trace_arityCheck_for_construct) \
    macro(llint_trace) \
    macro(llint_entry_osr) \
    macro(llint_entry_osr_function_for_call) \
    macro(llint_entry_osr_function_for_construct) \
    macro(llint_entry_osr_function_for_call_arityCheck) \
    macro(llint_entry_osr_function_for_construct_arityCheck) \
    macro(llint_loop_osr) \
    macro(llint_replace) \
    macro(llint_slow_path_new_object) \
    macro(llint_slow_path_new_array) \
    macro(llint_slow_path_new_array_with_size) \
    macro(llint_slow_path_new_reg_exp) \
    macro(llint_slow_path_new_reg_exp_shared) \
    macro(llint_slow_path_create_lexical_environment) \
    macro(llint_slow_path_create_direct_arguments) \
    macro(llint_slow_path_create_scoped_arguments) \
    macro(llint_slow_path_create_cloned_arguments) \
    macro(llint_slow_path_get_by_id_direct) \
    macro(llint_slow_path_get_by_id_with_this) \
    macro(llint_slow_path_get_by_id) \
    macro(llint_slow_path_get_length) \
    macro(llint_slow_path_iterator_open_get_next) \
    macro(llint_slow_path_iterator_next_get_done) \
    macro(llint_slow_path_iterator_next_get_value) \
    macro(llint_slow_path_get_hasInstance_from_instanceof) \
    macro(llint_slow_path_get_prototype_from_instanceof) \
    macro(llint_slow_path_instanceof_from_instanceof) \
    macro(llint_slow_path_instanceof) \
    macro(llint_slow_path_put_by_id) \
    macro(llint_slow_path_del_by_id) \
    macro(llint_slow_path_get_by_val) \
    macro(llint_slow_path_get_private_name) \
    macro(llint_slow_path_put_by_val) \
    macro(llint_slow_path_put_by_val_direct) \
    macro(llint_slow_path_put_private_name) \
    macro(llint_slow_path_set_private_brand) \
    macro(llint_slow_path_check_private_brand) \
    macro(llint_slow_path_del_by_val) \
    macro(llint_slow_path_in_by_id) \
    macro(llint_slow_path_in_by_val) \
    macro(llint_slow_path_has_private_name) \
    macro(llint_slow_path_has_private_brand) \
    macro(llint_slow_path_has_structure_with_flags) \
    macro(llint_slow_path_put_getter_by_id) \
    macro(llint_slow_path_put_setter_by_id) \
    macro(llint_slow_path_put_getter_setter_by_id) \
    macro(llint_slow_path_put_getter_by_val) \
    macro(llint_slow_path_put_setter_by_val) \
    macro(llint_slow_path_jtrue) \
    macro(llint_slow_path_jfalse) \
    macro(llint_slow_path_less) \
    macro(llint_slow_path_lesseq) \
    macro(llint_slow_path_greater) \
    macro(llint_slow_path_greatereq) \
    macro(llint_slow_path_jless) \
    macro(llint_slow_path_jnless) \
    macro(llint_slow_path_jgreater) \
    macro(llint_slow_path_jngreater) \
    macro(llint_slow_path_jlesseq) \
    macro(llint_slow_path_jnlesseq) \
    macro(llint_slow_path_jgreatereq) \
    macro(llint_slow_path_jngreatereq) \
    macro(llint_slow_path_jeq) \
    macro(llint_slow_path_jneq) \
    macro(llint_slow_path_jstricteq) \
    macro(llint_slow_path_jnstricteq) \
    macro(llint_slow_path_switch_imm) \
    macro(llint_slow_path_switch_char) \
    macro(llint_slow_path_switch_string) \
    macro(llint_slow_path_new_func) \
    macro(llint_slow_path_new_generator_func) \
    macro(llint_slow_path_new_async_func) \
    macro(llint_slow_path_new_async_generator_func) \
    macro(llint_slow_path_new_func_exp) \
    macro(llint_slow_path_new_generator_func_exp) \
    macro(llint_slow_path_new_async_func_exp) \
    macro(llint_slow_path_new_async_generator_func_exp) \
    macro(llint_slow_path_set_function_name) \
    macro(llint_slow_path_async_iterator_open_get_next) \
    macro(llint_slow_path_async_iterator_next_with_driver) \
    macro(llint_slow_path_ensure_call_link_info) \
    macro(llint_slow_path_size_frame_for_varargs) \
    macro(llint_slow_path_call_varargs) \
    macro(llint_slow_path_tail_call_varargs) \
    macro(llint_slow_path_construct_varargs) \
    macro(llint_slow_path_super_construct_varargs) \
    macro(llint_slow_path_call_direct_eval) \
    macro(llint_slow_path_call_direct_eval_wide16) \
    macro(llint_slow_path_call_direct_eval_wide32) \
    macro(llint_slow_path_strcat) \
    macro(llint_slow_path_to_primitive) \
    macro(llint_slow_path_throw) \
    macro(llint_slow_path_handle_traps) \
    macro(llint_slow_path_debug) \
    macro(llint_slow_path_handle_exception) \
    macro(llint_slow_path_get_from_scope) \
    macro(llint_slow_path_put_to_scope) \
    macro(llint_slow_path_retrieve_and_clear_exception_if_catchable) \
    macro(llint_slow_path_log_shadow_chicken_prologue) \
    macro(llint_slow_path_log_shadow_chicken_tail) \
    macro(llint_slow_path_profile_catch) \
    macro(llint_slow_path_out_of_line_jump_target) \
    macro(llint_slow_path_arityCheck) \

#define FOR_EACH_COMMON_SLOW_PATH(macro) \
    macro(slow_path_create_this) \
    macro(slow_path_create_promise) \
    macro(slow_path_new_promise) \
    macro(slow_path_create_generator) \
    macro(slow_path_create_async_generator) \
    macro(slow_path_new_generator) \
    macro(slow_path_new_async_function_generator) \
    macro(slow_path_to_this) \
    macro(slow_path_check_tdz) \
    macro(slow_path_throw_strict_mode_readonly_property_write_error) \
    macro(slow_path_not) \
    macro(slow_path_eq) \
    macro(slow_path_neq) \
    macro(slow_path_stricteq) \
    macro(slow_path_nstricteq) \
    macro(slow_path_inc) \
    macro(slow_path_dec) \
    macro(slow_path_to_string) \
    macro(slow_path_negate) \
    macro(slow_path_to_number) \
    macro(slow_path_to_numeric) \
    macro(slow_path_to_object) \
    macro(slow_path_add) \
    macro(slow_path_mul) \
    macro(slow_path_sub) \
    macro(slow_path_div) \
    macro(slow_path_mod) \
    macro(slow_path_pow) \
    macro(slow_path_lshift) \
    macro(slow_path_rshift) \
    macro(slow_path_urshift) \
    macro(slow_path_unsigned) \
    macro(slow_path_bitnot) \
    macro(slow_path_bitand) \
    macro(slow_path_bitor) \
    macro(slow_path_bitxor) \
    macro(slow_path_typeof) \
    macro(slow_path_typeof_is_object) \
    macro(slow_path_typeof_is_function) \
    macro(slow_path_throw_static_error_from_instanceof) \
    macro(slow_path_instanceof_custom_from_instanceof) \
    macro(slow_path_is_callable) \
    macro(slow_path_is_constructor) \
    macro(iterator_open_try_fast_narrow) \
    macro(iterator_open_try_fast_wide16) \
    macro(iterator_open_try_fast_wide32) \
    macro(async_iterator_open_try_fast_narrow) \
    macro(async_iterator_open_try_fast_wide16) \
    macro(async_iterator_open_try_fast_wide32) \
    macro(iterator_next_index_in_frame_narrow) \
    macro(iterator_next_index_in_frame_wide16) \
    macro(iterator_next_index_in_frame_wide32) \
    macro(iterator_next_try_fast_narrow) \
    macro(iterator_next_try_fast_wide16) \
    macro(iterator_next_try_fast_wide32) \
    macro(slow_path_iterator_close_check) \
    macro(slow_path_strcat) \
    macro(slow_path_to_primitive) \
    macro(slow_path_enter) \
    macro(slow_path_to_property_key) \
    macro(slow_path_to_property_key_or_number) \
    macro(slow_path_get_property_enumerator) \
    macro(slow_path_enumerator_next) \
    macro(slow_path_enumerator_get_by_val) \
    macro(slow_path_enumerator_in_by_val) \
    macro(slow_path_enumerator_put_by_val) \
    macro(slow_path_enumerator_has_own_property) \
    macro(slow_path_profile_type_clear_log) \
    macro(slow_path_unreachable) \
    macro(slow_path_push_with_scope) \
    macro(slow_path_resolve_scope_for_hoisting_func_decl_in_eval) \
    macro(slow_path_resolve_scope) \
    macro(slow_path_create_rest) \
    macro(slow_path_get_by_val_with_this) \
    macro(slow_path_get_prototype_of) \
    macro(slow_path_put_by_id_with_this) \
    macro(slow_path_put_by_val_with_this) \
    macro(slow_path_define_data_property) \
    macro(slow_path_define_accessor_property) \
    macro(slow_path_throw_static_error) \
    macro(slow_path_new_array_with_spread) \
    macro(slow_path_new_array_with_species) \
    macro(slow_path_new_array_buffer) \
    macro(slow_path_spread) \

namespace JSC {
namespace LLInt {

// One pointer per slow path; the member is named as the interpreter names the function (the leading underscore).
struct SlowPathTable {
#define JSC_DECLARE_SLOW_PATH_TABLE_ENTRY(name) void* _##name;
    FOR_EACH_LLINT_SLOW_PATH(JSC_DECLARE_SLOW_PATH_TABLE_ENTRY)
    FOR_EACH_COMMON_SLOW_PATH(JSC_DECLARE_SLOW_PATH_TABLE_ENTRY)
#undef JSC_DECLARE_SLOW_PATH_TABLE_ENTRY
};

// LLIntSlowPaths.cpp and CommonSlowPaths.cpp: store the copies for the process's threads mode.
void installLLIntSlowPaths(SlowPathTable&);
void installCommonSlowPaths(SlowPathTable&);

} // namespace LLInt
} // namespace JSC
