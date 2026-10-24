/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

using KeyOrderFields = Vector<uint32_t, 8>;

struct ExpectedKeyOrders {
    LayoutID layout;
    Vector<KeyOrderFields, 4> orders;
    bool coversEveryObject { false };
    uint32_t numberOfOrdersLeftOut { 0 };
};

struct KeyOrderLimits {
    uint64_t maxOrders { 0 };
    uint64_t maxFields { 0 };
};

bool isEnumerableStringField(const Program&, const Field&);

ExpectedKeyOrders expectedKeyOrdersOf(const Program&, LayoutID, KeyOrderLimits);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
