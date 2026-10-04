/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Opcode.h"

namespace JSC { namespace AOT {

struct OpcodeTraits {
    enum : uint16_t {
        IsClassified = 1 << 0,
        ReadsArgumentList = 1 << 1,
        ReadsCalleeWithoutOperand = 1 << 2,
        NeedsOwnFrame = 1 << 3,
        LetsScopeOut = 1 << 4,
        LooksUpVariableByName = 1 << 5,
        ReachesVariables = 1 << 6,
        StoresToVariables = 1 << 7,
        MakesCalls = 1 << 8,
        Returns = 1 << 9,
        ErrorMayQuoteSource = 1 << 10,
    };
};

constexpr uint16_t traitsOf(OpcodeID opcode)
{
    switch (opcode) {
    case op_call_direct_eval:
        return OpcodeTraits::IsClassified | OpcodeTraits::ReadsCalleeWithoutOperand | OpcodeTraits::NeedsOwnFrame | OpcodeTraits::LetsScopeOut | OpcodeTraits::LooksUpVariableByName | OpcodeTraits::ReachesVariables | OpcodeTraits::MakesCalls | OpcodeTraits::ErrorMayQuoteSource;
    case op_create_scoped_arguments:
        return OpcodeTraits::IsClassified | OpcodeTraits::ReadsArgumentList | OpcodeTraits::ReadsCalleeWithoutOperand | OpcodeTraits::NeedsOwnFrame | OpcodeTraits::LetsScopeOut | OpcodeTraits::ReachesVariables;
    case op_create_direct_arguments:
    case op_create_cloned_arguments:
        return OpcodeTraits::IsClassified | OpcodeTraits::ReadsArgumentList | OpcodeTraits::ReadsCalleeWithoutOperand | OpcodeTraits::NeedsOwnFrame;
    case op_super_construct_varargs:
    case op_super_construct:
        return OpcodeTraits::IsClassified | OpcodeTraits::NeedsOwnFrame | OpcodeTraits::MakesCalls | OpcodeTraits::ErrorMayQuoteSource;
    case op_put_to_scope:
        return OpcodeTraits::IsClassified | OpcodeTraits::LooksUpVariableByName | OpcodeTraits::ReachesVariables | OpcodeTraits::StoresToVariables;
    case op_tail_call_varargs:
    case op_tail_call:
        return OpcodeTraits::IsClassified | OpcodeTraits::MakesCalls | OpcodeTraits::Returns | OpcodeTraits::ErrorMayQuoteSource;
    case op_create_rest:
        return OpcodeTraits::IsClassified | OpcodeTraits::ReadsArgumentList | OpcodeTraits::NeedsOwnFrame;
    case op_push_with_scope:
        return OpcodeTraits::IsClassified | OpcodeTraits::NeedsOwnFrame | OpcodeTraits::LetsScopeOut;
    case op_create_generator_frame_environment:
        return OpcodeTraits::IsClassified | OpcodeTraits::LetsScopeOut | OpcodeTraits::StoresToVariables;
    case op_get_from_scope:
        return OpcodeTraits::IsClassified | OpcodeTraits::LooksUpVariableByName | OpcodeTraits::ReachesVariables;
    case op_call_varargs:
    case op_iterator_next:
    case op_construct_varargs:
    case op_iterator_open:
    case op_async_iterator_open:
    case op_instanceof:
    case op_construct:
    case op_call:
    case op_call_ignore_result:
    case op_async_iterator_next:
        return OpcodeTraits::IsClassified | OpcodeTraits::MakesCalls | OpcodeTraits::ErrorMayQuoteSource;
    case op_argument_count:
        return OpcodeTraits::IsClassified | OpcodeTraits::ReadsArgumentList;
    case op_catch:
        return OpcodeTraits::IsClassified | OpcodeTraits::NeedsOwnFrame;
    case op_new_func:
    case op_new_func_exp:
    case op_new_generator_func:
    case op_new_generator_func_exp:
    case op_new_async_func:
    case op_new_async_func_exp:
    case op_new_async_generator_func:
    case op_new_async_generator_func_exp:
        return OpcodeTraits::IsClassified | OpcodeTraits::LetsScopeOut;
    case op_resolve_scope:
    case op_resolve_scope_for_hoisting_func_decl_in_eval:
        return OpcodeTraits::IsClassified | OpcodeTraits::LooksUpVariableByName;
    case op_create_lexical_environment:
        return OpcodeTraits::IsClassified | OpcodeTraits::StoresToVariables;
    case op_iterator_close_check:
        return OpcodeTraits::IsClassified | OpcodeTraits::MakesCalls;
    case op_ret:
        return OpcodeTraits::IsClassified | OpcodeTraits::Returns;
    case op_set_private_brand:
    case op_check_private_brand:
    case op_put_by_id:
    case op_get_by_id:
    case op_get_length:
    case op_put_private_name:
    case op_get_private_name:
    case op_get_by_val_with_this:
    case op_get_by_val:
    case op_put_by_val:
    case op_put_by_val_direct:
    case op_in_by_val:
    case op_enumerator_in_by_val:
    case op_enumerator_has_own_property:
    case op_enumerator_put_by_val:
    case op_enumerator_get_by_val:
    case op_get_by_id_direct:
    case op_get_prototype_of:
    case op_get_by_id_with_this:
    case op_to_object:
    case op_in_by_id:
    case op_has_private_name:
    case op_has_private_brand:
    case op_del_by_id:
    case op_del_by_val:
    case op_check_tdz:
    case op_spread:
        return OpcodeTraits::IsClassified | OpcodeTraits::ErrorMayQuoteSource;
    case op_new_reg_exp_shared:
    case op_create_generator:
    case op_create_async_generator:
    case op_create_promise:
    case op_new_array_with_size:
    case op_new_array_buffer:
    case op_profile_type:
    case op_profile_control_flow:
    case op_new_array_with_species:
    case op_create_this:
    case op_new_object:
    case op_new_array:
    case op_enumerator_next:
    case op_to_this:
    case op_jneq_ptr:
    case op_get_argument:
    case op_get_from_arguments:
    case op_get_internal_field:
    case op_put_by_id_with_this:
    case op_put_by_val_with_this:
    case op_put_getter_by_id:
    case op_put_setter_by_id:
    case op_put_getter_setter_by_id:
    case op_put_getter_by_val:
    case op_put_setter_by_val:
    case op_define_data_property:
    case op_define_accessor_property:
    case op_jmp:
    case op_jtrue:
    case op_jfalse:
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null:
    case op_jeq_ptr:
    case op_jeq:
    case op_jstricteq:
    case op_jneq:
    case op_jnstricteq:
    case op_jless:
    case op_jlesseq:
    case op_jgreater:
    case op_jgreatereq:
    case op_jnless:
    case op_jnlesseq:
    case op_jngreater:
    case op_jngreatereq:
    case op_jbelow:
    case op_jbeloweq:
    case op_loop_hint:
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
    case op_set_function_name:
    case op_strcat:
    case op_to_primitive:
    case op_to_property_key:
    case op_to_property_key_or_number:
    case op_put_to_arguments:
    case op_get_parent_scope:
    case op_throw:
    case op_throw_static_error:
    case op_debug:
    case op_get_property_enumerator:
    case op_unreachable:
    case op_yield:
    case op_check_traps:
    case op_log_shadow_chicken_prologue:
    case op_log_shadow_chicken_tail:
    case op_put_internal_field:
    case op_nop:
    case op_type_tag:
    case op_super_sampler_begin:
    case op_wide16:
    case op_super_sampler_end:
    case op_wide32:
    case op_enter:
    case op_get_scope:
    case op_new_promise:
    case op_new_generator:
    case op_new_async_function_generator:
    case op_check_type:
    case op_new_array_with_spread:
    case op_new_reg_exp:
    case op_mov:
    case op_eq:
    case op_neq:
    case op_stricteq:
    case op_nstricteq:
    case op_less:
    case op_lesseq:
    case op_greater:
    case op_greatereq:
    case op_below:
    case op_beloweq:
    case op_mod:
    case op_pow:
    case op_urshift:
    case op_add:
    case op_mul:
    case op_div:
    case op_sub:
    case op_bitand:
    case op_bitor:
    case op_bitxor:
    case op_lshift:
    case op_rshift:
    case op_eq_null:
    case op_neq_null:
    case op_to_string:
    case op_is_empty:
    case op_typeof_is_undefined:
    case op_typeof_is_object:
    case op_typeof_is_function:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_big_int:
    case op_is_object:
    case op_is_callable:
    case op_is_constructor:
    case op_inc:
    case op_dec:
    case op_negate:
    case op_not:
    case op_identity_with_profile:
    case op_typeof:
    case op_is_cell_with_type:
    case op_has_structure_with_flags:
    case op_to_number:
    case op_to_numeric:
    case op_bitnot:
    case op_unsigned:
        return OpcodeTraits::IsClassified;
    default:
        return 0;
    }
}

#define AOT_VERIFY_OPCODE_IS_CLASSIFIED(opcode, length) \
    static_assert(!!(traitsOf(opcode) & OpcodeTraits::IsClassified), #opcode " has to be given its traits in traitsOf() and a lowering");
FOR_EACH_BYTECODE_ID(AOT_VERIFY_OPCODE_IS_CLASSIFIED)
#undef AOT_VERIFY_OPCODE_IS_CLASSIFIED

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
