/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include <optional>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

struct BrokenResultRule {
    enum class Law : uint8_t { NarrowerOperandGivesNarrowerResult, BothPathsAgree };

    Law law { Law::NarrowerOperandGivesNarrowerResult };
    ValueID value;
    uint32_t operandIndex { 0 };
    Type narrowOperand;
    Type resultOfNarrowOperand;
    Type wideOperand;
    Type resultOfWideOperand;

    void dump(PrintStream&, const Program&, const Function&) const;
};

using ShapesWithMonotoneResult = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

std::optional<BrokenResultRule> findBrokenResultRule(const Program&, Function&, TypeArena&, ShapesWithMonotoneResult&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
