/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Opcode.h"

namespace JSC { namespace B3 {

class InsertionSet;
class Procedure;
class Value;

struct RangeReduction {
    enum class Kind : uint8_t {
        None,
        Constant,
        Value,
        ZeroExtension,
        Comparison,
    };

    Kind kind { Kind::None };
    int64_t constant { 0 };
    B3::Value* value { nullptr };
    B3::Value* right { nullptr };
    Opcode opcode { Oops };
};

JS_EXPORT_PRIVATE RangeReduction findRangeReduction(const Procedure&, Value*);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
