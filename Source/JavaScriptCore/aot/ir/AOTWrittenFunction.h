/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

inline FunctionID writtenFunctionOf(const Program& program, FunctionID id)
{
    while (FunctionID source = program.at(id).derivation.source)
        id = source;
    return id;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
