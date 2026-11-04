/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCellSizeClasses.h"

#if ENABLE(AOT)

#include "JSObject.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

CellSizeClasses::CellSizeClasses(double progression)
    : m_sizes(MarkedSpace::sizeClassesFor(progression))
{
}

size_t CellSizeClasses::cellSizeFor(size_t bytes) const
{
    auto sizeClass = std::ranges::lower_bound(m_sizes, bytes);
    return sizeClass == m_sizes.end() ? WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(bytes) : *sizeClass;
}

size_t CellSizeClasses::largestCellSizeWithin(size_t bytes) const
{
    auto larger = std::ranges::upper_bound(m_sizes, bytes);
    if (larger == m_sizes.end())
        return bytes - bytes % MarkedSpace::sizeStep;
    RELEASE_ASSERT(larger != m_sizes.begin());
    return *std::prev(larger);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
