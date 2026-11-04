/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

// What makeObjectsAgainWhereTheyLeave() asks of the walker's own members.
class KnownJoin {
public:
    static bool standsUnderHandler(const Walker&);
    static bool someListIsWalkedByIndex(const Walker&);
    static const BodyLiveness& livenessOf(Walker&, const BodyIndex&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
