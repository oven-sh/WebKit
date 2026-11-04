/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CommonSlowPaths.h"

namespace JSC {

JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_bind_args);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_load_cell);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_store_cell);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_list_append);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_load_global);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_store_global);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_load_name);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_store_name);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_get_attr);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_set_attr);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_get_method);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_send);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_generator_enter);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_generator_suspend);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_generator_return);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_generator_registers);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_instrumented); // In PyMonitoring.cpp.
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_prepare_construct);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_finish_construct);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_get_item);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_set_item);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_binary);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_add);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_sub);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_mul);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_div);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_floor_div);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_mod);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_eq);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jeq);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jneq);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_ne);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jne);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jnne);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_lt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jlt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jnlt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_le);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jle);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jnle);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_gt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jgt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jngt);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_ge);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jge);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jnge);
// For the interpreter alone: see "Calls that instructions make" in PySlowPaths.cpp.
#define DECLARE_SLOW_PATHS_OF_JUMP(name) \
    JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_##name##_begin); \
    JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_##name##_again); \
    JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_##name##_finish);
#define DECLARE_SLOW_PATHS_OF_COMPARISON(name) \
    JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_##name##_again); \
    DECLARE_SLOW_PATHS_OF_JUMP(j##name) \
    DECLARE_SLOW_PATHS_OF_JUMP(jn##name)
DECLARE_SLOW_PATHS_OF_COMPARISON(eq)
DECLARE_SLOW_PATHS_OF_COMPARISON(ne)
DECLARE_SLOW_PATHS_OF_COMPARISON(lt)
DECLARE_SLOW_PATHS_OF_COMPARISON(le)
DECLARE_SLOW_PATHS_OF_COMPARISON(gt)
DECLARE_SLOW_PATHS_OF_COMPARISON(ge)
#undef DECLARE_SLOW_PATHS_OF_COMPARISON
#undef DECLARE_SLOW_PATHS_OF_JUMP
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_binary_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_add_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_sub_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_mul_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_div_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_floor_div_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_mod_again);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_unary);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_compare);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jtrue);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_jfalse);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_get_iter);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_for_iter);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_unpack);
JSC_DECLARE_COMMON_SLOW_PATH(slow_path_py_runtime);

} // namespace JSC
