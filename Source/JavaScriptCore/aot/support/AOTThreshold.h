/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// A threshold decides how fast or how large the code is, never what a program prints: how many members are told apart, how deep a walk goes, from which size on another road is taken.
// Most are so high that only a large program meets them.
// AOT_THRESHOLD stands in a namespace or a function, AOT_CLASS_THRESHOLD in a class.
#define AOT_THRESHOLD(type, name, ...) static constexpr type name = __VA_ARGS__
#define AOT_CLASS_THRESHOLD(type, name, ...) static constexpr type name = __VA_ARGS__
