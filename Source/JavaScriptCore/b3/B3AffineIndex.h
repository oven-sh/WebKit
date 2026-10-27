/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include <optional>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;
class Value;

// coefficient * first * second, where a null factor is 1. An Int32 factor is read as signed.
struct InvariantTerm {
    int64_t coefficient { 0 };
    Value* first { nullptr };
    Value* second { nullptr };

    friend bool operator==(const InvariantTerm&, const InvariantTerm&) = default;
};

struct InvariantSum {
    Vector<InvariantTerm, 2> terms;
    int64_t constant { 0 };

    bool isConstant() const { return terms.isEmpty(); }

    friend bool operator==(const InvariantSum&, const InvariantSum&) = default;
};

// scale * counter + offset over the integers. The described value equals this modulo 2^width.
struct AffineIndex {
    InvariantSum scale;
    InvariantSum offset;
};

struct AffineIndexInputs {
    const Procedure& proc;
    Value* counter;
    IntegerInterval counterRange;
    const ScopedLambda<Value*(Value*)>& canonicalInvariant;
};

JS_EXPORT_PRIVATE std::optional<AffineIndex> affineIndexOf(const AffineIndexInputs&, Value*);

// Ranges of Int64 arithmetic; nullopt if it can overflow.
using CheckedRange = std::optional<IntegerInterval>;

JS_EXPORT_PRIVATE CheckedRange checkedAdd(const CheckedRange&, const CheckedRange&);
JS_EXPORT_PRIVATE CheckedRange checkedMultiply(const CheckedRange&, const CheckedRange&);
inline CheckedRange checkedConstant(int64_t constant) { return IntegerInterval { constant, constant }; }
JS_EXPORT_PRIVATE CheckedRange checkedRangeOf(const Procedure&, const InvariantSum&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
