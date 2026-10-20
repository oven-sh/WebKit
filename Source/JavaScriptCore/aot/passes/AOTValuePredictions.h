/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct ValuePrediction {
    ValueID value;
    TypeID predicted;
};

bool predictValues(FunctionPassContext&, std::span<const ValuePrediction>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
