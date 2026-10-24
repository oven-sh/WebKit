/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLimits.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT {

LimitValues::LimitValues(Architecture architecture)
{
    for (LimitID limit : allMembersOf<LimitID>)
        m_values[static_cast<size_t>(limit)] = defaultLimitValue(limit, architecture);
}

void LimitValues::lower(LimitID lowered, uint64_t value)
{
    RELEASE_ASSERT(canBeLowered(lowered));
    RELEASE_ASSERT(value <= (*this)[lowered]);
    for (LimitID limit : allMembersOf<LimitID>)
        m_values[static_cast<size_t>(limit)] = valueWhenLowered(limit, (*this)[limit], lowered, value);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
