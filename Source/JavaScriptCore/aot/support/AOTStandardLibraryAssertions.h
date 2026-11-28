/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if defined(__cplusplus) && defined(NDEBUG) && defined(_GLIBCXX_ASSERTIONS) && __has_include(<bits/c++config.h>)
#include <bits/c++config.h>
#ifndef __glibcxx_assert
#error "This libstdc++ does not spell its assertion __glibcxx_assert, so the definition below would replace nothing."
#endif
#undef __glibcxx_assert
#define __glibcxx_assert(condition) do { if (__builtin_expect(!bool(condition), false)) __builtin_trap(); } while (false)
#endif
