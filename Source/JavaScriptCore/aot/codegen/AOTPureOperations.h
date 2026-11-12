/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"

namespace JSC { namespace AOT { namespace Typed {

enum class PureOperation : uint16_t {
    StringConstant,
    KeyConstant,
    BigIntConstant,
    BuiltinCell,
    ToInt32,
    TruncateToInt32,
    TruncateToInt64,
    ExactInt32,
    NonNegativeInt32,
    RuntimeOperation,
    BoxNumber,
    ResolveCallTarget,
    ReadFieldInExtension,
    ReadFieldInExtensionInLine,
    LengthOfFixedView,
    VectorOfFixedView,
    ModuleNamespace,
    ModuleBuiltinCell,
    SingletonClosure,
    ClampToByte,
};

constexpr Machine::PureFunction pureFunction(PureOperation operation, uint32_t payload = 0, bool dependsOnChildren = false)
{
    return { static_cast<uint64_t>(operation) << 32 | payload, dependsOnChildren };
}

constexpr Machine::PureFunction truncationToInt64(uint32_t payload)
{
    return { static_cast<uint64_t>(PureOperation::TruncateToInt64) << 32 | payload, true, true };
}

constexpr Machine::PureFunction functionOfMemory(PureOperation operation, uint32_t payload)
{
    return { static_cast<uint64_t>(operation) << 32 | payload, true, false, true };
}

static_assert(pureFunction(PureOperation::StringConstant, 7).id != pureFunction(PureOperation::KeyConstant, 7).id);
static_assert(pureFunction(PureOperation::StringConstant, UINT32_MAX).id < pureFunction(PureOperation::KeyConstant).id);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
