/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTSchemaRecords.h"
#include "AOTWire.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr Witness noWitness { Wire::none, Wire::none, noSourceLocation };

constexpr bool hasFunction(const Witness& witness) { return witness.function != Wire::none; }

constexpr bool isLowerWitness(const Witness& witness, const Witness& other)
{
    if (!hasFunction(witness))
        return false;
    return !hasFunction(other) || std::tie(witness.function, witness.value) < std::tie(other.function, other.value);
}

constexpr void keepLowerWitness(Witness& current, const Witness& candidate)
{
    if (isLowerWitness(candidate, current))
        current = candidate;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
