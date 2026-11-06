/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3Limits.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

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
    std::array<InvariantTerm, Limits::maxTermsPerInvariantSum> terms { };
    unsigned numberOfTerms { 0 };
    int64_t constant { 0 };

    bool isConstant() const { return !numberOfTerms; }
    std::span<const InvariantTerm> usedTerms() const { return std::span { terms }.first(numberOfTerms); }
};

// scale * counter + offset over the integers. The described value equals this modulo 2^width.
struct AffineIndex {
    InvariantSum scale;
    InvariantSum offset;
};

JS_EXPORT_PRIVATE bool isSameExceptConstantOffset(const AffineIndex&, const AffineIndex&);

// Ranges of Int64 arithmetic; nullopt if it can overflow.
using CheckedRange = std::optional<IntRange>;

struct AffineIndexInputs {
    const Procedure& proc;
    Value* counter;
    CheckedRange counterRange;
    const ScopedLambda<Value*(Value*)>& canonicalInvariant;
};

JS_EXPORT_PRIVATE bool findAffineIndex(const AffineIndexInputs&, Value*, AffineIndex&);

JS_EXPORT_PRIVATE CheckedRange checkedAdd(const CheckedRange&, const CheckedRange&);
JS_EXPORT_PRIVATE CheckedRange checkedMultiply(const CheckedRange&, const CheckedRange&);
JS_EXPORT_PRIVATE CheckedRange checkedRangeOf(const Procedure&, const InvariantSum&);
JS_EXPORT_PRIVATE CheckedRange checkedRangeOf(const Procedure&, Value*, Signedness);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
