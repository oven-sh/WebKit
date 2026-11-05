/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Opcode.h"
#include "B3Type.h"
#include <optional>

namespace JSC { namespace B3 {

class Value;

// Select(Equal(x, a), b, x), however it is spelled, is x with the constant a replaced by the constant b.
// Both constants are the bits of a value of the type of x. The test is on the bits of x, which are x itself or a BitwiseCast away from it.
struct ConstantReplacement {
    Value* original;
    Value* testedBits;
    uint64_t replaced;
    uint64_t replacement;
};

std::optional<uint64_t> bitsOfConstant(const Value*);
std::optional<ConstantReplacement> findConstantReplacement(Value*);
std::optional<bool> evaluateComparisonOfBits(Opcode, Type, uint64_t left, uint64_t right);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
