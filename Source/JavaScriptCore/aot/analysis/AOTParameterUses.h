/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ParameterUse : uint8_t {
    Used,
    ForwardedOnly,
    Unused,
};

struct ForwardedParameter {
    uint32_t parameter;
    FunctionID callee;
    uint32_t calleeParameter;
};

Vector<ParameterUse, 8> classifyParameterUses(const Function&, const BitVector& trackedEntryParameters, Vector<ForwardedParameter>* forwarded);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
