/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "DeferGC.h"
#include <wtf/Noncopyable.h>

namespace JSC {

namespace AOT {

class LeafOperationScope {
    WTF_MAKE_NONCOPYABLE(LeafOperationScope);
public:
    explicit LeafOperationScope(VM& vm)
        : m_deferGC(vm)
    {
    }

private:
    DeferGCForAWhile m_deferGC;
#if ASSERT_ENABLED
    AssertNoGC m_assertNoGC;
#endif
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)
