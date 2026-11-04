/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What makeObjectsAgainWhereTheyLeave() asks of the walker's own members.

#include "config.h"
#include "AOTWalkKnownJoins.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

const BodyLiveness& KnownJoin::livenessOf(Walker& walker, const BodyIndex& index)
{
    if (!walker.m_liveness.isComputedFor(index))
        walker.m_liveness.compute(index);
    return walker.m_liveness;
}

bool KnownJoin::someListIsWalkedByIndex(const Walker& walker)
{
    return !walker.m_iterationsByIndex.isEmpty();
}

bool KnownJoin::standsUnderHandler(const Walker& walker)
{
    return walker.isUnderHandler();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
