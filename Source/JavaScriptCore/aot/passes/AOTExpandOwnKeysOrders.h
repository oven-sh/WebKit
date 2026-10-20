/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

using KeyOrderFields = Vector<uint32_t, 8>;

enum class KeyOrderCoverage : uint8_t {
    Unknown,
    EveryObject,
};

struct ExpectedKeyOrders {
    LayoutID layout;
    Vector<KeyOrderFields, 4> orders;
    KeyOrderCoverage coverage { KeyOrderCoverage::Unknown };
    uint32_t numberOfOrdersLeftOut { 0 };
};

struct KeyOrderLimits {
    uint64_t maxOrders { 0 };
    uint64_t maxFields { 0 };
};

bool isEnumerableStringField(const Program&, const Field&);

ExpectedKeyOrders expectedKeyOrdersOf(const Program&, LayoutID, KeyOrderLimits);

std::optional<KeyOrderFields> keyOrderAtCreation(const Program&, const Function&, FunctionAnalyses&, ValueID object);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
