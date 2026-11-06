/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// The first thing a source of aot/ includes, by its compile flags.
// A failed assertion of libstdc++ hands its file, line, function and condition to a function that prints them: 31 bytes of code
// wherever a checked operator[] is inlined, and the name of every instantiation as a string. The checks stay. In a release build a
// failure traps, and the address says where.
#if defined(__cplusplus) && defined(NDEBUG) && defined(_GLIBCXX_ASSERTIONS) && __has_include(<bits/c++config.h>)
#include <bits/c++config.h>
#ifndef __glibcxx_assert
#error "This libstdc++ does not spell its assertion __glibcxx_assert, so the definition below would replace nothing."
#endif
#undef __glibcxx_assert
#define __glibcxx_assert(condition) do { if (__builtin_expect(!bool(condition), false)) __builtin_trap(); } while (false)
#endif
