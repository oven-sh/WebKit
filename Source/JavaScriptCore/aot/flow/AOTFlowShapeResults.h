/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"

namespace JSC { namespace AOT { namespace Typed {

enum class ShapePermanence : uint8_t {
    Unknown,
    CannotBeLost,
};

constexpr ShapePermanence shapePermanenceOf(const AllocationClass& allocationClass)
{
    bool isProven = allocationClass.layout && allocationClass.impossibleMutations.contains(ObjectMutation::LoseShape);
    return isProven ? ShapePermanence::CannotBeLost : ShapePermanence::Unknown;
}

ShapePermanence shapePermanenceOf(const Program&, const FlowResults&, LayoutID);
ShapePermanence shapePermanenceOf(const Program&, const FlowResults&, const LayoutSet&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
