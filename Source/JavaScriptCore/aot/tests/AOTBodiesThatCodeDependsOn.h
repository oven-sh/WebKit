/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<Vector<FunctionID>> findBodiesThatCodeDependsOn(std::span<const FunctionID> candidates, NOESCAPE const Invocable<std::optional<bool>(std::span<const FunctionID>)> auto& isSameBeside)
{
    Vector<FunctionID> dependedOn;
    auto isSameBesideFirst = [&](size_t count) {
        Vector<FunctionID> others = dependedOn;
        others.append(candidates.first(count));
        return isSameBeside(others.span());
    };
    size_t enough = candidates.size();
    if (!isSameBesideFirst(enough).value_or(false))
        return std::nullopt;
    while (enough) {
        std::optional<bool> isSameBesideDependedOn = isSameBesideFirst(0);
        if (!isSameBesideDependedOn)
            return std::nullopt;
        if (*isSameBesideDependedOn)
            break;
        size_t tooFew = 0;
        while (tooFew + 1 < enough) {
            size_t middle = tooFew + (enough - tooFew) / 2;
            std::optional<bool> isSameBesideHalf = isSameBesideFirst(middle);
            if (!isSameBesideHalf)
                return std::nullopt;
            (*isSameBesideHalf ? enough : tooFew) = middle;
        }
        dependedOn.append(candidates[--enough]);
    }
    return dependedOn;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
