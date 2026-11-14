/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <cstdint>

namespace JSC {

// An argument of LinkTimeConstant::decimalOperator in bytecode, which can be cached: a change to the values comes with a new
// cachedTypesFormatRevision.
enum class DecimalOperator : uint8_t { Add, Subtract, Multiply, Divide, Remainder, Equal, NotEqual, Less, LessOrEqual, Greater, GreaterOrEqual, Negate, Increment, Decrement, ToNumeric };

} // namespace JSC
