/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3PatchpointValue.h"

namespace JSC { namespace AOT { namespace Typed {

enum class PureOperation : uint16_t {
    StringConstant,
    KeyConstant,
    BigIntConstant,
    BuiltinCell,
    ModuleBuiltinCell,
    ToInt32,
    TruncateToInt64,
    ResolveCallTarget,
    NonNegativeInt32,
    RuntimeOperation,
    LiteralStringToString,
    ToLiteralString,
};

constexpr B3::PureFunction pureFunction(PureOperation operation, uint32_t payload = 0, bool dependsOnChildren = false)
{
    return { static_cast<uint64_t>(operation) << 32 | payload, dependsOnChildren };
}

static_assert(pureFunction(PureOperation::StringConstant, 7).id != pureFunction(PureOperation::KeyConstant, 7).id);
static_assert(pureFunction(PureOperation::StringConstant, UINT32_MAX).id < pureFunction(PureOperation::KeyConstant).id);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
