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

void LimitValues::lower(LimitID limit, uint64_t value)
{
    RELEASE_ASSERT(canBeLowered(limit));
    RELEASE_ASSERT(value <= (*this)[limit]);
    m_values[static_cast<size_t>(limit)] = value;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
